/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Call-free loops, list and table walks and record copies from retail XBE range
 * 0x00180000-0x00470000 (T77 round 4, batch B). Plain GAME_REPLACE adapters: eax is the result
 * when there is one, ecx and edx are scratch and the caller audit refuses any function whose
 * call site reads them. Field names state offsets only, no game meaning is assigned beyond the
 * INFERRED names in tools/data/function_names.csv. Proofs and rejects: docs/t77-round4-high.md.
 */
#include "game_replace.h"

static uint32_t game_u16_at(uint32_t address)
{
    return (uint32_t)guest_read8(address) | ((uint32_t)guest_read8(address + 1u) << 8);
}

static void game_store_u16(uint32_t address, uint32_t value)
{
    guest_write8(address, (uint8_t)value);
    guest_write8(address + 1u, (uint8_t)(value >> 8));
}

/* 0x003C4730: pointer array [object+8] with count [object+4]. Returns the first non-null
 * pointer whose fields +4 and +8 equal those of the record at probe, or 0. */
static uint32_t game_find_pointer_matching_fields_4_8(uint32_t object, uint32_t probe)
{
    const int32_t count = (int32_t)guest_read32(object + 4u);
    if (count <= 0) {
        return 0u;
    }
    const uint32_t array = guest_read32(object + 8u);
    for (int32_t index = 0; index < count; index++) {
        const uint32_t entry = guest_read32(array + (uint32_t)index * 4u);
        if (entry != 0u && guest_read32(entry + 4u) == guest_read32(probe + 4u) &&
            guest_read32(entry + 8u) == guest_read32(probe + 8u)) {
            return entry;
        }
    }
    return 0u;
}
GAME_REPLACE(003C4730, thiscall, 1, u32, game_find_pointer_matching_fields_4_8)

/* 0x00388972: smallest (unsigned) of the five dwords at object+0x28. */
static uint32_t game_min_of_five_dwords_at_0x28(uint32_t object)
{
    uint32_t smallest = 0xFFFFFFFFu;
    for (uint32_t index = 0u; index < 5u; index++) {
        const uint32_t value = guest_read32(object + 0x28u + index * 4u);
        if (value < smallest) {
            smallest = value;
        }
    }
    return smallest;
}
GAME_REPLACE(00388972, thiscall, 0, u32, game_min_of_five_dwords_at_0x28)

/* 0x00382CD6: a2 points just past a header. A clear bit 0 of byte [a2-0xB] gives -1. With bit
 * 3 set the result is dword [a2-0x18] minus word [a2-0x10], otherwise word [a2-0x10] * 16
 * minus byte [a2-0xA]. */
static uint32_t game_header_size_from_trailer(uint32_t first, uint32_t second, uint32_t trailer)
{
    (void)first;
    (void)second;
    const uint32_t flags = guest_read8(trailer - 0xBu);
    if ((flags & 1u) == 0u) {
        return 0xFFFFFFFFu;
    }
    if ((flags & 8u) != 0u) {
        return guest_read32(trailer - 0x18u) - game_u16_at(trailer - 0x10u);
    }
    return (game_u16_at(trailer - 0x10u) << 4) - guest_read8(trailer - 0xAu);
}
GAME_REPLACE(00382CD6, stdcall, 3, u32, game_header_size_from_trailer)

/* 0x0038282D: pointer p. With bit 3 of byte [p+5] set the result is p - 0x18, otherwise
 * p + word [p] * 16 - 0x10. */
static uint32_t game_record_start_from_header(uint32_t header)
{
    if ((guest_read8(header + 5u) & 8u) != 0u) {
        return header - 0x18u;
    }
    return (game_u16_at(header) << 4) + header - 0x10u;
}
GAME_REPLACE(0038282D, stdcall, 1, u32, game_record_start_from_header)

/* 0x0038DDA5: stores min(sample_count, [object+0xC] * [object+0xA]) as a word at
 * [object+0xF58], where sample_count is the low 16 bits of (first - second) and the product of
 * the two words is signed 32 bit. [object+0xF54] becomes [object+0x274] + 2 * low16(second).
 * Returns 0. */
static uint32_t game_set_sample_window(uint32_t object, uint32_t first, uint32_t second)
{
    uint32_t capacity = game_u16_at(object + 0xCu);
    const uint32_t difference = first - second;
    capacity *= game_u16_at(object + 0xAu);
    game_store_u16(object + 0xF58u, difference);
    uint32_t count = difference & 0xFFFFu;
    if ((int32_t)count >= (int32_t)capacity) {
        count = capacity;
    }
    const uint32_t base = guest_read32(object + 0x274u);
    game_store_u16(object + 0xF58u, count);
    guest_write32(object + 0xF54u, base + (second & 0xFFFFu) * 2u);
    return 0u;
}
GAME_REPLACE(0038DDA5, cdecl, 3, u32, game_set_sample_window)

/* 0x0038BAB0: copies the fields of the record at source to the record at destination with
 * their offsets rearranged, then returns 0 when the three words at destination +0, +8, +0xA
 * sum to the dword at source+4, else 0x19CA. */
static uint32_t game_copy_record_rearranged_and_check_sum(uint32_t source, uint32_t destination)
{
    game_store_u16(destination, game_u16_at(source));
    guest_write32(destination + 0x04u, guest_read32(source + 0x04u));
    guest_write32(destination + 0x08u, guest_read32(source + 0x08u));
    guest_write32(destination + 0x0Cu, guest_read32(source + 0x0Cu));
    guest_write32(destination + 0x10u, guest_read32(source + 0x10u));
    guest_write32(destination + 0x38u, guest_read32(source + 0x14u));
    guest_write32(destination + 0x3Cu, guest_read32(source + 0x18u));
    guest_write32(destination + 0x20u, guest_read32(source + 0x28u));
    guest_write32(destination + 0x28u, guest_read32(source + 0x30u));
    guest_write32(destination + 0x24u, guest_read32(source + 0x2Cu));
    guest_write32(destination + 0x14u, guest_read32(source + 0x1Cu));
    guest_write32(destination + 0x18u, guest_read32(source + 0x20u));
    guest_write32(destination + 0x1Cu, guest_read32(source + 0x24u));
    game_store_u16(destination + 0x40u, game_u16_at(source + 0x34u));
    game_store_u16(destination + 0x42u, game_u16_at(source + 0x36u));
    game_store_u16(destination + 0x44u, game_u16_at(source + 0x38u));
    guest_write32(destination + 0x50u, guest_read32(source + 0x44u));
    guest_write32(destination + 0x4Cu, guest_read32(source + 0x40u));
    guest_write32(destination + 0x48u, guest_read32(source + 0x3Cu));
    const uint32_t sum = game_u16_at(destination) + game_u16_at(destination + 0xAu) +
                         game_u16_at(destination + 8u);
    return sum == guest_read32(source + 4u) ? 0u : 0x19CAu;
}
GAME_REPLACE(0038BAB0, cdecl, 2, u32, game_copy_record_rearranged_and_check_sum)
