/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1535 scratch adapter for retail002D2BE0, no production registration.
 * Original contract/pins: docs/evidence/t1535/menu-index-plan.md.
 * cdecl2: first argument unused; second selects the 32-byte table record.
 */
#include "game_replace.h"

GAME_REPLACE_EXACT(002D2BE0, cdecl, 2, u32, game_menu_item_flag_mask_by_index_and_mode)
{
    g_ecx = (g_ecx & 0xFFFFFF00u) | guest_read8(0x007DE455u);
    __asm__ volatile("" ::: "memory");
    g_eax = 0u;
    if ((g_ecx & 0xFFu) != 0x0Au)
        g_eax = 0x00020000u;
    __asm__ volatile("" ::: "memory");
    g_ecx = guest_read32(g_esp + 8u);
    g_ecx <<= 5;
    __asm__ volatile("" ::: "memory");
    g_edx = guest_read32(g_ecx + 0x004D1F88u);
    if (g_edx == 0u)
        g_eax |= 0x02000000u;
    /* Exact wrapper owns plain RET: ESP advances4, both arguments remain. */
}
