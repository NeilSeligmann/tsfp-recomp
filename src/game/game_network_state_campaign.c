/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

GAME_REPLACE_EXACT(003B5FE0, stdcall, 3, u32, game_network_state_003b5fe0)
{
    g_eax = game_stack_arg(0u);
    g_eax = guest_read32(g_eax + 0x28u);
    g_ecx = (g_ecx & 0xFFFFFF00u) | (game_stack_arg(2u) & 0xFFu);
    g_eax = (g_eax & 0xFFFFFFu) | 0x80000000u;
    if ((g_ecx & 0xFFu) != 0u) g_eax |= 0x30000000u;
}

GAME_REPLACE_EXACT(000387D0, cdecl, 0, u32, game_network_state_000387d0)
{
    g_eax = guest_read32(0x68DDACu);
    if (g_eax != 0u) {
        g_eax = guest_read32(0x68DDB0u);
        if (g_eax != 0u) return;
    }
    g_eax = 0xFFFFFFFFu;
}
