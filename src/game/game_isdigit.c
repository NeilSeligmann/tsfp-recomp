/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

/* 0x003C88AC (_isdigit) uses the fast byte table while the CRT locale state is
 * at or below one. Once the locale is active, it makes a guest cdecl call to
 * 0x003C9C4E (__isctype), which is a simple leaf. The nested call keeps the
 * original return address and stack arguments visible to that guest body. */
GAME_REPLACE_EXACT(003C88AC, cdecl, 1, u32, game_isdigit)
{
    const uint32_t value = game_stack_arg(0u);
    if ((int32_t)guest_read32(0x0054D7F8u) > 1) {
        const uint32_t args[2] = {value, 4u};
        const game_guest_call_status status = game_guest_call(
            0x003C9C4Eu, GAME_CC_cdecl, 0x003C88C0u, 0u, 0u, 0u, args, 2u);
        if (status != GAME_GUEST_CALL_OK) {
            /* Do not turn an unresolved callee or stack mismatch into a plausible
             * classifier result. */
            __builtin_trap();
        }
        /* The original caller uses two `pop ecx` instructions for its cdecl
         * cleanup, so the second stack argument remains in ECX on return. */
        g_ecx = guest_read32(g_esp - 4u);
        return;
    }

    g_eax = value;
    g_ecx = guest_read32(0x0054D7F0u);
    g_eax = guest_read8(g_ecx + value * 2u) & 4u;
}
