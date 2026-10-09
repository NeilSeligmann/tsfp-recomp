/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#include <assert.h>
#include <stdio.h>
/* Previous layout, independently declared to catch offset/size changes. */
struct old_record {
    uint32_t va;
    const char *name;
    game_convention convention;
    uint8_t stack_args, returns_value, scratch_mask;
    const char *source;
    void (*adapter)(void);
    uint64_t calls;
};
_Static_assert(sizeof(game_replacement) == sizeof(struct old_record), "record stride changed");
_Static_assert(offsetof(game_replacement, source) == offsetof(struct old_record, source), "source moved");
_Static_assert(offsetof(game_replacement, adapter) == offsetof(struct old_record, adapter), "adapter moved");
_Static_assert(offsetof(game_replacement, calls) == offsetof(struct old_record, calls), "counter moved");
int main(void)
{
    size_t count;
    const game_replacement *entries = game_replacement_table(&count);
    assert(count == 9);
    unsigned seen = 0;
    for (size_t i = 0; i < count; ++i) {
        assert(game_replacement_valid(&entries[i]));
        assert(entries[i].va >= 0x10000 && entries[i].va <= 0x10080);
        unsigned bit = (entries[i].va - 0x10000) / 16;
        assert(!(seen & (1u << bit)));
        seen |= 1u << bit;
    }
    assert(seen == 511);
    game_replacement bad = entries[0];
    bad.convention = (game_convention)99;
    assert(!game_replacement_valid(&bad));
    bad = entries[0]; bad.returns_value = 2; assert(!game_replacement_valid(&bad));
    const unsigned invalid[] = {1, 0x10, 0x15, 0x21, 255};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        bad = entries[0]; bad.input_abi = invalid[i]; assert(!game_replacement_valid(&bad));
    }
    bad = entries[0]; bad.input_abi = GAME_INPUT_esi; bad.scratch_mask = 2;
    assert(!game_replacement_valid(&bad));
    bad.scratch_mask = 0; bad.stack_args = 9; assert(!game_replacement_valid(&bad));
    bad = entries[0]; bad.scratch_mask = 8; assert(!game_replacement_valid(&bad));
    printf("LAYOUT count=%zu size=%zu offsets=%zu,%zu,%zu valid=9 invalid=10\n", count,
           sizeof(game_replacement), offsetof(game_replacement, source),
           offsetof(game_replacement, adapter), offsetof(game_replacement, calls));
    return 0;
}
