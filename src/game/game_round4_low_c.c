/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T77 round 4, new small functions in 0x00000000-0x00180000, batch 2: table lookups,
 * record copies and small state resets. Each registers a call-free cdecl body. Names
 * describe observed behavior only. Receipt: docs/t77-round4-low.md
 */
#include "game_replace.h"

static void zero_dwords(guest_addr address, uint32_t count)
{
    for (uint32_t index = 0u; index < count; index++) {
        guest_write32(address + index * 4u, 0u);
    }
}

static uint32_t rd16u(guest_addr address)
{
    return (uint32_t)guest_read8(address) | ((uint32_t)guest_read8(address + 1u) << 8);
}

static int32_t rd16s(guest_addr address)
{
    return (int32_t)(int16_t)rd16u(address);
}

/* 0x00070D10: dword `index` of the array at +0x28 of the block [object+0x90]. */
static uint32_t game_block_array_dword(uint32_t object, uint32_t index)
{
    const uint32_t block = guest_read32(object + 0x90u);
    return guest_read32(block + index * 4u + 0x28u);
}
GAME_REPLACE(00070D10, cdecl, 2, u32, game_block_array_dword)

/* 0x00071590: copies the four dwords of `source` to +0x48..+0x54 of the 0x1E0-byte record
 * [0x007BA9E0] of 0x007BA260. Returns the record. */
static uint32_t game_store_current_record_quad(uint32_t source)
{
    const uint32_t record = guest_read32(0x007BA9E0u) * 0x1E0u + 0x007BA260u;
    guest_write32(record + 0x48u, guest_read32(source));
    guest_write32(record + 0x4Cu, guest_read32(source + 4u));
    guest_write32(record + 0x50u, guest_read32(source + 8u));
    guest_write32(record + 0x54u, guest_read32(source + 0xCu));
    return record;
}
GAME_REPLACE(00071590, cdecl, 1, u32, game_store_current_record_quad)

/* 0x00082E80: sets 0x007B9790 and 0x004D9658 to 1. When the mode [0x0079094C] is 0x65, 0x66,
 * 0x6A or 0x6B also clears 0x007B97A0, 0x007B99C4, 0x007B9BE8 and 0x007B9E0C. Returns 0 for
 * those modes, else the mode. */
static uint32_t game_reset_mode_flags(void)
{
    guest_write32(0x007B9790u, 1u);
    guest_write32(0x004D9658u, 1u);
    const uint32_t mode = guest_read32(0x0079094Cu);
    if (mode != 0x65u && mode != 0x66u && mode != 0x6Au && mode != 0x6Bu) {
        return mode;
    }
    guest_write32(0x007B97A0u, 0u);
    guest_write32(0x007B99C4u, 0u);
    guest_write32(0x007B9BE8u, 0u);
    guest_write32(0x007B9E0Cu, 0u);
    return 0u;
}
GAME_REPLACE(00082E80, cdecl, 0, u32, game_reset_mode_flags)

/* 0x0008DBB0: clears ([0x0070AAD8] * 512 rounded up to 16) bytes at [0x0070AADC], clears
 * 0x007B67D0 and 0x007B6984 and sets 0x004D9700 to ([0x006F73B0] == 0). Returns that flag. */
static uint32_t game_clear_scratch_and_flag(void)
{
    const uint32_t bytes = ((guest_read32(0x0070AAD8u) << 9) + 0xFu) & 0xFFFFFFF0u;
    const uint32_t target = guest_read32(0x0070AADCu);
    guest_write32(0x007B67D0u, 0u);
    guest_write32(0x007B6984u, 0u);
    zero_dwords(target, bytes >> 2);
    const uint32_t flag = guest_read32(0x006F73B0u) == 0u;
    guest_write32(0x004D9700u, flag);
    return flag;
}
GAME_REPLACE(0008DBB0, cdecl, 0, u32, game_clear_scratch_and_flag)

/* 0x00091390: copies the eight dwords at 0x0070AA58 to 0x0070AAAC. */
static void game_copy_octet_forward(void)
{
    for (uint32_t index = 0u; index < 8u; index++) {
        guest_write32(0x0070AAACu + index * 4u, guest_read32(0x0070AA58u + index * 4u));
    }
}
GAME_REPLACE(00091390, cdecl, 0, void, game_copy_octet_forward)

/* 0x00098DB0: sets +0x1C of the object [0x0070E164] to 1 and clears 0x004D977C. Returns the
 * object, or 0 (doing nothing) when there is none. */
static uint32_t game_arm_current_object(void)
{
    const uint32_t object = guest_read32(0x0070E164u);
    if (object == 0u) {
        return 0u;
    }
    guest_write32(object + 0x1Cu, 1u);
    guest_write32(0x004D977Cu, 0u);
    return object;
}
GAME_REPLACE(00098DB0, cdecl, 0, u32, game_arm_current_object)

/* 0x0009B160: follows the chain of 16-byte entries at 0x0070E178 (next index signed 16-bit at
 * +0, key signed 16-bit at +6) from the index stored at `start` to the first entry whose key
 * equals `key`. Returns its index, or -1 when the chain ends (negative index) first. */
static uint32_t game_find_chain_entry(uint32_t start, uint32_t key)
{
    uint32_t index = guest_read32(start);
    if ((int32_t)index < 0) {
        return 0xFFFFFFFFu;
    }
    for (;;) {
        const uint32_t entry = index << 4;
        if ((uint32_t)rd16s(entry + 0x0070E17Eu) == key) {
            return index;
        }
        index = (uint32_t)rd16s(entry + 0x0070E178u);
        if ((int32_t)index < 0) {
            return 0xFFFFFFFFu;
        }
    }
}
GAME_REPLACE(0009B160, cdecl, 2, u32, game_find_chain_entry)

/* 0x0009C570: clears bit 0 of the dword at +4 of the 32-byte record (`id` - 0x209E) of the
 * array at [0x00715F38]. Returns that dword's address. */
static uint32_t game_clear_record_bit(uint32_t id)
{
    const uint32_t address = ((id - 0x209Eu) << 5) + guest_read32(0x00715F38u) + 4u;
    guest_write32(address, guest_read32(address) & 0xFFFFFFFEu);
    return address;
}
GAME_REPLACE(0009C570, cdecl, 1, u32, game_clear_record_bit)

/* 0x0009E630: stores `value` at +4 of the 48-byte entry named by the signed 16-bit index at
 * 0x0078B860 + 2 * `slot`, in the array [0x00791D4C]. Returns the array pointer. */
static uint32_t game_set_mapped_entry_value(uint32_t slot, uint32_t value)
{
    const uint32_t mapped = (uint32_t)rd16s(slot * 2u + 0x0078B860u);
    const uint32_t array = guest_read32(0x00791D4Cu);
    guest_write32(mapped * 48u + array + 4u, value);
    return array;
}
GAME_REPLACE(0009E630, cdecl, 2, u32, game_set_mapped_entry_value)

/* 0x0009E730: the first dword of the 48-byte entry named by the signed 16-bit index at
 * 0x0078B860 + 2 * `slot`, in the array [0x00791D4C]. */
static uint32_t game_mapped_entry_value(uint32_t slot)
{
    const uint32_t mapped = (uint32_t)rd16s(slot * 2u + 0x0078B860u);
    return guest_read32(mapped * 48u + guest_read32(0x00791D4Cu));
}
GAME_REPLACE(0009E730, cdecl, 1, u32, game_mapped_entry_value)
