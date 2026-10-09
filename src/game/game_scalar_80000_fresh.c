/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
static void write_word(uint32_t address, uint16_t value)
{
    memcpy(game_host_ptr(address), &value, sizeof value);
}
GAME_REPLACE_EXACT(0008E970, cdecl, 1, u32, game_cutscene_prop_clear_track_slots_0x1b8_0x1ec_0x1f0_and_state_bytes_0x1f8_to_0x1fd)
{
    g_eax = game_stack_arg(0);
    g_ecx = 0u;
    g_edx = UINT32_MAX;
    guest_write32(g_eax + 0x1b8u, 0u);
    guest_write32(g_eax + 0x1ecu, 0u);
    guest_write32(g_eax + 0x1f0u, 0u);
    write_word(g_eax + 0x1f6u, 0u);
    guest_write8(g_eax + 0x1f8u, 0xffu);
    guest_write8(g_eax + 0x1f9u, 0xffu);
    guest_write8(g_eax + 0x1fau, 0u);
    guest_write8(g_eax + 0x87u, 0u);
    guest_write8(g_eax + 0x1fbu, 0u);
    guest_write8(g_eax + 0x1fcu, 0u);
    guest_write8(g_eax + 0x1fdu, 0u);
    write_word(g_eax + 0x7cu, 0xffffu);
}
