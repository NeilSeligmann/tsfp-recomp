/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1528 scratch candidate, retail 00286F10: ECX value input, no stack arguments, plain RET.
 * Pre-outcome contract: docs/evidence/t1528/ecx-table-index-plan.md.
 * Outside src/game: no production registration follows from this file alone.
 */
#include "game_replace.h"

GAME_REPLACE_EXACT_INPUTS(00286F10, cdecl, 0, u32, ecx, game_table5208b8_index_of_value)
{
    g_eax = 0u;
    do {
        __asm__ volatile("" ::: "memory");
        if (guest_read32(0x005208B8u + g_eax * 4u) == g_ecx) {
            return;
        }
        g_eax++;
    } while ((int32_t)g_eax < 8);
    g_eax = 0u;
}
