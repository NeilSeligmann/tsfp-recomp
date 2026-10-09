/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Refill the XMV bit cache when a requested bit span crosses its current word.
 */
#include "game_replace.h"

static void game_xmv_ensure_bits(void)
{
    const guest_addr state = g_ecx;
    uint32_t requested = g_edx;
    uint32_t remaining = guest_read32(state + 4u);
    uint32_t eax = remaining;

    if (requested > remaining) {
        const uint32_t excess = requested - remaining;
        guest_addr cursor = guest_read32(state + 8u);
        cursor += (excess >> 5) << 2;
        const uint32_t word = guest_read32(cursor);
        cursor += 4u;
        guest_write32(state + 8u, cursor);
        guest_write32(state, word);
        requested = excess & 31u;
        g_edx = requested;
        eax = 32u;
    }

    eax -= requested;
    guest_write32(state + 4u, eax);
    g_eax = eax;
}

GAME_REPLACE_EXACT(00448F8E, fastcall, 0, u32, game_xmv_ensure_bits)
{
    game_xmv_ensure_bits();
}
