/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Accessors for the 512-byte record table whose base pointer is the global dword at
 * 0x0074C458 (retail XBE range 0x00150000-0x002C0000, T77). Both index it with
 * `(id & 0xFFFF) << 9`. Title-level purposes are unknown, so names describe only the
 * measured behavior. See docs/t77-range-150000-2c0000-leaves.md.
 */
#include "game_replace.h"

#define GAME_RECORD_TABLE_BASE 0x0074C458u

static uint32_t game_record_address(uint32_t id, uint32_t offset)
{
    return guest_read32(GAME_RECORD_TABLE_BASE) + ((id & 0xFFFFu) << 9) + offset;
}

/* 0x001656D0: 1 when the signed dword at record offset 0x1D0 is positive, else 0. */
static uint32_t game_record_d0_positive(uint32_t id)
{
    return (int32_t)guest_read32(game_record_address(id, 0x1D0u)) > 0 ? 1u : 0u;
}
GAME_REPLACE(001656D0, cdecl, 1, u32, game_record_d0_positive)

/* 0x00168100: OR bits into the flags dword at record offset 0x1C0, return the old value. */
static uint32_t game_record_flags_set(uint32_t id, uint32_t bits)
{
    const uint32_t address = game_record_address(id, 0x1C0u);
    const uint32_t previous = guest_read32(address);
    guest_write32(address, previous | bits);
    return previous;
}
GAME_REPLACE(00168100, cdecl, 2, u32, game_record_flags_set)
