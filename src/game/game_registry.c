/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Access to the `game_replacements` linker section. See game_replace.h.
 */
#include "game_replace.h"

/* The linker defines these for a section whose name is a valid C identifier. They are
 * weak so that a binary with no replacement linked in reports an empty registry instead
 * of failing to link. */
extern game_replacement __start_game_replacements[] __attribute__((weak));
extern game_replacement __stop_game_replacements[] __attribute__((weak));

const game_replacement *game_replacement_table(size_t *count)
{
    if (__start_game_replacements == NULL || __stop_game_replacements == NULL) {
        *count = 0;
        return NULL;
    }
    *count = (size_t)(__stop_game_replacements - __start_game_replacements);
    return __start_game_replacements;
}

unsigned game_replacement_scratch(const game_replacement *entry)
{
    return entry->scratch_mask;
}
