/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1480 CRT case conversion and bounded single-byte space classification. */
#include "game_replace.h"
extern __thread uint32_t g_esi;

GAME_REPLACE_EXACT(003CD157, cdecl, 1, u32, game_ismbcspace)
{
    g_eax = game_stack_arg(0u);
    if (g_eax > 255u) {
        g_eax = 0u;
        return;
    }
    if ((int32_t)guest_read32(0x0054D7F8u) > 1) {
        const uint32_t args[2] = {g_eax, 8u};
        if (game_guest_call(0x003C9C4Eu, GAME_CC_cdecl, 0x003CD176u,
                            0u, 0u, 0u, args, 2u) != GAME_GUEST_CALL_OK)
            __builtin_trap();
        g_ecx = guest_read32(g_esp - 4u);
    } else {
        g_ecx = guest_read32(0x0054D7F0u);
        g_eax = guest_read8(g_ecx + g_eax * 2u) & 8u;
    }
}

static void convert_case(uint32_t mask, uint32_t return_pc, uint32_t delta)
{
    const uint32_t locale = guest_read32(0x0054D7F8u);
    guest_write32(g_esp - 4u, g_esi);
    g_esi = game_stack_arg(0u);
    if ((int32_t)locale > 1) {
        const uint32_t args[2] = {g_esi, mask};
        if (game_guest_call(0x003C9C4Eu, GAME_CC_cdecl, return_pc,
                            4u, 0u, 0u, args, 2u) != GAME_GUEST_CALL_OK)
            __builtin_trap();
        g_ecx = guest_read32(g_esp - 8u);
    } else {
        g_eax = guest_read32(0x0054D7F0u);
        g_eax = guest_read8(g_eax + g_esi * 2u) & mask;
    }
    g_eax = g_eax != 0u ? g_esi + delta : g_esi;
    g_esi = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(003CAD32, cdecl, 1, u32, game_toupper)
{
    convert_case(2u, 0x003CAD48u, 0xFFFFFFE0u);
}

GAME_REPLACE_EXACT(003CF214, cdecl, 1, u32, game_tolower)
{
    convert_case(1u, 0x003CF22Au, 0x20u);
}
