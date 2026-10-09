/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Signed median of three low-byte XMV coordinates.
 */
#include "game_replace.h"

#include <stdint.h>

static int8_t game_xmv_signed_median_value(int8_t a, int8_t b, int8_t c)
{
    if (a <= b) {
        if (a > c) return a;
        return b > c ? c : b;
    }
    if (b > c) return b;
    return a <= c ? a : c;
}

static void game_xmv_signed_median3(void)
{
    const uint32_t eax_before = g_eax;
    const uint32_t ecx_before = g_ecx;
    const int8_t a = (int8_t)game_stack_arg(0);
    const int8_t b = (int8_t)game_stack_arg(1);
    const int8_t c = (int8_t)game_stack_arg(2);

    g_eax = (eax_before & 0xFFFFFF00u) |
            (uint8_t)game_xmv_signed_median_value(a, b, c);
    g_ecx = (ecx_before & 0xFFFFFF00u) | (uint8_t)b;
}

GAME_REPLACE_EXACT(0044738C, stdcall, 3, u32, game_xmv_signed_median3)
{
    game_xmv_signed_median3();
}
