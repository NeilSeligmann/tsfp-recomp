/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1477 appendix census: roots that earlier records named (docs/t1477-appendix-census.md).
 * Each body passed the fixed matrix (seeds 20261001/20261006, O0/O2/O3, 600 random cases,
 * verdicts >= 100, coverage >= 0.9, null-agree <= 0.9, 0 DISAGREE) and is the frozen draft
 * of docs/data/t1477-appendix-census/drafts/ except for the include, the SPDX line and the
 * shared XMM0 slot. Original guest reads, writes, register exits and saved-register stack
 * slots are preserved, see each header comment. */
#include "game_replace.h"

/* Shared XMM0 slot of the legacy vector bodies. The runtime owns the strong slot, the weak
 * definition lets registry-only links resolve it. */
typedef union T1477AppendixXmm {
    uint32_t u[4];
    uint64_t q[2];
} T1477AppendixXmm;
__thread T1477AppendixXmm g_xmm0 __attribute__((weak, aligned(8)));

/* T1477 appendix draft, VA 0x0010E250, cdecl, three stack arguments (obj, set, mask), plain
 * `ret`, no register input.
 * Original (mov eax,[esp+4]; test eax,eax; je done; mov ecx,[esp+0xC]; mov edx,[eax+8];
 * not ecx; and ecx,edx; or ecx,[esp+8]; mov [eax+8],ecx; done: ret): for a non-null object
 * the flags dword at +8 becomes (old & ~mask) | set, 32-bit, no sign test. The reads are
 * mask (argument 2), [obj+8], then set (argument 1, read AFTER [obj+8] but before the store);
 * the only store is the final dword at obj+8, so a store onto an argument slot is last.
 * Overlay name kept verbatim (INFERRED curated name), it matches the body.
 * Guest globals touched: none. Guest writes: [obj+8] on the non-null path only.
 * No guest PUSH or POP in the original, ESP unchanged, plain ret.
 * Register exit contract:
 *   obj == 0 : EAX = 0, ECX and EDX unchanged (entry values), nothing read or written.
 *   obj != 0 : EAX = obj, ECX = the new flags value (also the stored dword),
 *              EDX = the old [obj+8] read before the store.
 * EBX/ESI/EDI/EBP untouched, DF untouched. Arithmetic EFLAGS exits (not/and/or) are covered
 * by the EXACT caller audit, which now passes for this root (the old scratch ecx/edx audit
 * refused it). */
GAME_REPLACE_EXACT(0010E250, cdecl, 3, u32, game_record_modify_flags_0x8)
{
    g_eax = game_stack_arg(0u);       /* obj */
    if (g_eax == 0u) {
        return;                       /* EAX = 0, ECX and EDX keep their entry values */
    }
    g_ecx = game_stack_arg(2u);       /* mask */
    g_edx = guest_read32(g_eax + 8u); /* old flags */
    g_ecx = ~g_ecx;
    g_ecx &= g_edx;
    g_ecx |= game_stack_arg(1u);      /* set, read after [obj+8] and before the store */
    guest_write32(g_eax + 8u, g_ecx);
}

/* T1477 appendix draft, VA 0x0010E640, cdecl, two stack arguments (obj, value), plain `ret`,
 * no register input.
 * Original (mov eax,[esp+4]; test eax,eax; je done; mov ecx,[esp+8]; mov [eax+0x88],ecx;
 * or dword ptr [eax+4],0x100000; done: ret): for a non-null object stores value at +0x88 and
 * then sets bit 0x100000 in the dword at +4 with a read-modify-write that happens AFTER the
 * +0x88 store (the two dwords are 0x84 bytes apart, so they never overlap each other).
 * Overlay name kept verbatim (INFERRED curated name), it matches the body (field 0x88 set and
 * a flag bit set in +4).
 * Guest globals touched: none. Guest writes, in order: [obj+0x88] = value, [obj+4] |= 0x100000
 * (a write is performed even when the bit is already set). No guest PUSH or POP in the
 * original, ESP unchanged, plain ret.
 * Register exit contract:
 *   obj == 0 : EAX = 0, ECX and EDX unchanged (entry values), no memory touched.
 *   obj != 0 : EAX = obj, ECX = value (argument 1), EDX unchanged.
 * EBX/ESI/EDI/EBP untouched, DF untouched. The EFLAGS left by test/or are covered by the
 * EXACT caller audit, which now passes for this root (the old scratch ecx/edx audit refused
 * it). */
GAME_REPLACE_EXACT(0010E640, cdecl, 2, u32, game_record_set_field_0x88_flag)
{
    g_eax = game_stack_arg(0u);       /* obj */
    if (g_eax == 0u) {
        return;                       /* EAX = 0, ECX and EDX keep their entry values */
    }
    g_ecx = game_stack_arg(1u);       /* value */
    guest_write32(g_eax + 0x88u, g_ecx);
    guest_write32(g_eax + 4u, guest_read32(g_eax + 4u) | 0x100000u);
}

/* T1477 appendix draft, VA 0x0010E6E0, cdecl, one stack argument (obj), plain `ret`, no
 * register input. 13-byte leaf.
 * Original (mov eax,[esp+4]; test eax,eax; je done; mov byte ptr [eax+0x12],1; done: ret):
 * for a non-null object writes the BYTE 1 at obj+0x12 (a one byte store, the neighbouring
 * bytes keep their value, address arithmetic wraps at 32 bits).
 * Overlay name kept verbatim (INFERRED curated name), it matches the body.
 * Guest globals touched: none. Guest writes: one byte at obj+0x12 on the non-null path. The
 * store is the last operation and the only one after the argument read, but its address is
 * caller controlled: obj+0x12 may land on the argument slot (obj = ESP+4-0x12), on a byte of
 * the return slot, or on the saved bytes of a caller frame. The argument was already loaded
 * into EAX so the exit EAX stays the entry argument, and the return address bytes that the
 * store hits are changed in guest memory exactly like the original (only one byte).
 * No guest PUSH or POP in the original, ESP unchanged, plain ret.
 * Register exit contract:
 *   obj == 0 : EAX = 0, ECX and EDX unchanged (entry values), no memory touched.
 *   obj != 0 : EAX = obj, ECX and EDX unchanged.
 * EBX/ESI/EDI/EBP untouched, DF untouched. Arithmetic EFLAGS exits (test eax,eax) are covered
 * by the EXACT caller audit, which now passes for this root (the old scratch ecx/edx audit
 * refused it). */
GAME_REPLACE_EXACT(0010E6E0, cdecl, 1, u32, game_record_set_byte_0x12_one)
{
    g_eax = game_stack_arg(0u);       /* obj */
    if (g_eax != 0u) {
        guest_write8(g_eax + 0x12u, 1u);
    }
}

/* T1477 appendix draft, VA 0x0010E6F0, cdecl, one stack argument (obj), plain `ret`, no
 * register input. 13-byte leaf.
 * Original (mov eax,[esp+4]; test eax,eax; je done; mov byte ptr [eax+0x12],0; done: ret):
 * for a non-null object writes the BYTE 0 at obj+0x12 (a one byte store, the neighbouring
 * bytes keep their value, address arithmetic wraps at 32 bits).
 * Overlay name kept verbatim (INFERRED curated name), it matches the body.
 * Guest globals touched: none. Guest writes: one byte at obj+0x12 on the non-null path. The
 * store is the last operation and the only one after the argument read, but its address is
 * caller controlled: obj+0x12 may land on the argument slot (obj = ESP+4-0x12), on a byte of
 * the return slot, or on the saved bytes of a caller frame. The argument was already loaded
 * into EAX so the exit EAX stays the entry argument, and the return address bytes that the
 * store hits are changed in guest memory exactly like the original (only one byte).
 * No guest PUSH or POP in the original, ESP unchanged, plain ret.
 * Register exit contract:
 *   obj == 0 : EAX = 0, ECX and EDX unchanged (entry values), no memory touched.
 *   obj != 0 : EAX = obj, ECX and EDX unchanged.
 * EBX/ESI/EDI/EBP untouched, DF untouched. Arithmetic EFLAGS exits (test eax,eax) are covered
 * by the EXACT caller audit, which now passes for this root (the old scratch ecx/edx audit
 * refused it). */
GAME_REPLACE_EXACT(0010E6F0, cdecl, 1, u32, game_record_set_byte_0x12_zero)
{
    g_eax = game_stack_arg(0u);       /* obj */
    if (g_eax != 0u) {
        guest_write8(g_eax + 0x12u, 0u);
    }
}

/*
 * T1477 appendix draft 0x0010EAB0 (legacy vector mode, cdecl, 2 stack args, no register input).
 * Original behavior: null-checked object argument. For a non-null object the flag dword at
 * +4 is read, the float argument is loaded (movss, upper lanes cleared), the float is stored at
 * +0x84 and the flags (old & ~2) | 4 are stored at +4. Overlay name kept as curated: it says
 * "mark dirty 0x4", the body sets bit 4 and clears bit 2.
 * Arguments: object, float bits. Guest globals touched: none.
 * Read order: object, [obj+4], float argument. Write order: [obj+0x84] = float bits, then
 * [obj+4] = flags. The stores come after both reads, so a store on an argument slot never
 * changes the exit registers. No PUSH or POP in the original.
 * Register exit contract: null object: EAX = 0, ECX and EDX unchanged, XMM0 unchanged, nothing
 * read or written. Non-null: EAX = object, ECX = (old [obj+4] & ~2) | 4, EDX unchanged,
 * XMM0 = { float bits, 0, 0, 0 }. EBX/ESI/EDI/EBP untouched, XMM1..7 untouched. */

GAME_REPLACE_EXACT(0010EAB0, cdecl, 2, u32, game_object_set_field_0x84_mark_dirty_0x4)
{
    g_eax = game_stack_arg(0u);                       /* object */
    if (g_eax == 0u) {
        return;
    }
    g_ecx = guest_read32(g_eax + 4u);                 /* flags */
    g_xmm0.u[0] = game_stack_arg(1u);                 /* movss xmm0, [float argument] */
    g_xmm0.u[1] = 0u;
    g_xmm0.u[2] = 0u;
    g_xmm0.u[3] = 0u;
    g_ecx &= 0xFFFFFFFDu;
    g_ecx |= 4u;
    guest_write32(g_eax + 0x84u, g_xmm0.u[0]);
    guest_write32(g_eax + 4u, g_ecx);
}

/*
 * T1477 appendix draft 0x0010EB50 (legacy vector mode, cdecl, 2 stack args, no register input).
 * Original behavior: null-checked object argument. For a non-null object the float argument is
 * loaded (movss, upper lanes cleared) and stored at +0x70.
 * Arguments: object, float bits. Guest globals touched: none. Writes: [obj+0x70] only.
 * Read order: object, then (non-null only) the float argument. No PUSH or POP in the original.
 * Register exit contract: null object: EAX = 0, ECX and EDX unchanged, XMM0 unchanged, nothing
 * read or written. Non-null: EAX = object, ECX and EDX unchanged, XMM0 = { float bits, 0, 0, 0 }.
 * EBX/ESI/EDI/EBP untouched, XMM1..7 untouched. */

GAME_REPLACE_EXACT(0010EB50, cdecl, 2, u32, game_object_set_field_0x70)
{
    g_eax = game_stack_arg(0u);                       /* object */
    if (g_eax == 0u) {
        return;
    }
    g_xmm0.u[0] = game_stack_arg(1u);                 /* movss xmm0, [float argument] */
    g_xmm0.u[1] = 0u;
    g_xmm0.u[2] = 0u;
    g_xmm0.u[3] = 0u;
    guest_write32(g_eax + 0x70u, g_xmm0.u[0]);
}

/*
 * T1477 appendix draft 0x0010ECB0 (legacy vector mode, cdecl, 2 stack args, no register input).
 * Original behavior: null-checked object argument. For a non-null object the float argument is
 * loaded (movss, upper lanes cleared) and stored at +0x64.
 * Arguments: object, float bits. Guest globals touched: none. Writes: [obj+0x64] only.
 * Read order: object, then (non-null only) the float argument. No PUSH or POP in the original.
 * Register exit contract: null object: EAX = 0, ECX and EDX unchanged, XMM0 unchanged, nothing
 * read or written. Non-null: EAX = object, ECX and EDX unchanged, XMM0 = { float bits, 0, 0, 0 }.
 * EBX/ESI/EDI/EBP untouched, XMM1..7 untouched. */

GAME_REPLACE_EXACT(0010ECB0, cdecl, 2, u32, game_object_set_field_0x64)
{
    g_eax = game_stack_arg(0u);                       /* object */
    if (g_eax == 0u) {
        return;
    }
    g_xmm0.u[0] = game_stack_arg(1u);                 /* movss xmm0, [float argument] */
    g_xmm0.u[1] = 0u;
    g_xmm0.u[2] = 0u;
    g_xmm0.u[3] = 0u;
    guest_write32(g_eax + 0x64u, g_xmm0.u[0]);
}

/*
 * T1477 appendix draft 0x001115F0 (legacy vector mode, cdecl, 8 stack args, no register input).
 * Original behavior: initialises a 0xCC-byte particle draw descriptor. Entry stack arguments:
 * a0 record, a1 draw kind, a2 blend selector, a3 flags, a4 float, a5 texture, a6 value, a7 float
 * (T1725 correction: the PUSH EDI shifts the esp-relative loads by 4, so the instruction
 * `[esp+0xC]` reads a1, not a2). The name says "zeroed 0xcc bytes", kept as curated.
 * Sequence (read/write order matters when the record overlaps the stack):
 *   EDX = a0 ; XMM0 = a4 (read BEFORE the push and the fill) ; push edi ; EAX = 0 ; ECX = 0x33 ;
 *   EDI = EDX ; rep stosd (0x33 zero dwords from the record, address step +4 with DF clear, -4
 *   with DF set, EDI and ECX end at EDI +- 0xCC and 0) ; then, all AFTER the fill:
 *   ECX = a2 ; EAX = a1 ; compare EAX with 5 ; [rec+0x14] = a2 ; ECX = a3 ; [rec] = a3 ;
 *   ECX = a5 ; [rec+0x10] = a5 ; ECX = a6 ; [rec+4] = a4 (stored from the PRE-fill XMM0) ;
 *   XMM0 = a7 ; [0x73BD68] = rec ; [rec+0xC] = a1 ; [rec+0x18] = a6 ; [rec+0x1C] = a7 ;
 *   pop edi (EDI = the CURRENT content of the saved guest slot, which the fill or any of the
 *   stores can have overwritten) ; when a1 == 5 or a1 == 6 : XMM0 = [0x475C78] and
 *   [rec+0xA0] = XMM0 (after the pop). Draw kind compare is plain equality.
 * Guest globals touched: 0x73BD68 (written), 0x475C78 (read when a1 is 5 or 6).
 * Register exit contract (single path): EAX = a1 as read after the fill, ECX = a6 as read after
 * the fill and the earlier stores, EDX = a0, EDI = popped saved slot, ESP back to entry,
 * EBX/ESI/EBP untouched, DF untouched. XMM0 = { a7 as read, 0, 0, 0 }, or { [0x475C78], 0, 0, 0 }
 * when a1 is 5 or 6. XMM1..7 untouched. EFLAGS are those of the compare chain, not modelled. */

extern __thread uint32_t g_edi;
extern __thread int g_df;

#define C96_1115F0_RECORD_DWORDS 0x33u /* 0xCC bytes */

GAME_REPLACE_EXACT(001115F0, cdecl, 8, u32, game_particle_draw_descriptor_init_zeroed_0xcc_bytes)
{
    const uint32_t step = g_df != 0 ? 0xFFFFFFFCu : 4u;
    g_edx = game_stack_arg(0u);                         /* record */
    g_xmm0.u[0] = game_stack_arg(4u);                   /* movss xmm0, [esp+0x14] before the push */
    g_xmm0.u[1] = 0u;
    g_xmm0.u[2] = 0u;
    g_xmm0.u[3] = 0u;
    guest_write32(g_esp - 4u, g_edi);                   /* push edi */
    g_esp -= 4u;
    g_eax = 0u;
    g_ecx = C96_1115F0_RECORD_DWORDS;
    g_edi = g_edx;
    for (; g_ecx != 0u; g_ecx--) {                      /* rep stosd, honours the guest DF */
        guest_write32(g_edi, g_eax);
        g_edi += step;
    }
    g_ecx = guest_read32(g_esp + 0x10u);                /* a2 (blend selector) */
    g_eax = guest_read32(g_esp + 0xCu);                 /* a1 (draw kind) */
    guest_write32(g_edx + 0x14u, g_ecx);
    g_ecx = guest_read32(g_esp + 0x14u);                /* a3 (flags) */
    guest_write32(g_edx, g_ecx);
    g_ecx = guest_read32(g_esp + 0x1Cu);                /* a5 (texture) */
    guest_write32(g_edx + 0x10u, g_ecx);
    g_ecx = guest_read32(g_esp + 0x20u);                /* a6 */
    guest_write32(g_edx + 4u, g_xmm0.u[0]);            /* movss [rec+4], xmm0 (a4, pre-fill read) */
    g_xmm0.u[0] = guest_read32(g_esp + 0x24u);          /* movss xmm0, [a7] */
    guest_write32(0x73BD68u, g_edx);
    guest_write32(g_edx + 0xCu, g_eax);
    guest_write32(g_edx + 0x18u, g_ecx);
    guest_write32(g_edx + 0x1Cu, g_xmm0.u[0]);
    g_edi = guest_read32(g_esp);                        /* pop edi */
    g_esp += 4u;
    if (g_eax == 5u || g_eax == 6u) {
        g_xmm0.u[0] = guest_read32(0x475C78u);          /* movss xmm0, [0x475C78] */
        g_xmm0.u[1] = 0u;
        g_xmm0.u[2] = 0u;
        g_xmm0.u[3] = 0u;
        guest_write32(g_edx + 0xA0u, g_xmm0.u[0]);
    }
}

/*
 * T1477 appendix draft 0x001123A0 (cdecl, 0 stack args, no register input, plain `ret`).
 * Overlay name game_global_73d950_clear_unless_6b7aa8 (curated INFERRED name kept verbatim: the
 * body clears 73d950 exactly when 6b7aa8 is ZERO, i.e. it leaves 73d950 alone "unless" the
 * gate is zero, which reads as the opposite of the word order but is the same condition).
 * Original behavior: eax = [0x6B7AA8]; test eax,eax; jne skip; mov [0x73D950], 0; ret.
 * Guest globals touched: read dword 0x6B7AA8 (always, on every path), write dword 0x73D950
 * (zero, only when the read value is zero).
 * Register exit contract (both paths): EAX = the dword read from 0x6B7AA8 (zero when the store
 * ran, the nonzero value otherwise), ECX and EDX unchanged, EBX/ESI/EDI/EBP/ESP unchanged
 * (no PUSH or POP, no saved registers; plain ret pops only the return address). DF unused. */

GAME_REPLACE_EXACT(001123A0, cdecl, 0, u32, game_global_73d950_clear_unless_6b7aa8)
{
    g_eax = guest_read32(0x6b7aa8u); /* gate, read on every path and left in EAX */
    if (g_eax == 0u) {
        guest_write32(0x73d950u, 0u);
    }
}

/*
 * T1477 appendix draft 0x0011FE40 (legacy vector mode, cdecl, 2 stack args, no register input).
 * Original behavior: arms an effect record. Arguments: record pointer, float bits. Sets bit 1 (value 2)
 * in the flag dword at +4, stores 2 at +0x88 and 0x3F at +0x8C, a zero float at +0x90 and the
 * float argument at +0x94 (the "mode 2" sibling of 0x11FE80). The name is kept as curated.
 * Read order: [esp+4], [record+4], then (after the +0x90 store) [esp+8]. Write order:
 * [record+0x90] = 0, [record+4] = old | 2, [record+0x88] = 2, [record+0x94] = float bits,
 * [record+0x8C] = 0x3F. A record whose +0x90 store lands on the float argument slot makes the
 * argument read as zero, because that store precedes the read.
 * Guest globals touched: none. No PUSH/POP in the original.
 * Register exit contract (single path): EAX = record, ECX = 2, EDX = old [record+4] | 2,
 * EBX/ESI/EDI/EBP and ESP (apart from the wrapper's pop) untouched, DF untouched.
 * XMM0 = { float bits as read, 0, 0, 0 } (xorps then movss load clears the upper lanes);
 * XMM1..7 untouched. EFLAGS are those of the `or edx, ecx` and are not modelled. */

GAME_REPLACE_EXACT(0011FE40, cdecl, 2, u32,
                   game_effect_arm_flag_0x2_mode_0x88_2_target_0x8c_0x3f_with_arg_0x94)
{
    g_eax = game_stack_arg(0u);                       /* record */
    g_edx = guest_read32(g_eax + 4u);                 /* flags */
    g_xmm0.q[0] = 0u;                                 /* xorps xmm0, xmm0 */
    g_xmm0.q[1] = 0u;
    g_ecx = 2u;
    g_edx |= g_ecx;
    guest_write32(g_eax + 0x90u, g_xmm0.u[0]);        /* movss [record+0x90], xmm0 (zero) */
    g_xmm0.u[0] = game_stack_arg(1u);                 /* movss xmm0, [float argument] */
    g_xmm0.u[1] = 0u;
    g_xmm0.u[2] = 0u;
    g_xmm0.u[3] = 0u;
    guest_write32(g_eax + 4u, g_edx);
    guest_write32(g_eax + 0x88u, g_ecx);
    guest_write32(g_eax + 0x94u, g_xmm0.u[0]);
    guest_write32(g_eax + 0x8Cu, 0x3Fu);
}

/*
 * T1477 appendix draft 0x00120FF0 (legacy vector mode, cdecl, 1 stack arg, no register input).
 * Original behavior: no null check. ECX = object, EAX = flags dword [object+4]. When bit 0x2000
 * is set (test ah, 0x20), XMM0 is cleared in all four lanes (xorps), EAX |= 0x4000, the flags
 * are stored back at +4 and the dword 0.0f is stored at +0xCC. Otherwise nothing is written.
 * Arguments: object. Guest globals touched: none. Write order: [obj+4] then [obj+0xCC].
 * No PUSH or POP in the original.
 * Register exit contract: flag clear: EAX = [obj+4] (unmodified), ECX = object, EDX unchanged,
 * XMM0 unchanged. Flag set: EAX = flags | 0x4000, ECX = object, EDX unchanged, XMM0 = 0 in all
 * four lanes. EBX/ESI/EDI/EBP untouched, XMM1..7 untouched. */

GAME_REPLACE_EXACT(00120FF0, cdecl, 1, u32,
                   game_effect_flag_0x2000_arm_set_flag_0x4000_zero_field_0xcc)
{
    g_ecx = game_stack_arg(0u);                       /* object */
    g_eax = guest_read32(g_ecx + 4u);                 /* flags */
    if ((g_eax & 0x2000u) == 0u) {                    /* test ah, 0x20 ; je */
        return;
    }
    g_xmm0.q[0] = 0u;                                 /* xorps xmm0, xmm0 */
    g_xmm0.q[1] = 0u;
    g_eax |= 0x4000u;
    guest_write32(g_ecx + 4u, g_eax);
    guest_write32(g_ecx + 0xCCu, g_xmm0.u[0]);        /* movss [obj+0xCC], xmm0 */
}

/*
 * T1477 appendix draft 0x001273E0 (cdecl, 0 stack args, no register input, plain `ret`).
 * Overlay name game_array_7ac6e4_clear_pointer_unless_global_6b7aa8 (curated INFERRED name kept
 * verbatim: the body clears the dword at 0x7AC6E4 exactly when 6b7aa8 is ZERO).
 * Original behavior: eax = [0x6B7AA8]; test eax,eax; jne skip; mov [0x7AC6E4], 0; ret.
 * Guest globals touched: read dword 0x6B7AA8 (always), write dword 0x7AC6E4 (zero, only when the
 * read value is zero).
 * Register exit contract (both paths): EAX = the dword read from 0x6B7AA8, ECX and EDX
 * unchanged, EBX/ESI/EDI/EBP/ESP unchanged (no PUSH or POP). DF unused. */

GAME_REPLACE_EXACT(001273E0, cdecl, 0, u32, game_array_7ac6e4_clear_pointer_unless_global_6b7aa8)
{
    g_eax = guest_read32(0x6b7aa8u); /* gate, read on every path and left in EAX */
    if (g_eax == 0u) {
        guest_write32(0x7ac6e4u, 0u);
    }
}

/*
 * T1477 appendix draft 0x0012B940 (legacy vector mode, cdecl, 1 stack arg, no register input).
 * Original behavior: reads the ring counter dword at 0x7450D0, forms index = counter - 1 and,
 * when that result is negative as a signed 32-bit value (jns not taken: counter 0 or counter
 * >= 0x80000001), adds 0xC8 (wraps at 32 bits). The float argument is loaded (movss, upper lanes
 * cleared), the index is scaled by 0x38 (imul, low 32 bits) and the float is stored at
 * 0x74253C + index*0x38 (the +0x2C field of the 0x38 byte entry at 0x742510).
 * Arguments: float bits. Guest globals touched: [0x7450D0] read, entry field written.
 * Read order: counter, float argument. The store comes last, so a store on the counter or on the
 * argument slot does not change the exit registers. No PUSH or POP in the original.
 * Register exit contract (single path): EAX = (index * 0x38) mod 2^32, ECX and EDX unchanged,
 * XMM0 = { float bits, 0, 0, 0 }. EBX/ESI/EDI/EBP untouched, XMM1..7 untouched. */

GAME_REPLACE_EXACT(0012B940, cdecl, 1, u32, game_ring_742510_set_prev_entry_field_0x2c)
{
    g_eax = guest_read32(0x7450D0u);                  /* ring counter */
    g_eax -= 1u;                                      /* dec eax */
    if ((int32_t)g_eax < 0) {                         /* jns not taken */
        g_eax += 0xC8u;
    }
    g_xmm0.u[0] = game_stack_arg(0u);                 /* movss xmm0, [float argument] */
    g_xmm0.u[1] = 0u;
    g_xmm0.u[2] = 0u;
    g_xmm0.u[3] = 0u;
    g_eax *= 0x38u;                                   /* imul eax, eax, 0x38 (low 32 bits) */
    guest_write32(g_eax + 0x74253Cu, g_xmm0.u[0]);
}

/*
 * T1477 appendix draft 0x0012C6E0 (cdecl, 4 stack args, no register input, plain `ret`).
 * Overlay name game_array_7450d8_append_record_max8 matches the body.
 * Original behavior: arguments are (a0, a1, vec3 pointer a2, a3). The signed record count at
 * guest 0x7ABF50 is read first. When the count is >= 8 (signed `jge`) nothing else happens.
 * Otherwise a 0x18-byte record is appended at 0x7450D8 + 0x18 * count (32 bit wrap, so a
 * negative count appends below the array) in this exact order: edx = a1 is read, the count is
 * stored back as count + 1, a0 is read and stored at +0x00, a2 is read, a1 is stored at +0x04,
 * then [a2], [a2+4] are read and stored at +0x08, +0x0C, [a2+8] is read, a3 is read and the
 * last two stores are +0x10 = [a2+8] and +0x14 = a3. Every argument slot and the vec3 are read
 * AFTER the earlier stores, so a record, the count global or the vec3 overlapping the caller
 * stack (or each other) changes what is read. Reads are kept in that order.
 * Guest globals touched: 0x7ABF50 (read, then written count + 1), record array at 0x7450D8.
 * No guest PUSH or POP. EBX, ESI, EDI, EBP untouched on every path. DF unused.
 * Register exit contract:
 *   count >= 8 (signed) : EAX = entry EAX (the original never writes it, this is the
 *                         "uninitialised" return of the T77 notes, a caller leftover),
 *                         ECX = count, EDX = entry EDX, no memory written.
 *   append              : EAX = record address, ECX = [a2+8] as read after the stores,
 *                         EDX = a3 as read after the stores.
 * EFLAGS are not modeled, as for every EXACT census body: the skip path leaves those of
 * `cmp ecx, 8`, the append path those of `inc ecx` (CF from the `cmp`). */

GAME_REPLACE_EXACT(0012C6E0, cdecl, 4, u32, game_array_7450d8_append_record_max8)
{
    g_ecx = guest_read32(0x7abf50u);
    if ((int32_t)g_ecx >= 8) {
        return; /* EAX and EDX stay the caller's, ECX = count */
    }
    g_edx = game_stack_arg(1u);
    g_eax = g_ecx + g_ecx * 2u;           /* lea eax, [ecx + ecx*2] */
    g_ecx += 1u;
    g_eax = g_eax * 8u + 0x7450d8u;       /* lea eax, [eax*8 + 0x7450D8] */
    guest_write32(0x7abf50u, g_ecx);
    g_ecx = game_stack_arg(0u);
    guest_write32(g_eax, g_ecx);
    g_ecx = game_stack_arg(2u);
    guest_write32(g_eax + 0x04u, g_edx);
    g_edx = guest_read32(g_ecx);
    guest_write32(g_eax + 0x08u, g_edx);
    g_edx = guest_read32(g_ecx + 4u);
    guest_write32(g_eax + 0x0cu, g_edx);
    g_ecx = guest_read32(g_ecx + 8u);
    g_edx = game_stack_arg(3u);
    guest_write32(g_eax + 0x10u, g_ecx);
    guest_write32(g_eax + 0x14u, g_edx);
}

/*
 * T1477 appendix draft 0x0012D1E0 (cdecl, 0 stack args, no register input, live-call-closure).
 * Original behavior: resets the HUD record at 0x7490AC..0x7490E0 (14 dwords) and derives two
 * layout values from the live callee 0x001CDAE0 (a tail `jmp 0x003567C0`, a player count
 * helper that reads the session flag through 0x003533F0 and the dwords 0x790950, 0x774028,
 * 0x5236C4). It is called twice, from two sites (return PCs 0x0012D239 and 0x0012D26E).
 * Ordered guest effects (every store is a real guest write, in this order):
 *   EAX = 0; [0x7490AC], [B0], [B4], [B8], [BC], [C0], [C4], [C8], [CC], [D0], [D4], [D8] = 0;
 *   PUSH ESI (real guest slot at entry ESP - 4); [DC] = 0; ESI = 1; [E0] = 0; [B0] = 1;
 *   call 0x1CDAE0 (return PC 0x12D239) -> n1 in EAX;
 *   ECX = (signed n1 <= 2) ? 1 : 0 (xor/cmp/setle);
 *   [B8] = [BC] = [D8] = [C0] = [E0] = 1; ECX = 2*ECX + 1; [B4] = ECX;
 *   call 0x1CDAE0 (return PC 0x12D26E) -> n2 in EAX;
 *   EDX = (signed n2 <= 2) ? 1 : 0;
 *   [C8] = [CC] = [D0] = [D4] = [DC] = 1; POP ESI (reloaded from the guest slot, so a stack that
 *   aliases the record changes ESI); [C4] = EDX.
 * Register exit contract: EAX = n2 (the second call's return), EDX = 0 or 1, ESI restored from
 * the pushed guest slot, EBX/EDI/EBP untouched. ECX is NOT the 1-or-3 value: the original never
 * rewrites ECX after the second call, so the exit ECX is whatever the live callee leaves there
 * (0x3567C0 loads ECX = [0x790950], measured with the original bytes in Unicorn). Each CALL
 * writes its return PC into the dword below the pushed ESI slot, like the original CALL, and the
 * live callee chain runs for real (its register and memory effects, including its own nested
 * return-PC slot, are visible; the original relied on no register being preserved across the
 * calls except ESI and the stack).
 * The callee chain 0x1CDAE0 -> 0x3567C0 -> 0x3533F0 stays live (the harness closure runs it);
 * native tests stub 0x1CDAE0 through recomp_lookup.
 * The overlay name is the curated INFERRED name; the body resets 14 dwords then sets flag
 * dwords to 1 and the layout dword [B4] to 1 or 3 and the dword [C4] to 0 or 1 from the two
 * count comparisons, which is consistent with it. */

static void hud_reset_call_player_count(uint32_t return_pc)
{
    if (game_guest_call(0x001CDAE0u, GAME_CC_cdecl, return_pc, 0u, 0u, 0u, NULL, 0u) !=
        GAME_GUEST_CALL_OK)
        __builtin_trap();
}

GAME_REPLACE_EXACT(0012D1E0, cdecl, 0, u32, game_hud_state_7490ac_reset_defaults_by_player_count)
{
    g_eax = 0u;
    guest_write32(0x007490ACu, g_eax);
    guest_write32(0x007490B0u, g_eax);
    guest_write32(0x007490B4u, g_eax);
    guest_write32(0x007490B8u, g_eax);
    guest_write32(0x007490BCu, g_eax);
    guest_write32(0x007490C0u, g_eax);
    guest_write32(0x007490C4u, g_eax);
    guest_write32(0x007490C8u, g_eax);
    guest_write32(0x007490CCu, g_eax);
    guest_write32(0x007490D0u, g_eax);
    guest_write32(0x007490D4u, g_eax);
    guest_write32(0x007490D8u, g_eax);
    g_esp -= 4u;                                  /* push esi */
    guest_write32(g_esp, g_esi);
    guest_write32(0x007490DCu, g_eax);
    g_esi = 1u;
    guest_write32(0x007490E0u, g_eax);
    guest_write32(0x007490B0u, g_esi);
    hud_reset_call_player_count(0x0012D239u);
    g_ecx = ((int32_t)g_eax <= 2) ? 1u : 0u;      /* xor ecx,ecx ; cmp eax,2 ; setle cl */
    guest_write32(0x007490B8u, g_esi);
    guest_write32(0x007490BCu, g_esi);
    guest_write32(0x007490D8u, g_esi);
    guest_write32(0x007490C0u, g_esi);
    guest_write32(0x007490E0u, g_esi);
    g_ecx = g_ecx + g_ecx + 1u;                   /* lea ecx,[ecx+ecx+1] */
    guest_write32(0x007490B4u, g_ecx);
    hud_reset_call_player_count(0x0012D26Eu);
    g_edx = ((int32_t)g_eax <= 2) ? 1u : 0u;     /* xor edx,edx ; cmp eax,2 ; setle dl */
    guest_write32(0x007490C8u, g_esi);
    guest_write32(0x007490CCu, g_esi);
    guest_write32(0x007490D0u, g_esi);
    guest_write32(0x007490D4u, g_esi);
    guest_write32(0x007490DCu, g_esi);
    g_esi = guest_read32(g_esp);                  /* pop esi */
    g_esp += 4u;
    guest_write32(0x007490C4u, g_edx);
}

/*
 * T1477 appendix draft 0x0012DE80 (legacy vector mode, cdecl, 2 stack args, no register input).
 * Original behavior: for slot index arg0 (SIGNED test, negative returns at once) of the 0x177C
 * stride table at 0x7A6160, whose group lives at slot+0x1768 (= 0x7A78C8 + index * 0x177C, 32-bit
 * wrap, no upper bound check): when the dword at +0 of that group equals arg1 (plain 32-bit
 * equality) it stores 0 at +4, then reads the float constant at 0x475D10 and stores it at +8,
 * and stores 2 at +0xC. Write/read order on the match path: [g+4] = 0, read [0x475D10],
 * [g+0xC] = 2, [g+8] = constant. A group whose +4 store lands on 0x475D10 makes the constant
 * read as zero. The name is kept as curated (it says "if equal" and "reset +0x176C",
 * which are the group's +4 and +8 here).
 * Guest globals touched: 0x475D10 read on the match path only. No PUSH/POP in the original.
 * Register exit contract:
 *   index < 0 (signed):   EAX = index, ECX and EDX unchanged, XMM0 unchanged.
 *   mismatch:             EAX = group address, ECX = [group], EDX = arg1, XMM0 unchanged.
 *   match:                EAX = group address, ECX = [group] (= arg1), EDX = arg1,
 *                         XMM0 = { [0x475D10], 0, 0, 0 }.
 * EBX/ESI/EDI/EBP untouched on every path, XMM1..7 untouched, DF untouched.
 * EFLAGS are those of the last test/cmp and are not modelled. */

GAME_REPLACE_EXACT(0012DE80, cdecl, 2, u32, game_slot_7a6160_reset_field_0x1768_group_if_equal)
{
    g_eax = game_stack_arg(0u);                       /* slot index */
    if ((int32_t)g_eax < 0) {                         /* test eax, eax ; jl */
        return;
    }
    g_edx = game_stack_arg(1u);                       /* value compared against the group head */
    g_eax = g_eax * 0x177Cu + 0x7A78C8u;              /* imul eax, eax, 0x177C ; add eax, 0x7A78C8 */
    g_ecx = guest_read32(g_eax);
    if (g_ecx != g_edx) {                             /* cmp ecx, edx ; jne */
        return;
    }
    g_xmm0.q[0] = 0u;                                 /* xorps xmm0, xmm0 */
    g_xmm0.q[1] = 0u;
    guest_write32(g_eax + 4u, g_xmm0.u[0]);           /* movss [group+4], xmm0 (zero) */
    g_xmm0.u[0] = guest_read32(0x475D10u);            /* movss xmm0, [0x475D10] */
    g_xmm0.u[1] = 0u;
    g_xmm0.u[2] = 0u;
    g_xmm0.u[3] = 0u;
    guest_write32(g_eax + 0xCu, 2u);
    guest_write32(g_eax + 8u, g_xmm0.u[0]);
}

/* Real guest stack operations: the original pushes and pops ESI. */
static void g150ef0_push(uint32_t value)
{
    guest_write32(g_esp - 4u, value);
    g_esp -= 4u;
}

static uint32_t g150ef0_pop(void)
{
    const uint32_t value = guest_read32(g_esp);
    g_esp += 4u;
    return value;
}

/* VA 0x00150EF0, cdecl, one stack argument (the source pointer), plain `ret`.
 * Original: latin1 to utf8 copy of the NUL terminated source into the static output buffer
 * at guest 0x749168 (byte granular). Bytes < 0x80 are copied, 0x80..0xBF become 0xC2 then
 * the byte, 0xC0..0xFF become 0xC3 then (byte - 0x40) (both compares unsigned). After every
 * byte stored the written-byte counter ECX is incremented and a signed `ecx >= 0x200` ends
 * the copy WITHOUT a terminator (so at most 0x200 bytes are written). A source that ends
 * normally gets a NUL stored after the last byte. A NULL source returns 0 and writes nothing.
 * The routine re-reads the source byte [esi] after the 0xC2/0xC3 store (and again at the top
 * of every iteration), so an output buffer overlapping the source changes what is read.
 * Name note: the curated name says latin1 to utf8, the body agrees (no contradiction).
 * Census baseline `skip:jump`: the two short unconditional `jmp` instructions at 0x150F2B
 * (0xC2 path rejoining the store) and the loop back edge are what the flag audit skips.
 * Guest globals: reads the argument and source bytes, writes the buffer 0x749168..+0x200.
 * Guest stack: PUSH ESI at (entry ESP - 4), the saved ESI is POPped from that slot at exit
 * (it may have been overwritten if the buffer overlaps the stack).
 * Register exits (EBX, EDI, EBP untouched, DF untouched and never used, ESI = popped slot):
 *   source == NULL : EAX = 0, ECX = 0, EDX unchanged
 *   otherwise      : EAX = 0x749168, ECX = number of bytes written (excluding the NUL),
 *                    EDX low byte = last DL (0 at a normal end, the source byte at a limit
 *                    exit after a 0xC2/0xC3 prefix, the stored byte at a limit exit after a
 *                    plain/translated store), EDX upper 24 bits unchanged, empty source
 *                    string leaves EDX completely unchanged.
 */
GAME_REPLACE_EXACT(00150EF0, cdecl, 1, u32, game_latin1_to_utf8_copy_to_static_buffer)
{
    uint8_t dl = (uint8_t)g_edx;
    g150ef0_push(g_esi);
    g_esi = guest_read32(g_esp + 8u); /* the argument, after the push */
    g_ecx = 0u;
    g_eax = 0x749168u;
    if (g_esi == 0u) {
        g_eax = 0u;
        g_esi = g150ef0_pop();
        return;
    }
    if (guest_read8(g_esi) == 0u) {
        goto terminate;
    }
    for (;;) {
        dl = guest_read8(g_esi); /* 0x150F10 */
        if (dl < 0x80u) {
            goto store;
        }
        if (dl >= 0xC0u) {
            guest_write8(g_eax, 0xC3u);
            g_eax += 1u;
            g_ecx += 1u;
            if ((int32_t)g_ecx >= 0x200) {
                goto finish;
            }
            dl = guest_read8(g_esi);
            dl = (uint8_t)(dl - 0x40u);
        } else {
            guest_write8(g_eax, 0xC2u);
            g_eax += 1u;
            g_ecx += 1u;
            if ((int32_t)g_ecx >= 0x200) {
                goto finish;
            }
            dl = guest_read8(g_esi);
        }
    store:
        guest_write8(g_eax, dl);
        g_eax += 1u;
        g_ecx += 1u;
        if ((int32_t)g_ecx >= 0x200) {
            goto finish;
        }
        dl = guest_read8(g_esi + 1u);
        g_esi += 1u;
        if (dl == 0u) {
            break;
        }
    }
terminate:
    guest_write8(g_eax, 0u);
finish:
    g_edx = (g_edx & 0xFFFFFF00u) | dl;
    g_eax = 0x749168u;
    g_esi = g150ef0_pop();
}

/*
 * T1477 appendix draft 0x00153B80 (cdecl, 1 stack arg, no register input, plain `ret`).
 * Overlay name game_rng_seed_lcg64_state_from_arg_plus_one (curated INFERRED name kept verbatim).
 * Original behavior: ecx = arg0; eax = 0; edx = 0; ecx += 1 (add); edx += eax + CF (adc, so
 * edx = 1 exactly when arg0 == 0xFFFFFFFF); then five dword stores in this order:
 *   [0x7497C8] = 0, [0x4E7950] = ecx (arg0 + 1, 32-bit wrap), [0x4E7954] = edx (carry),
 *   [0x7497CC] = 0, [0x7497D0] = 0.
 * Guest globals touched: none read. All five addresses are distinct non-overlapping dwords.
 * Register exit contract (single path): EAX = 0, ECX = arg0 + 1 (mod 2^32), EDX = carry
 * (1 when arg0 == 0xFFFFFFFF, else 0), EBX/ESI/EDI/EBP unchanged, ESP back at entry (no PUSH
 * or POP, plain ret). The argument is read first, so a store that lands on the argument slot
 * cannot change an exit register. DF unused. */

GAME_REPLACE_EXACT(00153B80, cdecl, 1, u32, game_rng_seed_lcg64_state_from_arg_plus_one)
{
    g_ecx = guest_read32(g_esp + 4u);   /* seed argument */
    g_eax = 0u;                         /* xor eax, eax */
    g_edx = 0u;                         /* xor edx, edx */
    g_ecx = g_ecx + 1u;                 /* add ecx, 1: CF set on wrap */
    g_edx = g_edx + g_eax + (g_ecx == 0u ? 1u : 0u); /* adc edx, eax */
    guest_write32(0x7497c8u, g_eax);
    guest_write32(0x4e7950u, g_ecx);
    guest_write32(0x4e7954u, g_edx);
    guest_write32(0x7497ccu, g_eax);
    guest_write32(0x7497d0u, g_eax);
}

/* VA 0x0015C3D0, cdecl, one stack argument (obj), plain `ret`. Leaf, no PUSH/POP.
 * Original: P = [obj+0x7C]. If bit 4 of [P+0x2C] is clear: return 0. Otherwise store
 * [P+0x2C] = (that dword & ~4), Q = [P], [Q+0x28] |= 0x61 (read-modify-write), then ECX low
 * byte = byte [P+0x28]: bit 7 set returns 0. Then [P+0x10] & 0x04000000 nonzero returns 0.
 * Then signed [P+8] < [0x790950] returns 0. Success: Q2 = [P] is RE-READ (the or store may
 * have changed it when Q+0x28 aliases P), n = [0x74C37C], array[n] (guest 0x7A3580 + 4n) =
 * [Q2+0x7C], [0x74C37C] = n+1, returns 1. The flag-clearing store and the or store happen
 * before the failure tests, so they persist on the later failure paths.
 * Guest globals: reads/writes [0x74C37C], array 0x7A3580, reads [0x790950]. No stack writes.
 * Register exits (EBX, ESI, EDI, EBP untouched, DF untouched):
 *   bit 4 clear           : EAX = 0, ECX = [P+0x2C] (unchanged flag dword), EDX unchanged
 *   [P+0x28] bit 7 set    : EAX = 0, ECX = (Q & 0xFFFFFF00) | [P+0x28] byte, EDX unchanged
 *   [P+0x10] mask nonzero : EAX = 0, ECX = 0x04000000, EDX = 0
 *   [P+8] < [0x790950]    : EAX = 0, ECX = [P+8], EDX = 0
 *   success               : EAX = 1, ECX = [Q2+0x7C], EDX = Q2
 */
GAME_REPLACE_EXACT(0015C3D0, cdecl, 1, u32, game_object_clear_inactive_flag_0x4_and_append_to_array)
{
    g_eax = guest_read32(g_esp + 4u);
    g_eax = guest_read32(g_eax + 0x7Cu);
    g_ecx = guest_read32(g_eax + 0x2Cu);
    if ((g_ecx & 4u) == 0u) {
        g_eax = 0u;
        return;
    }
    g_ecx &= 0xFFFFFFFBu;
    guest_write32(g_eax + 0x2Cu, g_ecx);
    g_ecx = guest_read32(g_eax);
    guest_write32(g_ecx + 0x28u, guest_read32(g_ecx + 0x28u) | 0x61u);
    g_ecx = (g_ecx & 0xFFFFFF00u) | guest_read8(g_eax + 0x28u);
    if ((g_ecx & 0x80u) != 0u) {
        g_eax = 0u;
        return;
    }
    g_ecx = guest_read32(g_eax + 0x10u);
    g_ecx &= 0x04000000u;
    g_edx = 0u;
    g_ecx |= g_edx;
    if (g_ecx != 0u) {
        g_eax = 0u;
        return;
    }
    g_ecx = guest_read32(g_eax + 8u);
    if ((int32_t)g_ecx < (int32_t)guest_read32(0x790950u)) {
        g_eax = 0u;
        return;
    }
    g_edx = guest_read32(g_eax);
    g_eax = guest_read32(0x74C37Cu);
    g_ecx = guest_read32(g_edx + 0x7Cu);
    guest_write32(g_eax * 4u + 0x7A3580u, g_ecx);
    g_eax += 1u;
    guest_write32(0x74C37Cu, g_eax);
    g_eax = 1u;
}

/*
 * T1477 appendix draft 0x001679D0 (cdecl, 1 stack arg, no register input, plain `ret`).
 * Overlay name game_entry_74c458_addr_field_0x140 (curated INFERRED name kept verbatim).
 * Original behavior: eax = arg0; ecx = [0x74C458]; eax &= 0xFFFF; eax <<= 9;
 * eax = eax + ecx + 0x140 (lea, 32-bit wrap); ret. Returns the address of field 0x140 of the
 * 0x200-byte entry with id (arg0 & 0xFFFF) in the table whose base is the dword at 0x74C458.
 * Guest globals touched: read dword 0x74C458 (after the argument read). No guest writes.
 * Register exit contract (single path): EAX = ((arg0 & 0xFFFF) << 9) + [0x74C458] + 0x140
 * (mod 2^32), ECX = [0x74C458] (the table base), EDX unchanged, EBX/ESI/EDI/EBP unchanged,
 * ESP back at entry (no PUSH or POP, plain ret). DF unused. */

GAME_REPLACE_EXACT(001679D0, cdecl, 1, u32, game_entry_74c458_addr_field_0x140)
{
    g_eax = guest_read32(g_esp + 4u);   /* id argument */
    g_ecx = guest_read32(0x74c458u);    /* table base */
    g_eax &= 0xffffu;
    g_eax <<= 9u;
    g_eax = g_eax + g_ecx + 0x140u;     /* lea eax, [eax + ecx + 0x140] */
}

/*
 * T1477 appendix draft 0x00167CA0 (cdecl, 2 stack args, no register input, plain `ret`).
 * Overlay name game_entry_74c458_set_vec3_0x140 (curated INFERRED name kept verbatim).
 * Original behavior: eax = arg0; ecx = [0x74C458]; eax = ((eax & 0xFFFF) << 9) + ecx;
 * ecx = arg1 (source pointer); then three interleaved copy steps, each a read followed by a
 * store (the source may overlap the destination, so the order matters):
 *   edx = [src];   [eax+0x140] = edx;
 *   edx = [src+4]; [eax+0x144] = edx;
 *   ecx = [src+8]; [eax+0x148] = ecx.
 * Guest globals touched: read dword 0x74C458 (after arg0, before arg1). Writes: three dwords
 * at entry+0x140, +0x144, +0x148 in that order, entry = ((arg0 & 0xFFFF) << 9) + [0x74C458].
 * Register exit contract (single path): EAX = entry address (before the +0x140 offsets),
 * ECX = [src+8] read AFTER the stores to +0x140 and +0x144 and BEFORE the store to +0x148,
 * EDX = [src+4] read after the store to +0x140 (the third read does not touch EDX).
 * EBX/ESI/EDI/EBP unchanged, ESP back at entry (no PUSH or POP, plain ret). DF unused. */

GAME_REPLACE_EXACT(00167CA0, cdecl, 2, u32, game_entry_74c458_set_vec3_0x140)
{
    g_eax = guest_read32(g_esp + 4u);   /* id argument */
    g_ecx = guest_read32(0x74c458u);    /* table base */
    g_eax &= 0xffffu;
    g_eax <<= 9u;
    g_eax += g_ecx;                     /* entry address */
    g_ecx = guest_read32(g_esp + 8u);   /* source vec3 pointer */
    g_edx = guest_read32(g_ecx);
    guest_write32(g_eax + 0x140u, g_edx);
    g_edx = guest_read32(g_ecx + 4u);
    guest_write32(g_eax + 0x144u, g_edx);
    g_ecx = guest_read32(g_ecx + 8u);
    guest_write32(g_eax + 0x148u, g_ecx);
}

/*
 * T1477 appendix draft 0x00168130 (cdecl, 2 stack args, no register input, plain `ret`).
 * Overlay name game_entry_74c458_clear_flag_bits_0x1c0 matches the body.
 * Original behavior: arguments are (id, mask). The entry address is
 * [0x74C458] + ((id & 0xFFFF) << 9) with 32 bit wrap. The dword at entry + 0x1C0 is read,
 * and written back as old & ~mask (the bits of mask are cleared). Read order: id, the base
 * global, mask, then the old value, then the store. The only write is the final store, which
 * comes after every read, so a store that lands on an argument slot has no later reader.
 * A store that lands on the return slot would redirect the original `ret` (the census
 * refusal "store through a register that may alias the stack at 0x168152"): the
 * replacement's wrapper always returns normally, so that case is only representable when the
 * stored value equals the return address (idempotent store).
 * Guest globals touched: 0x74C458 (read), entry + 0x1C0 (read then write).
 * No guest PUSH or POP. EBX, ESI, EDI, EBP untouched. DF unused.
 * Register exit contract (single path):
 *   EAX = old dword at entry + 0x1C0, ECX = entry address (base + ((id & 0xFFFF) << 9)),
 *   EDX = old & ~mask (the stored value).
 * EFLAGS after the original are those of `and edx, eax` (SF/ZF/PF of the result, CF = OF = 0),
 * not modeled, as for every EXACT census body (the census caller audit owns flag liveness). */

GAME_REPLACE_EXACT(00168130, cdecl, 2, u32, game_entry_74c458_clear_flag_bits_0x1c0)
{
    g_ecx = game_stack_arg(0u);
    g_eax = guest_read32(0x74c458u);
    g_edx = game_stack_arg(1u);
    g_ecx &= 0xffffu;
    g_ecx <<= 9;
    g_ecx += g_eax;
    g_eax = guest_read32(g_ecx + 0x1c0u);
    g_edx = ~g_edx;
    g_edx &= g_eax;
    guest_write32(g_ecx + 0x1c0u, g_edx);
}

/*
 * T1477 appendix draft 0x00168160 (cdecl, 2 stack args, no register input, plain `ret`).
 * Overlay name game_entry_74c458_set_field_0x1c4 (curated INFERRED name kept verbatim).
 * Original behavior: eax = arg0 (id); ecx = arg1 (value); edx = [0x74C458]; eax &= 0xFFFF;
 * eax <<= 9; [eax + edx + 0x1C4] = ecx; ret.
 * Guest globals touched: read dword 0x74C458 (after both argument reads, before the store).
 * Writes: one dword at ((arg0 & 0xFFFF) << 9) + [0x74C458] + 0x1C4 (32-bit wrap), the value is
 * arg1. All three reads precede the store, so a store that lands on an argument slot or the
 * return slot changes no exit register.
 * Register exit contract (single path): EAX = (arg0 & 0xFFFF) << 9 (the offset only, NOT the
 * entry address), ECX = arg1, EDX = [0x74C458] (the table base), EBX/ESI/EDI/EBP unchanged,
 * ESP back at entry (no PUSH or POP, plain ret). DF unused. */

GAME_REPLACE_EXACT(00168160, cdecl, 2, u32, game_entry_74c458_set_field_0x1c4)
{
    g_eax = guest_read32(g_esp + 4u);   /* id argument */
    g_ecx = guest_read32(g_esp + 8u);   /* value stored */
    g_edx = guest_read32(0x74c458u);    /* table base */
    g_eax &= 0xffffu;
    g_eax <<= 9u;
    guest_write32(g_eax + g_edx + 0x1c4u, g_ecx);
}

/*
 * T1477 appendix draft 0x00172C80 (cdecl, 2 stack args, no register input, plain `ret`).
 * Overlay name game_component_slot_stride_0x34_next_in_ring_by_index_wrapping_count_0x140
 * matches the body. The census baseline reason "skip:jump" is the unconditional short
 * `jmp 0x172CB4` at 0x172CA8 (taken when the clamped index is above count - 1, it skips the
 * lower clamp of the other branch), plus the conditional `jge`.
 * Original behavior: arguments are (object, index pointer X). R = [object + 0x7C] is read
 * BEFORE ESI and EDI are pushed (slots entry ESP - 4 ESI, - 8 EDI), then X = arg1 and
 * v = [X] (signed) are read after the pushes. clamped = v < 0 ? 0 : v. count = [R + 0x140].
 * If count - 1 < clamped (signed) idx = count - 1, else idx = clamped. [X] = idx is stored
 * first, slot = R + 0x70 + idx * 0x34 (32 bit wrap), then (idx + 1) is divided by
 * [R + 0x140], READ AGAIN after the store of idx, with a signed 32 bit `cdq`/`idiv`. The
 * remainder (sign of the dividend, so (idx + 1) mod count for count > 0) is stored to [X]. ESI
 * and EDI are reloaded from the guest slots at POP after that last store.
 * Guest writes: the two pushes, [X] = idx, [X] = remainder. Guest globals: none.
 * Aliases that are preserved because the order of reads and stores is: X == R + 0x140 makes the
 * divisor the stored idx, X on the saved slots or the argument slots changes v and the pops.
 * TRAPS (decision): `idiv` raises #DE for a zero divisor and for a quotient outside int32. The
 * second cannot occur here (see the control: idx + 1 is never INT_MIN with divisor -1), the
 * first occurs for count == 0 and when the aliased store makes the divisor zero (X pointing at
 * count with v == 0x80000000 or a count of 1 aliased to X). The harness oracle treats that
 * fault as a non-verdict. The draft does NOT invent behaviour: a zero divisor executes
 * __builtin_trap() (the host analog of #DE, no register or memory effect is claimed, the stores
 * already done stay done), through a helper that never performs a C division with a zero or
 * overflowing operand. On every non-trapping input it matches the original exactly.
 * Register exit contract (single non-trapping path; EBX, EBP untouched, DF unused):
 *   EAX = slot pointer R + 0x70 + idx * 0x34, ECX = R, EDX = remainder (also stored to [X]),
 *   ESI, EDI = reloaded from their guest slots.
 * EFLAGS after the original are those of the `idiv` (undefined), not modeled. */

extern __thread uint32_t g_edi;

static void a172c80_push(uint32_t value)
{
    guest_write32(g_esp - 4u, value);
    g_esp -= 4u;
}

static uint32_t a172c80_pop(void)
{
    const uint32_t value = guest_read32(g_esp);
    g_esp += 4u;
    return value;
}

/* idiv r/m32: EDX:EAX / divisor, EAX = quotient, EDX = remainder. A zero divisor or a quotient
 * outside int32 is #DE in the original and traps here. */
static void a172c80_idiv(uint32_t divisor)
{
    const int64_t d = (int32_t)divisor;
    const int64_t n = (int64_t)(((uint64_t)g_edx << 32) | g_eax);
    if (d == 0) {
        __builtin_trap();
    }
    if (d == -1 && n == INT64_MIN) {
        __builtin_trap();
    }
    const int64_t q = n / d;
    if (q > INT32_MAX || q < INT32_MIN) {
        __builtin_trap();
    }
    g_eax = (uint32_t)(int32_t)q;
    g_edx = (uint32_t)(int32_t)(n % d);
}

GAME_REPLACE_EXACT(00172C80, cdecl, 2, u32,
                   game_component_slot_stride_0x34_next_in_ring_by_index_wrapping_count_0x140)
{
    g_eax = game_stack_arg(0u);
    g_ecx = guest_read32(g_eax + 0x7cu);
    a172c80_push(g_esi);
    g_edx = 0u;
    a172c80_push(g_edi);
    g_edi = guest_read32(g_esp + 0x10u);             /* index pointer X */
    g_eax = guest_read32(g_edi);                     /* v */
    g_edx = (uint32_t)((int32_t)g_eax < 0);          /* setl dl */
    g_edx -= 1u;
    g_edx &= g_eax;
    g_esi = g_edx;                                   /* clamped v */
    g_edx = guest_read32(g_ecx + 0x140u);
    g_edx -= 1u;
    if ((int32_t)g_edx < (int32_t)g_esi) {
        g_eax = g_edx;                               /* idx = count - 1 */
    } else {
        g_edx = (uint32_t)((int32_t)g_eax < 0);
        g_edx -= 1u;
        g_eax &= g_edx;                              /* idx = clamped v */
    }
    g_edx = g_eax;
    g_edx = g_edx * 0x34u;
    guest_write32(g_edi, g_eax);
    g_esi = g_edx + g_ecx + 0x70u;
    g_eax += 1u;
    g_edx = (uint32_t)(((int32_t)g_eax) >> 31);      /* cdq */
    a172c80_idiv(guest_read32(g_ecx + 0x140u));      /* divisor read after the store */
    g_eax = g_esi;
    guest_write32(g_edi, g_edx);
    g_edi = a172c80_pop();
    g_esi = a172c80_pop();
}

/*
 * T1477 appendix draft 0x00177930 (legacy vector mode, cdecl, 2 stack args, no register input).
 * Original behavior: no null check. ECX = child pointer [object+0x7C]; the float argument is
 * loaded (movss, upper lanes cleared) and stored at [child+0x8FC].
 * Arguments: object, float bits. Guest globals touched: none. Writes: [child+0x8FC] only.
 * Read order: object, [object+0x7C], float argument, then the store. A store that lands on an
 * argument slot or on [object+0x7C] does not change the exit registers. No PUSH or POP.
 * Register exit contract (single path): EAX = object, ECX = [object+0x7C] (the child, never
 * modified), EDX unchanged, XMM0 = { float bits, 0, 0, 0 }. EBX/ESI/EDI/EBP untouched,
 * XMM1..7 untouched. */

GAME_REPLACE_EXACT(00177930, cdecl, 2, u32, game_object_set_child_0x7c_float_0x8fc)
{
    g_eax = game_stack_arg(0u);                       /* object */
    g_ecx = guest_read32(g_eax + 0x7Cu);              /* child */
    g_xmm0.u[0] = game_stack_arg(1u);                 /* movss xmm0, [float argument] */
    g_xmm0.u[1] = 0u;
    g_xmm0.u[2] = 0u;
    g_xmm0.u[3] = 0u;
    guest_write32(g_ecx + 0x8FCu, g_xmm0.u[0]);
}
