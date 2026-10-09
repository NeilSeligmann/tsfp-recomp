/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
extern __thread uint32_t g_esi;
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")
static uint32_t signed_word(uint32_t address)
{
    uint16_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return (value & 0x8000u) ? (uint32_t)value | 0xFFFF0000u : value;
}
GAME_REPLACE_EXACT(00064D60, cdecl, 1, u32, game_table_4d1ac0_find_index_by_word_0x6)
{
    g_edx = game_stack_arg(0u);
    ORDER_GUEST();
    g_eax = 0u;
    g_ecx = 0x004D1AC6u;
    ORDER_GUEST();
    guest_write32(g_esp - 4u, g_esi);
    ORDER_GUEST();
    g_esp -= 4u;
    ORDER_GUEST();
    for (;;) {
        g_esi = signed_word(g_ecx);
        ORDER_GUEST();
        if (g_edx == g_esi) break;
        g_ecx += 68u;
        g_eax += 1u;
        if (g_ecx >= 0x004D1E3Au) {
            g_eax = 0xFFFFFFFFu;
            break;
        }
    }
    g_esi = guest_read32(g_esp);
    ORDER_GUEST();
    g_esp += 4u;
    ORDER_GUEST();
}
#undef ORDER_GUEST
