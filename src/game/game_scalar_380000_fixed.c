/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
/* Retail stores the reversed bytes back into the caller argument before reloading EAX. */
GAME_REPLACE_EXACT(003BD320, cdecl, 1, u32, game_swap32_stack_arg_returned_in_eax)
{
    const uint32_t value = game_stack_arg(0);
    const uint32_t reversed = ((value & 0x000000FFu) << 24) |
                              ((value & 0x0000FF00u) << 8) |
                              ((value & 0x00FF0000u) >> 8) | (value >> 24);
    guest_write32(g_esp + 4u, reversed);
    g_eax = reversed;
}
/* SUB wraps before unsigned DIV; do not replace it with a signed ceiling formula. */
GAME_REPLACE_EXACT(004223B3, stdcall, 1, u32, game_ceil_arg_minus_one_div_500)
{
    const uint32_t value = game_stack_arg(0) - 1u;
    g_ecx = 500u;
    g_edx = value % 500u;
    g_eax = value / 500u + (g_edx != 0u);
}
