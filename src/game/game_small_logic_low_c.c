/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Third batch of small non-trivial call-free functions in 0x00000000-0x00180000: lookups,
 * handle checks, bounded list walks and flag predicates. Names describe observed behavior
 * only. Receipt: docs/t77-small-logic-low.md
 */
#include "game_replace.h"

static uint32_t rd16u(guest_addr address)
{
    return (uint32_t)guest_read8(address) | ((uint32_t)guest_read8(address + 1u) << 8);
}

/* Handle table: the top 10 bits of a handle select a 0x2E8-byte entry of the array at
 * [0x0072F5E8], which stores the live handle at +0x2C. */
static uint32_t handle_entry_address(uint32_t handle)
{
    return (handle >> 22) * 0x2E8u + guest_read32(0x0072F5E8u);
}

/* 0x0006A710: finds the 0x80-byte channel entry (first at 0x00515FFC, ends at the first entry
 * whose leading word is zero) whose dword at +4 equals the mode word [0x0079094C] and
 * returns 0x00515778 + 0x3C * its dword at +0x14, else 0. */
static uint32_t game_channel_mode_record(void)
{
    uint32_t entry = 0x00515FFCu;
    if (rd16u(entry) == 0u) {
        return 0u;
    }
    const uint32_t mode = guest_read32(0x0079094Cu);
    uint32_t index = 0u;
    while (guest_read32(entry + 4u) != mode) {
        entry += 0x80u;
        index++;
        if (rd16u(entry) == 0u) {
            return 0u;
        }
    }
    return guest_read32((index << 7) + 0x00515FF8u + 0x14u) * 0x3Cu + 0x00515778u;
}
GAME_REPLACE(0006A710, cdecl, 0, u32, game_channel_mode_record)

/* 0x0006F430: dword 0x0051FC48[index * 9] for 0 <= index < 0x14, else -1. */
static uint32_t game_table_value_checked(uint32_t index)
{
    if ((int32_t)index < 0 || (int32_t)index >= 0x14) {
        return 0xFFFFFFFFu;
    }
    return guest_read32(index * 36u + 0x0051FC48u);
}
GAME_REPLACE(0006F430, cdecl, 1, u32, game_table_value_checked)

/* 0x00070EF0: picks a dword from the block at [object+0x1C] by mask bits: bit 8 of `mask`
 * selects +0x4C, else bit 9 selects +0x44, else +0x48. */
static uint32_t game_select_block_field(uint32_t object, uint32_t mask)
{
    const uint32_t block = guest_read32(object + 0x1Cu);
    if (mask & 0x100u) {
        return guest_read32(block + 0x4Cu);
    }
    if (mask & 0x200u) {
        return guest_read32(block + 0x44u);
    }
    return guest_read32(block + 0x48u);
}
GAME_REPLACE(00070EF0, cdecl, 2, u32, game_select_block_field)

/* 0x000718E0: follows the +0x10 link chain `steps` times from the dword at [object+0xD8].
 * A null object returns 0, a non-positive step count returns the first link, a null link
 * stays null. */
static uint32_t game_follow_chain(uint32_t object, uint32_t steps)
{
    if (object == 0u) {
        return 0u;
    }
    uint32_t link = guest_read32(object + 0xD8u);
    for (int32_t remaining = (int32_t)steps; remaining > 0; remaining--) {
        if (link != 0u) {
            link = guest_read32(link + 0x10u);
        }
    }
    return link;
}
GAME_REPLACE(000718E0, cdecl, 2, u32, game_follow_chain)

/* 0x00071CD0: sets or clears bit 0 of [object+0xC] by `enable`. Returns the object. */
static uint32_t game_set_flag_bit(uint32_t object, uint32_t enable)
{
    const uint32_t flags = guest_read32(object + 0xCu) & 0xFFFFFFFEu;
    guest_write32(object + 0xCu, flags);
    if (enable != 0u) {
        guest_write32(object + 0xCu, flags | 1u);
    }
    return object;
}
GAME_REPLACE(00071CD0, cdecl, 2, u32, game_set_flag_bit)

/* 0x00074950: in the four 0x10-byte slots at [object+0x128], stores `value` at +4 of the
 * slot whose first dword equals `key`. Returns the slot address, or 4 when none matches. */
static uint32_t game_set_slot_value(uint32_t object, uint32_t key, uint32_t value)
{
    for (uint32_t index = 0u; index < 4u; index++) {
        const uint32_t slot = object + 0x128u + index * 0x10u;
        if (guest_read32(slot) == key) {
            guest_write32(slot + 4u, value);
            return slot;
        }
    }
    return 4u;
}
GAME_REPLACE(00074950, cdecl, 3, u32, game_set_slot_value)

/* 0x00074B80: ORs +0x48 into +0x40 for every node of the list at [object+0x16C] (next at
 * +0x30), then for the object itself. Returns the object's +0x48. */
static uint32_t game_propagate_flags(uint32_t object)
{
    uint32_t node = guest_read32(object + 0x16Cu);
    while (node != 0u) {
        guest_write32(node + 0x40u, guest_read32(node + 0x40u) | guest_read32(node + 0x48u));
        node = guest_read32(node + 0x30u);
    }
    const uint32_t extra = guest_read32(object + 0x48u);
    guest_write32(object + 0x40u, guest_read32(object + 0x40u) | extra);
    return extra;
}
GAME_REPLACE(00074B80, cdecl, 1, u32, game_propagate_flags)

/* 0x000813F0: rewrites every dword equal to `from` to `to` in the 1023 records of 0x18
 * bytes at 0x004D2D64. Returns the end address 0x004D8D4C. */
static uint32_t game_replace_matching_dwords(uint32_t from, uint32_t to)
{
    uint32_t cursor = 0x004D2D64u;
    do {
        if (guest_read32(cursor) == from) {
            guest_write32(cursor, to);
        }
        cursor += 0x18u;
    } while ((int32_t)cursor < 0x004D8D4C);
    return cursor;
}
GAME_REPLACE(000813F0, cdecl, 2, u32, game_replace_matching_dwords)

/* 0x0008E7D0: clears *out when nonnull, then sets 0x004D9700 to 1 when [0x006F73B0] is zero
 * (else 0). Returns that value. */
static uint32_t game_clear_and_flag(uint32_t unused, uint32_t out)
{
    (void)unused;
    if (out != 0u) {
        guest_write32(out, 0u);
    }
    const uint32_t flag = (guest_read32(0x006F73B0u) == 0u) ? 1u : 0u;
    guest_write32(0x004D9700u, flag);
    return flag;
}
GAME_REPLACE(0008E7D0, cdecl, 2, u32, game_clear_and_flag)

/* 0x000942F0: counts 0x10-byte steps from the record at [[object+0x10]] until a record whose
 * first dword is negative (signed). Returns the number of steps. */
static uint32_t game_count_until_negative(uint32_t object)
{
    uint32_t record = guest_read32(object + 0x10u);
    uint32_t steps = 0u;
    if ((int32_t)guest_read32(record) < 0) {
        return 0u;
    }
    do {
        const uint32_t next = guest_read32(record + 0x10u);
        record += 0x10u;
        steps++;
        if ((int32_t)next < 0) {
            break;
        }
    } while (1);
    return steps;
}
GAME_REPLACE(000942F0, cdecl, 1, u32, game_count_until_negative)

/* 0x0009E750: 1 when any of the first N (signed byte at +0x30) indices stored from +0x38 of
 * `object` selects a nonzero byte of the table at [0x007B977C]. */
static uint32_t game_any_index_flagged(uint32_t object)
{
    const int32_t count = (int32_t)(int8_t)guest_read8(object + 0x30u);
    const uint32_t table = guest_read32(0x007B977Cu);
    for (int32_t index = 0; index < count; index++) {
        const uint32_t selected = guest_read32(object + 0x38u + (uint32_t)index * 4u);
        if (guest_read8(table + selected) != 0u) {
            return 1u;
        }
    }
    return 0u;
}
GAME_REPLACE(0009E750, cdecl, 1, u32, game_any_index_flagged)

/* 0x000CF320: zeroes the dwords at stride 0x10 over the first 0x3E0 bytes of the block at
 * [object+0xF74], then the dwords at object +0x1030, +0x10FC, +0x11C8, +0x1294, +0x1360,
 * +0x142C and +0x1028. Returns 0x3E0. */
static uint32_t game_zero_object_tables(uint32_t object)
{
    uint32_t offset = 0u;
    do {
        guest_write32(offset + guest_read32(object + 0xF74u), 0u);
        offset += 0x10u;
    } while ((int32_t)offset < 0x3E0);
    guest_write32(object + 0x1030u, 0u);
    guest_write32(object + 0x10FCu, 0u);
    guest_write32(object + 0x11C8u, 0u);
    guest_write32(object + 0x1294u, 0u);
    guest_write32(object + 0x1360u, 0u);
    guest_write32(object + 0x142Cu, 0u);
    guest_write32(object + 0x1028u, 0u);
    return offset;
}
GAME_REPLACE(000CF320, cdecl, 1, u32, game_zero_object_tables)

/* 0x000D2D20: (dword +8 of the object - [0x007B0CE4]) divided by 4092 (signed, truncating),
 * or -1 when +8 is zero. The original compiles the division to a multiply by 0x80200803,
 * an add and a shift, which is exactly truncating division by 4092 for every 32-bit input
 * (checked exhaustively, docs/t77-small-logic-low.md). */
static uint32_t game_elapsed_units(uint32_t object)
{
    const uint32_t stamp = guest_read32(object + 8u);
    if (stamp == 0u) {
        return 0xFFFFFFFFu;
    }
    const int32_t delta = (int32_t)(stamp - guest_read32(0x007B0CE4u));
    return (uint32_t)(delta / 4092);
}
GAME_REPLACE(000D2D20, cdecl, 1, u32, game_elapsed_units)

/* 0x000D7380: validates the handle in [0x007B0C44] against the handle table. Returns the
 * entry address when live. A stale handle is cleared. 0 for an empty or stale handle. */
static uint32_t game_validate_global_handle(void)
{
    const uint32_t handle = guest_read32(0x007B0C44u);
    if (handle == 0u) {
        return 0u;
    }
    const uint32_t entry = handle_entry_address(handle);
    if (guest_read32(entry + 0x2Cu) == handle) {
        return entry;
    }
    guest_write32(0x007B0C44u, 0u);
    return 0u;
}
GAME_REPLACE(000D7380, cdecl, 0, u32, game_validate_global_handle)

/* 0x000DB340: moves the node at [object+0x234] to the head ([object+0x230]) of the doubly
 * linked list (next at +0x18, previous at +0x14) and bumps the count at +0x238, only while
 * the count is below 0x14. Returns the new head, 0 when the list is full. */
static uint32_t game_rotate_tail_to_head(uint32_t object)
{
    if ((int32_t)guest_read32(object + 0x238u) >= 0x14) {
        return 0u;
    }
    const uint32_t tail = guest_read32(object + 0x234u);
    const uint32_t old_head = guest_read32(object + 0x230u);
    guest_write32(object + 0x230u, tail);
    const uint32_t new_tail = guest_read32(tail + 0x18u);
    guest_write32(object + 0x234u, new_tail);
    if (new_tail != 0u) {
        guest_write32(new_tail + 0x14u, 0u);
    }
    guest_write32(guest_read32(object + 0x230u) + 0x18u, old_head);
    guest_write32(guest_read32(object + 0x230u) + 0x14u, 0u);
    if (old_head != 0u) {
        guest_write32(old_head + 0x14u, guest_read32(object + 0x230u));
    }
    guest_write32(object + 0x238u, guest_read32(object + 0x238u) + 1u);
    return guest_read32(object + 0x230u);
}
GAME_REPLACE(000DB340, cdecl, 1, u32, game_rotate_tail_to_head)

/* 0x000DB3B0: unlinks `node` from the doubly linked list of `object` (head [object+0x230],
 * tail [object+0x234], next +0x18, previous +0x14), makes it the tail ahead of the old
 * tail and decrements the count at +0x238. Returns the object. */
static uint32_t game_move_node_to_tail(uint32_t object, uint32_t node)
{
    const uint32_t old_tail = guest_read32(object + 0x234u);
    const uint32_t previous = guest_read32(node + 0x14u);
    if (previous != 0u) {
        guest_write32(previous + 0x18u, guest_read32(node + 0x18u));
    } else {
        guest_write32(object + 0x230u, guest_read32(node + 0x18u));
    }
    const uint32_t next = guest_read32(node + 0x18u);
    if (next != 0u) {
        guest_write32(next + 0x14u, guest_read32(node + 0x14u));
    }
    guest_write32(object + 0x234u, node);
    guest_write32(node + 0x14u, 0u);
    guest_write32(guest_read32(object + 0x234u) + 0x18u, old_tail);
    guest_write32(old_tail + 0x14u, guest_read32(object + 0x234u));
    guest_write32(object + 0x238u, guest_read32(object + 0x238u) - 1u);
    return object;
}
GAME_REPLACE(000DB3B0, cdecl, 2, u32, game_move_node_to_tail)

/* 0x00119A80: clears 0x0073D9B8 when 0x006B7AA8 is zero and either 0x0073D97C or 0x0073D994
 * is nonzero. Returns [0x006B7AA8]. */
static uint32_t game_clear_if_idle(void)
{
    const uint32_t first = guest_read32(0x0073D97Cu);
    const uint32_t idle = guest_read32(0x006B7AA8u);
    if (first != 0u && idle == 0u) {
        guest_write32(0x0073D9B8u, 0u);
    }
    if (guest_read32(0x0073D994u) != 0u && idle == 0u) {
        guest_write32(0x0073D9B8u, 0u);
    }
    return idle;
}
GAME_REPLACE(00119A80, cdecl, 0, u32, game_clear_if_idle)

/* 0x0011D990: with a nonnull object sets bit 19 in dword +0xC6C and stores `value` at
 * +0xD38 of the block at [object+0x7C]. Returns the block, 0 for a null object. */
static uint32_t game_tag_block(uint32_t object, uint32_t value)
{
    if (object == 0u) {
        return 0u;
    }
    const uint32_t block = guest_read32(object + 0x7Cu);
    guest_write32(block + 0xC6Cu, guest_read32(block + 0xC6Cu) | 0x80000u);
    guest_write32(block + 0xD38u, value);
    return block;
}
GAME_REPLACE(0011D990, cdecl, 2, u32, game_tag_block)

/* 0x0015B530: in the block at [object+0x7C], finds the first of three dwords at +0xE78
 * that is zero and fills it with dword +0x2C of `node`, then clears +0x9AC and +0x9A8
 * (also when all three are taken). Returns the slot index, or 3 when none was free. */
static uint32_t game_assign_free_slot(uint32_t object, uint32_t node)
{
    const uint32_t block = guest_read32(object + 0x7Cu);
    for (uint32_t index = 0u; index < 3u; index++) {
        if (guest_read32(block + 0xE78u + index * 4u) == 0u) {
            guest_write32(block + index * 4u + 0xE78u, guest_read32(node + 0x2Cu));
            guest_write32(block + 0x9ACu, 0u);
            guest_write32(block + 0x9A8u, 0u);
            return index;
        }
    }
    guest_write32(block + 0x9ACu, 0u);
    guest_write32(block + 0x9A8u, 0u);
    return 3u;
}
GAME_REPLACE(0015B530, cdecl, 2, u32, game_assign_free_slot)

/* 0x00160850: appends `object` to the 5-entry list at 0x007A3560 (count [0x007A36C8]) when
 * the count is below 5 (signed) and flag 0x40 of byte +8 of the object's +0x7C block is set.
 * Returns the (possibly updated) count. */
static uint32_t game_append_flagged_object(uint32_t object)
{
    uint32_t count = guest_read32(0x007A36C8u);
    if ((int32_t)count >= 5) {
        return count;
    }
    if ((guest_read8(guest_read32(object + 0x7Cu) + 8u) & 0x40u) == 0u) {
        return count;
    }
    guest_write32(count * 4u + 0x007A3560u, object);
    count++;
    guest_write32(0x007A36C8u, count);
    return count;
}
GAME_REPLACE(00160850, cdecl, 1, u32, game_append_flagged_object)

/* 0x00160EC0: 1 when some bit i of `mask` (i < 20) has a nonzero dword at the 0x60-byte
 * entry i of 0x007A2D88. */
static uint32_t game_mask_selects_entry(uint32_t mask)
{
    uint32_t entry = 0x007A2D88u;
    for (uint32_t bit = 0u; (int32_t)entry < 0x007A3508; bit++, entry += 0x60u) {
        if ((mask & (1u << bit)) != 0u && guest_read32(entry) != 0u) {
            return 1u;
        }
    }
    return 0u;
}
GAME_REPLACE(00160EC0, cdecl, 1, u32, game_mask_selects_entry)

/* 0x00166B60: byte at +0x1C9 of the 0x200-byte record (low 16 bits of `record`) at
 * [0x0074C458], masked by `mask`. Returns 1 when either argument is zero. */
static uint32_t game_record_byte_masked(uint32_t mask, uint32_t record)
{
    if (record == 0u || mask == 0u) {
        return 1u;
    }
    return guest_read8(((record & 0xFFFFu) << 9) + guest_read32(0x0074C458u) + 0x1C9u) & mask;
}
GAME_REPLACE(00166B60, cdecl, 2, u32, game_record_byte_masked)
