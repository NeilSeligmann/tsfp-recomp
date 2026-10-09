/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A 64-bit flag set stored as two adjacent 32-bit words, tested against a 64-bit mask.
 */
#include "game_replace.h"

#define FLAGSET_LOW_WORD_OFFSET 0x10u
#define FLAGSET_HIGH_WORD_OFFSET 0x14u

/* True when any bit of the 64-bit mask (high:low) is set in the object's flag set.
 *
 * The two halves are tested separately and combined with OR, so there is no 64-bit
 * arithmetic and nothing to carry between the halves. The result is normalised to 0 or 1.
 * Pure read: the original writes no guest memory.
 */
static uint32_t game_flagset64_intersects(guest_addr object, uint32_t mask_low,
                                          uint32_t mask_high)
{
    uint32_t low = guest_read32(object + FLAGSET_LOW_WORD_OFFSET) & mask_low;
    uint32_t high = guest_read32(object + FLAGSET_HIGH_WORD_OFFSET) & mask_high;
    return (low | high) != 0;
}

GAME_REPLACE_EXACT(00017510, cdecl, 3, u32, game_flagset64_intersects)
{
    guest_addr object = game_stack_arg(0);
    uint32_t mask_low = game_stack_arg(1);
    uint32_t mask_high = game_stack_arg(2);
    g_eax = game_flagset64_intersects(object, mask_low, mask_high);
    g_ecx = guest_read32(object + FLAGSET_HIGH_WORD_OFFSET) & mask_high;
    g_edx = mask_low;
}
