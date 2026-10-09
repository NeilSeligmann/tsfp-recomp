/* SPDX-License-Identifier: GPL-3.0-or-later
 * The measured retail digest-state initializer.
 */
#include "game_replace.h"

#define DIGEST_STATE_WORD_COUNT 16u
#define DIGEST_STATE_TAG 0x2035444Du

static void game_digest_state_init(guest_addr state)
{
    /* Preserve the original store ordering: clear the two trailing fields, then
     * zero sixteen dwords from +4 before publishing the initialized state words. */
    guest_write32(state + 0x58u, 0u);
    guest_write32(state + 0x54u, 0u);
    for (uint32_t word = 0u; word < DIGEST_STATE_WORD_COUNT; word++) {
        guest_write32(state + 4u * word + 4u, 0u);
    }
    guest_write32(state + 0x44u, 0x67452301u);
    guest_write32(state + 0x48u, 0xEFCDAB89u);
    guest_write32(state + 0x4Cu, 0x98BADCFEu);
    guest_write32(state + 0x50u, 0x10325476u);
    guest_write32(state, DIGEST_STATE_TAG);
}

/* Original: stdcall, one stack argument, `ret 4`. The exact output registers are
 * EAX=1, ECX=0, EDX=the state pointer; EDI and the remaining GPRs are preserved.
 * The differential harness does not observe EFLAGS; all six current call sites kill
 * incoming arithmetic flags before reading them.
 */
GAME_REPLACE_EXACT(0044442C, stdcall, 1, void, game_digest_state_init)
{
    guest_addr state = game_stack_arg(0);
    game_digest_state_init(state);
    g_eax = 1u;
    g_ecx = 0u;
    g_edx = state;
}
