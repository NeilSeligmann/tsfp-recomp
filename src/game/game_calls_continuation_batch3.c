/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1475 continuation, batch 3: call-bearing root whose single nested guest call keeps the
 * original frame and callee EAX/ECX leftovers, so the adapter is register-exact. */
#include "game_replace.h"

static void call_guest(uint32_t target, game_convention convention, uint32_t return_pc,
                       uint32_t caller_bytes, uint32_t this_pointer, const uint32_t *arguments,
                       unsigned count)
{
    if (game_guest_call(target, convention, return_pc, caller_bytes, this_pointer, 0u, arguments,
                        count) != GAME_GUEST_CALL_OK)
        __builtin_trap();
}

/* 00042640: cdecl(void). Unless the 0x800000 bit of the global flags [0x7DE458] is set: when
 * the state word [0x6B98A4] is 3 or 5, pass ([0x6B94C0] - [0x6B989C]) to cdecl 00069EA0 (stores
 * it in the current pool's +0xC word, return PC 0x4266C) and set the state word to 4. Always
 * sets the flag word [0x6B98B4] = 1. */
GAME_REPLACE_EXACT(00042640, cdecl, 0, void, game_state_6b98a4_enter_4_from_3_or_5_and_flag)
{
    if ((guest_read32(0x007DE458u) & 0x00800000u) == 0u) {
        g_eax = guest_read32(0x006B98A4u);
        if (g_eax == 3u || g_eax == 5u) {
            g_eax = guest_read32(0x006B94C0u);
            g_eax -= guest_read32(0x006B989Cu);
            const uint32_t arguments[1] = {g_eax};
            call_guest(0x00069EA0u, GAME_CC_cdecl, 0x0004266Cu, 0u, 0u, arguments, 1u);
            guest_write32(0x006B98A4u, 4u);
        }
    }
    guest_write32(0x006B98B4u, 1u);
}
