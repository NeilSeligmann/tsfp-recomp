/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1752: ordered retail stores; MD5 count is bytes, not the RFC two-word bit count. */
#include "game_replace.h"
GAME_REPLACE_EXACT(003BC270, cdecl, 1, u32, game_fesl_md5_context_init_count_zero_and_state_67452301_efcdab89_98badcfe_10325476)
{
    g_eax = game_stack_arg(0u);
    guest_write32(g_eax, 0u);
    guest_write32(g_eax + 4u, 0x67452301u);
    guest_write32(g_eax + 8u, 0xefcdab89u);
    guest_write32(g_eax + 12u, 0x98badcfeu);
    guest_write32(g_eax + 16u, 0x10325476u);
}
