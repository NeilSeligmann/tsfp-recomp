/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
game_guest_function recomp_lookup(uint32_t va)
{
    size_t count;
    const game_replacement *table = game_replacement_table(&count);
    for (size_t index = 0; index < count; ++index)
        if (table[index].va == va) return table[index].adapter;
    return NULL;
}
