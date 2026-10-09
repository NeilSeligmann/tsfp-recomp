/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Small call-free functions with branches or loops from retail XBE range
 * 0x00180000-0x00470000 (T77 follow-up). Every body is register-exact
 * (GAME_REPLACE_EXACT): eax, ecx and edx are left as the original leaves them. Only
 * measured behaviour is named, no game meaning is assigned to fields or globals.
 * See docs/t77-small-logic-180000-470000.md for proofs and rejects.
 */
#include "game_replace.h"

static uint32_t game_read16(uint32_t address)
{
    return (uint32_t)guest_read8(address) | ((uint32_t)guest_read8(address + 1u) << 8);
}

/* 0x001B63D0: a0 = object, a1 = state. True when the object's field 0xE8 equals the state
 * but field 0xF8 does not. ecx = object. */
GAME_REPLACE_EXACT(001B63D0, cdecl, 2, u32, game_field_e8_only_matches)
{
    const uint32_t state = game_stack_arg(1u);
    g_ecx = game_stack_arg(0u);
    g_eax = (guest_read32(g_ecx + 0xE8u) == state && guest_read32(g_ecx + 0xF8u) != state) ? 1u : 0u;
}

/* 0x001B7580: true when object field 0xE8 is one of the kinds 10, 11, 12 or 27. */
GAME_REPLACE_EXACT(001B7580, cdecl, 1, u32, game_kind_is_10_11_12_27)
{
    const uint32_t kind = guest_read32(game_stack_arg(0u) + 0xE8u);
    g_eax = (kind == 10u || kind == 11u || kind == 12u || kind == 27u) ? 1u : 0u;
}

/* 0x00228C20: a score of 100 for the 16 bit codes 0x5C7, 0x5D2 and 0x5E1 at field 0x94,
 * 75 for any other code. */
GAME_REPLACE_EXACT(00228C20, cdecl, 1, u32, game_code_weight)
{
    const uint32_t code = game_read16(game_stack_arg(0u) + 0x94u);
    g_eax = (code == 0x5C7u || code == 0x5D2u || code == 0x5E1u) ? 100u : 75u;
}

/* 0x0031D2D0: true when the record's field 0x34 is 1, 3 or 5. */
GAME_REPLACE_EXACT(0031D2D0, cdecl, 1, u32, game_record_34_is_odd_small)
{
    const uint32_t value = guest_read32(game_stack_arg(0u) + 0x34u);
    g_eax = (value == 1u || value == 3u || value == 5u) ? 1u : 0u;
}

/* 0x003346E0: true when both 16 bit words at the argument are at least 0x3E9. */
GAME_REPLACE_EXACT(003346E0, cdecl, 1, u32, game_both_words_at_least_3e9)
{
    const uint32_t record = game_stack_arg(0u);
    g_eax = (game_read16(record) >= 0x3E9u && game_read16(record + 2u) >= 0x3E9u) ? 1u : 0u;
}

/* 0x003951D0: copy the 8 byte entry at index a0 of table a1 to a3. An index at or beyond
 * the count a2 reads entry 0. The low dword is stored before the high one is read, as the
 * original does. eax = high dword, ecx = table, edx = destination. */
GAME_REPLACE_EXACT(003951D0, cdecl, 4, u32, game_copy_table_entry_wrapped)
{
    uint32_t index = game_stack_arg(0u);
    if (index >= game_stack_arg(2u)) {
        index = 0u;
    }
    g_ecx = game_stack_arg(1u);
    g_edx = game_stack_arg(3u);
    guest_write32(g_edx, guest_read32(g_ecx + index * 8u));
    g_eax = guest_read32(g_ecx + index * 8u + 4u);
    guest_write32(g_edx + 4u, g_eax);
}

/* 0x003C3D00: a1 plus 8 when the dword at a0 is non-zero. */
GAME_REPLACE_EXACT(003C3D00, cdecl, 2, u32, game_add_header_size)
{
    g_eax = game_stack_arg(1u);
    if (guest_read32(game_stack_arg(0u)) != 0u) {
        g_eax += 8u;
    }
}

/* 0x003C3CD0: a1 minus 8 when the block a0 has a non-zero dword 0 and dword 1 and
 * a1 >= 8 (signed), else a1. ecx = dword 1 when dword 0 is non-zero. */
GAME_REPLACE_EXACT(003C3CD0, cdecl, 2, u32, game_subtract_header_size)
{
    const uint32_t block = game_stack_arg(0u);
    g_eax = game_stack_arg(1u);
    if (guest_read32(block) == 0u) {
        return;
    }
    g_ecx = guest_read32(block + 4u);
    if (g_ecx != 0u && (int32_t)g_eax >= 8) {
        g_eax -= 8u;
    }
}

/* 0x003CADD6: wcschr. a0 = 16 bit string, a1 = character (low 16 bits). Returns the
 * address of the first match (the terminator matches character 0) or 0. cx and dx hold
 * the last word read and the character, their high halves are untouched. */
GAME_REPLACE_EXACT(003CADD6, cdecl, 2, u32, game_wcschr)
{
    const uint32_t wanted = game_stack_arg(1u) & 0xFFFFu;
    uint32_t cursor = game_stack_arg(0u);
    uint32_t word = game_read16(cursor);
    g_edx = (g_edx & 0xFFFF0000u) | wanted;
    while (word != 0u) {
        if (word == wanted) {
            break;
        }
        cursor += 2u;
        word = game_read16(cursor);
    }
    g_ecx = (g_ecx & 0xFFFF0000u) | word;
    g_eax = (word == wanted) ? cursor : 0u;
}

/* 0x003CFED6: copy three dwords from a1 to a0, one at a time. eax = a1 + 12,
 * ecx = a0 - a1, edx = 0. */
GAME_REPLACE_EXACT(003CFED6, cdecl, 2, u32, game_copy_three_dwords)
{
    g_eax = game_stack_arg(1u);
    g_ecx = game_stack_arg(0u) - g_eax;
    for (g_edx = 3u; g_edx != 0u; g_edx--) {
        guest_write32(g_ecx + g_eax, guest_read32(g_eax));
        g_eax += 4u;
    }
}

/* 0x003CFEF1: true when all three dwords at the argument are zero. ecx = argument. */
GAME_REPLACE_EXACT(003CFEF1, cdecl, 1, u32, game_three_dwords_zero)
{
    g_ecx = game_stack_arg(0u);
    g_eax = (guest_read32(g_ecx) == 0u && guest_read32(g_ecx + 4u) == 0u &&
             guest_read32(g_ecx + 8u) == 0u) ? 1u : 0u;
}

/* 0x00376A20: put a1 into the first zero dword of the 16 slots at a0 + 0x14 and return
 * its index, or return 16 when full. ecx = a1 when stored, else the slot end address;
 * edx = a0. */
GAME_REPLACE_EXACT(00376A20, cdecl, 2, u32, game_slot_insert)
{
    uint32_t index = 0u;
    g_edx = game_stack_arg(0u);
    g_ecx = g_edx + 0x14u;
    while (guest_read32(g_ecx) != 0u) {
        index++;
        g_ecx += 4u;
        if ((int32_t)index >= 16) {
            g_eax = index;
            return;
        }
    }
    g_ecx = game_stack_arg(1u);
    guest_write32(g_edx + index * 4u + 0x14u, g_ecx);
    g_eax = index;
}

/* 0x00264740: when the sub-object [a0 + 0x7C] has state [+0x20] of 0, 1, 2 or 4 store 3
 * in its field 0x9D0. eax = the state, ecx = the sub-object. */
GAME_REPLACE_EXACT(00264740, cdecl, 1, u32, game_set_mode_three_unless_done)
{
    g_ecx = guest_read32(game_stack_arg(0u) + 0x7Cu);
    g_eax = guest_read32(g_ecx + 0x20u);
    if (g_eax == 0u || g_eax == 1u || g_eax == 2u || g_eax == 4u) {
        guest_write32(g_ecx + 0x9D0u, 3u);
    }
}

/* 0x00243040: false when the state at [[a0 + 0x7C] + 0xC] is 7 or 9, else true.
 * ecx = the 0x7C sub-object. */
GAME_REPLACE_EXACT(00243040, cdecl, 1, u32, game_state_not_7_or_9)
{
    g_ecx = guest_read32(game_stack_arg(0u) + 0x7Cu);
    g_eax = guest_read32(g_ecx + 0xCu);
    g_eax = (g_eax == 7u || g_eax == 9u) ? 0u : 1u;
}
