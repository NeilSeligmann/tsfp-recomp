/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1619 leftover pilot list (docs/data/t-gemini-jev-pilot/list-25.txt): 25 call-free,
 * register-input-free leaf helpers that no earlier pilot attempted. Hand drafted by a Claude
 * Sonnet 5.5 agent from the disassembly, one attempt per function, proven by
 * `tools.replace.batch_prove` (seeds 20261001 and 20261006, O0 and O3, 600 cases) and read
 * against the disassembly. Only the 9 functions that passed the unloosened gate are registered
 * here. The other 16 failed a reach gate (0 DISAGREE everywhere) and stay unproven, their
 * drafts are kept verbatim in docs/data/t-pilot-leftover/. Names describe observed memory
 * effects only. A leftover EAX of the original is returned where it is a plain computed value
 * so the generic adapter stays register-faithful on EAX. The string instructions (`rep movsd`,
 * `rep stosd`) are reproduced for DF = 0, the calling-convention state at function entry.
 * Receipt: docs/t-pilot-leftover-draft.md
 */
#include "game_replace.h"

/* 0x0016B790: record (index & 0xFFFF) * 512 of the table pointer at 0x0074C458 gets two
 * bytes at +0x1C9 and +0x1CA. Returns the record address. */
static uint32_t game_record_512_store_bytes_1c9_1ca(uint32_t index, uint32_t first, uint32_t second)
{
    const uint32_t record = guest_read32(0x0074C458u) + ((index & 0xFFFFu) << 9);
    guest_write8(record + 0x1C9u, (uint8_t)first);
    guest_write8(record + 0x1CAu, (uint8_t)second);
    return record;
}
GAME_REPLACE(0016B790, cdecl, 3, u32, game_record_512_store_bytes_1c9_1ca)

/* 0x00246D20: stores the argument at 0x0075EAF8 and 1 at 0x0075EB00, and sets 0x0075EAE4 to
 * 1 when it was zero. Returns 1. */
static uint32_t game_store_75eaf8_set_flags_75eb00_75eae4(uint32_t value)
{
    const uint32_t previous = guest_read32(0x0075EAE4u);
    guest_write32(0x0075EAF8u, value);
    guest_write32(0x0075EB00u, 1u);
    if (previous == 0u) {
        guest_write32(0x0075EAE4u, 1u);
    }
    return 1u;
}
GAME_REPLACE(00246D20, cdecl, 1, u32, game_store_75eaf8_set_flags_75eb00_75eae4)

/* 0x0025B770: five stack arguments (a, b, c, d, e) stored to 0x0078B540.. in the order
 * a, c, d, e, b. Returns e. */
static uint32_t game_store_five_args_78b540(uint32_t first, uint32_t second, uint32_t third,
                                            uint32_t fourth, uint32_t fifth)
{
    guest_write32(0x0078B540u, first);
    guest_write32(0x0078B544u, third);
    guest_write32(0x0078B548u, fourth);
    guest_write32(0x0078B54Cu, fifth);
    guest_write32(0x0078B550u, second);
    return fifth;
}
GAME_REPLACE(0025B770, cdecl, 5, u32, game_store_five_args_78b540)

/* 0x00352750: when the dword at 0x0076B10C is non-zero, clears it and the dwords at
 * 0x0076B108 and 0x0076B0FC. Returns 0. */
static uint32_t game_clear_three_globals_76b10c_gate(void)
{
    if (guest_read32(0x0076B10Cu) != 0u) {
        guest_write32(0x0076B108u, 0u);
        guest_write32(0x0076B10Cu, 0u);
        guest_write32(0x0076B0FCu, 0u);
    }
    return 0u;
}
GAME_REPLACE(00352750, cdecl, 0, u32, game_clear_three_globals_76b10c_gate)

/* 0x003561B0: when the destination pointer and the value are both non-zero, stores the value
 * through the pointer and increments the dword at 0x00774168. */
static void game_store_nonzero_value_count_774168(uint32_t destination, uint32_t value)
{
    if (destination == 0u || value == 0u) {
        return;
    }
    guest_write32(destination, value);
    guest_write32(0x00774168u, guest_read32(0x00774168u) + 1u);
}
GAME_REPLACE(003561B0, cdecl, 2, void, game_store_nonzero_value_count_774168)

/* 0x00358E60: byte 0 of the block is a player number. Copies the 0x1F dwords after it to the
 * pointer at (0x1584 * number + dword[0x007B0C48] + 0x1500), then sets bit 4 of the dword of
 * the 0x124-byte record `number` at 0x0077728C. Returns that record address. */
static uint32_t game_block_copy_1f_dwords_and_set_record_bit4(uint32_t block)
{
    const uint32_t number = guest_read8(block);
    const uint32_t source = block + 1u;
    const uint32_t table = guest_read32(0x007B0C48u);
    const uint32_t destination = guest_read32(number * 0x1584u + table + 0x1500u);
    for (uint32_t i = 0u; i < 0x1Fu; i++) {
        guest_write32(destination + i * 4u, guest_read32(source + i * 4u));
    }
    const uint32_t record = number * 0x124u + 0x0077728Cu;
    guest_write32(record, guest_read32(record) | 0x10u);
    return record;
}
GAME_REPLACE(00358E60, cdecl, 1, u32, game_block_copy_1f_dwords_and_set_record_bit4)

/* 0x00359E30: record `slot` of 0x30C bytes at 0x00774180 gets the low byte of `value` at
 * +0x2FA when the selector is 0, or at +0x2FB when it is 1. Other selectors store nothing.
 * Returns the record address. */
static uint32_t game_record_30c_store_byte_by_selector(uint32_t selector, uint32_t value, uint32_t slot)
{
    const uint32_t record = slot * 0x30Cu + 0x00774180u;
    if (selector == 0u) {
        guest_write8(record + 0x2FAu, (uint8_t)value);
    } else if (selector == 1u) {
        guest_write8(record + 0x2FBu, (uint8_t)value);
    }
    return record;
}
GAME_REPLACE(00359E30, cdecl, 3, u32, game_record_30c_store_byte_by_selector)

/* 0x0035E5F0: increments the dword at 0x0076BBBC (non-zero argument) or 0x0076BBC4 (zero
 * argument), then decrements the dword at 0x0076BBC0. Returns the argument. */
static uint32_t game_count_by_flag_and_decrement_76bbc0(uint32_t flag)
{
    if (flag != 0u) {
        guest_write32(0x0076BBBCu, guest_read32(0x0076BBBCu) + 1u);
    } else {
        guest_write32(0x0076BBC4u, guest_read32(0x0076BBC4u) + 1u);
    }
    guest_write32(0x0076BBC0u, guest_read32(0x0076BBC0u) - 1u);
    return flag;
}
GAME_REPLACE(0035E5F0, cdecl, 1, u32, game_count_by_flag_and_decrement_76bbc0)

/* 0x0038F4B5: fills `count & 0xFFFF` 16-bit words at the destination with the low word of the
 * value, as (count >> 1) dword stores of the doubled word plus one trailing word store when
 * the count is odd. A zero count does nothing. */
static void game_fill_words(uint32_t destination, uint32_t value, uint32_t count)
{
    const uint32_t words = count & 0xFFFFu;
    if (words == 0u) {
        return;
    }
    const uint32_t word = value & 0xFFFFu;
    const uint32_t pair = (word << 16) | word;
    uint32_t cursor = destination;
    for (uint32_t i = 0u; i < (words >> 1); i++) {
        guest_write32(cursor, pair);
        cursor += 4u;
    }
    if ((words & 1u) != 0u) {
        guest_write8(cursor, (uint8_t)word);
        guest_write8(cursor + 1u, (uint8_t)(word >> 8));
    }
}
GAME_REPLACE(0038F4B5, cdecl, 3, void, game_fill_words)
