/*
 * %CopyrightBegin%
 *
 * Copyright Ericsson AB 2020-2022. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * %CopyrightEnd%
 */
#include <algorithm>
#include "beam_asm.hpp"

using namespace asmjit;

extern "C"
{
#include "erl_map.h"
#include "erl_term_hashing.h"
#include "beam_common.h"
}

static const Uint32 INTERNAL_HASH_SALT = 3432918353;
static const Uint32 HCONST_22 = 0x98C475E6UL;
static const Uint32 HCONST = 0x9E3779B9;

/* ARG3 = incoming hash
 * ARG6 = lower 32
 * ARG7 = upper 32
 * ARG8 = type constant
 *
 * Helper function for calculating the internal hash of keys before looking
 * them up in a map.
 *
 * This is essentially just a manual expansion of the `UINT32_HASH_2` macro.
 * Whenever the internal hash algorithm is updated, this and all of its users
 * must follow suit.
 *
 * Result is returned in ARG3. All arguments are clobbered. */
void BeamGlobalAssembler::emit_internal_hash_helper() {
    a64::Gp hash = ARG3.w(), lower = ARG6.w(), upper = ARG7.w(),
            constant = ARG8.w();

    a.add(lower, lower, constant);
    a.add(upper, upper, constant);

#if defined(ERL_INTERNAL_HASH_CRC32C)
    a.crc32cw(lower, hash, lower);
    a.add(hash, hash, lower);
    a.crc32cw(hash, hash, upper);
#else
    using rounds =
            std::initializer_list<std::tuple<a64::Gp, a64::Gp, a64::Gp, int>>;
    for (const auto &round : rounds{{lower, upper, hash, 13},
                                    {upper, hash, lower, -8},
                                    {hash, lower, upper, 13},
                                    {lower, upper, hash, 12},
                                    {upper, hash, lower, -16},
                                    {hash, lower, upper, 5},
                                    {lower, upper, hash, 3},
                                    {upper, hash, lower, -10},
                                    {hash, lower, upper, 15}}) {
        const auto &[r_a, r_b, r_c, shift] = round;

        a.sub(r_a, r_a, r_b);
        a.sub(r_a, r_a, r_c);

        if (shift > 0) {
            a.eor(r_a, r_a, r_c, arm::lsr(shift));
        } else {
            a.eor(r_a, r_a, r_c, arm::lsl(-shift));
        }
    }
#endif

    a.ret(a64::x30);
}

/* ARG1 = untagged hash map root
 * ARG2 = key
 * ARG3 = key hash
 * ARG4 = node header
 *
 * Result is returned in RET. ZF is set on success. */
void BeamGlobalAssembler::emit_hashmap_get_element() {
    Label node_loop = a.newLabel();

    arm::Gp node = ARG1, key = ARG2, key_hash = ARG3, header_val = ARG4,
            depth = TMP4, index = TMP5, one = TMP6;

    const int header_shift =
            (_HEADER_ARITY_OFFS + MAP_HEADER_TAG_SZ + MAP_HEADER_ARITY_SZ);

    /* Skip the root header, which is two words long (child headers are one
     * word). */
    a.add(node, node, imm(sizeof(Eterm[2])));
    mov_imm(depth, 0);

    a.bind(node_loop);
    {
        Label fail = a.newLabel(), leaf_node = a.newLabel(),
              skip_index_adjustment = a.newLabel(), update_hash = a.newLabel();

        /* Find out which child we should follow, and shift the hash for the
         * next round. */
        a.and_(index, key_hash, imm(0xF));
        a.lsr(key_hash, key_hash, imm(4));
        a.add(depth, depth, imm(1));

        /* The entry offset is always equal to the index on fully populated
         * nodes, so we'll skip adjusting them. */
        ERTS_CT_ASSERT(header_shift == 16);
        a.asr(header_val.w(), header_val.w(), imm(header_shift));
        a.cmn(header_val.w(), imm(1));
        a.b_eq(skip_index_adjustment);
        {
            /* If our bit isn't set on this node, the key can't be found.
             *
             * Note that we jump directly to the return sequence as ZF is clear
             * at this point. */
            a.lsr(TMP1, header_val, index);
            a.tbz(TMP1, imm(0), fail);

            /* The actual offset of our entry is the number of bits set (in
             * essence "entries present") before our index in the bitmap. Clear
             * the upper bits and count the remainder. */
            a.lsl(TMP2, TMP1, index);
            a.eor(TMP1, TMP2, header_val);
            a.fmov(a64::d0, TMP1);
            a.cnt(a64::v0.b8(), a64::v0.b8());
            a.addv(a64::b0, a64::v0.b8());
            a.fmov(index, a64::d0);
        }
        a.bind(skip_index_adjustment);

        a.ldr(TMP1, arm::Mem(node, index, arm::lsl(3)));
        emit_untag_ptr(node, TMP1);

        /* Have we found our leaf? */
        emit_is_boxed(leaf_node, TMP1);

        /* Nope, we have to search another node. Read and skip past the header
         * word. */
        a.ldr(header_val, arm::Mem(node).post(sizeof(Eterm)));

        /* After 8 nodes we've run out of the 32 bits we started with, so we
         * need to update the hash to keep going. */
        a.tst(depth, imm(0x7));
        a.b_eq(update_hash);
        a.b(node_loop);

        a.bind(leaf_node);
        {
            /* We've arrived at a leaf, set ZF according to whether its key
             * matches ours and speculatively place the element in ARG1. */
            a.ldp(TMP1, ARG1, arm::Mem(node));
            a.cmp(TMP1, key);

            /* See comment at the jump. */
            a.bind(fail);
            a.ret(a64::x30);
        }

        /* After 8 nodes we've run out of the 32 bits we started with, so we
         * must calculate a new hash to continue.
         *
         * This is a manual expansion `make_map_hash` from utils.c, and all
         * changes to that function must be mirrored here. */
        a.bind(update_hash);
        {
            emit_enter_runtime_frame();

            /* NOTE: ARG3 (key_hash) is always 0 at this point. */
            a.lsr(ARG6, depth, imm(3));
            mov_imm(ARG7, 1);
            mov_imm(ARG8, HCONST_22);
            a.bl(labels[internal_hash_helper]);

            mov_imm(TMP1, INTERNAL_HASH_SALT);
            a.eor(ARG3, ARG3, TMP1);

            a.mov(ARG6.w(), key.w());
            a.lsr(ARG7, key, imm(32));
            mov_imm(ARG8, HCONST);
            a.bl(labels[internal_hash_helper]);

            emit_leave_runtime_frame();

            a.b(node_loop);
        }
    }
}

/* ARG1 = untagged flat map
 * ARG2 = key
 * ARG5 = size
 *
 * Result is returned in ARG1. ZF is set on success. */
void BeamGlobalAssembler::emit_flatmap_get_element() {
    Label fail = a.newLabel(), loop = a.newLabel();

    /* Bump size by 1 to slide past the `keys` field in the map, and the header
     * word in the key array. Also set flags to ensure ZF == 0 when entering
     * the loop. */
    a.adds(ARG5, ARG5, imm(1));

    /* Adjust our map pointer to the `keys` field before loading it. This
     * saves us from having to bump it to point at the values later on. */
    a.ldr(TMP1, arm::Mem(ARG1).pre(offsetof(flatmap_t, keys)));
    emit_untag_ptr(TMP1, TMP1);

    a.bind(loop);
    {
        a.sub(ARG5, ARG5, imm(1));
        a.cbz(ARG5, fail);

        a.ldr(TMP4, arm::Mem(TMP1, ARG5, arm::lsl(3)));
        a.cmp(ARG2, TMP4);
        a.b_ne(loop);
    }

    a.ldr(ARG1, arm::Mem(ARG1, ARG5, arm::lsl(3)));

    a.bind(fail);
    a.ret(a64::x30);
}

void BeamGlobalAssembler::emit_new_map_shared() {
    emit_enter_runtime_frame();
    emit_enter_runtime<Update::eStack | Update::eHeap | Update::eXRegs |
                       Update::eReductions>();

    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    runtime_call<5>(erts_gc_new_map);

    emit_leave_runtime<Update::eStack | Update::eHeap | Update::eXRegs |
                       Update::eReductions>();
    emit_leave_runtime_frame();

    a.ret(a64::x30);
}

void BeamModuleAssembler::emit_new_map(const ArgRegister &Dst,
                                       const ArgWord &Live,
                                       const ArgWord &Size,
                                       const Span<ArgVal> &args) {
    embed_vararg_rodata(args, ARG5);

    mov_arg(ARG3, Live);
    mov_imm(ARG4, args.size());
    fragment_call(ga->get_new_map_shared());

    mov_arg(Dst, ARG1);
}

void BeamGlobalAssembler::emit_i_new_small_map_lit_shared() {
    emit_enter_runtime_frame();
    emit_enter_runtime<Update::eStack | Update::eHeap | Update::eXRegs |
                       Update::eReductions>();

    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    runtime_call<5>(erts_gc_new_small_map_lit);

    emit_leave_runtime<Update::eStack | Update::eHeap | Update::eXRegs |
                       Update::eReductions>();
    emit_leave_runtime_frame();

    a.ret(a64::x30);
}

void BeamModuleAssembler::emit_i_new_small_map_lit(const ArgRegister &Dst,
                                                   const ArgWord &Live,
                                                   const ArgLiteral &Keys,
                                                   const ArgWord &Size,
                                                   const Span<ArgVal> &args) {
    ASSERT(Size.get() == args.size());

    embed_vararg_rodata(args, ARG5);

    ASSERT(Keys.isLiteral());
    mov_arg(ARG3, Keys);
    mov_arg(ARG4, Live);

    fragment_call(ga->get_i_new_small_map_lit_shared());

    mov_arg(Dst, ARG1);
}

/* ARG1 = map
 * ARG2 = key
 *
 * Result is returned in RET. ZF is set on success. */
void BeamGlobalAssembler::emit_i_get_map_element_shared() {
    Label generic = a.newLabel(), hashmap = a.newLabel();

    a.and_(TMP1, ARG2, imm(_TAG_PRIMARY_MASK));
    a.cmp(TMP1, imm(TAG_PRIMARY_IMMED1));
    a.b_ne(generic);

    emit_untag_ptr(ARG1, ARG1);

    /* hashmap_get_element expects node header in ARG4, flatmap_get_element
     * expects size in ARG5 */
    ERTS_CT_ASSERT_FIELD_PAIR(flatmap_t, thing_word, size);
    a.ldp(ARG4, ARG5, arm::Mem(ARG1));
    a.and_(TMP1, ARG4, imm(_HEADER_MAP_SUBTAG_MASK));
    a.cmp(TMP1, imm(HAMT_SUBTAG_HEAD_FLATMAP));
    a.b_ne(hashmap);

    emit_flatmap_get_element();

    a.bind(generic);
    {
        emit_enter_runtime_frame();

        emit_enter_runtime();
        runtime_call<2>(get_map_element);
        emit_leave_runtime();

        a.cmp(ARG1, imm(THE_NON_VALUE));

        /* Invert ZF, we want it to be set when RET is a value. */
        a.cset(TMP1, arm::CondCode::kEQ);
        a.tst(TMP1, TMP1);

        emit_leave_runtime_frame();
        a.ret(a64::x30);
    }

    a.bind(hashmap);
    {
        emit_enter_runtime_frame();

        /* Calculate the internal hash of ARG2 before diving into the HAMT. */
        mov_imm(ARG3, INTERNAL_HASH_SALT);
        a.mov(ARG6.w(), ARG2.w());
        a.lsr(ARG7, ARG2, imm(32));
        mov_imm(ARG8, HCONST);
        a.bl(labels[internal_hash_helper]);

        emit_leave_runtime_frame();

        emit_hashmap_get_element();
    }
}

void BeamModuleAssembler::emit_i_get_map_element(const ArgLabel &Fail,
                                                 const ArgRegister &Src,
                                                 const ArgRegister &Key,
                                                 const ArgRegister &Dst) {
    mov_arg(ARG1, Src);
    mov_arg(ARG2, Key);

    if (masked_types(Key, BEAM_TYPE_MASK_IMMEDIATE) != BEAM_TYPE_NONE) {
        fragment_call(ga->get_i_get_map_element_shared());
        a.b_ne(resolve_beam_label(Fail, disp1MB));
    } else {
        emit_enter_runtime();
        runtime_call<2>(get_map_element);
        emit_leave_runtime();

        emit_branch_if_not_value(ARG1, resolve_beam_label(Fail, dispUnknown));
    }

    /*
     * Don't store the result if the destination is the scratch X register.
     * (This instruction was originally a has_map_fields instruction.)
     */
    if (!(Dst.isXRegister() && Dst.as<ArgXRegister>().get() == SCRATCH_X_REG)) {
        mov_arg(Dst, ARG1);
    }
}

void BeamModuleAssembler::emit_i_get_map_elements(const ArgLabel &Fail,
                                                  const ArgSource &Src,
                                                  const ArgWord &Size,
                                                  const Span<ArgVal> &args) {
    Label generic = a.newLabel(), next = a.newLabel();

    /* We're not likely to gain much from inlining huge extractions, and the
     * resulting code is quite large, so we'll cut it off after a handful
     * elements.
     *
     * Note that the arguments come in flattened triplets of
     * `{Key, Dst, KeyHash}` */
    bool can_inline = args.size() < (8 * 3);

    ASSERT(Size.get() == args.size());
    ASSERT((Size.get() % 3) == 0);

    for (size_t i = 0; i < args.size(); i += 3) {
        can_inline &= args[i].isImmed();
    }

    mov_arg(ARG1, Src);

    if (can_inline) {
        comment("simplified multi-element lookup");

        emit_untag_ptr(TMP1, ARG1);

        ERTS_CT_ASSERT_FIELD_PAIR(flatmap_t, thing_word, size);
        a.ldp(TMP2, TMP3, arm::Mem(TMP1, offsetof(flatmap_t, thing_word)));
        a.and_(TMP2, TMP2, imm(_HEADER_MAP_SUBTAG_MASK));
        a.cmp(TMP2, imm(HAMT_SUBTAG_HEAD_FLATMAP));
        a.b_ne(generic);

        check_pending_stubs();

        /* Bump size by 1 to slide past the `keys` field in the map, and the
         * header word in the key array. */
        a.add(TMP3, TMP3, imm(1));

        /* Adjust our map pointer to the `keys` field before loading it. This
         * saves us from having to bump it to point at the values later on. */
        ERTS_CT_ASSERT(sizeof(flatmap_t) ==
                       offsetof(flatmap_t, keys) + sizeof(Eterm));
        a.ldr(TMP2, arm::Mem(TMP1).pre(offsetof(flatmap_t, keys)));
        emit_untag_ptr(TMP2, TMP2);

        for (ssize_t i = args.size() - 3; i >= 0; i -= 3) {
            Label loop = a.newLabel();

            a.bind(loop);
            {
                a.subs(TMP3, TMP3, imm(1));
                a.b_eq(resolve_beam_label(Fail, disp1MB));

                a.ldr(TMP4, arm::Mem(TMP2, TMP3, arm::lsl(3)));

                const auto &Comparand = args[i];
                cmp_arg(TMP4, Comparand);
                a.b_ne(loop);
            }

            /* Don't store the result if the destination is the scratch X
             * register. (This instruction was originally a has_map_fields
             * instruction.) */
            const auto &Dst = args[i + 1];
            if (!(Dst.isXRegister() &&
                  Dst.as<ArgXRegister>().get() == SCRATCH_X_REG)) {
                mov_arg(Dst, arm::Mem(TMP1, TMP3, arm::lsl(3)));
            }
        }

        a.b(next);
    }

    a.bind(generic);
    {
        embed_vararg_rodata(args, ARG5);

        a.mov(ARG3, E);

        emit_enter_runtime<Update::eXRegs>();

        mov_imm(ARG4, args.size() / 3);
        load_x_reg_array(ARG2);
        runtime_call<5>(beam_jit_get_map_elements);

        emit_leave_runtime<Update::eXRegs>();

        a.cbz(ARG1, resolve_beam_label(Fail, disp1MB));
    }

    a.bind(next);
}

/* ARG1 = map
 * ARG2 = key
 * ARG3 = key hash
 *
 * Result is returned in RET. ZF is set on success. */
void BeamGlobalAssembler::emit_i_get_map_element_hash_shared() {
    Label hashmap = a.newLabel();

    emit_untag_ptr(ARG1, ARG1);

    /* hashmap_get_element expects node header in ARG4, flatmap_get_element
     * expects size in ARG5 */
    ERTS_CT_ASSERT_FIELD_PAIR(flatmap_t, thing_word, size);
    a.ldp(ARG4, ARG5, arm::Mem(ARG1));
    a.and_(TMP1, ARG4, imm(_HEADER_MAP_SUBTAG_MASK));
    a.cmp(TMP1, imm(HAMT_SUBTAG_HEAD_FLATMAP));
    a.b_ne(hashmap);

    emit_flatmap_get_element();

    a.bind(hashmap);
    emit_hashmap_get_element();
}

void BeamModuleAssembler::emit_i_get_map_element_hash(const ArgLabel &Fail,
                                                      const ArgRegister &Src,
                                                      const ArgConstant &Key,
                                                      const ArgWord &Hx,
                                                      const ArgRegister &Dst) {
    mov_arg(ARG1, Src);
    mov_arg(ARG2, Key);
    mov_arg(ARG3, Hx);

    if (Key.isImmed()) {
        fragment_call(ga->get_i_get_map_element_hash_shared());
        a.b_ne(resolve_beam_label(Fail, disp1MB));
    } else {
        emit_enter_runtime();
        runtime_call<3>(get_map_element_hash);
        emit_leave_runtime();

        emit_branch_if_not_value(ARG1, resolve_beam_label(Fail, dispUnknown));
    }

    /*
     * Don't store the result if the destination is the scratch X register.
     * (This instruction was originally a has_map_fields instruction.)
     */
    if (!(Dst.isXRegister() && Dst.as<ArgXRegister>().get() == SCRATCH_X_REG)) {
        mov_arg(Dst, ARG1);
    }
}

/* ARG3 = live registers, ARG4 = update vector size, ARG5 = update vector. */
void BeamGlobalAssembler::emit_update_map_assoc_shared() {
    emit_enter_runtime_frame();
    emit_enter_runtime<Update::eStack | Update::eHeap | Update::eXRegs |
                       Update::eReductions>();

    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    runtime_call<5>(erts_gc_update_map_assoc);

    emit_leave_runtime<Update::eStack | Update::eHeap | Update::eXRegs |
                       Update::eReductions>();
    emit_leave_runtime_frame();

    a.ret(a64::x30);
}

void BeamModuleAssembler::emit_update_map_assoc(const ArgSource &Src,
                                                const ArgRegister &Dst,
                                                const ArgWord &Live,
                                                const ArgWord &Size,
                                                const Span<ArgVal> &args) {
    auto src_reg = load_source(Src, TMP1);

    ASSERT(Size.get() == args.size());

    embed_vararg_rodata(args, ARG5);

    mov_arg(ArgXRegister(Live.get()), src_reg.reg);
    mov_arg(ARG3, Live);
    mov_imm(ARG4, args.size());

    fragment_call(ga->get_update_map_assoc_shared());

    mov_arg(Dst, ARG1);
}

/* ARG3 = live registers, ARG4 = update vector size, ARG5 = update vector.
 *
 * Result is returned in RET, error is indicated by ZF. */
void BeamGlobalAssembler::emit_update_map_exact_guard_shared() {
    emit_enter_runtime_frame();
    emit_enter_runtime<Update::eStack | Update::eHeap | Update::eXRegs |
                       Update::eReductions>();

    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    runtime_call<5>(erts_gc_update_map_exact);

    emit_leave_runtime<Update::eStack | Update::eHeap | Update::eXRegs |
                       Update::eReductions>();
    emit_leave_runtime_frame();

    a.ret(a64::x30);
}

/* ARG3 = live registers, ARG4 = update vector size, ARG5 = update vector.
 *
 * Does not return on error. */
void BeamGlobalAssembler::emit_update_map_exact_body_shared() {
    Label error = a.newLabel();

    emit_enter_runtime_frame();
    emit_enter_runtime<Update::eStack | Update::eHeap | Update::eXRegs |
                       Update::eReductions>();

    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    runtime_call<5>(erts_gc_update_map_exact);

    emit_leave_runtime<Update::eStack | Update::eHeap | Update::eXRegs |
                       Update::eReductions>();
    emit_leave_runtime_frame();

    emit_branch_if_not_value(ARG1, error);
    a.ret(a64::x30);

    a.bind(error);
    {
        mov_imm(ARG4, 0);
        a.b(labels[raise_exception]);
    }
}

void BeamModuleAssembler::emit_update_map_exact(const ArgSource &Src,
                                                const ArgLabel &Fail,
                                                const ArgRegister &Dst,
                                                const ArgWord &Live,
                                                const ArgWord &Size,
                                                const Span<ArgVal> &args) {
    auto src_reg = load_source(Src, TMP1);

    ASSERT(Size.get() == args.size());

    embed_vararg_rodata(args, ARG5);

    /* We _KNOW_ Src is a map */

    mov_arg(ArgXRegister(Live.get()), src_reg.reg);
    mov_arg(ARG3, Live);
    mov_imm(ARG4, args.size());

    if (Fail.get() != 0) {
        fragment_call(ga->get_update_map_exact_guard_shared());
        emit_branch_if_not_value(ARG1, resolve_beam_label(Fail, dispUnknown));
    } else {
        fragment_call(ga->get_update_map_exact_body_shared());
    }

    mov_arg(Dst, ARG1);
}
