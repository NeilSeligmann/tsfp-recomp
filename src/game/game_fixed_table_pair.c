/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

enum {
    FIXED_TABLE_FIRST = 0x0054F168u,
    FIXED_TABLE_END = 0x0054F968u,
    FIXED_TABLE_STRIDE = 8u,
    FIXED_TABLE_RESULT_BIAS = 0x100u,
};

/* Preserve the original order of guest register and memory effects across accesses. */
#define GAME_ORDER_GUEST_EFFECTS() __asm__ volatile("" ::: "memory")

/* Exact 0x19260 register and ordered-memory behavior; table purpose is unnamed. */
GAME_REPLACE_EXACT(00019260, cdecl, 1, u32, game_fixed_table_pair_update)
{
    g_ecx = 0u;
    GAME_ORDER_GUEST_EFFECTS();
    g_eax = FIXED_TABLE_FIRST;
    GAME_ORDER_GUEST_EFFECTS();

    for (;;) {
        g_edx = guest_read32(g_eax + 4u);
        GAME_ORDER_GUEST_EFFECTS();
        if (g_edx == 0u) {
            break;
        }

        g_eax += FIXED_TABLE_STRIDE;
        GAME_ORDER_GUEST_EFFECTS();
        g_ecx += 1u;
        GAME_ORDER_GUEST_EFFECTS();
        if ((int32_t)g_eax >= (int32_t)FIXED_TABLE_END) {
            g_eax = 0u;
            return;
        }
    }

    g_edx = game_stack_arg(0u);
    GAME_ORDER_GUEST_EFFECTS();
    guest_write32(g_eax + 4u, g_edx);
    GAME_ORDER_GUEST_EFFECTS();
    g_edx = guest_read32(g_eax);
    GAME_ORDER_GUEST_EFFECTS();
    g_ecx += FIXED_TABLE_RESULT_BIAS;
    GAME_ORDER_GUEST_EFFECTS();
    g_edx += g_ecx;
    GAME_ORDER_GUEST_EFFECTS();
    guest_write32(g_eax, g_edx);
    GAME_ORDER_GUEST_EFFECTS();
    g_eax = g_edx;
}

#undef GAME_ORDER_GUEST_EFFECTS
