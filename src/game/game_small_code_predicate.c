/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

/* 0x12E10 recognizes four small code values used by the retail callers. */
GAME_REPLACE_EXACT(00012E10, cdecl, 1, u32, game_small_code_predicate)
{
    const uint32_t value = game_stack_arg(0u);
    g_eax = (value == 0u || value == 1u || value == 0x15u || value == 0x7Cu) ? 1u : 0u;
}
