/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

/* Exact 0x3F94AA mapping for its two scalar arguments; semantic naming is unknown. */
GAME_REPLACE_EXACT(003F94AA, stdcall, 2, u32, game_nibble_pair_result)
{
    g_eax = game_stack_arg(0u);
    __asm__ volatile("" ::: "memory");
    g_ecx = 0u;
    __asm__ volatile("" ::: "memory");
    const uint32_t first = g_eax;
    const uint32_t second = game_stack_arg(1u);
    uint32_t result = 0u;

    if (second == 0u) {
        if (first == 1u) {
            result = 0x10u;
        } else if (first == 2u) {
            result = 0x20u;
        } else if (first == 15u) {
            result = 0x30u;
        }
    } else if (first == 0u) {
        result = 0x08u;
    } else if (first == 1u) {
        result = 0x18u;
    }

    if (result != 0u) {
        g_eax = 0u;
        __asm__ volatile("" ::: "memory");
        guest_write32(g_esp - 4u, result);
    }
    g_eax = result;
}
