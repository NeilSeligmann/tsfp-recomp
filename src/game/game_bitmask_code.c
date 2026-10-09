/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

/* Exact two-bit mapping in the 13-instruction retail body at 0x00331990. */
GAME_REPLACE_EXACT(00331990, cdecl, 1, u32, game_bitmask_code)
{
    const uint32_t value = game_stack_arg(0u);
    const uint32_t selected = value & 0x09000000u;

    if ((value & 0x01000000u) != 0u && (value & 0x08000000u) != 0u) {
        g_eax = 5u;
    } else {
        g_eax = selected == 0u ? 1u : 4u;
    }
}
