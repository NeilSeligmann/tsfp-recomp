/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")
static uint32_t read_word16(uint32_t address)
{
    uint16_t value;
    memcpy(&value, game_host_ptr(address), sizeof(value));
    return value;
}
GAME_REPLACE_EXACT(0001E750, cdecl, 1, u32, game_slot_0x34_get_word_0x2a_or_default)
{
    g_eax = game_stack_arg(0u);
    ORDER_GUEST();
    if ((int32_t)g_eax < 0) {
        g_eax = guest_read32(0x004E8CF4u);
        ORDER_GUEST();
        return;
    }
    g_ecx = guest_read32(0x004B85C8u);
    ORDER_GUEST();
    g_eax *= 52u;
    ORDER_GUEST();
    g_edx = guest_read32(g_ecx);
    ORDER_GUEST();
    g_eax = read_word16(g_eax + g_edx + 0x2Au);
    ORDER_GUEST();
}
#undef ORDER_GUEST
