/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
GAME_REPLACE_EXACT(001164A0, cdecl, 1, u32, game_global_73d96c_get_entry_stride_40)
{
    g_eax = game_stack_arg(0u);
    g_ecx = guest_read32(0x0073D96Cu);
    g_eax = g_ecx + g_eax * 40u;
}
GAME_REPLACE_EXACT(00153AF0, cdecl, 0, u32, game_global_7497b4_is_nonzero)
{
    g_ecx = guest_read32(0x007497B4u);
    g_eax = g_ecx != 0u;
}
