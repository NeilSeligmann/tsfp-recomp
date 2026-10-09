/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")
static uint32_t low_sete_decrement(uint32_t value)
{
    return (value & 0xFFFFFF00u) | (value == 0u ? 0u : 255u);
}
GAME_REPLACE_EXACT(0031DCD0, cdecl, 1, u32, game_pack_flag_struct_fields_10_c_4_8_into_bitmask)
{
    g_ecx = game_stack_arg(0u);
    ORDER_GUEST();
    g_eax = guest_read32(g_ecx + 0x10u);
    ORDER_GUEST();
    g_edx = guest_read32(g_ecx + 0xCu);
    ORDER_GUEST();
    g_eax = low_sete_decrement(g_eax);
    g_eax &= 4u;
    g_edx = low_sete_decrement(g_edx);
    g_edx &= 2u;
    g_eax |= g_edx;
    ORDER_GUEST();
    g_edx = guest_read32(g_ecx + 4u);
    ORDER_GUEST();
    g_edx = low_sete_decrement(g_edx);
    g_edx &= 8u;
    g_eax |= g_edx;
    ORDER_GUEST();
    g_edx = guest_read32(g_ecx + 8u);
    ORDER_GUEST();
    g_ecx = (g_ecx & 0xFFFFFF00u) | (g_edx != 0u);
    g_eax |= g_ecx & 255u;
}
#undef ORDER_GUEST
