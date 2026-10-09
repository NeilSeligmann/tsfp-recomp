/* SPDX-License-Identifier: GPL-3.0-or-later
 * Original 0x0003E560/0x0003E5A0: append and release in the second ledger.
 * Releasing the newest row coalesces trailing tombstones with positive sizes.
 * Every address and accounting operation retains guest uint32_t wrapping.
 */
#include "game_replace.h"

#define SECOND_ROWS 0x006B7B18u
#define SECOND_BYTES 0x006B8368u
#define SECOND_BASE 0x006B8380u
#define SECOND_COUNT 0x006B838Cu

static int secondary_positive(uint32_t value)
{
    return value != 0u && (value & 0x80000000u) == 0u;
}

GAME_REPLACE_EXACT(0003E560, cdecl, 1, u32, game_append_secondary)
{
    g_ecx = guest_read32(SECOND_BYTES);
    g_eax = guest_read32(SECOND_BASE);
    g_edx = game_stack_arg(0);
    g_eax += g_ecx;
    g_ecx += g_edx;
    guest_write32(SECOND_BYTES, g_ecx);
    g_ecx = guest_read32(SECOND_COUNT);
    guest_write32(SECOND_ROWS + g_ecx * 8u, g_eax);
    guest_write32(SECOND_ROWS + g_ecx * 8u + 4u, g_edx);
    ++g_ecx;
    guest_write32(SECOND_COUNT, g_ecx);
}

GAME_REPLACE_EXACT(0003E5A0, cdecl, 1, u32, game_release_secondary)
{
    g_eax = guest_read32(SECOND_COUNT);
    g_edx = game_stack_arg(0);
    g_ecx = guest_read32(SECOND_ROWS + g_eax * 8u - 8u);
    --g_eax;
    if (g_edx == g_ecx) {
        g_edx = guest_read32(SECOND_BYTES);
        g_edx -= guest_read32(SECOND_ROWS + g_eax * 8u + 4u);
        guest_write32(SECOND_BYTES, g_edx);
        for (;;) {
            --g_eax;
            guest_write32(SECOND_ROWS + g_eax * 8u + 12u, UINT32_MAX);
            if ((g_eax & 0x80000000u) != 0u) break;
            g_ecx = guest_read32(SECOND_ROWS + g_eax * 8u);
            if (g_ecx != 0u) break;
            g_ecx = guest_read32(SECOND_ROWS + g_eax * 8u + 4u);
            if (!secondary_positive(g_ecx)) break;
            g_edx -= g_ecx;
        }
        ++g_eax;
        /* Unlike the first ledger, count is published before the final total. */
        guest_write32(SECOND_COUNT, g_eax);
        guest_write32(SECOND_BYTES, g_edx);
        g_eax = 1u;
        return;
    }
    g_ecx = 0u;
    if (secondary_positive(g_eax)) {
        do {
            if (guest_read32(SECOND_ROWS + g_ecx * 8u) == g_edx) {
                guest_write32(SECOND_ROWS + g_ecx * 8u, 0u);
                g_eax = 1u;
                return;
            }
            ++g_ecx;
        } while (g_ecx < g_eax);
    }
    g_eax = 0u;
}
