/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1602: hand C drafts for the 4 executed leaf roots of
 * docs/data/t1598-ui-hud-mapedit-lists/list-001.json (T1601 screen: hud 2, map editor 2).
 * Only 00042F90 passed the batch gate and is registered. All 4 drafts as proved are kept
 * verbatim in docs/data/t1602-ui-list001/proved-all-4-drafts.c.txt, the 3 rejected in
 * rejected-drafts.c.txt.
 * Drafted from the retail disassembly of the pinned XBE (sha256 3cfd001a...1816bc). The body is
 * register-exact and keeps the original read order. Names describe memory
 * effects only. Record: docs/t-ui-draft-list001.md. */
#include "game_replace.h"

/* 0x00042F90: EAX = 0 when dword [0x007DE458] has any bit of 0xE0000200, else (signed dword
 * [0x0079094C] > 0x64 gives 1), else (dword [0x007B0C48] == 0 gives 1), else EAX = 0 when
 * dword [that + 0x114] == 0x100, otherwise 1. */
GAME_REPLACE_EXACT(00042F90, cdecl, 0, u32, game_hud_prompt_allowed_by_flags_7de458_and_player_field_0x114)
{
    if ((guest_read32(0x007DE458u) & 0xE0000200u) != 0u) {
        g_eax = 0u;
        return;
    }
    if ((int32_t)guest_read32(0x0079094Cu) > 0x64) {
        g_eax = 1u;
        return;
    }
    g_eax = guest_read32(0x007B0C48u);
    if (g_eax == 0u) {
        g_eax = 1u;
        return;
    }
    if (guest_read32(g_eax + 0x114u) == 0x100u) {
        g_eax = 0u;
        return;
    }
    g_eax = 1u;
}
