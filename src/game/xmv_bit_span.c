/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Extract a bit span from the XMV decoder's current word and refill cache.
 */
#include "game_replace.h"

static void game_xmv_read_bit_span(void)
{
    const guest_addr state = guest_read32(g_esp + 4u);
    const uint32_t requested = guest_read32(g_esp + 8u);
    const uint32_t remaining = guest_read32(state + 4u);
    const uint32_t word = guest_read32(state);

    if (requested <= remaining) {
        const uint32_t next_remaining = remaining - requested;
        const uint32_t mask = UINT32_MAX >> ((32u - requested) & 31u);
        guest_write32(state + 4u, next_remaining);
        g_edx = word >> (next_remaining & 31u);
        g_eax = g_edx & mask;
        g_ecx = next_remaining;
        return;
    }

    const uint32_t excess = requested - remaining;
    const uint32_t old_mask = UINT32_MAX >> ((32u - remaining) & 31u);
    const uint32_t old_part = (word & old_mask) * (remaining != 0u);
    guest_addr cursor = guest_read32(state + 8u);
    const uint32_t next_word = guest_read32(cursor);
    cursor += 4u;
    guest_write32(state + 8u, cursor);
    guest_write32(state, next_word);

    const uint32_t next_remaining = 32u - excess;
    guest_write32(state + 4u, next_remaining);
    g_eax = (next_word >> (next_remaining & 31u)) | (old_part << (excess & 31u));
    g_ecx = excess;
    g_edx = state;
}

GAME_REPLACE_EXACT(00448F26, stdcall, 2, u32, game_xmv_read_bit_span)
{
    game_xmv_read_bit_span();
}
