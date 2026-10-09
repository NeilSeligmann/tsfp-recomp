/* SPDX-License-Identifier: GPL-3.0-or-later
 * Raw absolute dword read at 0x0047005F.
 */
#include "game_replace.h"

GAME_REPLACE_EXACT(0047005F, cdecl, 0, u32, game_global_word_77144c)
{
    g_eax = guest_read32(0x0077144Cu);
}
