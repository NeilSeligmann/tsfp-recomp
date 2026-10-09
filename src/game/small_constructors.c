/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Four field-initialising constructors. `this` arrives in ecx and is returned in eax,
 * which is the thiscall shape. All four are on the boot path: the host reaches them
 * 66, 64, 11 and 3 times before the first Direct3D call.
 */
#include "game_replace.h"

/* A three-word record: two caller-supplied words and a zero. */
#define PAIR_FIRST_OFFSET 0x00u
#define PAIR_SECOND_OFFSET 0x04u
#define PAIR_THIRD_OFFSET 0x08u

static uint32_t game_pair_construct(guest_addr self, uint32_t first, uint32_t second)
{
    guest_write32(self + PAIR_FIRST_OFFSET, first);
    guest_write32(self + PAIR_SECOND_OFFSET, second);
    guest_write32(self + PAIR_THIRD_OFFSET, 0);
    return self;
}

GAME_REPLACE_EXACT(003BA830, thiscall, 2, u32, game_pair_construct)
{
    guest_addr self = g_ecx;
    uint32_t first = game_stack_arg(0);
    uint32_t second = game_stack_arg(1);
    g_eax = game_pair_construct(self, first, second);
    g_ecx = first;
    g_edx = second;
}

/* The same record shape with the second word fixed to a guest data address and the
 * caller's second argument stored in the third slot. 0x004766D8 is a constant in the
 * original's own data and is left as the address it is. */
#define TAGGED_PAIR_TAG_ADDRESS 0x004766D8u

static uint32_t game_tagged_pair_construct(guest_addr self, uint32_t first, uint32_t third)
{
    guest_write32(self + PAIR_FIRST_OFFSET, first);
    guest_write32(self + PAIR_THIRD_OFFSET, third);
    guest_write32(self + PAIR_SECOND_OFFSET, TAGGED_PAIR_TAG_ADDRESS);
    return self;
}

GAME_REPLACE_EXACT(003BA850, thiscall, 2, u32, game_tagged_pair_construct)
{
    guest_addr self = g_ecx;
    uint32_t first = game_stack_arg(0);
    uint32_t third = game_stack_arg(1);
    g_eax = game_tagged_pair_construct(self, first, third);
    g_ecx = first;
    g_edx = third;
}

/* Resets a 0x50-byte record to its idle state: most words zero, a mode word of 3 and an
 * "invalid index" of -1.
 *
 * Only the first byte at +0x00 is cleared, not the whole dword: the original stores the
 * low byte of its zero register there and the three bytes above it are left as they
 * were. The bytes between the cleared words (+0x20..+0x27, +0x30..+0x4B) are not touched
 * either, so the write set is exactly the stores listed.
 */
static uint32_t game_idle_record_construct(guest_addr self)
{
    guest_write8(self + 0x00u, 0);
    for (guest_addr offset = 0x04u; offset <= 0x1Cu; offset += 4u) {
        guest_write32(self + offset, 0);
    }
    guest_write32(self + 0x2Cu, 0);
    guest_write32(self + 0x28u, 3u);
    guest_write32(self + 0x4Cu, 0xFFFFFFFFu);
    return self;
}

GAME_REPLACE_EXACT(00029870, thiscall, 0, u32, game_idle_record_construct)
{
    g_eax = game_idle_record_construct(g_ecx);
    g_ecx = 0;
}

/* A small object that points at a guest-side table and starts with three zero words. */
#define TABLE_OBJECT_TABLE_ADDRESS 0x004B3170u

static uint32_t game_table_object_construct(guest_addr self)
{
    guest_write32(self + 0x00u, TABLE_OBJECT_TABLE_ADDRESS);
    guest_write32(self + 0x0Cu, 0);
    guest_write32(self + 0x04u, 0);
    guest_write32(self + 0x08u, 0);
    return self;
}

GAME_REPLACE_EXACT(003A9D20, thiscall, 0, u32, game_table_object_construct)
{
    g_eax = game_table_object_construct(g_ecx);
    g_ecx = 0;
}
