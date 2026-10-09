/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Leaf helpers that update a guest global bitmask (T77 range 0x002C0000-0x00470000).
 * Only measured behaviour is named: no meaning is assigned to the globals.
 */
#include "game_replace.h"

/* 0x002D2BC0: global at 0x78AD24 |= argument. Leaves eax = argument, ecx = new value. */
static void game_global_or_mask(void)
{
    const uint32_t mask = game_stack_arg(0u);
    const uint32_t updated = guest_read32(0x78AD24u) | mask;
    guest_write32(0x78AD24u, updated);
    g_ecx = updated;
    g_eax = mask;
}

GAME_REPLACE_EXACT(002D2BC0, cdecl, 1, u32, game_global_or_mask)
{
    game_global_or_mask();
}

/* 0x00352B80: global at 0x76B0A4 &= ~(1 << (argument & 31)). Leaves eax = ~bit, ecx = new value. */
static void game_global_clear_bit(void)
{
    const uint32_t keep = ~(1u << (game_stack_arg(0u) & 31u));
    const uint32_t updated = guest_read32(0x76B0A4u) & keep;
    guest_write32(0x76B0A4u, updated);
    g_ecx = updated;
    g_eax = keep;
}

GAME_REPLACE_EXACT(00352B80, cdecl, 1, u32, game_global_clear_bit)
{
    game_global_clear_bit();
}
