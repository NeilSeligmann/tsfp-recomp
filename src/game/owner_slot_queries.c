/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Queries on an object that points at an "owner" record holding two comparable slots.
 *
 * Names here describe what the code does, not what the original developers called it:
 * nothing in the binary carries a name for game code (0 of 9,322 game functions are
 * named), so a name is a claim made by whoever read the function.
 */
#include "game_replace.h"

/* Offsets inside the object and the owner record it points at. */
#define OBJECT_OWNER_OFFSET 0x04u
#define OWNER_PRIMARY_SLOT_OFFSET 0xD4u
#define OWNER_SECONDARY_SLOT_OFFSET 0x12Cu

/* True when `value` equals either of the owner's two slots.
 *
 * The original is called from 727 sites in the lifted tree, more than any other
 * candidate the harness can judge, and it is a pure read: no guest memory is written.
 * It returns exactly 0 or 1 in eax, never the loaded slot, so callers that test the
 * whole register keep working. The primary slot is tested first, which matters only for
 * the faults a bad `object` or `owner` pointer would raise, and both orders reach the
 * same load.
 */
static uint32_t game_owner_has_either_slot(guest_addr object, uint32_t value)
{
    guest_addr owner = guest_read32(object + OBJECT_OWNER_OFFSET);
    if (guest_read32(owner + OWNER_PRIMARY_SLOT_OFFSET) == value) {
        return 1;
    }
    return guest_read32(owner + OWNER_SECONDARY_SLOT_OFFSET) == value;
}

GAME_REPLACE_EXACT(00059D20, cdecl, 2, u32, game_owner_has_either_slot)
{
    guest_addr object = game_stack_arg(0);
    uint32_t value = game_stack_arg(1);
    g_eax = game_owner_has_either_slot(object, value);
    g_ecx = value;
}
