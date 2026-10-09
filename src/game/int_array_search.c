/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
extern __thread uint32_t g_esi;
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")
GAME_REPLACE_EXACT(00070C90, cdecl, 2, u32, game_int_array_find_index_or_minus1)
{
    g_ecx = game_stack_arg(0u);
    ORDER_GUEST();
    guest_write32(g_esp - 4u, g_esi);
    ORDER_GUEST();
    g_esp -= 4u;
    ORDER_GUEST();
    if (g_ecx != 0u) {
        g_edx = guest_read32(g_ecx);
        ORDER_GUEST();
        g_eax = 0u;
        if ((int32_t)g_edx > 0) {
            g_esi = guest_read32(g_esp + 12u);
            ORDER_GUEST();
            g_ecx += 4u;
            for (;;) {
                uint32_t value = guest_read32(g_ecx);
                ORDER_GUEST();
                if (value == g_esi) goto found;
                g_eax += 1u;
                g_ecx += 4u;
                if ((int32_t)g_eax >= (int32_t)g_edx) break;
            }
        }
    }
    g_eax = 0xFFFFFFFFu;
found:
    g_esi = guest_read32(g_esp);
    ORDER_GUEST();
    g_esp += 4u;
    ORDER_GUEST();
}
#undef ORDER_GUEST
