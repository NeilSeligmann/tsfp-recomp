/* SPDX-License-Identifier: GPL-3.0-or-later
 * Unregistered T1479 draft; native-profile-predecl.md is the contract.
 */
#include "game_replace.h"
extern __thread uint32_t g_esi;

void t1479_candidate_2fe520(void)
{
    guest_write32(g_esp - 4u, g_esi);
    g_esi = game_stack_arg(0u);
    const uint32_t first[2] = {0u, g_esi};
    if (game_guest_call(0x2FE480u, GAME_CC_cdecl, 0x2FE52Du, 4u,
                        0u, 0u, first, 2u) != GAME_GUEST_CALL_OK)
        __builtin_trap();
    if (g_eax != 0u) {
        g_eax = 1u;
        g_esi = guest_read32(g_esp - 4u);
        return;
    }
    const uint32_t second[1] = {g_esi};
    if (game_guest_call(0x2FE4E0u, GAME_CC_cdecl, 0x2FE53Au, 4u,
                        0u, 0u, second, 1u) != GAME_GUEST_CALL_OK)
        __builtin_trap();
    if (g_eax < 0x80000000u) {
        g_eax = 1u;
    } else {
        g_eax = guest_read32(0x7844A8u);
        if (g_esi == guest_read32(g_eax + 0x2F38u) &&
            guest_read32(g_eax + 0x2F34u) == 0x46u)
            g_eax = 1u;
        else
            g_eax = 0u;
    }
    g_esi = guest_read32(g_esp - 4u);
}
