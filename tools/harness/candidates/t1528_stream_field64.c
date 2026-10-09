/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1528 scratch candidate, retail00029E40: ECX input, one stack argument, RET4.
 * Pre-outcome contract: docs/evidence/t1528/stream-field64-plan.md.
 * Outside src/game: no production registration follows from this file alone.
 */
#include "game_replace.h"

GAME_REPLACE_EXACT_INPUTS(00029E40, stdcall, 1, u32, ecx, game_sound_stream_set_field_0x64)
{
    g_eax = guest_read32(g_esp + 4u);
    __asm__ volatile("" ::: "memory");
    guest_write32(g_ecx + 0x64u, g_eax);
    /* The exact wrapper owns RET4: return-address plus one argument, eight bytes. */
}
