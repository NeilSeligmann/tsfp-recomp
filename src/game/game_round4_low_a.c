/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T77 round 4, retried candidates in 0x00000000-0x00180000: lookups and flag predicates
 * that the earlier passes could not prove at the default 256 cases per function (too few
 * verdicts or too high a null-agree rate). Each is proven again with a documented
 * --cases-per-function value, listed in the receipt. Names describe observed behavior only.
 * Receipt: docs/t77-round4-low.md
 */
#include "game_replace.h"

/* 0x00074670: finds the node of the list at [object+0x16C] (next at +0x30) whose dword at +4
 * equals `key`, else 0. */
static uint32_t game_find_node_by_key(uint32_t object, uint32_t key)
{
    uint32_t node = guest_read32(object + 0x16Cu);
    while (node != 0u) {
        if (guest_read32(node + 4u) == key) {
            return node;
        }
        node = guest_read32(node + 0x30u);
    }
    return 0u;
}
GAME_REPLACE(00074670, cdecl, 2, u32, game_find_node_by_key)

/* 0x00164B60: 1 unless [0x0074C3D8] is negative and [0x0074C3FC] is zero. */
static uint32_t game_flags_allow(void)
{
    if ((int32_t)guest_read32(0x0074C3D8u) >= 0) {
        return 1u;
    }
    return guest_read32(0x0074C3FCu) != 0u;
}
GAME_REPLACE(00164B60, cdecl, 0, u32, game_flags_allow)

/* 0x000427F0: 0 while [0x006B9888] is set, else a dword of a 0x3C-byte record picked by
 * `index`: directly (record 0x006B94DC + 0x3C * index) when [0x0076B0FC] is nonzero, else
 * through the index list at 0x006B93C0 (record 0x006B93E4 + 0x3C * list[index]). */
static uint32_t game_indexed_record_dword_a(uint32_t index)
{
    if (guest_read32(0x006B9888u) != 0u) {
        return 0u;
    }
    if (guest_read32(0x0076B0FCu) != 0u) {
        return guest_read32(index * 0x3Cu + 0x006B94DCu);
    }
    const uint32_t slot = guest_read32(index * 4u + 0x006B93C0u);
    return guest_read32(slot * 0x3Cu + 0x006B93E4u);
}
GAME_REPLACE(000427F0, cdecl, 1, u32, game_indexed_record_dword_a)

/* 0x00042BF0: like 0x000427F0 for the dword at +0x34 of the record (0x006B94C8 + 0x3C *
 * index, or through the 0x006B93C0 list at 0x006B93D0 + 0x3C * list[index]). */
static uint32_t game_indexed_record_dword_b(uint32_t index)
{
    if (guest_read32(0x006B9888u) != 0u) {
        return 0u;
    }
    if (guest_read32(0x0076B0FCu) != 0u) {
        return guest_read32(index * 0x3Cu + 0x006B94C8u + 0x34u);
    }
    const uint32_t slot = guest_read32(index * 4u + 0x006B93C0u);
    return guest_read32(slot * 0x3Cu + 0x006B93D0u + 0x34u);
}
GAME_REPLACE(00042BF0, cdecl, 1, u32, game_indexed_record_dword_b)

/* 0x00073E10: 0 when the dword at +0x128 of the 16-byte slot `slot` of the object is -1 or the
 * dword at +0x130 of the same slot is 2, else 1. */
static uint32_t game_slot_ready(uint32_t object, uint32_t slot)
{
    if (guest_read32((slot << 4) + object + 0x128u) == 0xFFFFFFFFu) {
        return 0u;
    }
    return guest_read32(((slot + 0x13u) << 4) + object) != 2u;
}
GAME_REPLACE(00073E10, cdecl, 2, u32, game_slot_ready)

/* 0x0006B7E0: 1 when both bytes +0x10 and +0x11 of the 20-byte entry `index` of the array at
 * [0x006F2ED8] are nonzero. */
static uint32_t game_entry_bytes_set(uint32_t index)
{
    const uint32_t entry = guest_read32(0x006F2ED8u) + index * 20u;
    if (guest_read8(entry + 0x10u) == 0u) {
        return 0u;
    }
    return guest_read8(entry + 0x11u) != 0u;
}
GAME_REPLACE(0006B7E0, cdecl, 1, u32, game_entry_bytes_set)

/* 0x000DC290: dword at +0 of the 64-byte entry `index` of 0x004DEB40, 0 for index -1. */
static uint32_t game_entry_word_or_zero(uint32_t index)
{
    if (index == 0xFFFFFFFFu) {
        return 0u;
    }
    return guest_read32((index << 6) + 0x004DEB40u);
}
GAME_REPLACE(000DC290, cdecl, 1, u32, game_entry_word_or_zero)

/* 0x00069A20: moves the 0x7D5-dword slot `from` of the 0x1F54-byte slots at 0x007BEDC0 over
 * slot `to` and clears the first dword of `from`. Returns the address of slot `from`. The
 * slots are forward-copied dword by dword like `rep movsd`. */
static uint32_t game_move_slot(uint32_t to, uint32_t from)
{
    const uint32_t source = from * 0x1F54u + 0x007BEDC0u;
    if (to != from) {
        const uint32_t target = to * 0x1F54u + 0x007BEDC0u;
        for (uint32_t offset = 0u; offset < 0x7D5u * 4u; offset += 4u) {
            guest_write32(target + offset, guest_read32(source + offset));
        }
        guest_write32(source, 0u);
    }
    return source;
}
GAME_REPLACE(00069A20, cdecl, 2, u32, game_move_slot)

/* 0x00069980: address of the 40-byte record `record` at +0x288 of a block. The block is
 * 0x006F11C0 when bit 23 of [0x007DE458] is set or the 0x1F54-byte slot `slot` (0x007BEDC0)
 * starts with a zero dword, else the slot's area at +0x1290 (0x007C0050 + slot * 0x1F54). */
static uint32_t game_slot_record_address(uint32_t slot, uint32_t record)
{
    uint32_t block = 0x006F11C0u;
    if ((guest_read32(0x007DE458u) & 0x00800000u) == 0u) {
        const uint32_t offset = slot * 0x1F54u;
        if (guest_read32(offset + 0x007BEDC0u) != 0u) {
            block = offset + 0x007C0050u;
        }
    }
    return block + record * 40u + 0x288u;
}
GAME_REPLACE(00069980, cdecl, 2, u32, game_slot_record_address)
