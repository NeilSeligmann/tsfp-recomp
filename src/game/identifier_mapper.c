/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

/* Map the supported selectors to one of two identifiers. Nonzero mode is
 * Boolean, including negative-looking unsigned inputs. Unknown selectors have
 * no identifier. The original caller uses both modes. */
static uint32_t game_identifier_mapper(uint32_t selector, uint32_t mode)
{
    if (selector == 54u || selector == 55u) {
        return mode == 0u ? 3427u : 3426u;
    }
    if (selector == 230u && mode != 0u) {
        return 1640u;
    }
    return UINT32_MAX;
}

/* No integer register except EAX changes; the wrapper owns the cdecl pop.
 * This boundary makes no claim about EFLAGS or startup reachability. */
GAME_REPLACE_EXACT(000B3D60, cdecl, 2, u32, game_identifier_mapper)
{
    g_eax = game_identifier_mapper(game_stack_arg(0), game_stack_arg(1));
}
