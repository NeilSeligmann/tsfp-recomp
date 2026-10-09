/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
/* Original 0x10E210: retain ECX argument and return its nonnull predicate. */
GAME_REPLACE_EXACT(0010E210, cdecl, 1, u32, game_is_nonnull)
{
    g_ecx = game_stack_arg(0u);
    g_eax = g_ecx != 0u;
}
/* Original 0x153BB0: wrapping LCG state update, then high-15-bit result. */
GAME_REPLACE_EXACT(00153BB0, cdecl, 0, u32, game_rand_next_15bit)
{
    g_eax = guest_read32(0x007497CCu) * 0x343FDu + 0x269EC3u;
    guest_write32(0x007497CCu, g_eax);
    g_eax = (g_eax >> 16u) & 0x7FFFu;
}
/* Original 0x163810: preserve loaded ECX; mode 4 returns 7, otherwise 8. */
GAME_REPLACE_EXACT(00163810, cdecl, 0, u32, game_global_79094c_ne_4_plus_7)
{
    g_ecx = guest_read32(0x0079094Cu);
    g_eax = 7u + (g_ecx != 4u);
}
