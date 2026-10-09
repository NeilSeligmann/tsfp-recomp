/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

/* Seven ordered dword stores observed in the call-free leaf at 0x003C6B30.
 * The neutral name describes the measured operation without assigning a type. */
GAME_REPLACE_EXACT(003C6B30, cdecl, 1, u32, game_word_state_init)
{
    const guest_addr state = game_stack_arg(0u);

    g_eax = state;
    guest_write32(state + 0x00u, 0u);
    guest_write32(state + 0x04u, 0u);
    guest_write32(state + 0x08u, 0x67452301u);
    guest_write32(state + 0x0Cu, 0xEFCDAB89u);
    guest_write32(state + 0x10u, 0x98BADCFEu);
    guest_write32(state + 0x14u, 0x10325476u);
    guest_write32(state + 0x18u, 0xC3D2E1F0u);
}
