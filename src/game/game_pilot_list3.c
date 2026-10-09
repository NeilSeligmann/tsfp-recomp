/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1520 cheap-model pilot, list 3: leaf functions drafted by claude-sonnet-5-5 from the
 * disassembly only. None is trusted because of that: each is admitted only after
 * `python -m tools.replace prove --only-va` passed at O0 and O3 for both seeds 20261001 and
 * 20261006 (single attempt, no best-of-N). Receipt: docs/t-cheap-model-pilot-list3.md
 */
#include "game_replace.h"

/* 0x002586A0: zeroes the dwords at 0x75F5CC, 0x5201E8 and 0x5201EC, returns 0. */
static uint32_t game_clear_three_globals(void)
{
    guest_write32(0x0075F5CCu, 0u);
    guest_write32(0x005201E8u, 0u);
    guest_write32(0x005201ECu, 0u);
    return 0u;
}
GAME_REPLACE(002586A0, cdecl, 0, u32, game_clear_three_globals)

/* 0x0031BAA0: cdecl (index). Only the low byte of index is used. Returns the address of record
 * (index & 0xFF) of 0xE4 bytes, plus 0x288, relative to the base pointer [0x7844A8]. */
static uint32_t game_record_address_by_byte_index(uint32_t index)
{
    const uint32_t base = guest_read32(0x007844A8u);
    return (index & 0xFFu) * 0xE4u + base + 0x288u;
}
GAME_REPLACE(0031BAA0, cdecl, 1, u32, game_record_address_by_byte_index)

/* 0x00351580: pops the stack of dwords at 0x76AAAC whose count is at 0x76A9A0. Returns the
 * popped entry, or 0xFFFFFFFF when the count is 0 (count unchanged). */
static uint32_t game_pool_stack_pop(void)
{
    uint32_t count = guest_read32(0x0076A9A0u);
    if (count == 0u) {
        return 0xFFFFFFFFu;
    }
    count -= 1u;
    guest_write32(0x0076A9A0u, count);
    return guest_read32(count * 4u + 0x0076AAACu);
}
GAME_REPLACE(00351580, cdecl, 0, u32, game_pool_stack_pop)

/* 0x003574C0: cdecl (pointer). Reads index = [pointer], stores 7 at 0x77419C + index * 0x30C.
 * Returns pointer (eax). */
static uint32_t game_indexed_record_set_state_7(uint32_t pointer)
{
    const uint32_t index = guest_read32(pointer);
    guest_write32(index * 0x30Cu + 0x0077419Cu, 7u);
    return pointer;
}
GAME_REPLACE(003574C0, cdecl, 1, u32, game_indexed_record_set_state_7)

/* 0x00390CD0: cdecl (first, second). Stores 0x4A57A8 at *first and 0x59 at *second. Returns
 * first (eax). */
static uint32_t game_store_two_constants_through_pointers(uint32_t first, uint32_t second)
{
    guest_write32(first, 0x004A57A8u);
    guest_write32(second, 0x59u);
    return first;
}
GAME_REPLACE(00390CD0, cdecl, 2, u32, game_store_two_constants_through_pointers)
