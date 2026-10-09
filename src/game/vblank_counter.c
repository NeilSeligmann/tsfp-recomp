/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#define VBLANK_COUNTER 0x00563918u
/* Executes only when invoked; callback delivery/timing is the caller's concern.
 * Original ignores its callback argument and preserves all integer registers.
 * Unsigned addition reproduces INC modulo32. EFLAGS is outside replacement ABI. */
GAME_REPLACE_EXACT(00022020, cdecl, 1, void, game_vblank_counter_increment)
{
    guest_write32(VBLANK_COUNTER,guest_read32(VBLANK_COUNTER)+1u);
}
GAME_REPLACE_EXACT(00022030, cdecl, 0, u32, game_vblank_counter_read)
{
    g_eax=guest_read32(VBLANK_COUNTER);
}
