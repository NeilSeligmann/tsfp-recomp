/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

#define TRANSFORM_INPUT 0x007497B0u
#define TRIANGLE_OUTPUT 0x007BA034u
#define BIT_OUTPUT 0x007BA17Cu

/* Fold a seven-bit phase around its midpoint, then bias it by 64. The second
 * output encodes input bit 5 as 64 or 128. Both outputs are bytes. */
static void game_byte_transform(uint8_t input)
{
    uint8_t phase = input & 127u;
    uint8_t folded = phase < 64u ? phase : (uint8_t)(128u - phase);
    guest_write8(TRIANGLE_OUTPUT, (uint8_t)(64u + folded));
    guest_write8(BIT_OUTPUT, (input & 32u) != 0u ? 128u : 64u);
}

/* Original 0x00081480..0x000814AD changes AL and CL, and changes DL only
 * in the upper-half branch. Preserve the other bits and the untouched DL.
 * This boundary reproduces integer registers and writes, not EFLAGS. */
GAME_REPLACE_EXACT(00081480, cdecl, 0, void, game_byte_transform)
{
    uint8_t input = guest_read8(TRANSFORM_INPUT);
    uint8_t phase = input & 127u;
    if (phase >= 64u) {
        g_edx = (g_edx & 0xFFFFFF00u) | (128u - phase);
    }
    game_byte_transform(input);
    g_ecx = (g_ecx & 0xFFFFFF00u) | guest_read8(TRIANGLE_OUTPUT);
    g_eax = (g_eax & 0xFFFFFF00u) | guest_read8(BIT_OUTPUT);
}
