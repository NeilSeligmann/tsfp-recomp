/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1479 admitted actor body; original and native evidence in docs/evidence/t1479.
 */
#include "game_replace.h"

static void actor_call(uint32_t argument, uint32_t return_pc)
{
    if (game_guest_call(0x77470u, GAME_CC_cdecl, return_pc, 0u,
                        0u, 0u, &argument, 1u) != GAME_GUEST_CALL_OK)
        __builtin_trap();
}

void t1479_candidate_3158c0(void)
{
    g_eax = guest_read32(0x7BA9E0u) * 0x1E0u;
    g_ecx = guest_read32(g_eax + 0x7BA26Cu);
    actor_call(g_ecx, 0x3158D7u);
    g_ecx = guest_read32(0x783B94u);
    if (g_ecx != 0u) {
        g_eax = g_eax != 0u && guest_read32(g_eax + 0x1B0u) == 4u
            ? 2u : 0xFFFFFFFEu;
        return;
    }
    g_edx = guest_read32(0x7BA9E0u) * 0x1E0u;
    g_eax = guest_read32(g_edx + 0x7BA26Cu);
    actor_call(g_eax, 0x315919u);
    g_ecx = (g_ecx & 0xFFFFFF00u) | guest_read8(g_eax + 0xC4u);
    if ((g_ecx & 0x10u) != 0u) {
        g_eax = 0u;
        return;
    }
    g_ecx = guest_read32(0x7BA9E0u) * 0x1E0u;
    g_edx = guest_read32(g_ecx + 0x7BA26Cu);
    actor_call(g_edx, 0x315942u);
    const uint32_t byte = guest_read8(g_eax + 0xC4u);
    g_eax = (byte & 0x20u) != 0u ? 1u : 0xFFFFFFFFu;
}
