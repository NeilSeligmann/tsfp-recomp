/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1520 cheap-model pilot, list 4 (docs/data/t-pilot-lists/list-4.txt): call-free,
 * register-input-free helpers. Drafted by a Claude Sonnet 5.5 agent from the disassembly,
 * proven by `tools.replace prove` (both seeds 20261001 and 20261006, O0 and O3) and read
 * against the disassembly. Only the 4 admitted functions of the 25 are registered here, the
 * other 21 failed the unloosened gate. Names describe observed memory effects only.
 * Receipt: docs/t-cheap-model-pilot-list4.md
 */
#include "game_replace.h"

/* 0x0024A130: returns [0x0078B6D4] as read, and stores 1 there when it was zero. */
static uint32_t game_global_78b6d4_default_one(void)
{
    const uint32_t value = guest_read32(0x0078B6D4u);
    if (value == 0u) {
        guest_write32(0x0078B6D4u, 1u);
    }
    return value;
}
GAME_REPLACE(0024A130, cdecl, 0, u32, game_global_78b6d4_default_one)

/* 0x002AEFB0: dword (first + second * 4) of the table at 0x00520A48. */
static uint32_t game_table_520a48_entry(uint32_t first, uint32_t second)
{
    return guest_read32((first + second * 4u) * 4u + 0x00520A48u);
}
GAME_REPLACE(002AEFB0, cdecl, 2, u32, game_table_520a48_entry)

/* 0x00321420: address 0x0077D874 + 20 * (int16 at object+0x74 * 11 + second). */
static uint32_t game_entry_20_address(uint32_t object, uint32_t second)
{
    const uint32_t low = (uint32_t)guest_read8(object + 0x74u);
    const uint32_t high = (uint32_t)guest_read8(object + 0x75u);
    const uint32_t word = low | (high << 8);
    const uint32_t extended = (word & 0x8000u) != 0u ? (word | 0xFFFF0000u) : word;
    return (extended * 11u + second) * 20u + 0x0077D874u;
}
GAME_REPLACE(00321420, cdecl, 2, u32, game_entry_20_address)

/* 0x00349710: in the 0x68-byte record `index` at 0x00778800 clears bit 1 of the dword at +0
 * and sets the dword at +0x40 to 2. Returns the record address. */
static uint32_t game_record_68_clear_bit1_set_state(uint32_t index)
{
    const uint32_t record = index * 0x68u + 0x00778800u;
    const uint32_t flags = guest_read32(record) & 0xFFFFFFFDu;
    guest_write32(record + 0x40u, 2u);
    guest_write32(record, flags);
    return record;
}
GAME_REPLACE(00349710, cdecl, 1, u32, game_record_68_clear_bit1_set_state)
