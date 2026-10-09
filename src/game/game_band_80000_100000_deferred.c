/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1476 provisional exact ID-link and snapshot candidates.
 * Original frame contracts: docs/t1476-deferred-proof-plan.md.
 * Real guest callees preserve their own memory, register and fault effects.
 */
#include "game_replace.h"

extern __thread uint32_t g_esi;

static void deferred_call(uint32_t target, uint32_t return_va, uint32_t local_bytes,
                          const uint32_t *args, unsigned count)
{
    if (game_guest_call(target, GAME_CC_cdecl, return_va, local_bytes,
                        0u, 0u, args, count) != GAME_GUEST_CALL_OK)
        __builtin_trap();
}

static void id_link(uint32_t mask, uint32_t first_return,
                    uint32_t second_return, uint32_t link_return)
{
    guest_write32(g_esp - 4u, g_esi);
    g_esi = game_stack_arg(0u);
    if (g_esi == 0u) {
        g_eax = 0u;
        g_esi = guest_read32(g_esp - 4u);
        return;
    }
    g_eax = game_stack_arg(1u);
    if (g_eax == 0u) {
        g_eax = 0u;
        g_esi = guest_read32(g_esp - 4u);
        return;
    }
    const uint32_t second[3] = {g_eax, mask, 0u};
    deferred_call(0x0009A510u, first_return, 4u, second, 3u);
    const uint32_t first[2] = {g_esi, g_eax};
    deferred_call(0x0009A510u, second_return, 12u, first, 2u);
    const uint32_t id[1] = {g_eax};
    deferred_call(0x0009D5E0u, link_return, 16u, id, 1u);
    g_esi = guest_read32(g_esp - 4u);
}

GAME_REPLACE_EXACT(0009D740, cdecl, 2, u32, game_object_find_link_with_mask_10000)
{
    id_link(0x10000u, 0x0009D75Eu, 0x0009D768u, 0x0009D771u);
}

GAME_REPLACE_EXACT(0009D780, cdecl, 2, u32, game_object_find_link_without_mask)
{
    id_link(0u, 0x0009D79Bu, 0x0009D7A5u, 0x0009D7AEu);
}

