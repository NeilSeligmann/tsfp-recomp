/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T77 round 4, new small functions in 0x00000000-0x00180000, batch 1: global table and
 * record initializers, structure copies and small selectors. Each registers a call-free
 * cdecl body. Names describe observed behavior only. Receipt: docs/t77-round4-low.md
 */
#include "game_replace.h"

static void zero_dwords(guest_addr address, uint32_t count)
{
    for (uint32_t index = 0u; index < count; index++) {
        guest_write32(address + index * 4u, 0u);
    }
}

/* 0x00024810: clears the 0x100 dwords at 0x005651A8 and the 0x84 dwords at 0x005655B0, then
 * seeds the 0x60-byte entries from 0x005655A8 (state word at +0x10 is 1, 2, 4, 8 and mask
 * word at +0x40 is 0x10000, 0x20000, 0x40000, 0x80000 for entries 0 to 3), and the byte pairs
 * at 0x00565760 and 0x00565790. Returns 1. */
static uint32_t game_init_mask_table(void)
{
    zero_dwords(0x005655B0u, 0x84u);
    zero_dwords(0x005651A8u, 0x100u);
    guest_write32(0x005655A8u, 0u);
    guest_write32(0x005655B8u, 1u);
    guest_write32(0x005655E8u, 0x10000u);
    guest_write32(0x00565618u, 2u);
    guest_write32(0x00565648u, 0x20000u);
    guest_write32(0x00565678u, 4u);
    guest_write32(0x005656A8u, 0x40000u);
    guest_write32(0x005656D8u, 8u);
    guest_write32(0x00565708u, 0x80000u);
    guest_write8(0x00565760u, 1u);
    guest_write8(0x00565763u, 0x55u);
    guest_write8(0x00565790u, 1u);
    guest_write8(0x00565793u, 0x54u);
    return 1u;
}
GAME_REPLACE(00024810, cdecl, 0, u32, game_init_mask_table)

/* 0x0002E810: copies the 0x15 dwords of `source` to 0x00666590, its dwords +0x3C and +0x40 to
 * 0x00666588 and 0x0066658C, and its four dwords from +0x44 to 0x006865FC..0x00686608.
 * A null source does nothing and returns 0, else the address of its +0x44 part. */
static uint32_t game_load_state_block(uint32_t source)
{
    if (source == 0u) {
        return 0u;
    }
    for (uint32_t index = 0u; index < 0x15u; index++) {
        guest_write32(0x00666590u + index * 4u, guest_read32(source + index * 4u));
    }
    guest_write32(0x00666588u, guest_read32(source + 0x3Cu));
    guest_write32(0x0066658Cu, guest_read32(source + 0x40u));
    const uint32_t tail = source + 0x44u;
    guest_write32(0x006865FCu, guest_read32(tail));
    guest_write32(0x00686600u, guest_read32(tail + 4u));
    guest_write32(0x00686604u, guest_read32(tail + 8u));
    guest_write32(0x00686608u, guest_read32(tail + 0xCu));
    return tail;
}
GAME_REPLACE(0002E810, cdecl, 1, u32, game_load_state_block)

/* 0x00039720: sets bit 2 of the byte at +0x17F of the 0x1A0-byte record `index` of the array at
 * [0x0068C6FC]. Returns that byte's address. */
static uint32_t game_mark_record_flag(uint32_t index)
{
    const uint32_t address = index * 0x1A0u + guest_read32(0x0068C6FCu) + 0x17Fu;
    guest_write8(address, (uint8_t)(guest_read8(address) | 4u));
    return address;
}
GAME_REPLACE(00039720, cdecl, 1, u32, game_mark_record_flag)

/* 0x000426E0: clears dwords +8..+0x14 of `record` and stores 0x80 in +0x18..+0x30. Returns
 * the record. */
static uint32_t game_reset_record_range(uint32_t record)
{
    for (uint32_t offset = 8u; offset <= 0x14u; offset += 4u) {
        guest_write32(record + offset, 0u);
    }
    for (uint32_t offset = 0x18u; offset <= 0x30u; offset += 4u) {
        guest_write32(record + offset, 0x80u);
    }
    return record;
}
GAME_REPLACE(000426E0, cdecl, 1, u32, game_reset_record_range)

/* 0x00046B80: clears the 12 dwords at 0x006D6028, then stores -1 in dwords 0, 1, 3, 4, 6, 7,
 * 9 and 10. Returns -1. */
static uint32_t game_init_slot_table_a(void)
{
    zero_dwords(0x006D6028u, 12u);
    static const uint8_t slots[] = {0u, 1u, 3u, 4u, 6u, 7u, 9u, 10u};
    for (uint32_t index = 0u; index < sizeof slots; index++) {
        guest_write32(0x006D6028u + (uint32_t)slots[index] * 4u, 0xFFFFFFFFu);
    }
    return 0xFFFFFFFFu;
}
GAME_REPLACE(00046B80, cdecl, 0, u32, game_init_slot_table_a)

/* 0x00061680: clears the 12 dwords at 0x006D7840, stores -1 in its dwords 7 to 11, 1 in dword 0
 * and 0 in 0x006D7870 and 0x006D7874. Returns 0. */
static uint32_t game_init_slot_table_b(void)
{
    zero_dwords(0x006D7840u, 12u);
    guest_write32(0x006D785Cu, 0xFFFFFFFFu);
    guest_write32(0x006D7868u, 0xFFFFFFFFu);
    guest_write32(0x006D7860u, 0xFFFFFFFFu);
    guest_write32(0x006D786Cu, 0xFFFFFFFFu);
    guest_write32(0x006D7864u, 0xFFFFFFFFu);
    guest_write32(0x006D7840u, 1u);
    guest_write32(0x006D7870u, 0u);
    guest_write32(0x006D7874u, 0u);
    return 0u;
}
GAME_REPLACE(00061680, cdecl, 0, u32, game_init_slot_table_b)
