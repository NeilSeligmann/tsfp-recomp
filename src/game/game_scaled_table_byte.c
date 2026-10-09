/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

/* Exact 0x24E90 register and memory behavior; the table's purpose is unnamed. */
GAME_REPLACE_EXACT(00024E90, cdecl, 1, u32, game_scaled_table_byte)
{
    g_eax = game_stack_arg(0u);
    if (g_eax < 11u) {
        g_eax *= 3u;
        g_eax <<= 4u;
        const uint8_t byte = guest_read8(0x005655B0u + g_eax);
        g_eax = (g_eax & 0xFFFFFF00u) | byte;
    } else {
        g_eax &= 0xFFFFFF00u;
    }
}
