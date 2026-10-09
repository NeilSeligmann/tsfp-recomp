/* SPDX-License-Identifier: GPL-3.0-or-later
 * Source-backed 12-byte initializer at 0x00410D30.
 */
#include "game_replace.h"

GAME_REPLACE_EXACT(00410D30, thiscall, 0, u32, game_byte_range_init)
{
    const guest_addr base = g_ecx;

    /* Preserve the original register and write order for a faulting destination. */
    g_eax = base;
    g_ecx = 0u;
    guest_write32(base + 4u, 0u);
    guest_write32(base, 0u);
    guest_write32(base + 8u, 0u);
}
