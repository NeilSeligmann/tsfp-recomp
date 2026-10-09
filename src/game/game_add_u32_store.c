/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

extern __thread uint32_t g_ebp, g_ebx, g_esi, g_edi;

#define GAME_ORDER_GUEST_EFFECTS() __asm__ volatile("" ::: "memory")

/* Exact 0x003D0A5C add/carry result writer; title-level purpose is unknown. */
GAME_REPLACE_EXACT(003D0A5C, cdecl, 3, u32, game_add_u32_store_carry)
{
    const uint32_t entry_esp = g_esp;
    g_edx = guest_read32(entry_esp + 4u);
    GAME_ORDER_GUEST_EFFECTS();

    guest_write32(entry_esp - 4u, g_esi);
    g_esp = entry_esp - 4u;
    GAME_ORDER_GUEST_EFFECTS();
    g_esi = guest_read32(entry_esp + 8u);
    GAME_ORDER_GUEST_EFFECTS();

    g_ecx = g_edx + g_esi;
    g_eax = 0u;
    if (g_ecx < g_edx || g_ecx < g_esi) {
        g_eax = 1u;
    }
    GAME_ORDER_GUEST_EFFECTS();

    g_edx = guest_read32(entry_esp + 12u);
    GAME_ORDER_GUEST_EFFECTS();
    guest_write32(g_edx, g_ecx);
    GAME_ORDER_GUEST_EFFECTS();

    g_esi = guest_read32(g_esp);
    g_esp += 4u;
}

#undef GAME_ORDER_GUEST_EFFECTS
