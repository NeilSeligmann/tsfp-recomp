/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One-bit XMV decoder reader used throughout the measured movie decode path.
 */
#include "game_replace.h"

static void game_xmv_read_bit(void)
{
    const guest_addr state = g_ecx;
    uint32_t remaining = guest_read32(state + 4u);
    if (remaining != 0u) {
        remaining--;
        guest_write32(state + 4u, remaining);
        const uint32_t word = guest_read32(state);
        g_ecx = remaining;
        g_eax = (word >> (remaining & 31u)) & 1u;
        return;
    }

    guest_addr cursor = guest_read32(state + 8u);
    const uint32_t word = guest_read32(cursor);
    cursor += 4u;
    guest_write32(state, word);
    guest_write32(state + 8u, cursor);
    guest_write32(state + 4u, 31u);
    g_ecx = cursor;
    g_edx = word;
    g_eax = word >> 31;
}

/* The original starts with CMP and therefore kills all incoming arithmetic flags. */
GAME_REPLACE_EXACT(00448ED9, fastcall, 0, u32, game_xmv_read_bit)
{
    game_xmv_read_bit();
}
