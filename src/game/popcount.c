/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

/* Count bits without signed arithmetic, platform intrinsics or table reads. */
static uint32_t game_popcount(uint32_t bits)
{
    uint32_t count = 0u;
    for (unsigned index = 0u; index < 32u; index++) {
        count += bits & 1u;
        bits >>= 1u;
    }
    return count;
}

/* The original leaves the low-half count in ECX as well as the full count
 * in EAX. Other integer registers are unchanged; EFLAGS is not reproduced. */
GAME_REPLACE_EXACT(00151090, cdecl, 1, u32, game_popcount)
{
    uint32_t bits = game_stack_arg(0);
    g_eax = game_popcount(bits);
    g_ecx = game_popcount(bits & 0xFFFFu);
}
