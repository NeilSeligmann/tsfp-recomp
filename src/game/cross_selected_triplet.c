/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
extern __thread uint32_t g_ebx, g_esi, g_edi;
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")
static void save_word(uint32_t value)
{
    guest_write32(g_esp - 4u, value);
    ORDER_GUEST();
    g_esp -= 4u;
    ORDER_GUEST();
}
static uint32_t restore_word(void)
{
    uint32_t value = guest_read32(g_esp);
    ORDER_GUEST();
    g_esp += 4u;
    ORDER_GUEST();
    return value;
}
GAME_REPLACE_EXACT(00018000, cdecl, 3, u32, game_cross_selected_record_find_triplet_and_get_word10_or_zero)
{
    g_eax = guest_read32(0x0054E5E8u);
    ORDER_GUEST();
    g_edx = guest_read32(0x0054EEA4u + g_eax * 4u);
    ORDER_GUEST();
    save_word(g_ebx);
    save_word(g_esi);
    g_ecx = 0u;
    save_word(g_edi);
    if ((int32_t)g_edx > 0) {
        g_eax = guest_read32(0x0054E560u);
        ORDER_GUEST();
        g_eax = guest_read32(0x0054EEE8u + g_eax * 4u);
        ORDER_GUEST();
        g_esi = guest_read32(g_esp + 24u);
        ORDER_GUEST();
        g_edi = guest_read32(g_esp + 20u);
        ORDER_GUEST();
        g_ebx = guest_read32(g_esp + 16u);
        ORDER_GUEST();
        do {
            uint32_t word = guest_read32(g_eax + 4u);
            ORDER_GUEST();
            if (word == g_edi) {
                word = guest_read32(g_eax + 8u);
                ORDER_GUEST();
                if (word == g_esi) {
                    word = guest_read32(g_eax);
                    ORDER_GUEST();
                    if (word == g_ebx) {
                        g_eax = guest_read32(g_eax + 16u);
                        ORDER_GUEST();
                        g_edi = restore_word();
                        g_esi = restore_word();
                        g_ebx = restore_word();
                        return;
                    }
                }
            }
            g_ecx += 1u;
            g_eax += 28u;
        } while ((int32_t)g_ecx < (int32_t)g_edx);
    }
    g_edi = restore_word();
    g_esi = restore_word();
    g_eax = 0u;
    g_ebx = restore_word();
}
#undef ORDER_GUEST
