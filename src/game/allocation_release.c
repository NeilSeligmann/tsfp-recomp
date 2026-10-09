/* SPDX-License-Identifier: GPL-3.0-or-later
 * Original 0x0003E4E0: release a block from the title's allocation ledger.
 * A middle release is a tombstone; releasing the newest block also reclaims
 * consecutive, previously released predecessors with positive stored sizes.
 */
#include "game_replace.h"

#define ALLOCATION_ROWS 0x006B7B68u
#define ALLOCATION_BYTES 0x006B8394u
#define ALLOCATION_COUNT 0x006B8398u

static int positive_word(uint32_t value)
{
    return value != 0u && (value & 0x80000000u) == 0u;
}

static void release_allocation(uint32_t count, uint32_t block)
{
    uint32_t last = count - 1u;
    uint32_t pointer = guest_read32(ALLOCATION_ROWS + last * 8u);
    g_eax = last;
    g_ecx = pointer;
    g_edx = block;

    if (block == pointer) {
        uint32_t bytes = guest_read32(ALLOCATION_BYTES);
        bytes -= guest_read32(ALLOCATION_ROWS + last * 8u + 4u);
        guest_write32(ALLOCATION_BYTES, bytes);
        /* The original always marks the removed size, including count zero's
         * wrapped row. Do not add a bounds check or silently ignore it. */
        for (;;) {
            --last;
            guest_write32(ALLOCATION_ROWS + last * 8u + 12u, UINT32_MAX);
            if ((last & 0x80000000u) != 0u) break;
            pointer = guest_read32(ALLOCATION_ROWS + last * 8u);
            g_ecx = pointer;
            if (pointer != 0u) break;
            uint32_t size = guest_read32(ALLOCATION_ROWS + last * 8u + 4u);
            g_ecx = size;
            if (!positive_word(size)) break;
            bytes -= size;
        }
        g_eax = last + 1u;
        g_edx = bytes;
        guest_write32(ALLOCATION_BYTES, bytes);
        guest_write32(ALLOCATION_COUNT, g_eax);
        return;
    }

    g_ecx = 0u;
    if (!positive_word(last)) return;
    do {
        if (guest_read32(ALLOCATION_ROWS + g_ecx * 8u) == block) {
            guest_write32(ALLOCATION_ROWS + g_ecx * 8u, 0u);
            return;
        }
        ++g_ecx;
    } while (g_ecx < last);
}

GAME_REPLACE_EXACT(0003E4E0, cdecl, 1, void, game_release_allocation)
{
    /* Original read ordering: count, stack argument, then newest pointer. */
    uint32_t count = guest_read32(ALLOCATION_COUNT);
    uint32_t block = game_stack_arg(0);
    release_allocation(count, block);
}
