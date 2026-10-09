/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Second batch of small non-trivial call-free functions in 0x00000000-0x00180000: record
 * and list walks, handle checks, flag predicates. Names describe observed behavior only.
 * Receipt: docs/t77-small-logic-low.md
 */
#include "game_replace.h"

/* 0x00022AC0: records an owner for channel `slot` (0x80 bytes per channel at 0x003E3A..).
 * Stores `owner` at 0x005651A0 (returning the previous value). A nonzero owner marks the
 * channel with state 3 and `argument` at +0x3E3AE0, a zero owner state 2 and 1. In both
 * cases bit `slot` (mod 32) is set in 0x003E3AB8. */
static uint32_t game_set_channel_owner(uint32_t owner, uint32_t slot, uint32_t argument)
{
    const uint32_t previous = guest_read32(0x005651A0u);
    const uint32_t base = slot << 7;
    guest_write32(0x005651A0u, owner);
    if (owner != 0u) {
        guest_write32(base + 0x003E3AD0u, 3u);
        guest_write32(base + 0x003E3ACCu, 3u);
        guest_write32(0x003E3AB8u, guest_read32(0x003E3AB8u) | (1u << (slot & 31u)));
        guest_write32(base + 0x003E3AE0u, argument);
    } else {
        guest_write32(base + 0x003E3AD0u, 2u);
        guest_write32(base + 0x003E3ACCu, 2u);
        const uint32_t mask = guest_read32(0x003E3AB8u);
        guest_write32(base + 0x003E3AE0u, 1u);
        guest_write32(0x003E3AB8u, mask | (1u << (slot & 31u)));
    }
    return previous;
}
GAME_REPLACE(00022AC0, cdecl, 3, u32, game_set_channel_owner)

/* 0x00025210: copies `length` bytes (<= 0x400) from `source` to 0x005651A8 and records the
 * length at 0x005655A8. Returns the length, copying nothing above 0x400. As compiled it
 * copies whole dwords first, then the remaining bytes. */
static uint32_t game_copy_small_buffer(uint32_t source, uint32_t length)
{
    if (length > 0x400u) {
        return length;
    }
    uint32_t from = source;
    uint32_t to = 0x005651A8u;
    for (uint32_t words = length >> 2; words != 0u; words--, from += 4u, to += 4u) {
        guest_write32(to, guest_read32(from));
    }
    for (uint32_t tail = length & 3u; tail != 0u; tail--, from++, to++) {
        guest_write8(to, guest_read8(from));
    }
    guest_write32(0x005655A8u, length);
    return length;
}
GAME_REPLACE(00025210, cdecl, 2, u32, game_copy_small_buffer)

/* 0x0003E930: rotates three history slots (0x006B839C, A0, A4), keeping the largest
 * difference (slot 0 minus slot 1) seen in 0x006B83B0. Returns the new slot 0. */
static uint32_t game_rotate_history(void)
{
    const uint32_t newest = guest_read32(0x006B839Cu);
    const uint32_t middle = guest_read32(0x006B83A0u);
    const int32_t largest = (int32_t)guest_read32(0x006B83B0u);
    const int32_t span = (int32_t)(newest - middle);
    if (span > largest) {
        guest_write32(0x006B83B0u, (uint32_t)span);
    }
    const uint32_t oldest = guest_read32(0x006B83A4u);
    guest_write32(0x006B83A4u, middle);
    guest_write32(0x006B83A0u, oldest);
    guest_write32(0x006B839Cu, oldest);
    return oldest;
}
GAME_REPLACE(0003E930, cdecl, 0, u32, game_rotate_history)

/* 0x000426B0: index (0..3) of the first dword equal to `value` in the table at 0x006B93C0,
 * or -1. */
static uint32_t game_find_table_index(uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; index++) {
        if (guest_read32(0x006B93C0u + index * 4u) == value) {
            return index;
        }
    }
    return 0xFFFFFFFFu;
}
GAME_REPLACE(000426B0, cdecl, 1, u32, game_find_table_index)

/* 0x00042720: with 0x006B9888 clear, scans the 4 records (0x3C bytes, flag dword at
 * 0x006B93D4) and writes the index of each nonzero-flag record into the first four dwords of
 * 0x006B93C0 (more are counted but not stored). Stores the count at 0x004CA9AC and returns
 * it. Returns 0 immediately when 0x006B9888 is set. */
static uint32_t game_collect_active_records(void)
{
    if (guest_read32(0x006B9888u) != 0u) {
        return 0u;
    }
    uint32_t count = 0u;
    uint32_t index = 0u;
    for (uint32_t record = 0x006B93D4u; (int32_t)record < 0x006B94C4; record += 0x3Cu, index++) {
        if (guest_read32(record) != 0u) {
            if ((int32_t)count < 4) {
                guest_write32(0x006B93C0u + count * 4u, index);
            }
            count++;
        }
    }
    guest_write32(0x004CA9ACu, count);
    return count;
}
GAME_REPLACE(00042720, cdecl, 0, u32, game_collect_active_records)

/* 0x00042CE0: clears dwords +0x24 (and the two dwords below it) of the 16 records at
 * 0x006B94D8 + 0x3C*i. Returns the end address 0x006B989C. */
static uint32_t game_clear_record_fields(void)
{
    uint32_t cursor = 0x006B94DCu;
    do {
        guest_write32(cursor - 4u, 0u);
        guest_write32(cursor, 0u);
        guest_write32(cursor + 0x24u, 0u);
        cursor += 0x3Cu;
    } while ((int32_t)cursor < 0x006B989C);
    return cursor;
}
GAME_REPLACE(00042CE0, cdecl, 0, u32, game_clear_record_fields)

/* 0x00045710: takes the running id from 0x004CA9B0 (wraps to 1 after 0x3FFFFFF) and ORs flag
 * bits derived from the entry (0x14 bytes at [0x004CA9B4] indexed by the dword at [ptr]):
 * entry bit 12 gives 0x02000000, bit 6 0x10000000, bit 10 0x04000000, bit 4 0x40000000, and a
 * nonzero dword at [ptr+4] gives 0x20000000. Returns id | flags. */
static uint32_t game_compose_id_flags(uint32_t pointer)
{
    uint32_t result = guest_read32(0x004CA9B0u);
    uint32_t next = result + 1u;
    guest_write32(0x004CA9B0u, next);
    if ((int32_t)next > 0x3FFFFFF) {
        guest_write32(0x004CA9B0u, 1u);
    }
    const uint32_t entry = guest_read32(pointer);
    const uint32_t bits = guest_read32(guest_read32(0x004CA9B4u) + entry * 0x14u + 8u);
    if (bits & 0x1000u) {
        result |= 0x02000000u;
    }
    if (bits & 0x40u) {
        result |= 0x10000000u;
    }
    if (bits & 0x400u) {
        result |= 0x04000000u;
    }
    if (bits & 0x10u) {
        result |= 0x40000000u;
    }
    if (guest_read32(pointer + 4u) != 0u) {
        result |= 0x20000000u;
    }
    return result;
}
GAME_REPLACE(00045710, cdecl, 1, u32, game_compose_id_flags)

/* 0x00064180: stores `value` in the dword table at 0x007DE500 for a 1-based index 1..4.
 * Returns the zero-based index (index - 1). */
static uint32_t game_set_indexed_value(uint32_t index, uint32_t value)
{
    const int32_t zero_based = (int32_t)(index - 1u);
    if (zero_based >= 0 && zero_based < 4) {
        guest_write32(0x007DE500u + (uint32_t)zero_based * 4u, value);
    }
    return (uint32_t)zero_based;
}
GAME_REPLACE(00064180, cdecl, 2, u32, game_set_indexed_value)

/* 0x00066430: ranks the bit flags in 0x007DE45C: 0x800000 gives 4, 0x400000 gives 3,
 * 0x200000 gives 2, otherwise bit 20 (0 or 1). */
static uint32_t game_flag_rank(void)
{
    const uint32_t flags = guest_read32(0x007DE45Cu);
    if (flags & 0x800000u) {
        return 4u;
    }
    if (flags & 0x400000u) {
        return 3u;
    }
    if (flags & 0x200000u) {
        return 2u;
    }
    return (flags >> 20) & 1u;
}
GAME_REPLACE(00066430, cdecl, 0, u32, game_flag_rank)
