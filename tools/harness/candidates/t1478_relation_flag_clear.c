/* SPDX-License-Identifier: GPL-3.0-or-later
 * UNREGISTERED220900/220960 drafts; 9CC40 and9D740 remain real guest calls.
 * Declaration: docs/evidence/t1478/relation-flag-native-plan.md.
 */
#include "game_replace.h"
#include <stdlib.h>
extern __thread uint32_t g_esi;
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")

static void dispatch(uint32_t target, uint32_t return_pc)
{
    g_esp -= 4u;
    guest_write32(g_esp, return_pc);
    ORDER_GUEST();
    if (recomp_lookup == NULL) abort();
    game_guest_function callee = recomp_lookup(target);
    if (callee == NULL) abort();
    callee();
    ORDER_GUEST();
}

static void clear_pair_flags(uint32_t begin, uint32_t end, uint32_t right_pc,
                             uint32_t left_pc, uint32_t relation_pc)
{
    g_esp -= 4u;
    guest_write32(g_esp, g_esi);
    ORDER_GUEST();
    g_esi = begin;
    do {
        g_eax = guest_read32(g_esi + 4u);
        ORDER_GUEST();
        g_esp -= 4u;
        guest_write32(g_esp, g_eax);
        ORDER_GUEST();
        dispatch(0x0009CC40u, right_pc);
        g_edx = guest_read32(0x00715F2Cu);
        ORDER_GUEST();
        g_ecx = guest_read32(g_esi);
        ORDER_GUEST();
        g_eax <<= 5u;
        g_esp += 4u;
        g_eax += g_edx;
        ORDER_GUEST();
        g_esp -= 4u;
        guest_write32(g_esp, g_eax);
        ORDER_GUEST();
        g_esp -= 4u;
        guest_write32(g_esp, g_ecx);
        ORDER_GUEST();
        dispatch(0x0009CC40u, left_pc);
        g_edx = guest_read32(0x00715F2Cu);
        ORDER_GUEST();
        g_eax <<= 5u;
        g_eax += g_edx;
        g_esp += 4u;
        ORDER_GUEST();
        g_esp -= 4u;
        guest_write32(g_esp, g_eax);
        ORDER_GUEST();
        dispatch(0x0009D740u, relation_pc);
        g_esp += 8u;
        ORDER_GUEST();
        if (g_eax != 0u) {
            uint32_t flags = guest_read32(g_eax);
            ORDER_GUEST();
            guest_write32(g_eax, flags & 0xFFFEFFFFu);
            ORDER_GUEST();
        }
        g_esi += 8u;
        ORDER_GUEST();
    } while ((int32_t)g_esi < (int32_t)end);
    g_esi = guest_read32(g_esp);
    g_esp += 4u;
    ORDER_GUEST();
    g_esp += 4u;
}

void t1478_draft_relation_flag_clear_220900(void)
{
    clear_pair_flags(0x00509720u, 0x00509778u, 0x0022090Fu, 0x00220926u, 0x0022093Au);
}
void t1478_draft_relation_flag_clear_220960(void)
{
    clear_pair_flags(0x00509778u, 0x005097A0u, 0x0022096Fu, 0x00220986u, 0x0022099Au);
}
#undef ORDER_GUEST
