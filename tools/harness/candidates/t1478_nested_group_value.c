/* SPDX-License-Identifier: GPL-3.0-or-later
 * UNREGISTERED preparation draft for retail 0x00272430 (six instructions).
 * Contract/provenance: docs/evidence/t1478/nested-group-native-plan.md.
 * No GAME_REPLACE record: this file cannot admit an unproven replacement.
 */
#include "game_replace.h"
#include <stdlib.h>

#define ORDER_GUEST() __asm__ volatile("" ::: "memory")

void t1478_draft_nested_group_value(void)
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
    g_esp += 4u; /* root RET, matching an EXACT adapter's final stack effect */
}

#undef ORDER_GUEST
