/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Call-free store, list and counter leaves in the 0x00000000-0x00180000 range of the
 * retail XBE. Names are neutral: no subsystem meaning is inferred.
 * Receipt: docs/t77-low-range-scalar-leaves.md
 */
#include "game_replace.h"

/* 0x00063C60: [dst] = base, [dst+4] = base + length, returns the end. */
static uint32_t game_store_span(uint32_t destination, uint32_t base, uint32_t length)
{
    guest_write32(destination, base);
    guest_write32(destination + 4u, base + length);
    return base + length;
}
GAME_REPLACE(00063C60, cdecl, 3, u32, game_store_span)

/* 0x0010E6C0: when obj is nonnull, [obj+0x78] = [obj+0x7C] = value. Returns obj. */
static uint32_t game_store_pair_78(uint32_t object, uint32_t value)
{
    if (object != 0u) {
        guest_write32(object + 0x78u, value);
        guest_write32(object + 0x7Cu, value);
    }
    return object;
}
GAME_REPLACE(0010E6C0, cdecl, 2, u32, game_store_pair_78)

/* 0x0010E780: when obj is nonnull, byte [obj+0x13] = low byte of value. Returns obj. */
static uint32_t game_store_byte_13(uint32_t object, uint32_t value)
{
    if (object != 0u) {
        guest_write8(object + 0x13u, (uint8_t)value);
    }
    return object;
}
GAME_REPLACE(0010E780, cdecl, 2, u32, game_store_byte_13)

/* 0x001665E0: pushes `node` onto the list headed at `head`: [node+0x34] = [head],
 * [head] = node. Returns head. */
static uint32_t game_push_node_34(uint32_t head, uint32_t node)
{
    guest_write32(node + 0x34u, guest_read32(head));
    guest_write32(head, node);
    return head;
}
GAME_REPLACE(001665E0, cdecl, 2, u32, game_push_node_34)

/* 0x00156E10: pops the node at [list+4] (if any): [list+4] = [node], [list+8] -= 1.
 * Returns the popped node or 0. */
static uint32_t game_pop_node(uint32_t list)
{
    const uint32_t node = guest_read32(list + 4u);
    if (node != 0u) {
        guest_write32(list + 4u, guest_read32(node));
        guest_write32(list + 8u, guest_read32(list + 8u) - 1u);
    }
    return node;
}
GAME_REPLACE(00156E10, cdecl, 1, u32, game_pop_node)

/* Shared body of the four bounded pair-append leaves: when the signed count is below the
 * limit, store (second, first) at array[count*8], bump the count, and return the new
 * count, else return the unchanged count. */
static uint32_t game_append_pair(uint32_t count_address, uint32_t array, uint32_t limit,
                                 uint32_t first, uint32_t second)
{
    uint32_t count = guest_read32(count_address);
    if ((int32_t)count >= (int32_t)limit) {
        return count;
    }
    guest_write32(array + count * 8u, second);
    guest_write32(array + count * 8u + 4u, first);
    count++;
    guest_write32(count_address, count);
    return count;
}

/* 0x00160820: pair array 0x007A2960, count at 0x007A2BCC, limit 0x37. */
static uint32_t game_append_pair_7a2960(uint32_t first, uint32_t second)
{
    return game_append_pair(0x007A2BCCu, 0x007A2960u, 0x37u, first, second);
}
GAME_REPLACE(00160820, cdecl, 2, u32, game_append_pair_7a2960)

/* 0x00160910: pair array 0x007A3620, count at 0x0074C398, limit 0x14. */
static uint32_t game_append_pair_7a3620(uint32_t first, uint32_t second)
{
    return game_append_pair(0x0074C398u, 0x007A3620u, 0x14u, first, second);
}
GAME_REPLACE(00160910, cdecl, 2, u32, game_append_pair_7a3620)

/* 0x00160940: pair array 0x007A36E0, count at 0x0074C390, limit 0x19. */
static uint32_t game_append_pair_7a36e0(uint32_t first, uint32_t second)
{
    return game_append_pair(0x0074C390u, 0x007A36E0u, 0x19u, first, second);
}
GAME_REPLACE(00160940, cdecl, 2, u32, game_append_pair_7a36e0)

/* 0x00160970: pair array 0x007A2C60, count at 0x0074C394, limit 0x14. */
static uint32_t game_append_pair_7a2c60(uint32_t first, uint32_t second)
{
    return game_append_pair(0x0074C394u, 0x007A2C60u, 0x14u, first, second);
}
GAME_REPLACE(00160970, cdecl, 2, u32, game_append_pair_7a2c60)

/* 0x00155530: dword i of the 24-entry table at 0x007497E8 becomes 0x168A + i. Returns 24. */
static uint32_t game_fill_table_7497e8(void)
{
    for (uint32_t index = 0u; index < 0x18u; index++) {
        guest_write32(0x007497E8u + index * 4u, 0x168Au + index);
    }
    return 0x18u;
}
GAME_REPLACE(00155530, cdecl, 0, u32, game_fill_table_7497e8)
