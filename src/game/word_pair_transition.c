/* SPDX-License-Identifier: GPL-3.0-or-later
 * Measured conditional two-word transition at 0x0042E716.
 */
#include "game_replace.h"

GAME_REPLACE_EXACT(0042E716, stdcall, 1, u32, game_word_pair_transition)
{
    const guest_addr base = game_stack_arg(0u);

    /* MOV EAX,[ESP+4] precedes the first possibly faulting object access. */
    g_eax = base;
    if (guest_read32(base + 0x20u) != 0u) {
        guest_write32(base + 0x20u, 0u);
        guest_write32(base + 0x24u, guest_read32(base + 0x24u) + 1u);
    }
}
