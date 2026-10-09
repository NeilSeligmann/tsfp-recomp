/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")
GAME_REPLACE_EXACT(003194E0, cdecl, 1, u32, game_find_color_index_in_783a80_ignoring_low_byte)
{
    g_ecx = game_stack_arg(0u);
    ORDER_GUEST();
    g_eax = 0u;
    g_ecx &= 0xFFFFFF00u;
    for (;;) {
        g_edx = guest_read32(0x00783A80u + g_eax * 4u);
        ORDER_GUEST();
        g_edx &= 0xFFFFFF00u;
        if (g_edx == g_ecx) return;
        g_eax += 1u;
        if (g_eax >= 64u) break;
    }
    g_eax &= 0xFFFFFF00u;
}
#undef ORDER_GUEST
