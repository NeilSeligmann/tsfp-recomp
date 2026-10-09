/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")
GAME_REPLACE_EXACT(00024E60, cdecl, 1, u32, game_memory_unit_slot_is_active)
{
    g_eax = game_stack_arg(0u);
    ORDER_GUEST();
    if (g_eax < 11u) {
        g_eax = g_eax * 48u + 0x005655B0u;
        ORDER_GUEST();
        if (guest_read8(g_eax) != 0u) {
            ORDER_GUEST();
            g_ecx = (g_ecx & 0xFFFFFF00u) | guest_read8(g_eax + 3u);
            ORDER_GUEST();
            if ((g_ecx & 0xFFu) != 0u) {
                g_eax = (g_eax & 0xFFFFFF00u) | 1u;
                ORDER_GUEST();
                return;
            }
        }
    }
    g_eax &= 0xFFFFFF00u;
    ORDER_GUEST();
}
#undef ORDER_GUEST
