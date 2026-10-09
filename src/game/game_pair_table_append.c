/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

/* Exact conditional pair write and counter update from 0x003C0E30. */
GAME_REPLACE_EXACT(003C0E30, cdecl, 2, u32, game_pair_table_append)
{
    g_edx = game_stack_arg(0u);
    if (g_edx == 0u) {
        return;
    }

    g_ecx = game_stack_arg(1u);
    if (g_ecx == 0u) {
        return;
    }

    const uint32_t index = guest_read32(0x007724C8u);
    g_eax = index;
    const guest_addr slot = 0x007723A0u + index * 8u;
    guest_write32(slot, g_edx);
    guest_write32(slot + 4u, g_ecx);
    g_eax = index + 1u;
    guest_write32(0x007724C8u, g_eax);
}
