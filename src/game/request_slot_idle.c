/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

#define ORDER_GUEST() __asm__ volatile("" ::: "memory")

/* Original 0x294b0 checks only the masked index and request busy word.
 * A true result says neither generation validity nor successful I/O. */
GAME_REPLACE_EXACT(000294B0, cdecl, 1, u32, game_request_slot_idle_or_invalid)
{
    g_eax = game_stack_arg(0u);
    ORDER_GUEST();
    g_eax &= 0xFFFFu;
    if ((int32_t)g_eax < 0 || (int32_t)g_eax >= 200) {
        g_eax = 1u;
        return;
    }
    g_eax *= 68u;
    g_eax += 0x00661370u;
    ORDER_GUEST();
    if (g_eax == 0u) {
        g_eax = 1u;
        return;
    }
    g_edx = guest_read32(g_eax + 0x2Cu);
    ORDER_GUEST();
    g_ecx = g_edx == 0u ? 1u : 0u;
    g_eax = g_ecx;
}

#undef ORDER_GUEST
