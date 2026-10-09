/* SPDX-License-Identifier: GPL-3.0-or-later
 * Exact byte-field setter used by the Xbox input object wrappers.
 */
#include "game_replace.h"
#include "game_guest.h"

#include <stdint.h>

static void game_input_set_byte7(void)
{
    const uint32_t eax_before = g_eax;
    const uint8_t value = guest_read8(g_esp + 4u);

    guest_write8(g_ecx + 7u, value);
    g_eax = (eax_before & 0xFFFFFF00u) | value;
}

GAME_REPLACE_EXACT(0046FF7B, thiscall, 1, void, game_input_set_byte7)
{
    game_input_set_byte7();
}
