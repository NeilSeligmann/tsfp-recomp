/* SPDX-License-Identifier: GPL-3.0-or-later
 * UNREGISTERED retail 0x001BFA60 draft; original 1BA130 stays a dispatched callee.
 * Declaration: docs/evidence/t1478/predecessor-actor-native-plan.md.
 */
#include "game_replace.h"
#include <stdlib.h>

extern __thread uint32_t g_esi;
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")

void t1478_draft_predecessor_actor_flags(void)
{
    g_eax = guest_read32(g_esp + 4u);
    ORDER_GUEST();
    g_ecx = guest_read32(0x007B0C48u);
    ORDER_GUEST();
    g_esp -= 4u;
    guest_write32(g_esp, g_esi);
    ORDER_GUEST();
    g_esi = guest_read32(g_eax + 8u);
    ORDER_GUEST();
    g_esi *= 0x1584u;
    g_esi += g_ecx;
    ORDER_GUEST();
    g_ecx = guest_read32(g_esp + 12u);
    ORDER_GUEST();
    g_esp -= 4u;
    guest_write32(g_esp, g_ecx);
    ORDER_GUEST();
    g_esp -= 4u;
    guest_write32(g_esp, g_eax);
    ORDER_GUEST();
    g_esp -= 4u;
    guest_write32(g_esp, 0x001BFA81u);
    ORDER_GUEST();
    if (recomp_lookup == NULL) abort();
    game_guest_function predecessor = recomp_lookup(0x001BA130u);
    if (predecessor == NULL) abort();
    predecessor();
    ORDER_GUEST();
    /* Read before cleanup, store after cleanup, exactly as at 1BFA81..1BFA8C. */
    g_eax = guest_read32(g_esi + 12u);
    ORDER_GUEST();
    g_eax |= 0x4000u;
    g_esp += 8u;
    ORDER_GUEST();
    guest_write32(g_esi + 12u, g_eax);
    ORDER_GUEST();
    g_esi = guest_read32(g_esp);
    g_esp += 4u;
    ORDER_GUEST();
    g_esp += 4u;
}

#undef ORDER_GUEST
