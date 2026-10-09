/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

/* Exact register behavior of the 15-instruction body at 0x003CA440. */
GAME_REPLACE_EXACT(003CA440, cdecl, 0, u32, game_u64_shift_right)
{
    const uint32_t count = g_ecx & 0xFFu;
    const uint32_t low = g_eax;
    const uint32_t high = g_edx;

    if (count >= 64u) {
        g_eax = 0u;
        g_edx = 0u;
    } else if (count >= 32u) {
        const uint32_t shift = count & 0x1Fu;
        g_eax = high >> shift;
        g_edx = 0u;
        g_ecx = (g_ecx & 0xFFFFFF00u) | shift;
    } else if (count != 0u) {
        g_eax = (low >> count) | (high << (32u - count));
        g_edx = high >> count;
    }
}
