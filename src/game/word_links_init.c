/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

/* Exact six-store leaf at 0x0040C0FE. The neutral name avoids assigning a class or
 * semantic structure type to this one observed call. */
GAME_REPLACE_EXACT(0040C0FE, thiscall, 0, void, game_word_links_init)
{
    const guest_addr self = g_ecx;

    g_eax = self;
    guest_write32(self + 0x08u, self + 0x04u);
    guest_write32(self + 0x04u, self + 0x04u);
    guest_write32(self + 0x00u, 0x004A1D84u);
    guest_write32(self + 0x1Cu, 0x00412494u);
    guest_write32(self + 0x10u, self + 0x0Cu);
    guest_write32(self + 0x0Cu, self + 0x0Cu);
    g_ecx = self + 0x0Cu;
}
