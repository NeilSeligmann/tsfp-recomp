/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Small call-free object and record helpers from retail XBE range 0x00180000-0x00470000
 * (T77 round 4). Field names state offsets only, no game meaning is assigned beyond the
 * INFERRED names in tools/data/function_names.csv. Where a caller audit found a site that
 * reads ecx or edx after the call the body is a register-exact GAME_REPLACE_EXACT, the others
 * are plain GAME_REPLACE with eax as the only result. Proofs and rejects:
 * docs/t77-round4-high.md.
 */
#include "game_replace.h"

/* 0x001B6310: sets bit 2 of [object+0x34]. When [object+0xE8] equals [object+0xF8] the
 * latter is cleared. Returns the object. */
static uint32_t game_object_set_flag_0x34_bit2_clear_0xf8_if_equals_0xe8(uint32_t object)
{
    const uint32_t flags = guest_read32(object + 0x34u);
    const uint32_t pending = guest_read32(object + 0xF8u);
    guest_write32(object + 0x34u, flags | 4u);
    if (guest_read32(object + 0xE8u) == pending) {
        guest_write32(object + 0xF8u, 0u);
    }
    return object;
}
GAME_REPLACE(001B6310, cdecl, 1, u32,
             game_object_set_flag_0x34_bit2_clear_0xf8_if_equals_0xe8)

/* 0x001B75B0: block = object + 0x184 (selector zero) or object + 0x55C. Returns the dword
 * at block+0x68 when the dword at block+0x54 is nonzero, else 0. */
static uint32_t game_object_select_block_0x184_or_0x55c_get_field_0x68_if_field_0x54_set(
    uint32_t object, uint32_t selector)
{
    const uint32_t block = object + (selector != 0u ? 0x55Cu : 0x184u);
    return guest_read32(block + 0x54u) != 0u ? guest_read32(block + 0x68u) : 0u;
}
GAME_REPLACE(001B75B0, cdecl, 2, u32,
             game_object_select_block_0x184_or_0x55c_get_field_0x68_if_field_0x54_set)

/* 0x0022E070: address of record `index` (stride 0x38). A signed index below 7 selects the
 * table at 0x5107E0, otherwise the table at 0x513B60 with index - 7. */
GAME_REPLACE_EXACT(0022E070, cdecl, 1, u32, game_get_record_38_by_index_from_tables_5107e0_513b60)
{
    const uint32_t index = game_stack_arg(0u);
    g_eax = ((int32_t)index < 7) ? 0x005107E0u + index * 0x38u
                                 : 0x00513B60u + (index - 7u) * 0x38u;
}

/* 0x00234270: null-safe test that [object+0x34] is nonzero. A non-null object leaves
 * edx = the field and ecx = eax = the result, a null object leaves ecx and edx alone. */
GAME_REPLACE_EXACT(00234270, cdecl, 1, u32, game_is_object_field_34_nonzero_null_safe)
{
    const uint32_t object = game_stack_arg(0u);
    if (object == 0u) {
        g_eax = 0u;
        return;
    }
    g_edx = guest_read32(object + 0x34u);
    g_ecx = (g_edx != 0u) ? 1u : 0u;
    g_eax = g_ecx;
}

/* 0x002F8020: 0 -> 0x46, 1 -> 0x47, any other value -> 0xFFFFFFFF. */
GAME_REPLACE_EXACT(002F8020, cdecl, 1, u32, game_code_to_id_0x46_0x47_else_minus1)
{
    const uint32_t code = game_stack_arg(0u);
    g_eax = (code == 0u) ? 0x46u : (code == 1u ? 0x47u : 0xFFFFFFFFu);
}
