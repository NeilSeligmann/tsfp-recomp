/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Source-backed integer leaf at 0x004417F5. No enclosing object type or field
 * meaning is inferred; the name only describes the measured selection.
 */
#include "game_replace.h"

#include <stdint.h>

static void game_u32_sum_or_field(void)
{
    const uint32_t object = game_stack_arg(0u);
    const uint32_t total = guest_read32(object + 0xB0u) + guest_read32(object + 0xACu);
    const uint32_t kind = guest_read8(object + 0x7Cu);

    g_ecx = total;
    if (kind < 4u) {
        g_eax = guest_read32(object + 0xA0u);
        return;
    }

    const uint32_t base = guest_read32(object + 0xA0u);
    g_edx = base - total;
    g_eax = ((g_edx & 0x80000000u) != 0u || g_edx == 0u) ? base : total;
}

/* The measured bytes end in RET 4. The generated annotation calls the function
 * cdecl, but the raw return instruction and both callers establish callee cleanup. */
GAME_REPLACE_EXACT(004417F5, stdcall, 1, u32, game_u32_sum_or_field)
{
    game_u32_sum_or_field();
}
