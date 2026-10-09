/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1576 batch C: hand C for roots whose original holds a guarded jump table or an exact-entry
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

/* mov word [address], value (low 16 bits) without a 4 byte write. */
static __attribute__((unused)) void gb_write16(guest_addr address, uint32_t value)
{
    guest_write8(address, (uint8_t)(value & 0xFFu));
    guest_write8(address + 1u, (uint8_t)((value >> 8) & 0xFFu));
}

/* movzx r32, word [address] and movsx r32, word [address] without a 4 byte read. */
static __attribute__((unused)) uint32_t gb_read16(guest_addr address)
{
    return (uint32_t)guest_read8(address) | ((uint32_t)guest_read8(address + 1u) << 8);
}

/* 0x002D6490: cdecl(), T1576 batch C (corrected arm order, C5 of batch B). While a live net mode is active (0x35E960) the disconnect
 * reason (0x35E970) 1 answers the descriptor 0x4E9A14 and 2 or 3 answer 0x4E9A24. Otherwise the
 * state [0x78AD20] - 4 selects a menu descriptor through the original jump table
 * `cmp eax,0xC / ja default / jmp [eax*4 + 0x2D6514]` (13 slots, slots 7 to 9 and the JA default
 * share one arm that answers 0). Every arm sets eax, so the index is not kept. */
GAME_REPLACE_EXACT(002D6490, cdecl, 0, u32, game_menu_descriptor_by_state_78ad20_minus_4)
{
    gb_call(0x0035E960u, GAME_CC_cdecl, 0x002D6495u, 0u, NULL, 0u);
    if (g_eax != 0u) {
        gb_call(0x0035E970u, GAME_CC_cdecl, 0x002D649Eu, 0u, NULL, 0u);
        if (g_eax == 1u) {
            g_eax = 0x004E9A14u;
            return;
        }
        gb_call(0x0035E970u, GAME_CC_cdecl, 0x002D64AEu, 0u, NULL, 0u);
        int closing = (g_eax == 2u);
        if (!closing) {
            gb_call(0x0035E970u, GAME_CC_cdecl, 0x002D64B8u, 0u, NULL, 0u);
            closing = (g_eax == 3u);
        }
        if (closing) {
            g_eax = 0x004E9A24u;
            return;
        }
    }
    g_eax = guest_read32(0x0078AD20u) - 4u;
    uint32_t slot = 0xFFFFFFFFu;
    if (g_eax <= 0xCu) {
        slot = g_eax;
    }
    switch (slot) {
    case 0:
        g_eax = 0x005238E0u;
        break;
    case 5:
        /* Original slot 5 jumps into slot 1's code (jne 0x2D64F2): this label comes first and
         * falls through into the next one, so the hit set is slots 1 and 5 like the original's. */
        if (guest_read8(0x0078ADAEu) == 2u) {
            g_eax = 0x00523890u;
            break;
        }
        /* fall through */
    case 1:
        g_eax = 0x00523800u;
        break;
    case 2:
    case 3:
        g_eax = 0x00523860u;
        break;
    case 4:
        g_eax = 0x00523830u;
        break;
    case 6:
    case 12:
        g_eax = 0x00524578u;
        break;
    case 10:
        g_eax = 0x00523920u;
        break;
    case 11:
        g_eax = 0x00524598u;
        break;
    case 7:
    case 8:
    case 9:
    default:
        g_eax = 0u;
        break;
    }
}

/* 0x00338BC0: cdecl(src, dst), T1576 batch C (re-dispatch to the default join, C5 of batch B). Expands the fx record `src` into the entry `dst`.
 * The record's dword at +4 selects the entry kind through the original jump table
 * `cmp ecx,3 / ja Lstore / jmp [ecx*4 + 0x338C28]` (kinds 0, 1, 2, 3 store 0, 3, 2, 1, the JA
 * default stores the initial 0). The two palette bytes go through 0x3194C0 (live). The original
 * saves ESI and EDI (restored on return), so they are restored here too. */
GAME_REPLACE_EXACT(00338BC0, cdecl, 2, u32, game_expand_fx_record_into_entry_with_two_palette_colors)
{
    const uint32_t entry_esi = g_esi;
    const uint32_t entry_edi = g_edi;
    g_edi = game_stack_arg(0u);
    g_ecx = guest_read32(g_edi + 4u);
    g_edx = 0u;
    g_eax = 0u;
    uint32_t slot = 0xFFFFFFFFu;
    if (g_ecx <= 3u) {
        slot = g_ecx;
    }
    /* Every original arm jumps to the join 0x338BF0, which is also the JA default target, so the
     * original hit set of every slot contains the default. The switch is therefore entered a
     * second time with the default sentinel after each arm (a re-dispatch, not a goto: the
     * compiler counts the default label only when the switch reaches it). */
    for (;;) {
        switch (slot) {
        case 0:
            g_eax = 0u;
            slot = 0xFFFFFFFFu;
            continue;
        case 1:
            g_eax = 3u;
            slot = 0xFFFFFFFFu;
            continue;
        case 2:
            g_eax = 2u;
            slot = 0xFFFFFFFFu;
            continue;
        case 3:
            g_eax = 1u;
            slot = 0xFFFFFFFFu;
            continue;
        default:
            break;
        }
        break;
    }
    g_esi = game_stack_arg(1u);
    guest_write32(g_esi, g_eax);
    g_eax = guest_read32(g_edi + 0xCu);
    guest_write32(g_esi + 4u, g_eax);
    g_ecx = guest_read32(g_edi + 0x10u);
    guest_write32(g_esi + 8u, g_ecx);
    guest_write32(g_esi + 0xCu, g_edx);
    guest_write32(g_esi + 0x10u, g_edx);
    g_edx = guest_read8(g_edi);
    uint32_t color_index = g_edx;
    gb_call(0x003194C0u, GAME_CC_cdecl, 0x00338C11u, 8u, &color_index, 1u);
    guest_write32(g_esi + 0x14u, g_eax);
    g_eax = guest_read8(g_edi + 1u);
    color_index = g_eax;
    gb_call(0x003194C0u, GAME_CC_cdecl, 0x00338C1Eu, 8u, &color_index, 1u);
    g_edi = entry_edi;
    guest_write32(g_esi + 0x18u, g_eax);
    g_esi = entry_esi;
}
