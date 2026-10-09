/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
extern __thread uint32_t g_esi, g_edi;
GAME_REPLACE_EXACT(000366B0, cdecl, 0, u32, game_online_search_entry_pair_store_and_flag)
{
    g_eax = guest_read32(0x68c5e8u);
    g_ecx = guest_read32(0x68c5ecu);
    guest_write32(0x68bd18u, 1u);
    guest_write32(0x68bd10u, g_eax);
    guest_write32(0x68bd14u, g_ecx);
}
GAME_REPLACE_EXACT(00024EB0, cdecl, 1, u32, game_memory_unit_slot_addr_0x10)
{
    g_eax = game_stack_arg(0u) * 48u + 0x5655c0u;
}
GAME_REPLACE_EXACT(00056E90, cdecl, 0, u32, game_state_6d60d8_is_one_of_1_3_7_11_12_13)
{
    g_eax = guest_read32(0x6d60d8u);
    g_eax = g_eax == 1u || g_eax == 3u || g_eax == 7u || g_eax == 11u || g_eax == 12u || g_eax == 13u;
}
GAME_REPLACE_EXACT(00056F70, cdecl, 0, u32, game_state_6d60d8_is_one_of_1_3_7_8_9_11_12_13_when_6d60a8_differs_from)
{
    g_eax = guest_read32(0x6d60a8u);
    if (g_eax == guest_read32(0x7e3f6cu)) { g_eax = 1u; return; }
    g_eax = guest_read32(0x6d60d8u);
    g_eax = g_eax == 1u || g_eax == 3u || g_eax == 7u || g_eax == 11u || g_eax == 12u || g_eax == 13u || g_eax == 8u || g_eax == 9u;
}
