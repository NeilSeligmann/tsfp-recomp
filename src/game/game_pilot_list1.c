/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1520 cheap-model pilot, list 1: four call-free cdecl bodies that passed the unloosened
 * single-VA proof gate (both seeds, O0 and O3). Drafted by the pilot agent from the
 * disassembly, proven by `tools.replace prove`, then read against the disassembly.
 * Receipt: docs/t-cheap-model-pilot-list1.md
 */
#include "game_replace.h"

/* 0x00308AF0: record `index` (0xE4 bytes each) of the object at 0x007844A8: its dword +0x2E8
 * indexes the dword table at 0x0052CE00, returns that dword. */
static uint32_t game_record_e4_field_0x2e8_table_lookup(uint32_t index)
{
    const uint32_t object = guest_read32(0x007844A8u);
    const uint32_t key = guest_read32(index * 0xE4u + object + 0x2E8u);
    return guest_read32(key * 4u + 0x0052CE00u);
}
GAME_REPLACE(00308AF0, cdecl, 1, u32, game_record_e4_field_0x2e8_table_lookup)

/* 0x00321480: sign-extended word at +0x74 of `base` plus (index << 6) plus 0x00778D40. */
static uint32_t game_word_0x74_plus_index_shl6_plus_778d40(uint32_t base, uint32_t index)
{
    const uint16_t word = (uint16_t)(guest_read8(base + 0x74u) | (guest_read8(base + 0x75u) << 8));
    const uint32_t extended = (uint32_t)(int32_t)(int16_t)word;
    return extended + (index << 6) + 0x00778D40u;
}
GAME_REPLACE(00321480, cdecl, 2, u32, game_word_0x74_plus_index_shl6_plus_778d40)

/* 0x00344290: when the signed counter at 0x00765F38 is below 2, stores the argument at
 * 0x00765E5C + 4 * counter and increments the counter. */
static void game_push_value_into_two_slot_list(uint32_t value)
{
    const uint32_t count = guest_read32(0x00765F38u);
    if ((int32_t)count < 2) {
        guest_write32(count * 4u + 0x00765E5Cu, value);
        guest_write32(0x00765F38u, count + 1u);
    }
}
GAME_REPLACE(00344290, cdecl, 1, void, game_push_value_into_two_slot_list)

/* 0x003C1680: stores the second and third arguments at +0x4644 and +0x4648 of the first. */
static void game_object_store_pair_0x4644(uint32_t object, uint32_t first, uint32_t second)
{
    guest_write32(object + 0x4644u, first);
    guest_write32(object + 0x4648u, second);
}
GAME_REPLACE(003C1680, cdecl, 3, void, game_object_store_pair_0x4644)
