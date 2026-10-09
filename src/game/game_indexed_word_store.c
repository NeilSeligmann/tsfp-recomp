/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

enum {
    INDEXED_WORD_BASE = 0x0072F2E0u,
};

#define GAME_ORDER_GUEST_EFFECTS() __asm__ volatile("" ::: "memory")

/* Exact scalar register and guest-memory behavior of the original word store. */
GAME_REPLACE_EXACT(0009E2D0, cdecl, 3, u32, game_indexed_word_store)
{
    g_ecx = game_stack_arg(0u);
    GAME_ORDER_GUEST_EFFECTS();
    g_eax = 0u;

    for (;;) {
        if (g_ecx == 1u) {
            break;
        }
        g_ecx = (g_ecx >> 1) | (g_ecx & 0x80000000u);
        g_eax += 1u;
        if ((int32_t)g_eax >= 32) {
            g_eax |= UINT32_MAX;
            break;
        }
    }
    GAME_ORDER_GUEST_EFFECTS();

    g_ecx = game_stack_arg(1u);
    GAME_ORDER_GUEST_EFFECTS();
    g_eax += g_eax * 2u;
    g_edx = g_ecx + g_eax * 2u;
    GAME_ORDER_GUEST_EFFECTS();
    g_eax = game_stack_arg(2u);
    GAME_ORDER_GUEST_EFFECTS();
    guest_write32(INDEXED_WORD_BASE + g_edx * 4u, g_eax);
}

#undef GAME_ORDER_GUEST_EFFECTS
