/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Exact register and read-order behavior of the 13-instruction body at 0x00331C70.
 * The name identifies its literal compare value, not the value's purpose.
 */
#include "game_replace.h"

static uint16_t game_read_u16(guest_addr address)
{
    uint16_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return value;
}

GAME_REPLACE_EXACT(00331C70, cdecl, 2, u32, game_8008_stack_predicate)
{
    g_eax = 0xFFFF8008u;
    const uint16_t first_low = game_read_u16(g_esp + 4u);

    if (first_low == 0x8008u) {
        g_eax = 0u;
        return;
    }

    g_ecx = game_stack_arg(0u) >> 16;
    if ((g_ecx & 0xFFFFu) == 0x8008u) {
        g_eax = 0u;
        return;
    }

    const uint16_t second_low = game_read_u16(g_esp + 8u);
    g_eax = second_low == 0x8008u ? 0u : 1u;
}
