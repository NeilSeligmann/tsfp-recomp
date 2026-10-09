/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1475, band 0x00000000-0x00080000: table resets, a free-slot finder, a slot-id clear,
 * a blank-string predicate, an address-table filler and a charset selector. Each registers a
 * call-free body. Names describe observed behavior only.
 * Receipt: docs/t77-band-0-80000.md
 */
#include "game_replace.h"

#define SLOT_ID_OFFSET 0x128u
#define SLOT_STRIDE 0x10u
#define SLOT_COUNT 4u

/* 0x00021060: sets the id dword of the first [0x5608C0] sixteen-byte entries at 0x55FC98 to
 * -1 (nothing when the count is not positive), sets 0x4B8D20 and 0x4B8D24 to -1 and clears
 * 0x5608CC. EAX is left scratch. */
static void game_table_55fc98_reset_ids_to_minus1(void)
{
    const int32_t count = (int32_t)guest_read32(0x005608C0u);
    for (int32_t index = 0; index < count; index++) {
        guest_write32(0x0055FC98u + (uint32_t)index * 0x10u, 0xFFFFFFFFu);
    }
    guest_write32(0x004B8D20u, 0xFFFFFFFFu);
    guest_write32(0x005608CCu, 0u);
    guest_write32(0x004B8D24u, 0xFFFFFFFFu);
}
GAME_REPLACE(00021060, cdecl, 0, void, game_table_55fc98_reset_ids_to_minus1)

/* 0x00028AF0: when [0x4B9138] is nonzero, sets the forty dwords at 0x6612D0 to -1 and
 * clears it. Returns the index of the first dword equal to -1 (0 right after a reset), or -1
 * when none is. Does not reserve the slot. */
static uint32_t game_file_handle_slot_find_free(void)
{
    if (guest_read32(0x004B9138u) != 0u) {
        for (uint32_t index = 0u; index < 0x28u; index++) {
            guest_write32(0x006612D0u + index * 4u, 0xFFFFFFFFu);
        }
        guest_write32(0x004B9138u, 0u);
    }
    for (uint32_t index = 0u; index < 0x28u; index++) {
        if (guest_read32(0x006612D0u + index * 4u) == 0xFFFFFFFFu) {
            return index;
        }
    }
    return 0xFFFFFFFFu;
}
GAME_REPLACE(00028AF0, cdecl, 0, u32, game_file_handle_slot_find_free)

/* 0x00074840: stores -1 in the four slot ids (+0x128, stride 0x10) of `object`. Returns
 * `object`. */
static uint32_t game_obj_clear_slot_ids_0x128_to_minus1(uint32_t object)
{
    for (uint32_t slot = 0u; slot < SLOT_COUNT; slot++) {
        guest_write32(object + SLOT_ID_OFFSET + slot * SLOT_STRIDE, 0xFFFFFFFFu);
    }
    return object;
}
GAME_REPLACE(00074840, cdecl, 1, u32, game_obj_clear_slot_ids_0x128_to_minus1)

/* 0x0007C4D0: `text` arrives in ECX. 1 when the string is empty or only spaces (0x20), else 0. */
static uint32_t game_str_is_empty_or_spaces_ecx(uint32_t text)
{
    while (guest_read8(text) == 0x20u) {
        text++;
    }
    return guest_read8(text) == 0u;
}
GAME_REPLACE(0007C4D0, thiscall, 0, u32, game_str_is_empty_or_spaces_ecx)

/* 0x00032550: fills `table` with ten address pairs. Pair k holds a fixed address at +8k and
 * at +8k+4 one of two addresses, 0x4768E8 when flag bit k of `flags` is set (bits 0-6, then
 * 0x200 for pair 7 and 0x100 for pair 8), else 0x4768EC. Pair 9 is two fixed addresses. Returns `table`. */
static uint32_t game_init_flag_selected_address_table(uint32_t table, uint32_t flags)
{
    static const uint32_t fixed[9] = {0x00476890u, 0x00476898u, 0x0047689Cu, 0x004768A8u,
                                      0x004768B0u, 0x004768B8u, 0x004768C0u, 0x004768CCu,
                                      0x004768D4u};
    static const uint32_t flag_masks[9] = {0x01u, 0x02u, 0x04u, 0x08u, 0x10u, 0x20u, 0x40u,
                                           0x200u, 0x100u};
    for (uint32_t pair = 0u; pair < 9u; pair++) {
        guest_write32(table + pair * 8u, fixed[pair]);
        guest_write32(table + pair * 8u + 4u,
                      (flags & flag_masks[pair]) != 0u ? 0x004768E8u : 0x004768ECu);
    }
    guest_write32(table + 0x48u, 0x004768DCu);
    guest_write32(table + 0x4Cu, 0x004768F8u);
    return table;
}
GAME_REPLACE(00032550, cdecl, 2, u32, game_init_flag_selected_address_table)

/* 0x0007C1B0: `out` arrives in ECX (may be null) and `flags` in EDX. Picks one of two
 * five-row tables of (record, companion) address pairs by the sign of the low byte of
 * `flags`, then a row: row 0 by default, row 1 when exactly one of bits 1 and 2 is set,
 * row 2 when bit 3 is set (decided at once), else row 3 when bit 12 is set, upgraded to row 4
 * when exactly one of bits 1 and 2 is set. Stores the chosen companion at `out` when it is
 * not null and returns the chosen record address. */
static uint32_t game_font_charset_select(uint32_t out, uint32_t flags)
{
    static const uint32_t rows[2][5][2] = {
        {{0x004D8DD0u, 0x004D8E74u},
         {0x004D8EA0u, 0x004D8F44u},
         {0x004D8F70u, 0x004D9014u},
         {0x004D9040u, 0x004D90E4u},
         {0x004D9110u, 0x004D91B4u}},
        {{0x004D91E0u, 0x004D9254u},
         {0x004D9278u, 0x004D92ECu},
         {0x004D9310u, 0x004D9384u},
         {0x004D93A8u, 0x004D941Cu},
         {0x004D9440u, 0x004D94B4u}},
    };
    const uint32_t (*table)[2] = rows[(flags & 0x80u) != 0u];
    const int mixed = (flags & 6u) == 2u || (flags & 6u) == 4u;
    uint32_t row = 0u;
    if (mixed) {
        row = 1u;
    }
    if ((flags & 8u) != 0u) {
        row = 2u;
    } else if ((flags & 0x1000u) != 0u) {
        row = mixed ? 4u : 3u;
    }
    if (out != 0u) {
        guest_write32(out, table[row][1]);
    }
    return table[row][0];
}
GAME_REPLACE(0007C1B0, fastcall, 0, u32, game_font_charset_select)
