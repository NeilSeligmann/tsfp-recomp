/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1538 scratch adapter, retail00139160; no production registration.
 * Original contract: docs/evidence/t1538/hud-slot-plan.md.
 */
#include "game_replace.h"

GAME_REPLACE_EXACT(00139160, cdecl, 0, u32, game_hud_local_player_slot_state_not_0xc_or_0x11_and_flag_0x40_clear)
{
    g_ecx = guest_read32(0x007B0C7Cu);
    __asm__ volatile("" ::: "memory");
    g_eax = guest_read32(g_ecx + 4u);
    if ((g_eax & 0x80000000u) != 0u) {
        g_eax = 0u;
        return;
    }
    __asm__ volatile("" ::: "memory");
    g_edx = (g_edx & 0xFFFFFF00u) | guest_read8(g_ecx + 0xCu);
    g_eax = g_eax * 0x177Cu + 0x007A6160u;
    if ((g_edx & 0x40u) != 0u) {
        g_eax = 0u;
        return;
    }
    __asm__ volatile("" ::: "memory");
    g_eax = guest_read32(g_eax);
    g_eax = (g_eax != 0xCu && g_eax != 0x11u) ? 1u : 0u;
    /* Exact wrapper owns plain RET: ESP advances4. Flags require caller audit. */
}
