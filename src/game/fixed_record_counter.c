/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")
static void count_flag(uint32_t address)
{
    uint8_t value = guest_read8(address);
    ORDER_GUEST();
    if (value & (uint8_t)g_edx) g_ecx += 1u;
    ORDER_GUEST();
}
GAME_REPLACE_EXACT(00356290, cdecl, 0, u32, game_net_count_free_record_124_slots_of_16)
{
    g_ecx = 0u;
    g_eax = 0x007773B0u;
    g_edx = (g_edx & 0xFFFFFF00u) | 1u;
    ORDER_GUEST();
    do {
        count_flag(g_eax - 0x124u);
        count_flag(g_eax);
        count_flag(g_eax + 0x124u);
        count_flag(g_eax + 0x248u);
        g_eax += 0x490u;
        ORDER_GUEST();
    } while (g_eax < 0x007785F0u);
    g_eax = 16u;
    g_eax -= g_ecx;
    g_ecx = ((int32_t)g_eax < 0 ? 1u : 0u) - 1u;
    g_eax &= g_ecx;
}
#undef ORDER_GUEST
