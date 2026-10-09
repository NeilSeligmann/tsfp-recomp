/* SPDX-License-Identifier: GPL-3.0-or-later
 * Retail 0x00272430 (six instructions), scoped amended-fixture admission.
 * Contract/provenance: docs/evidence/t1478/nested-group-native-plan.md.
 * Executable text matches the frozen375-entry scratch proof adapter.
 * Default/amended receipts and limits: nested-group-baseline-plan.md.
 */
#include "game_replace.h"
#include <stdlib.h>

#define ORDER_GUEST() __asm__ volatile("" ::: "memory")

GAME_REPLACE_EXACT(00272430, cdecl, 1, u32, game_nested_group_value_candidate)
{
    /* mov eax,[esp+4]; push eax; CALL 271950. Retain the retail write order. */
    g_eax = guest_read32(g_esp + 4u);
    ORDER_GUEST();
    g_esp -= 4u;
    guest_write32(g_esp, g_eax);
    ORDER_GUEST();
    g_esp -= 4u;
    guest_write32(g_esp, 0x0027243Au);
    ORDER_GUEST();
    if (recomp_lookup == NULL) abort();
    game_guest_function search = recomp_lookup(0x00271950u);
    if (search == NULL) abort();
    search();
    ORDER_GUEST();

    /* The original dereferences before ADD ESP,4. In particular, a null result
     * must retain the live argument at the read fault; no null-to-zero policy. */
    g_eax = guest_read32(g_eax + 8u);
    ORDER_GUEST();
    g_esp += 4u;
    ORDER_GUEST();
    /* EXACT wrapper supplies the sole root RET stack effect. */
}

#undef ORDER_GUEST
