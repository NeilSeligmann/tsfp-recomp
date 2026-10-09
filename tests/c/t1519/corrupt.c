/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#ifndef BAD_FIELD
#define BAD_FIELD 0
#endif
__attribute__((constructor)) static void corrupt_record(void)
{
    size_t count;
    game_replacement *entries = (game_replacement *)game_replacement_table(&count);
    if (!count) return;
    /* Select an explicit record by metadata rather than unspecified link ordering. */
    for (size_t i = 0; i < count; ++i) {
        if (!entries[i].input_abi) continue;
        if (BAD_FIELD == 1) entries[i].convention = (game_convention)99;
        if (BAD_FIELD == 2) entries[i].input_abi = 0x15;
        if (BAD_FIELD == 3) entries[i].scratch_mask = GAME_SCRATCH_ECX;
        if (BAD_FIELD == 4) entries[i].stack_args = 9;
        if (BAD_FIELD == 5) entries[i].returns_value = 2;
        break;
    }
}
