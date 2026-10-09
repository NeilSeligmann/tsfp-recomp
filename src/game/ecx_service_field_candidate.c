/* SPDX-License-Identifier: GPL-3.0-or-later
 * Retail417ADA. Contract: docs/evidence/t1480/ecx-service-field-predeclaration.md.
 * Ordered retail stack writes; original417932 remains the nested callee. */
#include "game_replace.h"
#include <stdlib.h>

extern __thread uint32_t g_ebp;
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")

GAME_REPLACE_EXACT_INPUTS(00417ADA, stdcall, 1, u32, ecx, game_service_field_from_id)
{
    g_esp -= 4u;
    guest_write32(g_esp, g_ebp);
    ORDER_GUEST();
    g_ebp = g_esp;
    g_esp -= 12u;
    g_eax = g_ebp - 12u;
    g_esp -= 4u;
    guest_write32(g_esp, g_eax);
    ORDER_GUEST();
    const uint32_t service_id = guest_read32(g_ebp + 8u);
    ORDER_GUEST();
    g_esp -= 4u;
    guest_write32(g_esp, service_id);
    ORDER_GUEST();
    g_esp -= 4u;
    guest_write32(g_esp, 0x00417AECu);
    ORDER_GUEST();
    if (recomp_lookup == NULL) abort();
    game_guest_function service = recomp_lookup(0x00417932u);
    if (service == NULL) abort();
    service();
    ORDER_GUEST();
    if ((int32_t)g_eax < 0) {
        g_eax = 0u;
    } else {
        g_eax = guest_read32(g_ebp - 8u);
        ORDER_GUEST();
    }
    g_esp = g_ebp;
    g_ebp = guest_read32(g_esp);
    ORDER_GUEST();
    g_esp += 4u;
    /* The EXACT adapter supplies the root's sole RET4 stack effect. */
}
#undef ORDER_GUEST
