/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pseudo-random step (T77 range 0x002C0000-0x00470000). Only measured behaviour is named.
 */
#include "game_replace.h"

/* 0x003CAECC (cdecl, no arguments): linear congruential step of the global at 0x54DB00
 * (seed = seed * 0x343FD + 0x269EC3, the MSVC rand constants), returning bits 16..30. */
static void game_lcg_next(void)
{
    const uint32_t seed = guest_read32(0x54DB00u) * 0x343FDu + 0x269EC3u;
    guest_write32(0x54DB00u, seed);
    g_eax = (seed >> 16) & 0x7FFFu;
}

GAME_REPLACE_EXACT(003CAECC, cdecl, 0, u32, game_lcg_next)
{
    game_lcg_next();
}
