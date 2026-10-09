/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two functions that put a record back to its default contents.
 */
#include "game_replace.h"

#define NO_INDEX 0xFFFFFFFFu

/* Zeroes the first 0x17 dwords (92 bytes) of a record, then marks six fields as unset.
 *
 * Four of them become -1 and one stays 0; the order is the original's and does not
 * matter for the result. The original zeroes with `rep stosd`, which walks DOWNWARD if
 * the direction flag is set. The ABI requires the flag clear at every call boundary and
 * the harness never sets it, so this assumes the upward walk and the DF=1 case is NOT
 * covered.
 */
#define RECORD_ZEROED_DWORDS 0x17u

static void game_record_reset(guest_addr record)
{
    for (uint32_t index = 0; index < RECORD_ZEROED_DWORDS; index++) {
        guest_write32(record + 4u * index, 0);
    }
    guest_write32(record + 0x14u, NO_INDEX);
    guest_write32(record + 0x18u, NO_INDEX);
    guest_write32(record + 0x24u, NO_INDEX);
    guest_write32(record + 0x44u, NO_INDEX);
    guest_write32(record + 0x48u, 0);
    guest_write32(record + 0x1Cu, NO_INDEX);
}

GAME_REPLACE_EXACT(000B91F0, cdecl, 1, void, game_record_reset)
{
    guest_addr record = game_stack_arg(0);
    game_record_reset(record);
    g_eax = NO_INDEX;
    g_ecx = 0;
    g_edx = record;
}

/* Fills one 13-dword slot of a global array with the float -0.2.
 *
 * The slot is chosen by a guest global holding the current index, and slots are 0x1E0
 * bytes apart. The index is multiplied in 32 bits, so an index above 0x88888 wraps just
 * as the original's `imul` does. 0xBE4CCCCD is the IEEE-754 single for -0.2.
 */
#define SLOT_ARRAY_ADDRESS 0x007BA2DCu
#define SLOT_INDEX_ADDRESS 0x007BA9E0u
#define SLOT_STRIDE_BYTES 0x1E0u
#define SLOT_FILL_WORDS 13u
#define NEGATIVE_ONE_FIFTH_BITS 0xBE4CCCCDu

static guest_addr game_slot_fill_default(void)
{
    guest_addr slot = SLOT_ARRAY_ADDRESS + guest_read32(SLOT_INDEX_ADDRESS) * SLOT_STRIDE_BYTES;
    for (uint32_t word = 0; word < SLOT_FILL_WORDS; word++) {
        guest_write32(slot + 4u * word, NEGATIVE_ONE_FIFTH_BITS);
    }
    return slot;
}

GAME_REPLACE_EXACT(00073C80, cdecl, 0, void, game_slot_fill_default)
{
    g_eax = game_slot_fill_default();
    g_ecx = NEGATIVE_ONE_FIFTH_BITS;
}
