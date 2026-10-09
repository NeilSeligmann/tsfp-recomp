/* SPDX-License-Identifier: GPL-3.0-or-later
 * UNREGISTERED retail1BA1B0 draft; real1BA130 remains dispatched.
 * Declaration: docs/evidence/t1478/predecessor-select-native-plan.md.
 */
#include "game_replace.h"
#include <stdlib.h>
extern __thread uint32_t g_ebx, g_esi;
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")

void t1478_draft_predecessor_select(void)
{
    g_esp -= 4u;
    guest_write32(g_esp, g_ebx);
    ORDER_GUEST();
    g_esp -= 4u;
    guest_write32(g_esp, g_esi);
    ORDER_GUEST();
    g_esi = guest_read32(g_esp + 12u);
    ORDER_GUEST();
    g_edx = guest_read32(g_esi + 0x94u);
    g_eax = g_edx;
    g_ecx = g_eax * 0x3Cu;
    ORDER_GUEST();
    if ((guest_read8(g_ecx + 0x004FA128u) & 4u) != 0u) {
        g_ecx = guest_read32(g_esi + 0x80u);
        ORDER_GUEST();
        if (g_ecx != 0u) {
            g_ecx = guest_read32(0x007DE458u);
            ORDER_GUEST();
            if ((g_ecx & 0x1000u) == 0u) {
                g_eax -= 1u;
                g_ecx = g_eax * 0x3Cu;
                g_ebx = (g_ebx & 0xFFFFFF00u) | guest_read8(g_ecx + 0x004FA128u);
                ORDER_GUEST();
                g_ecx += 0x004FA128u;
                while ((g_ebx & 0x80u) == 0u) {
                    g_ebx = (g_ebx & 0xFFFFFF00u) | guest_read8(g_ecx - 0x3Cu);
                    ORDER_GUEST();
                    g_ecx -= 0x3Cu;
                    g_eax -= 1u;
                    ORDER_GUEST();
                }
            }
        }
    }
    if (g_eax != g_edx) {
        g_esp -= 4u;
        guest_write32(g_esp, g_eax);
        ORDER_GUEST();
        g_esp -= 4u;
        guest_write32(g_esp, g_esi);
        ORDER_GUEST();
        g_esp -= 4u;
        guest_write32(g_esp, 0x001BA20Du);
        ORDER_GUEST();
        if (recomp_lookup == NULL) abort();
        game_guest_function select = recomp_lookup(0x001BA130u);
        if (select == NULL) abort();
        select();
        ORDER_GUEST();
        g_esp += 8u;
        ORDER_GUEST();
    }
    g_esi = guest_read32(g_esp);
    g_esp += 4u;
    ORDER_GUEST();
    g_ebx = guest_read32(g_esp);
    g_esp += 4u;
    ORDER_GUEST();
    g_esp += 4u;
}
#undef ORDER_GUEST
