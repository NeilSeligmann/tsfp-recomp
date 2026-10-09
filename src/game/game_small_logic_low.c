/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Small non-trivial call-free functions in the 0x00000000-0x00180000 range of the retail
 * XBE: branches, loops, table and record walks. Names describe the observed behavior only,
 * no subsystem meaning is inferred. Every function was decompiled from the raw bytes and
 * is differential-proven (docs/t77-small-logic-low.md).
 */
#include "game_replace.h"

static uint32_t read16(guest_addr address)
{
    return (uint32_t)guest_read8(address) | ((uint32_t)guest_read8(address + 1u) << 8);
}

/* 0x00012820: element address in a table of 8 or 16 byte entries. Entry size is chosen by
 * flag 0x10 of byte [object+0xC]. Table base is [table+4], index is the word at [key+6]. */
static uint32_t game_table_entry_address(uint32_t object, uint32_t table, uint32_t key)
{
    const uint32_t eight_byte_entries = guest_read8(object + 0xCu) & 0x10u;
    const uint32_t index = read16(key + 6u);
    const uint32_t base = guest_read32(table + 4u);
    if (eight_byte_entries) {
        return base + index * 8u;
    }
    return base + (index << 4);
}
GAME_REPLACE(00012820, cdecl, 3, u32, game_table_entry_address)

/* 0x00032860: mode [0x006891A8]: -1 stores [0x006891C8] to *first, 2 stores [0x006891B8] to
 * *second, other values store nothing. Returns the mode. */
static uint32_t game_device_query_c(uint32_t first, uint32_t second)
{
    const uint32_t mode = guest_read32(0x006891A8u);
    if (mode == 0xFFFFFFFFu) {
        guest_write32(first, guest_read32(0x006891C8u));
    } else if (mode == 2u) {
        guest_write32(second, guest_read32(0x006891B8u));
    }
    return mode;
}
GAME_REPLACE(00032860, cdecl, 2, u32, game_device_query_c)
