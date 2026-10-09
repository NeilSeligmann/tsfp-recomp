/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Exact integer bit-index helper at 0x0046E666. The name records only the measured
 * lowest-set-bit scan; it does not assign a meaning to the caller's input.
 */
#include "game_replace.h"

static void game_bit_index(void)
{
    g_ecx = 0u;
    g_eax &= 0xFFFFFF00u;
    g_ecx++;

    for (;;) {
        if ((game_stack_arg(0u) & g_ecx) != 0u)
            break;
        g_eax = (g_eax & 0xFFFFFF00u) | ((g_eax + 1u) & 0xFFu);
        g_ecx <<= 1;
        if ((g_eax & 0xFFu) >= 32u)
            break;
    }
}

GAME_REPLACE_EXACT(0046E666, stdcall, 1, u32, game_bit_index)
{
    game_bit_index();
}
