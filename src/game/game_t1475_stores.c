/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
/* Preserve original load/store order, including caller-frame aliasing. */
GAME_REPLACE_EXACT(00071740, cdecl, 2, u32, game_object_set_field_0x24_and_flag_0x2c_bit0)
{
    g_eax = guest_read32(g_esp + 4u);
    guest_write32(g_eax + 0x2cu, guest_read32(g_eax + 0x2cu) | 1u);
    g_ecx = guest_read32(g_esp + 8u);
    guest_write32(g_eax + 0x24u, g_ecx);
}
GAME_REPLACE_EXACT(00070C30, cdecl, 2, u32, game_list_init_count_zero_mode_0xf4)
{
    g_eax = guest_read32(g_esp + 4u);
    g_ecx = guest_read32(g_esp + 8u);
    guest_write32(g_eax, 0u);
    guest_write32(g_eax + 0xf4u, g_ecx);
}
