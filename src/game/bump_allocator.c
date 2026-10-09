/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A bump allocator over a region the title set up elsewhere.
 */
#include "game_replace.h"

/* The two guest globals the allocator maintains: where the next block starts, and how
 * many bytes have been handed out so far. */
#define BUMP_CURSOR_ADDRESS 0x006B839Cu
#define BUMP_TOTAL_ADDRESS 0x006B83B8u
#define BUMP_ALIGNMENT 16u

/* Hands out `size` bytes rounded up to a multiple of 16 and returns the block's address.
 *
 * Nothing is checked: there is no bounds test and no zeroing, exactly as in the original.
 * The rounding is done in 32 bits, so a size within 15 of 2^32 wraps to a small block
 * (size + 15 overflows) and the cursor and total advance by that small amount. That
 * wraparound is part of the contract callers see, so it is kept and not "fixed".
 */
static uint32_t game_bump_allocate(uint32_t size)
{
    uint32_t block_size = (size + (BUMP_ALIGNMENT - 1u)) & ~(BUMP_ALIGNMENT - 1u);
    uint32_t block = guest_read32(BUMP_CURSOR_ADDRESS);
    guest_write32(BUMP_CURSOR_ADDRESS, block + block_size);
    guest_write32(BUMP_TOTAL_ADDRESS, guest_read32(BUMP_TOTAL_ADDRESS) + block_size);
    return block;
}

GAME_REPLACE_EXACT(0003E970, cdecl, 1, u32, game_bump_allocate)
{
    uint32_t size = game_stack_arg(0);
    g_eax = game_bump_allocate(size);
    g_ecx = (size + (BUMP_ALIGNMENT - 1u)) & ~(BUMP_ALIGNMENT - 1u);
    g_edx = g_eax + g_ecx;
}
