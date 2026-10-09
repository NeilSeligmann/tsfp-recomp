/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
GAME_REPLACE_EXACT(00190D50, cdecl, 0, u32, game_is_global_74f9e4_nonzero)
{
    g_ecx = guest_read32(0x0074F9E4u);
    g_eax = g_ecx != 0u;
}
GAME_REPLACE_EXACT(00243060, cdecl, 0, u32, game_mode_79094c_select_minus1_or_3066)
{
    g_ecx = guest_read32(0x0079094Cu);
    g_eax = g_ecx == 4u ? 0xFFFFFFFFu : 3066u;
}
