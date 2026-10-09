/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Register-exact (GAME_REPLACE_EXACT) call-free helpers from retail XBE range
 * 0x00180000-0x00470000 (T77 round 4, batch C). Their first GAME_REPLACE form was refused by
 * the caller audit because a caller reads eax, ecx or edx after the call, so these reproduce
 * eax, ecx and edx exactly as the original leaves them (partial register writes included).
 * Field names state offsets only. Proofs and rejects: docs/t77-round4-high.md.
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

/* 0x0037F6AC: copies the zero terminated string at a0 to a1, then appends the zero terminated
 * string at 0x4A1824 over the first terminator. eax = length + a1 - 0x4A1824, ecx = the end of
 * the constant string, edx keeps its upper 24 bits and has dl = 0. */
GAME_REPLACE_EXACT(0037F6AC, stdcall, 2, u32, game_copy_string_append_constant_4a1824)
{
    const uint32_t source = game_stack_arg(0u);
    const uint32_t destination = game_stack_arg(1u);
    uint32_t length = 0u;
    while (guest_read8(source + length) != 0u) {
        length++;
    }
    uint32_t value;
    uint32_t offset = 0u;
    do {
        value = guest_read8(source + offset);
        guest_write8(destination + offset, (uint8_t)value);
        offset++;
    } while (value != 0u);
    uint32_t cursor = 0x004A1824u;
    do {
        value = guest_read8(cursor);
        guest_write8(destination + length + (cursor - 0x004A1824u), (uint8_t)value);
        cursor++;
    } while (value != 0u);
    g_eax = length + destination - 0x004A1824u;
    g_ecx = cursor;
    g_edx = (g_edx & 0xFFFFFF00u) | value;
}

/* 0x0038726D: when [object+4] is zero, [object+0x28] becomes 3 if it was 1 and 2 otherwise.
 * Always clears [object+0x24] and returns 0. ecx = object, edx unchanged. */
GAME_REPLACE_EXACT(0038726D, thiscall, 0, u32, game_toggle_mode_28_and_clear_24)
{
    const uint32_t object = g_ecx;
    if (guest_read32(object + 4u) == 0u) {
        guest_write32(object + 0x28u, guest_read32(object + 0x28u) == 1u ? 3u : 2u);
    }
    guest_write32(object + 0x24u, 0u);
    g_eax = 0u;
}

/* 0x0038A2B8: when [object+0xFA0] is nonzero the pair at +0xF88/+0xF8C is copied to
 * +0xF80/+0xF84 and the pair at +0xF90/+0xF94 is cleared. [object+0xF9C] is always cleared.
 * eax = object, ecx = 0, edx = [object+0xF8C] when the copy happened. */
GAME_REPLACE_EXACT(0038A2B8, cdecl, 1, u32, game_shift_pair_f88_to_f80_clear_f9c)
{
    const uint32_t object = game_stack_arg(0u);
    g_eax = object;
    g_ecx = 0u;
    if (guest_read32(object + 0xFA0u) != 0u) {
        guest_write32(object + 0xF80u, guest_read32(object + 0xF88u));
        g_edx = guest_read32(object + 0xF8Cu);
        guest_write32(object + 0xF84u, g_edx);
        guest_write32(object + 0xF90u, 0u);
        guest_write32(object + 0xF94u, 0u);
    }
    guest_write32(object + 0xF9Cu, 0u);
}

/* 0x003BB730: adds the length of the string plus one to [object+4] (a null string adds
 * zero). Returns that amount in eax, ecx = object, edx = the new total (null string: edx is
 * unchanged, non-null string: edx = new [object+4]). */
GAME_REPLACE_EXACT(003BB730, thiscall, 1, u32, game_add_string_size_to_total)
{
    const uint32_t object = g_ecx;
    const uint32_t text = game_stack_arg(0u);
    if (text == 0u) {
        /* The original adds zero in place (`add [ecx+4], eax`), which still reads and writes
         * the dword. The volatile read keeps the access, an optimizer would drop a no-op. */
        const uint32_t total = *(const volatile uint32_t *)game_host_ptr(object + 4u);
        guest_write32(object + 4u, total);
        g_eax = 0u;
        return;
    }
    uint32_t length = 0u;
    while (guest_read8(text + length) != 0u) {
        length++;
    }
    g_edx = guest_read32(object + 4u) + length + 1u;
    guest_write32(object + 4u, g_edx);
    g_eax = length + 1u;
}

/* 0x0038F510: reads `count` bits (at most 32, none for count <= 0) most significant bit first
 * from the byte string at a0 starting at bit position a1, and returns them as a number. The
 * stack slot of the count argument is left holding 0 after a read, as the original counts it
 * down in place. ecx = twice the result before the last bit (count - 1 for no bits), edx = the
 * bit index within its byte after the last bit. */
GAME_REPLACE_EXACT(0038F510, cdecl, 3, u32, game_read_bits_msb_first)
{
    const uint32_t data = game_stack_arg(0u);
    const uint32_t bit_position = game_stack_arg(1u);
    uint32_t remaining = game_stack_arg(2u);
    if ((int32_t)remaining > 0x20) {
        remaining = 0x20u;
    }
    uint32_t cursor = data + (uint32_t)((int32_t)bit_position >> 3);
    uint32_t bit = bit_position & 7u;
    uint32_t result = 0u;
    g_edx = bit;
    if (((remaining - 1u) & 0x80000000u) != 0u) {
        /* The original tests the sign of count - 1, so 0x80000000 still reads (2^31 bits). */
        g_ecx = remaining - 1u;
        g_eax = 0u;
        return;
    }
    guest_write32(g_esp + 12u, remaining);
    do {
        const uint32_t shift = 7u - bit;
        const uint32_t selected = ((1u << shift) & guest_read8(cursor)) >> shift;
        g_ecx = result << 1;
        result = selected | g_ecx;
        bit++;
        if (bit >= 8u) {
            bit = 0u;
            cursor++;
        }
        g_edx = bit;
        remaining--;
        guest_write32(g_esp + 12u, remaining);
    } while (remaining != 0u);
    g_eax = result;
}

/* 0x00271AB0: removes the entry whose first dword equals the key from the 8 byte entries at
 * object+0xDC (signed 16 bit count at +0x10), shifting the later entries down, then clears
 * the vacated last slot, stores 0xFFFFFC18 in its second dword and decrements the count. The
 * ending registers follow the path taken: edx = object, eax and ecx as the original leaves
 * them (the count before the decrement after a removal, the entry address and index after a
 * failed search, the count word in the low half of eax for an empty table). */
GAME_REPLACE_EXACT(00271AB0, cdecl, 2, u32, game_remove_keyed_entry_8)
{
    const uint32_t object = game_stack_arg(0u);
    const uint32_t key = game_stack_arg(1u);
    g_edx = object;
    const uint32_t first_word = game_u16_at(object + 0x10u);
    g_eax = (g_eax & 0xFFFF0000u) | first_word;
    if (first_word == 0u) {
        return;
    }
    const int32_t first_count = (int16_t)first_word;
    g_ecx = 0u;
    if (first_count <= 0) {
        return;
    }
    int32_t index = 0;
    uint32_t entry = object + 0xDCu;
    g_eax = entry;
    while (guest_read32(g_eax) != key) {
        const int32_t current = (int16_t)game_u16_at(object + 0x10u);
        index++;
        g_ecx = (uint32_t)index;
        g_eax += 8u;
        if (index >= current) {
            return;
        }
    }
    g_ecx = (uint32_t)index;
    int32_t limit = first_count - 1;
    if (index < limit) {
        g_eax = object + (uint32_t)index * 8u + 0xDCu;
        do {
            guest_write32(g_eax, guest_read32(g_eax + 8u));
            guest_write32(g_eax + 4u, guest_read32(g_eax + 0xCu));
            limit = (int16_t)game_u16_at(object + 0x10u);
            index++;
            g_eax += 8u;
            limit--;
        } while (index < limit);
        g_ecx = (uint32_t)index;
    }
    g_eax = (uint32_t)(int32_t)(int16_t)game_u16_at(object + 0x10u);
    guest_write32(object + g_eax * 8u + 0xD4u, 0u);
    g_ecx = (uint32_t)(int32_t)(int16_t)game_u16_at(object + 0x10u);
    guest_write32(object + g_ecx * 8u + 0xD8u, 0xFFFFFC18u);
    game_store_u16(object + 0x10u, game_u16_at(object + 0x10u) - 1u);
}
