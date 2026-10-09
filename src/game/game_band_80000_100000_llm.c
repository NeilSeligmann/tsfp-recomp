/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1486 pilot, band 0x00080000-0x00100000: two replacements drafted by gpt-5.5 (OpenAI API) from
 * the function's disassembly. Neither is trusted because of that: each passed
 * `python -m tools.replace prove --only-va` (single documented run, O0 and O3) AND was read line
 * by line against the original instructions by a human-supervised agent before registration.
 * Receipt: docs/t1486-llm-replacement-pilot.md
 */
#include "game_replace.h"

/* 0x00094290: stdcall-shaped cdecl (object, value). Stores the low 16 bits of value at
 * object+0x7C, loads the header pointer [object], sets bit 2 of the dword at header+0x18 and
 * returns the header (EAX). ECX is scratch (the original loads CX). */
static uint32_t game_object_set_word_7c_and_header_flag_bit2(uint32_t object, uint32_t value)
{
    guest_write8(object + 0x7Cu, (uint8_t)(value & 0xFFu));
    guest_write8(object + 0x7Du, (uint8_t)((value >> 8) & 0xFFu));

    const uint32_t header = guest_read32(object);
    guest_write32(header + 0x18u, guest_read32(header + 0x18u) | 4u);
    return header;
}
GAME_REPLACE(00094290, cdecl, 2, u32, game_object_set_word_7c_and_header_flag_bit2)

/* 0x000A2C60: cdecl (previous, type). Slots are 0x2E8 bytes, count at [0x7B4FAC], first slot
 * pointer at [0x72F5E8]. The scan starts one slot after `previous` (after the first slot when
 * previous is 0), wraps to the first slot, and returns the first slot whose dword at +0x20
 * equals type, or (type == -1) is not 2. It returns 0 on reaching `previous` again or after the
 * fourth wrap. */
static uint32_t game_object_slot_find_next_by_type_field_0x20_cyclic(uint32_t previous, uint32_t type)
{
    const uint32_t slot_count = guest_read32(0x007B4FACu);
    const uint32_t first_slot = guest_read32(0x0072F5E8u);
    const uint32_t last_slot = first_slot + slot_count * 0x2E8u - 0x2E8u;
    uint32_t wrap_count = 0u;
    uint32_t slot = (previous != 0u) ? previous : first_slot;

    slot += 0x2E8u;
    if (slot > last_slot) {
        slot = first_slot;
        wrap_count = 1u;
    }

    for (;;) {
        if (slot == previous) {
            return 0u;
        }
        const uint32_t slot_type = guest_read32(slot + 0x20u);
        if (slot_type == type) {
            return slot;
        }
        if (type == 0xFFFFFFFFu && slot_type != 2u) {
            return slot;
        }
        slot += 0x2E8u;
        if (slot > last_slot) {
            slot = first_slot;
            wrap_count += 1u;
        }
        if ((int32_t)wrap_count >= 4) {
            return 0u;
        }
    }
}
GAME_REPLACE(000A2C60, cdecl, 2, u32, game_object_slot_find_next_by_type_field_0x20_cyclic)
