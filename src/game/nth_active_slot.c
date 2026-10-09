/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
extern __thread uint32_t g_esi, g_edi;
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")
static void save_word(uint32_t value)
{
    guest_write32(g_esp - 4u, value);
    ORDER_GUEST();
    g_esp -= 4u;
    ORDER_GUEST();
}
static void restore_pair(void)
{
    g_edi = guest_read32(g_esp);
    ORDER_GUEST();
    g_esp += 4u;
    ORDER_GUEST();
    g_esi = guest_read32(g_esp);
    ORDER_GUEST();
    g_esp += 4u;
    ORDER_GUEST();
}
GAME_REPLACE_EXACT(00038FD0, cdecl, 1, u32, game_messenger_slot_get_nth_active_68c6fc)
{
    save_word(g_esi);
    g_esi = guest_read32(g_esp + 8u);
    ORDER_GUEST();
    save_word(g_edi);
    g_edi = guest_read32(0x0068C6FCu);
    ORDER_GUEST();
    g_edx = 0u;
    g_eax = 0u;
    g_ecx = g_edi + 0x17Fu;
    ORDER_GUEST();
    do {
        if ((guest_read8(g_ecx) & 1u) != 0u) {
            ORDER_GUEST();
            if (g_edx == g_esi) {
                g_eax = g_eax * 416u + g_edi;
                ORDER_GUEST();
                restore_pair();
                return;
            }
            g_edx++;
        }
        g_eax++;
        g_ecx += 416u;
        ORDER_GUEST();
    } while (g_eax < 20u);
    restore_pair();
    g_eax = 0u;
    ORDER_GUEST();
}
#undef ORDER_GUEST
