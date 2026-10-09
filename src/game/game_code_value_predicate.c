/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

/* The raw 0x00064E70 body recognizes two code words and three signed values.
 * Keep this name descriptive of its inputs; no higher-level type is established. */
GAME_REPLACE_EXACT(00064E70, cdecl, 2, u32, game_code_value_predicate)
{
    const uint32_t code = guest_read32(g_esp + 4u);
    g_eax = code; /* The original writes EAX before either comparison or second load. */
    if (code != 0x0Bu && code != 0x0Cu) {
        g_eax = 0u;
        return;
    }

    const uint32_t value = guest_read32(g_esp + 8u);
    /* These unsigned tests have the same accepted set as the original signed
     * comparisons: only 12, 13, and 20 pass; all high-bit values fail as well. */
    if (value < 0x0Cu) {
        g_eax = 0u;
        return;
    }
    g_eax = (value <= 0x0Du || value == 0x14u) ? 1u : 0u;
}
