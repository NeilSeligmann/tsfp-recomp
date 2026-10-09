/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Test whether the signed rectangle described by four XMV coordinate pairs spans
 * within a supplied signed span after an early first-range check.
 */
#include "game_replace.h"

static int32_t game_xmv_signed_min(int32_t left, int32_t right)
{
    return left < right ? left : right;
}

static int32_t game_xmv_signed_max(int32_t left, int32_t right)
{
    return left > right ? left : right;
}

static void game_xmv_rect_span(void)
{
    const int32_t x0 = (int32_t)game_stack_arg(0);
    const int32_t y0 = (int32_t)game_stack_arg(1);
    const int32_t x1 = (int32_t)game_stack_arg(2);
    const int32_t y1 = (int32_t)game_stack_arg(3);
    const int32_t x2 = (int32_t)game_stack_arg(4);
    const int32_t y2 = (int32_t)game_stack_arg(5);
    const int32_t y3 = (int32_t)game_stack_arg(6);
    const int32_t x3 = (int32_t)game_stack_arg(7);
    const int32_t threshold = (int32_t)game_stack_arg(8);

    int32_t min_x = game_xmv_signed_min(game_xmv_signed_min(x0, x3),
                                        game_xmv_signed_min(x1, x2));
    int32_t max_x = game_xmv_signed_max(game_xmv_signed_max(x0, x3),
                                        game_xmv_signed_max(x1, x2));
    const int32_t width = (int32_t)((uint32_t)max_x - (uint32_t)min_x);
    if (width >= threshold) {
        g_eax = 0u;
        g_ecx = (uint32_t)min_x;
        g_edx = (uint32_t)width;
        return;
    }

    const int32_t min_y = game_xmv_signed_min(game_xmv_signed_min(y0, y1),
                                              game_xmv_signed_min(y2, y3));
    const int32_t max_y = game_xmv_signed_max(game_xmv_signed_max(y0, y1),
                                              game_xmv_signed_max(y2, y3));
    const int32_t min_all = game_xmv_signed_min(min_x, min_y);
    const int32_t max_all = game_xmv_signed_max(max_x, max_y);
    const int32_t total_span = (int32_t)((uint32_t)max_all - (uint32_t)min_all);
    const uint32_t spans = total_span < threshold ? 1u : 0u;
    g_eax = spans;
    g_ecx = spans;
    g_edx = (uint32_t)y3;
}

GAME_REPLACE_EXACT(0044BCC6, stdcall, 9, u32, game_xmv_rect_span)
{
    game_xmv_rect_span();
}
