/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1576 batch B: hand C for roots whose original holds a guarded jump table or an exact-entry
 * tail jmp, proved with the opt-in guarded-jump-schema3 contract (no vector state needed).
 * Record: docs/evidence/t1576/batch-b.md. Every body is byte-identical to the text that was
 * proved (the scratch file of each proof is this prefix plus exactly one body). */
#include "game_guest_tail.h"

extern __thread uint32_t g_edi;

/* A real nested guest call with the retail return VA of the original call site.
 * `reserved` is the number of bytes the original had pushed below ESP before the call
 * (saved registers, not the arguments). */
static void gb_call(
    uint32_t target, game_convention convention, uint32_t return_va, uint32_t reserved,
    const uint32_t *args, unsigned count)
{
    if (game_guest_call(target, convention, return_va, reserved, 0u, 0u, args, count) !=
        GAME_GUEST_CALL_OK) {
        __builtin_trap();
    }
}

/* movzx r32, word [address] and movsx r32, word [address] without a 4 byte read. */
static __attribute__((unused)) uint32_t gb_read16(guest_addr address)
{
    return (uint32_t)guest_read8(address) | ((uint32_t)guest_read8(address + 1u) << 8);
}

/* 0x00345710: cdecl(), T1576 batch B. A random value (0x153B40, the 64 bit LCG) modulo 5 selects,
 * through the original jump table `cmp edx,3 / ja default / jmp [edx*4 + 0x345770]` (4 slots), the
 * index passed to 0x80F20 (table 4D2D64 entry by index). Remainder 4 (the JA default) uses 0x3C3. */
GAME_REPLACE_EXACT(00345710, cdecl, 0, u32, game_pick_random_one_of_five_table_entry)
{
    gb_call(0x00153B40u, GAME_CC_cdecl, 0x00345715u, 0u, NULL, 0u);
    g_edx = 0u;
    g_ecx = 5u;
    const uint32_t dividend = g_eax;
    g_eax = dividend / 5u;
    g_edx = dividend % 5u;
    uint32_t slot = 0xFFFFFFFFu;
    if (g_edx <= 3u) {
        slot = g_edx;
    }
    uint32_t index;
    uint32_t return_va;
    switch (slot) {
    case 0:
        index = 0x3C2u;
        return_va = 0x00345734u;
        break;
    case 1:
        index = 0x3BEu;
        return_va = 0x00345742u;
        break;
    case 2:
        index = 0x3BFu;
        return_va = 0x00345750u;
        break;
    case 3:
        index = 0x3C1u;
        return_va = 0x0034575Eu;
        break;
    default:
        index = 0x3C3u;
        return_va = 0x0034576Cu;
        break;
    }
    gb_call(0x00080F20u, GAME_CC_cdecl, return_va, 0u, &index, 1u);
}
