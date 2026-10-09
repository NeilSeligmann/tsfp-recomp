/* SPDX-License-Identifier: GPL-3.0-or-later
 * EAX-input CRT hexadecimal conversion; original does not validate ASCII. */
#include "game_replace.h"
extern __thread uint32_t g_esi;

GAME_REPLACE_EXACT(003CDA3C, cdecl, 0, u32, game_hextodec)
{
    const uint32_t locale = guest_read32(0x0054D7F8u);
    guest_write32(g_esp - 4u, g_esi);
    g_esi = g_eax;
    if ((int32_t)locale > 1) {
        const uint32_t args[2] = {g_esi, 4u};
        if (game_guest_call(0x003C9C4Eu, GAME_CC_cdecl, 0x003CDA50u,
                            4u, 0u, 0u, args, 2u) != GAME_GUEST_CALL_OK)
            __builtin_trap();
        g_ecx = guest_read32(g_esp - 8u);
    } else {
        g_eax = guest_read32(0x0054D7F0u);
        g_eax = guest_read8(g_eax + g_esi * 2u) & 4u;
    }
    if (g_eax == 0u)
        g_esi = (g_esi & 0xFFFFFFDFu) - 7u;
    g_eax = g_esi;
    g_esi = guest_read32(g_esp - 4u);
}
