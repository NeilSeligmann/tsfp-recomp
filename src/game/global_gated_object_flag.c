/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")
GAME_REPLACE_EXACT(00012EC0, cdecl, 1, u32, game_global_gated_object_flag02000000_mask)
{
    uint8_t flags = guest_read8(0x007DE458u);
    ORDER_GUEST();
    if (flags & 0x40u) {
        g_eax = 0u;
        return;
    }
    g_eax = guest_read32(0x0079094Cu);
    ORDER_GUEST();
    if (g_eax == 100u || g_eax == 56u) {
        g_eax = 0u;
        return;
    }
    uint8_t mode = guest_read8(0x007DE455u);
    ORDER_GUEST();
    if (mode != 10u && g_eax != 102u) {
        g_eax = 0u;
        return;
    }
    g_eax = game_stack_arg(0u);
    ORDER_GUEST();
    g_eax = guest_read32(g_eax + 0x28u);
    ORDER_GUEST();
    g_eax &= 0x02000000u;
}
#undef ORDER_GUEST
