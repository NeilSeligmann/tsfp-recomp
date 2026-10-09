/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Small state mutators on one family of game objects.
 */
#include "game_replace.h"

#define OBJECT_FLAGS_OFFSET 0x0Cu
#define OBJECT_MODE_FLAGS_OFFSET 0x28u
#define OBJECT_PREVIOUS_STATE_OFFSET 0x110u
#define OBJECT_STATE_OFFSET 0x114u
#define OBJECT_STATE_TIMER_OFFSET 0x118u

/* The value of the state word that means "locked": a state change request is ignored. */
#define OBJECT_STATE_LOCKED 0x10u
/* The two flag bits (0x00000400 and 0x00020000) a state change clears. */
#define OBJECT_STATE_CHANGE_CLEARED_FLAGS 0x00020400u
#define MODE_FLAG_ENABLED 0x00000001u

/* Sets or clears bit 0 of the mode word, leaving every other bit alone.
 *
 * Any non-zero `enable` counts as true, not only 1: the original tests the whole
 * register. It stores the cleared word first and the final word second, so the first
 * store is unobservable in the write set and is not reproduced.
 */
static uint32_t game_object_set_enabled(guest_addr object, uint32_t enable)
{
    uint32_t mode = guest_read32(object + OBJECT_MODE_FLAGS_OFFSET) & ~MODE_FLAG_ENABLED;
    if (enable != 0) {
        mode |= MODE_FLAG_ENABLED;
    }
    guest_write32(object + OBJECT_MODE_FLAGS_OFFSET, mode);
    return mode;
}

GAME_REPLACE_EXACT(00071C70, cdecl, 2, void, game_object_set_enabled)
{
    guest_addr object = game_stack_arg(0);
    uint32_t enable = game_stack_arg(1);
    g_ecx = game_object_set_enabled(object, enable);
    g_eax = object;
    g_edx = enable;
}

/* Moves the object to `new_state` unless it is locked.
 *
 * On a change it remembers the old state, resets the state timer, and clears two flag
 * bits. A locked object (state 0x10) is left completely untouched and nothing is
 * written. The requested state is loaded through its guest argument address after
 * storing the previous state, preserving the original even if those locations alias.
 */
static uint32_t game_object_request_state(guest_addr object, guest_addr state_argument)
{
    uint32_t current = guest_read32(object + OBJECT_STATE_OFFSET);
    if (current == OBJECT_STATE_LOCKED) {
        return current;
    }
    guest_write32(object + OBJECT_PREVIOUS_STATE_OFFSET, current);
    uint32_t new_state = guest_read32(state_argument);
    guest_write32(object + OBJECT_STATE_OFFSET, new_state);
    uint32_t flags = guest_read32(object + OBJECT_FLAGS_OFFSET) &
                     ~OBJECT_STATE_CHANGE_CLEARED_FLAGS;
    guest_write32(object + OBJECT_STATE_TIMER_OFFSET, 0);
    guest_write32(object + OBJECT_FLAGS_OFFSET, flags);
    return flags;
}

GAME_REPLACE_EXACT(000D2CE0, cdecl, 2, void, game_object_request_state)
{
    guest_addr object = game_stack_arg(0);
    g_ecx = game_object_request_state(object, g_esp + 8u);
    g_eax = object;
}
