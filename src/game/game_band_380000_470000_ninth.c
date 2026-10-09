/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1480 ninth batch: small leaf-calling roots in 0x380000-0x470000. */
#include "game_replace.h"

/* Index of the lowest set bit (BSF). A zero source leaves the destination as it was. */
static uint32_t lowest_set_bit_or(uint32_t value, uint32_t previous)
{
    return value != 0u ? (uint32_t)__builtin_ctz(value) : previous;
}

/* 003EE358: stdcall(a, b, out_low, out_high). Let la and lb be the lowest set bit
 * indexes of a and b (BSF, a zero keeps the previous register value: the incoming EAX
 * for a, la for b). m = min(la, lb) and edge = (1 << (2m & 31)) - 1. The two masks are
 * built from the 0x5555 and 0xAAAA patterns of edge or its complement, selected by
 * the order of la and lb, then both are cut to (1 << ((la + lb) & 31)) - 1 and stored
 * through out_low and out_high. EAX is the value stored high, ECX the out_high pointer
 * and EDX the high mask before the cut. */
GAME_REPLACE_EXACT(003EE358, stdcall, 4, u32, game_bit_index_pair_pattern_masks)
{
    const uint32_t lowest_a = lowest_set_bit_or(game_stack_arg(0u), g_eax);
    const uint32_t lowest_b = lowest_set_bit_or(game_stack_arg(1u), lowest_a);
    const uint32_t smaller = lowest_a < lowest_b ? lowest_a : lowest_b;
    const uint32_t edge = (1u << ((smaller + smaller) & 31u)) - 1u;
    const uint32_t complement = ~edge;
    const uint32_t low_pattern = lowest_a > lowest_b ? (complement | 0x55555555u) : (edge & 0x55555555u);
    const uint32_t high_pattern = lowest_a < lowest_b ? (complement | 0xAAAAAAAAu) : (edge & 0xAAAAAAAAu);
    const uint32_t cut = (1u << ((lowest_a + lowest_b) & 31u)) - 1u;

    guest_write32(game_stack_arg(2u), cut & low_pattern);
    g_ecx = game_stack_arg(3u);
    g_eax = cut & high_pattern;
    guest_write32(g_ecx, g_eax);
    g_edx = high_pattern;
}
