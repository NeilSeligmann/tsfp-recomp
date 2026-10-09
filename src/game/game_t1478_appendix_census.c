/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1478 appendix census: eight roots that earlier records named (docs/t1478-appendix-census.md).
 * Each body passed the fixed matrix (seeds 20261001/20261006, O0/O2/O3, 600 random cases,
 * verdicts >= 100, coverage >= 0.9, null-agree <= 0.9, 0 DISAGREE) and is the frozen draft
 * of docs/data/t1478-appendix-census/drafts/ except for the include, the shared XMM0 slot
 * and the SPDX line. Original guest reads, writes, register exits and saved-register
 * stack slots are preserved; see each header comment. */
#include "game_replace.h"

/* Shared XMM0 slot of the three legacy vector bodies. The runtime owns the strong slot, the
 * weak definition lets registry-only links resolve it. */
typedef union T1478AppendixXmm {
    uint32_t u[4];
    uint64_t q[2];
} T1478AppendixXmm;
__thread T1478AppendixXmm g_xmm0 __attribute__((weak, aligned(8)));

/* T1478 appendix draft 0x001B5680 (legacy vector mode, cdecl, 2 stack args, no register input).
 * Original behavior: stores the second argument at object+0x44 and a zero float at object+0x48,
 * where the object is the first argument. The float zero comes from a cleared XMM0.
 * Guest globals touched: none. Writes: [object+0x44] (dword, argument 1) then
 * [object+0x48] (dword, bit pattern of 0.0f).
 * Register exit contract (single path): EAX = object (argument 0), ECX = argument 1,
 * EDX unchanged. XMM0 is cleared in all four lanes (xorps xmm0, xmm0); XMM1..7 untouched.
 * Both arguments are read before either store, so a store that lands on an argument slot has no
 * effect on the exit registers. */


GAME_REPLACE_EXACT(001B5680, cdecl, 2, u32, game_set_dword_44_and_clear_float_48_of_object)
{
    g_eax = game_stack_arg(0u);                  /* object */
    g_ecx = game_stack_arg(1u);                  /* value stored at +0x44 */
    g_xmm0.q[0] = 0u;                            /* xorps xmm0, xmm0: all lanes cleared */
    g_xmm0.q[1] = 0u;
    guest_write32(g_eax + 0x44u, g_ecx);
    guest_write32(g_eax + 0x48u, g_xmm0.u[0]);   /* movss [object+0x48], xmm0 */
}

/* T1478 appendix draft 0x001B62D0 (legacy vector mode, cdecl, 2 stack args, no register input).
 * Original behavior: marks an object by setting bit 3 of its flag dword at +0x34, stores the
 * float argument at +0xF0, and clears the dword at +0xF8 when that dword equals the dword at
 * +0xE8 (compared as raw 32-bit values, as they were before the flag update and float store).
 * Arguments: object, float bits. Guest globals touched: none.
 * Read order: [obj+0x34], [obj+0xF8], float argument, then (after the +0x34 write) [obj+0xE8].
 * Write order: [obj+0x34] = old | 8, [obj+0xF0] = float bits, then [obj+0xF8] = 0 if equal.
 * Register exit contract (single path for registers): EAX = object, ECX = [obj+0xE8],
 * EDX = [obj+0xF8] as read at entry (not cleared even when the clear store happens).
 * XMM0 = { float bits, 0, 0, 0 } (movss load clears the upper lanes); XMM1..7 untouched. */


GAME_REPLACE_EXACT(001B62D0, cdecl, 2, u32,
                   game_object_set_flag_0x34_bit3_store_float_0xf0_clear_0xf8_if_equals_0xe8)
{
    g_eax = game_stack_arg(0u);                       /* object */
    g_ecx = guest_read32(g_eax + 0x34u);              /* flags */
    g_edx = guest_read32(g_eax + 0xF8u);              /* value compared against +0xE8 */
    g_xmm0.u[0] = game_stack_arg(1u);                 /* movss xmm0, [float argument] */
    g_xmm0.u[1] = 0u;
    g_xmm0.u[2] = 0u;
    g_xmm0.u[3] = 0u;
    g_ecx |= 8u;
    guest_write32(g_eax + 0x34u, g_ecx);
    g_ecx = guest_read32(g_eax + 0xE8u);              /* re-read after the flag store */
    guest_write32(g_eax + 0xF0u, g_xmm0.u[0]);
    if (g_ecx == g_edx) {
        guest_write32(g_eax + 0xF8u, 0u);
    }
}

/*
 * 0x001B6350, cdecl, 3 stack args, no register inputs, plain `ret`.
 *
 * Arguments (stack): record, id, mode.
 *   record+0x190 : pointer to an array of 12-byte table entries
 *   record+0x194 : signed entry count
 *   entry+0 (sx16) : id, compared as a 32-bit value against the id argument
 *   entry+2 (sx16) : mode, compared as a 32-bit value against the mode argument
 *
 * Behavior: scan the entries in order. An entry whose id equals the id argument and whose
 * mode equals the mode argument ends the scan at once and that entry pointer is returned.
 * An entry whose id matches and whose mode word is 2 while the mode argument is 0 (or whose
 * mode word is 0 while the mode argument is 2) is remembered as a fallback (the last one
 * wins). When the scan runs out the fallback (or 0) is returned. A count that is not
 * positive (signed) returns 0 without reading the table pointer.
 *
 * Guest globals: none. Guest writes: only the four register saves pushed on the stack
 * (ebx, ebp, esi, edi in that order); each is restored from its stack slot at the end.
 *
 * Register exit contract:
 *   eax : matching entry pointer, else fallback entry pointer, else 0.
 *   count <= 0      : ecx = entry ecx, edx = 0.
 *   exact match     : edx = index of the match, ecx = sx16(id word) with its low 16 bits
 *                     replaced by the mode word of the match.
 *   scan exhausted  : edx = count, ecx = the value left by the last scanned entry (sx16 of
 *                     its id word, low 16 bits replaced by its mode word if its id matched).
 *   ebx, ebp, esi, edi are restored from the guest stack slots; esp is back at entry.
 */
extern __thread uint32_t g_ebx, g_ebp, g_edi;

static uint32_t entry_find_read_sx16(uint32_t address)
{
    int16_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return (uint32_t)(int32_t)value;
}

static uint32_t entry_find_read_u16(uint32_t address)
{
    uint16_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return value;
}

static void entry_find_push(uint32_t value)
{
    guest_write32(g_esp - 4u, value);
    g_esp -= 4u;
}

static uint32_t entry_find_pop(void)
{
    const uint32_t value = guest_read32(g_esp);
    g_esp += 4u;
    return value;
}

GAME_REPLACE_EXACT(001B6350, cdecl, 3, u32, game_record_29c_find_table_entry_0x190_by_id_and_mode)
{
    g_eax = guest_read32(g_esp + 4u); /* record */
    entry_find_push(g_ebx);
    entry_find_push(g_ebp);
    entry_find_push(g_esi);
    entry_find_push(g_edi);
    /* After the four saves: id is at esp+0x18, mode at esp+0x1c. */
    g_edi = guest_read32(g_eax + 0x194u); /* entry count */
    g_ebx = 0u;                           /* fallback entry */
    g_edx = 0u;                           /* index */
    if ((int32_t)g_edi > 0) {
        g_eax = guest_read32(g_eax + 0x190u); /* entry pointer */
        g_esi = guest_read32(g_esp + 0x1cu);  /* mode argument */
        int exact = 0;
        do {
            g_ecx = entry_find_read_sx16(g_eax);
            if (guest_read32(g_esp + 0x18u) == g_ecx) {
                /* mov cx, [entry+2]: only the low 16 bits of ecx change. */
                g_ecx = (g_ecx & 0xFFFF0000u) | entry_find_read_u16(g_eax + 2u);
                g_ebp = (uint32_t)(int32_t)(int16_t)(uint16_t)g_ecx;
                if (g_esi == g_ebp) {
                    exact = 1;
                    break;
                }
                const uint16_t mode_word = (uint16_t)g_ecx;
                if ((mode_word == 2u && g_esi == 0u) || (mode_word == 0u && g_esi == 2u)) {
                    g_ebx = g_eax;
                }
            }
            g_edx += 1u;
            g_eax += 12u;
        } while ((int32_t)g_edx < (int32_t)g_edi);
        if (!exact) {
            g_eax = g_ebx;
        }
    } else {
        g_eax = g_ebx;
    }
    g_edi = entry_find_pop();
    g_esi = entry_find_pop();
    g_ebp = entry_find_pop();
    g_ebx = entry_find_pop();
}

/* T1478 appendix draft 0x001B7AA0 (legacy vector mode, cdecl, 3 stack args, no register input).
 * Original behavior: initialises a 0x3D8-byte record. Arguments: record pointer, value A,
 * value B. The record is zeroed with a forward `rep stosd` of 0xF6 dwords (direction flag clear,
 * as the ABI guarantees), then these dwords are stored in this order: +0x30 = -1, +0x44 = 0,
 * +0x34 = 0, +0x54 = 0, +0x5C = 0, +0xC8 = -1, then value B is read from the stack, +0x68 = 0,
 * value A is read from the stack, +0x48 = 0 (the cleared XMM0), +0x3CC = A, +0x3C = B.
 * EDI is saved with a real stack push before the fill and restored by a real pop at the end.
 * Because the stack arguments for A and B are read AFTER the fill and the earlier stores, a
 * record overlapping the caller stack changes what is read, and the popped EDI is whatever the
 * saved slot holds at pop time (it can be overwritten by the fill or by a later store).
 * Guest globals touched: none.
 * Register exit contract (single path): EAX = A as read, ECX = B as read, EDX = record pointer
 * (argument 0, read before the push), EDI = value popped from the saved slot, ESP back to entry.
 * XMM0 is cleared in all four lanes; XMM1..7 untouched. */

extern __thread uint32_t g_edi;


#define B7AA0_RECORD_DWORDS 0xF6u /* 0x3D8 bytes */

GAME_REPLACE_EXACT(001B7AA0, cdecl, 3, u32, game_zero_0x3d8_byte_struct_init_fields_0x30_0x3c_0x3cc_from_args)
{
    g_edx = game_stack_arg(0u);                         /* record pointer */
    g_xmm0.q[0] = 0u;                                   /* xorps xmm0, xmm0 */
    g_xmm0.q[1] = 0u;
    g_eax = 0u;
    guest_write32(g_esp - 4u, g_edi);                   /* push edi */
    g_esp -= 4u;
    g_ecx = B7AA0_RECORD_DWORDS;
    g_edi = g_edx;
    for (; g_ecx != 0u; g_ecx--) {                      /* rep stosd, ascending addresses */
        guest_write32(g_edi, g_eax);
        g_edi += 4u;
    }
    g_ecx = 0xFFFFFFFFu;                                /* or ecx, -1 */
    guest_write32(g_edx + 0x30u, g_ecx);
    guest_write32(g_edx + 0x44u, g_eax);
    guest_write32(g_edx + 0x34u, g_eax);
    guest_write32(g_edx + 0x54u, g_eax);
    guest_write32(g_edx + 0x5Cu, g_eax);
    guest_write32(g_edx + 0xC8u, g_ecx);
    g_ecx = guest_read32(g_esp + 0x10u);                /* value B (third stack argument) */
    guest_write32(g_edx + 0x68u, g_eax);
    g_eax = guest_read32(g_esp + 0xCu);                 /* value A (second stack argument) */
    guest_write32(g_edx + 0x48u, g_xmm0.u[0]);          /* movss [record+0x48], xmm0 (zero) */
    guest_write32(g_edx + 0x3CCu, g_eax);
    guest_write32(g_edx + 0x3Cu, g_ecx);
    g_edi = guest_read32(g_esp);                        /* pop edi */
    g_esp += 4u;
}

/* T1478 appendix draft 0x00230510 (cdecl, 0 stack args, no register input).
 * Original behavior: reads the dword game mode at 0x7DE470 and uses it as a signed index into
 * one of two descriptor pointer tables: the dword at 0x510788 + 4*mode while mode < 0x15,
 * otherwise the dword at 0x513D04 + 4*mode. There is no -1 special case and no range check;
 * the byte offset 4*mode wraps at 32 bits. Sibling of 0x0022E050, whose index comes from the
 * stack instead.
 * Guest globals touched: [0x7DE470] (dword, read), tables 0x510788 / 0x513D04 (read). No writes.
 * Register exit contract (both paths): EAX = the loaded dword, ECX and EDX unchanged. */

#define T1478A_00230510_MODE 0x007DE470u        /* current game mode, read as a full dword */
#define T1478A_00230510_TABLE_LOW 0x00510788u   /* used while (int32_t)mode < 0x15 */
#define T1478A_00230510_TABLE_HIGH 0x00513D04u  /* used for (int32_t)mode >= 0x15 */
#define T1478A_00230510_SPLIT 0x15

GAME_REPLACE_EXACT(00230510, cdecl, 0, u32, game_state_descriptor_get_for_current_state)
{
    g_eax = guest_read32(T1478A_00230510_MODE);
    if ((int32_t)g_eax < T1478A_00230510_SPLIT) {            /* cmp eax, 0x15 ; jge */
        g_eax = guest_read32(g_eax * 4u + T1478A_00230510_TABLE_LOW);
    } else {
        g_eax = guest_read32(g_eax * 4u + T1478A_00230510_TABLE_HIGH);
    }
}

/* VA 0x0023EF90, cdecl, one stack argument (the value), `ret` without immediate.
 * Searches 24 records of 0x5C bytes starting at guest 0x513EB8 (end 0x514758) and
 * compares the FIRST dword of each record with the argument, in ascending order. The
 * scan stops at the first equal record. The loop is do-while shaped, so record 0 is
 * always read; the end test is a signed compare of the record pointer with 0x514758.
 * Reads only: no guest writes, no stack use beyond the argument.
 * Register exits (EDX, EBX, ESI, EDI, EBP untouched by the original):
 *   found     : EAX = 1, ECX = value
 *   not found : EAX = 0, ECX = value
 */
GAME_REPLACE_EXACT(0023EF90, cdecl, 1, u32, game_is_value_in_record_5c_table_513eb8_24_entries)
{
    g_ecx = game_stack_arg(0u);
    g_eax = 0x513eb8u;
    do {
        if (g_ecx == guest_read32(g_eax)) {
            g_eax = 1u;
            return;
        }
        g_eax += 0x5cu;
    } while ((int32_t)g_eax < (int32_t)0x514758u);
    g_eax = 0u;
}

/* VA 0x00247A70, cdecl, two stack arguments (object, out pointer), `ret` without
 * immediate. The object must have its class dword (+0x20) equal to 0x10000000, which is
 * compared AFTER the pointer at +0x7C has been read (that read happens on every path).
 * Then follows the chain  P = [obj+0x7C] (non-null),  [P+4] (non-null),
 * Q = [P+0x10] (non-null),  R = [Q+0x150] (non-null),  value = [R+4] (signed >= 0).
 * On success the out pointer (argument 1, read only now) receives the value unless it
 * is null, and the result is 1. Any failed step returns 0 without touching memory.
 * Guest writes: only [out] on success.
 * Register exits (EDX untouched):
 *   class mismatch or [obj+0x7C] == 0 : EAX = 0, ECX = obj
 *   [P+4] == 0                         : EAX = 0, ECX = 0
 *   [P+0x10], [Q+0x150] == 0 or value < 0 (signed) : EAX = 0, ECX = [P+4]
 *   success                            : EAX = 1, ECX = out pointer (argument 1)
 */
GAME_REPLACE_EXACT(00247A70, cdecl, 2, u32,
                   game_object_class_10000000_get_controller_field_150_slot_4)
{
    int class_matches;

    g_ecx = game_stack_arg(0u);
    class_matches = guest_read32(g_ecx + 0x20u) == 0x10000000u;
    g_eax = guest_read32(g_ecx + 0x7cu);
    if (!class_matches || g_eax == 0u) {
        goto fail;
    }
    g_ecx = guest_read32(g_eax + 4u);
    if (g_ecx == 0u) {
        goto fail;
    }
    g_eax = guest_read32(g_eax + 0x10u);
    if (g_eax == 0u) {
        goto fail;
    }
    g_eax = guest_read32(g_eax + 0x150u);
    if (g_eax == 0u) {
        goto fail;
    }
    g_eax = guest_read32(g_eax + 4u);
    if ((int32_t)g_eax < 0) {
        goto fail;
    }
    g_ecx = game_stack_arg(1u);
    if (g_ecx != 0u) {
        guest_write32(g_ecx, g_eax);
    }
    g_eax = 1u;
    return;
fail:
    g_eax = 0u;
}

/* Real guest stack operations: the original pushes and pops ESI. */
static void g25fa10_push(uint32_t value)
{
    guest_write32(g_esp - 4u, value);
    g_esp -= 4u;
}

static uint32_t g25fa10_pop(void)
{
    const uint32_t value = guest_read32(g_esp);
    g_esp += 4u;
    return value;
}

/* VA 0x0025FA10, cdecl, one stack argument (the key), `ret` without immediate.
 * Linear search of the table at guest 0x78B2E0 (stride 0x20) comparing the first dword
 * of each entry with the key. The entry count is the signed dword at 0x78B520, read
 * before ESI is pushed. A count <= 0 scans nothing. The key is read after the push and
 * only when the count is positive. The result is the index of the first equal entry. A
 * miss ALSO returns 0, so a hit at index 0 and a miss are indistinguishable.
 * Guest writes: only the push of ESI at (entry ESP - 4); ESI is popped from that slot.
 * Register exits (EBX, EDI, EBP untouched; ESI restored from the stack slot):
 *   count <= 0 : EAX = 0, ECX unchanged (entry value), EDX = count
 *   hit        : EAX = index, ECX = 0x78B2E0 + 0x20 * index, EDX = count
 *   miss       : EAX = 0, ECX = 0x78B2E0 + 0x20 * count, EDX = count
 */
GAME_REPLACE_EXACT(0025FA10, cdecl, 1, u32, game_find_index_in_table_78b2e0_stride_20)
{
    g_edx = guest_read32(0x78b520u);
    g25fa10_push(g_esi);
    g_eax = 0u;
    if ((int32_t)g_edx > 0) {
        g_esi = guest_read32(g_esp + 8u); /* the key: argument 0 after the push */
        g_ecx = 0x78b2e0u;
        do {
            if (guest_read32(g_ecx) == g_esi) {
                g_esi = g25fa10_pop();
                return; /* EAX = index of the hit */
            }
            g_eax += 1u;
            g_ecx += 0x20u;
        } while ((int32_t)g_eax < (int32_t)g_edx);
        g_eax = 0u;
    }
    g_esi = g25fa10_pop();
}
