/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")
static uint32_t read_index16(uint32_t address)
{
    uint16_t value;
    memcpy(&value, game_host_ptr(address), sizeof(value));
    return value;
}
GAME_REPLACE_EXACT(00012BA0, cdecl, 3, u32, game_flag10_selected_u16_indexed_field_pointer)
{
    g_eax = game_stack_arg(0u);
    ORDER_GUEST();
    uint8_t flags = guest_read8(g_eax + 12u);
    ORDER_GUEST();
    if (flags & 0x10u) {
        g_ecx = game_stack_arg(1u);
        ORDER_GUEST();
        g_eax = guest_read32(g_ecx + 4u);
        ORDER_GUEST();
        if (g_eax != 0u) {
            g_edx = game_stack_arg(2u);
            ORDER_GUEST();
            g_ecx = read_index16(g_edx + 6u);
            ORDER_GUEST();
            g_eax += g_ecx * 8u + 2u;
        }
    } else {
        g_edx = game_stack_arg(1u);
        ORDER_GUEST();
        g_eax = guest_read32(g_edx + 4u);
        ORDER_GUEST();
        if (g_eax != 0u) {
            g_ecx = game_stack_arg(2u);
            ORDER_GUEST();
            g_edx = read_index16(g_ecx + 6u);
            ORDER_GUEST();
            g_edx <<= 4u;
            g_eax += g_edx + 12u;
        }
    }
}
#undef ORDER_GUEST
