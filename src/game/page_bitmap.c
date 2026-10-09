/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Clears a set of bits in a one-byte mask according to which 4 KiB page an address is in.
 */
#include "game_replace.h"

#define PAGE_REGION_BASE 0x007E72E0u
#define PAGE_SIZE_BYTES 4096
#define PAGE_BIT_TABLE_ADDRESS 0x006B7AF4u
#define PAGE_MASK_BYTE_ADDRESS 0x006B7B00u

/* Clears from the mask byte every bit that the address's page entry has set.
 *
 * The page index is the SIGNED distance from the region base divided by 4096, rounded
 * toward zero. The original does that with `cdq; and edx,0xFFF; add; sar 12`, which is
 * the classic signed divide by a power of two, and C's `/` is defined to truncate toward
 * zero in exactly the same way. An address BELOW the base therefore gives a negative or
 * zero index and reads the byte table at a lower address than its start, as the original
 * does. Only the low byte of the table entry matters, so the entry is a byte.
 */
typedef struct page_bitmap_result {
    int32_t page_index;
    uint8_t keep_bits;
    uint8_t remaining_mask;
} page_bitmap_result;

static page_bitmap_result game_page_bitmap_clear(uint32_t address)
{
    int32_t distance = (int32_t)(address - PAGE_REGION_BASE);
    int32_t page_index = distance / PAGE_SIZE_BYTES;
    uint8_t entry = guest_read8(PAGE_BIT_TABLE_ADDRESS + (uint32_t)page_index);
    uint8_t mask = guest_read8(PAGE_MASK_BYTE_ADDRESS);
    uint8_t keep_bits = (uint8_t)~entry;
    uint8_t remaining_mask = (uint8_t)(mask & keep_bits);
    guest_write8(PAGE_MASK_BYTE_ADDRESS, remaining_mask);
    return (page_bitmap_result){page_index, keep_bits, remaining_mask};
}

GAME_REPLACE_EXACT(0003D6B0, cdecl, 1, void, game_page_bitmap_clear)
{
    uint32_t address = game_stack_arg(0);
    page_bitmap_result result = game_page_bitmap_clear(address);
    /* cdq / and edx,0xfff leaves 0xf00 in the upper bytes when the distance is
     * negative; mov dl and and dl replace only the low byte. Likewise mov al and
     * not al preserve the upper bytes of the signed page index in eax. */
    g_eax = ((uint32_t)result.page_index & ~0xffu) | result.keep_bits;
    g_edx = ((int32_t)(address - PAGE_REGION_BASE) < 0 ? 0xf00u : 0u) |
            result.remaining_mask;
}
