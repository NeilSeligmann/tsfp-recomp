/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T77 round 4, new small functions in 0x00000000-0x00180000, batch 3: table resets, small
 * accessors and record initializers. Each registers a call-free cdecl body. Names describe
 * observed behavior only. Receipt: docs/t77-round4-low.md
 */
#include "game_replace.h"

static void zero_dwords(guest_addr address, uint32_t count)
{
    for (uint32_t index = 0u; index < count; index++) {
        guest_write32(address + index * 4u, 0u);
    }
}

/* 0x000B2D40: clears every dword equal to `value` among the first [0x00732A10] (signed)
 * dwords of 0x007B2E20. Returns the count (0 when it is not positive). */
static uint32_t game_clear_matching_slots(uint32_t value)
{
    const int32_t count = (int32_t)guest_read32(0x00732A10u);
    int32_t index = 0;
    for (; index < count; index++) {
        const guest_addr slot = 0x007B2E20u + (uint32_t)index * 4u;
        if (guest_read32(slot) == value) {
            guest_write32(slot, 0u);
        }
    }
    return (uint32_t)index;
}
GAME_REPLACE(000B2D40, cdecl, 1, u32, game_clear_matching_slots)

/* 0x000DC250: bit 1 of the byte at +0x14 of the 64-byte entry `index` of 0x004DEB40. */
static uint32_t game_entry_flag_bit(uint32_t index)
{
    const uint32_t value = (uint32_t)(int32_t)(int8_t)guest_read8((index << 6) + 0x004DEB54u);
    return (value & 2u) >> 1;
}
GAME_REPLACE(000DC250, cdecl, 1, u32, game_entry_flag_bit)

/* 0x000DCB60: remembers `value` in 0x00739A90 and returns it, except that -1 returns the
 * remembered value instead. */
static uint32_t game_remember_or_recall(uint32_t value)
{
    if (value == 0xFFFFFFFFu) {
        return guest_read32(0x00739A90u);
    }
    guest_write32(0x00739A90u, value);
    return value;
}
GAME_REPLACE(000DCB60, cdecl, 1, u32, game_remember_or_recall)

/* 0x000F8AA0: sets [0x007AD29C] to 0x40 and [0x007AF31C] to 0x38 (0x40 when [0x0074CF64] is
 * nonzero). Returns the second value. */
static uint32_t game_set_display_defaults(void)
{
    const uint32_t flag = guest_read32(0x0074CF64u) != 0u;
    guest_write32(0x007AD29Cu, 0x40u);
    const uint32_t value = flag * 8u + 0x38u;
    guest_write32(0x007AF31Cu, value);
    return value;
}
GAME_REPLACE(000F8AA0, cdecl, 0, u32, game_set_display_defaults)

/* 0x00121A90: clears the 0x7D dwords at `record`, stores `value` in the first and -1 in the
 * dwords +0x54 and +0x74. Returns -1. */
static uint32_t game_init_record_with_value(uint32_t record, uint32_t value)
{
    zero_dwords(record, 0x7Du);
    guest_write32(record, value);
    guest_write32(record + 0x54u, 0xFFFFFFFFu);
    guest_write32(record + 0x74u, 0xFFFFFFFFu);
    return 0xFFFFFFFFu;
}
GAME_REPLACE(00121A90, cdecl, 2, u32, game_init_record_with_value)

/* 0x0013DCD0: when [0x0074911C] is 6, clears it and returns 1, else 0. */
static uint32_t game_consume_state_six(void)
{
    if (guest_read32(0x0074911Cu) != 6u) {
        return 0u;
    }
    guest_write32(0x0074911Cu, 0u);
    return 1u;
}
GAME_REPLACE(0013DCD0, cdecl, 0, u32, game_consume_state_six)

/* 0x00167A90: address of +0x188 of the 512-byte record (low 16 bits of `id`) of the array at
 * [0x0074C458]. */
static uint32_t game_record_field_address(uint32_t id)
{
    return ((id & 0xFFFFu) << 9) + guest_read32(0x0074C458u) + 0x188u;
}
GAME_REPLACE(00167A90, cdecl, 1, u32, game_record_field_address)

/* 0x00168090: dword at +0x1D0 of the same 512-byte record. */
static uint32_t game_record_field_value(uint32_t id)
{
    return guest_read32(((id & 0xFFFFu) << 9) + guest_read32(0x0074C458u) + 0x1D0u);
}
GAME_REPLACE(00168090, cdecl, 1, u32, game_record_field_value)

/* 0x0016B590: in the 128-byte row `row` of the array at [0x0074C464], stores the dwords of
 * `value` (3 dwords): first at 16 * (`column` + 2), the others at +0x24 and +0x28 from
 * 16 * `column`. Returns the address of the 16 * `column` position. */
static uint32_t game_store_row_triple(uint32_t row, uint32_t column, uint32_t value)
{
    const uint32_t base = (row << 7) + guest_read32(0x0074C464u);
    guest_write32(((column + 2u) << 4) + base, guest_read32(value));
    const uint32_t position = base + (column << 4);
    guest_write32(position + 0x24u, guest_read32(value + 4u));
    guest_write32(position + 0x28u, guest_read32(value + 8u));
    return position;
}
GAME_REPLACE(0016B590, cdecl, 3, u32, game_store_row_triple)

/* 0x00177EC0: initializes the request block at [object+0x7C] +0x568..+0x588: arguments
 * `first` at +0x56C, `second` at +0x578, `third` at +0x57C, 0 at +0x568, -1 at +0x570,
 * +0x584 and +0x588, 1 at +0x574 and 0x12 at +0x580. Returns the block. */
static uint32_t game_init_request_block(uint32_t object, uint32_t first, uint32_t second,
                                        uint32_t third)
{
    const uint32_t block = guest_read32(object + 0x7Cu);
    guest_write32(block + 0x56Cu, first);
    guest_write32(block + 0x578u, second);
    guest_write32(block + 0x568u, 0u);
    guest_write32(block + 0x570u, 0xFFFFFFFFu);
    guest_write32(block + 0x574u, 1u);
    guest_write32(block + 0x57Cu, third);
    guest_write32(block + 0x580u, 0x12u);
    guest_write32(block + 0x584u, 0xFFFFFFFFu);
    guest_write32(block + 0x588u, 0xFFFFFFFFu);
    return block;
}
GAME_REPLACE(00177EC0, cdecl, 4, u32, game_init_request_block)
