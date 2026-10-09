/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

#define GAME_ORDER_GUEST_EFFECTS() __asm__ volatile("" ::: "memory")

/* Exact read-only scalar lookup at 0x00059DB0; title-level purpose is unknown. */
GAME_REPLACE_EXACT(00059DB0, cdecl, 2, u32, game_scalar_object_lookup)
{
    const uint32_t entry_esp = g_esp;

    g_eax = guest_read32(entry_esp + 4u);
    GAME_ORDER_GUEST_EFFECTS();
    g_eax = guest_read32(g_eax + 4u);
    GAME_ORDER_GUEST_EFFECTS();
    g_ecx = guest_read32(g_eax + 0xD4u);
    GAME_ORDER_GUEST_EFFECTS();
    const uint32_t compare_value = guest_read32(entry_esp + 8u);
    GAME_ORDER_GUEST_EFFECTS();

    if (g_ecx == compare_value) {
        g_eax = guest_read32(g_eax + 0x124u);
    } else {
        g_eax = 0u;
    }
    GAME_ORDER_GUEST_EFFECTS();

}

#undef GAME_ORDER_GUEST_EFFECTS
