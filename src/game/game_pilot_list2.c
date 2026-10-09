/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1520 cheap-model pilot, list 2: call-free cdecl and stdcall bodies that passed the
 * unloosened single-VA proof gate (both seeds 20261001 and 20261006, O0 and O3). Drafted by
 * the pilot agent from the disassembly, proven by `tools.replace prove`, then read against
 * the disassembly. Receipt: docs/t-cheap-model-pilot-list2.md
 */
#include "game_replace.h"

/* 0x001608E0: record `record` (pointer): index = dword [[record+0x7C]+0x1C]. Stores `value` at
 * 0x7A2BE0 + index*8, re-reads index and stores `record` at 0x7A2BE4 + index*8. Returns the
 * re-read index. */
static uint32_t game_register_record_pair_by_index(uint32_t record, uint32_t value)
{
    const uint32_t child = guest_read32(record + 0x7Cu);
    const uint32_t first_index = guest_read32(child + 0x1Cu);
    guest_write32(first_index * 8u + 0x007A2BE0u, value);
    const uint32_t second_index = guest_read32(child + 0x1Cu);
    guest_write32(second_index * 8u + 0x007A2BE4u, record);
    return second_index;
}
GAME_REPLACE(001608E0, cdecl, 2, u32, game_register_record_pair_by_index)

/* 0x00320FA0: address 0x00782E60 + 6 * (int16 at record+0x74 + index). */
static uint32_t game_record_indexed_entry_782e60(uint32_t record, uint32_t index)
{
    const uint32_t low = guest_read8(record + 0x74u);
    const uint32_t high = guest_read8(record + 0x75u);
    const int32_t base = (int16_t)(uint16_t)(low | (high << 8));
    const uint32_t sum = (uint32_t)base + index;
    return sum * 6u + 0x00782E60u;
}
GAME_REPLACE(00320FA0, cdecl, 2, u32, game_record_indexed_entry_782e60)

/* 0x00350C40: dword at 0x00535B80 + 4*index for a signed index in [0, 0xCA), else 0xAC44. */
static uint32_t game_table_535b80_or_default(uint32_t index)
{
    const int32_t signed_index = (int32_t)index;
    if (signed_index < 0 || signed_index >= 0xCA) {
        return 0xAC44u;
    }
    return guest_read32(index * 4u + 0x00535B80u);
}
GAME_REPLACE(00350C40, cdecl, 1, u32, game_table_535b80_or_default)

/* 0x00356600: copies 0x50 dwords (forward) from `source` to 0x00774040. */
static void game_copy_0x50_dwords_to_774040(uint32_t source)
{
    for (uint32_t index = 0u; index < 0x50u; index++) {
        guest_write32(0x00774040u + index * 4u, guest_read32(source + index * 4u));
    }
}
GAME_REPLACE(00356600, cdecl, 1, void, game_copy_0x50_dwords_to_774040)

/* 0x00357510: entry = 0x00774180 + 0x30C * byte[pointer]. Sets bit 2 of the byte at entry+0x34, then
 * stores 5 at 0x0077419C + 0x30C * dword[entry]. Returns entry. */
static uint32_t game_mark_entry_flag_and_state(uint32_t pointer)
{
    const uint32_t entry = (uint32_t)guest_read8(pointer) * 0x30Cu + 0x00774180u;
    guest_write8(entry + 0x34u, (uint8_t)(guest_read8(entry + 0x34u) | 4u));
    guest_write32(guest_read32(entry) * 0x30Cu + 0x0077419Cu, 5u);
    return entry;
}
GAME_REPLACE(00357510, cdecl, 1, u32, game_mark_entry_flag_and_state)

/* 0x003734E0: stores `object` at 0x0076CA34, then clears the dword at [object+0x7C]+0x20.
 * Returns the dword at object+0x7C. */
static uint32_t game_set_current_object_and_clear_child_field(uint32_t object)
{
    guest_write32(0x0076CA34u, object);
    const uint32_t child = guest_read32(object + 0x7Cu);
    guest_write32(child + 0x20u, 0u);
    return child;
}
GAME_REPLACE(003734E0, cdecl, 1, u32, game_set_current_object_and_clear_child_field)

/* 0x00385B82: stores `value` into the dword at `destination`. Returns 0. */
static uint32_t game_store_dword_return_zero(uint32_t destination, uint32_t value)
{
    guest_write32(destination, value);
    return 0u;
}
GAME_REPLACE(00385B82, stdcall, 2, u32, game_store_dword_return_zero)

/* 0x003BAB00: stores (flag == 1) as a 16-bit value at destination+0x8C. Returns it. */
static uint32_t game_store_word_flag_equals_one(uint32_t destination, uint32_t flag)
{
    const uint32_t value = flag == 1u ? 1u : 0u;
    guest_write8(destination + 0x8Cu, (uint8_t)value);
    guest_write8(destination + 0x8Du, 0u);
    return value;
}
GAME_REPLACE(003BAB00, cdecl, 2, u32, game_store_word_flag_equals_one)
