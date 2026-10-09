/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")
GAME_REPLACE_EXACT(00031040, cdecl, 0, u32, game_slot_find_free_687eb4)
{
    g_edx = guest_read32(0x00687EB4u);
    ORDER_GUEST();
    g_eax = 0u;
    g_ecx = g_edx;
    ORDER_GUEST();
    while ((guest_read8(g_ecx) & 1u) != 0u) {
        ORDER_GUEST();
        g_eax++;
        g_ecx += 20u;
        ORDER_GUEST();
        if (g_eax >= 500u) {
            g_eax = 0u;
            ORDER_GUEST();
            return;
        }
    }
    g_eax = g_edx + g_eax * 20u;
    ORDER_GUEST();
}
#undef ORDER_GUEST
