/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#include <stdlib.h>
extern __thread uint32_t g_esi;
GAME_REPLACE_EXACT(00056FF0, cdecl, 1, u32, game_prop_anim_matrix_slot_release_to_pool)
{
    guest_write32(g_esp - 4u, g_esi);
    g_esp -= 4u;
    g_esi = guest_read32(g_esp + 8u);
    g_eax = guest_read32(g_esi + 0x98u);
    if (g_eax != 0u) {
        g_ecx = guest_read32(g_esi);
        g_edx = guest_read32(g_ecx + 0x2cu);
        if (g_edx == guest_read32(g_eax + 0xf4u)) {
            const uint32_t args[2] = {0x6d77d0u, g_eax};
            if (game_guest_call(0x156e30u, GAME_CC_cdecl, 0x57017u, 0u, 0u, 0u, args, 2u) != GAME_GUEST_CALL_OK) abort();
            g_eax = guest_read32(g_esi + 0x98u);
            guest_write32(g_eax + 0xf0u, 0u);
        }
        guest_write32(g_esi + 0x98u, 0u);
    }
    g_esi = guest_read32(g_esp);
    g_esp += 4u;
}
