/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

#define ORDER_GUEST() __asm__ volatile("" ::: "memory")

/* Original 0x174e0 reads both selected words before either stack mask.
 * ECX's masked high word is observable; EDX is untouched. */
GAME_REPLACE_EXACT(000174E0, cdecl, 3, u32, game_object_mode_selected_flag_pair_intersects)
{
    uint8_t mode = guest_read8(0x007DE455u);
    ORDER_GUEST();
    g_ecx = game_stack_arg(0u);
    ORDER_GUEST();
    uint32_t offset = mode == 10u ? 0x10u : 0x18u;
    g_eax = guest_read32(g_ecx + offset);
    ORDER_GUEST();
    g_ecx = guest_read32(g_ecx + offset + 4u);
    ORDER_GUEST();
    g_eax &= game_stack_arg(1u);
    ORDER_GUEST();
    g_ecx &= game_stack_arg(2u);
    ORDER_GUEST();
    g_eax |= g_ecx;
    g_eax = g_eax != 0u ? 1u : 0u;
}

#undef ORDER_GUEST
