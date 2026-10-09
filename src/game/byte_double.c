/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

/* Double the three upper bytes independently modulo 256. Keep the low byte;
 * discard each byte's carry rather than carrying into the adjacent byte. */
static uint32_t game_byte_double(uint32_t input)
{
    uint32_t result = input & 0xFFu;
    for (unsigned shift = 8u; shift < 32u; shift += 8u) {
        uint32_t byte = (input >> shift) & 0xFFu;
        result |= ((byte * 2u) & 0xFFu) << shift;
    }
    return result;
}

/* Original 0x0032D9B0 leaves the original low byte in ECX and the doubled
 * second byte, positioned at bits 8..15, in EDX. EFLAGS is not reproduced. */
GAME_REPLACE_EXACT(0032D9B0, cdecl, 1, u32, game_byte_double)
{
    uint32_t input = game_stack_arg(0);
    g_eax = game_byte_double(input);
    g_ecx = input & 0xFFu;
    g_edx = (((input >> 8u) & 0xFFu) * 2u & 0xFFu) << 8u;
}
