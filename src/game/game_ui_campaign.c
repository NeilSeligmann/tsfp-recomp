/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1791: original-body-exact qualified UI leaves. All other drafts are
 * archived, including quarantined/sampling-insensitive candidates.
 * Proof metrics refer to the frozen20-source campaign; snapshot unchanged.
 * Contracts and receipts: docs/t-prove-campaign-ui.md. */
#include "game_replace.h"

GAME_REPLACE_EXACT(00032F10, cdecl, 2, u32, game_ui_campaign_status_689178_write_result)
{
    g_eax = guest_read32(0x689178u);
    if (g_eax == 0xFFFFFFFFu) {
        g_ecx = game_stack_arg(1u);
        g_edx = guest_read32(0x6891ACu);
        guest_write32(g_ecx, g_edx);
        return;
    }
    if (g_eax == 2u) {
        g_ecx = game_stack_arg(0u);
        g_edx = guest_read32(0x689160u);
        guest_write32(g_ecx, g_edx);
    }
}

GAME_REPLACE_EXACT(00147200, cdecl, 4, u32, game_ui_campaign_append_notice_record1c)
{
    g_ecx = game_stack_arg(0u);
    g_eax = guest_read32(g_ecx + 0xE0u);
    g_edx = game_stack_arg(1u);
    g_eax = g_eax * 0x1Cu + g_ecx;
    guest_write32(g_eax, g_edx);
    g_edx = game_stack_arg(3u);
    guest_write32(g_eax + 4u, g_edx);
    g_edx = game_stack_arg(2u);
    guest_write32(g_eax + 8u, 0u);
    g_eax = guest_read32(g_ecx + 0xE0u);
    g_eax = g_eax * 0x1Cu + g_ecx;
    guest_write32(g_eax + 0xCu, g_edx);
    guest_write32(g_eax + 0x18u, 0xFFFFFFFFu);
    guest_write32(g_ecx + 0xE0u, guest_read32(g_ecx + 0xE0u) + 1u);
}
