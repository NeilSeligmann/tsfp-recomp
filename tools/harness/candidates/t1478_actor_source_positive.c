/* SPDX-License-Identifier: GPL-3.0-or-later
 * UNREGISTERED retail1B6890 draft; real1B67E0 remains dispatched.
 * Declaration: docs/evidence/t1478/actor-source-native-plan.md.
 */
#include "game_replace.h"
#include <stdlib.h>

extern __thread uint32_t g_esi, g_edi;
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")

void t1478_draft_actor_source_positive(void)
{
    g_eax = guest_read32(g_esp + 8u);
    ORDER_GUEST();
    g_esp -= 4u;
    guest_write32(g_esp, g_esi);
    ORDER_GUEST();
    g_esp -= 4u;
    guest_write32(g_esp, g_edi);
    ORDER_GUEST();
    g_edi = guest_read32(g_esp + 12u);
    ORDER_GUEST();
    g_ecx = (g_ecx & 0xFFFFFF00u) | guest_read8(g_edi + g_eax + 0x54Cu);
    ORDER_GUEST();
    if ((g_ecx & 0xFFu) != 0u) {
        g_ecx = guest_read32(0x004F9BACu);
        ORDER_GUEST();
        g_eax *= 0x3Cu;
        ORDER_GUEST();
        g_eax = guest_read32(g_eax + 0x004FA138u);
        ORDER_GUEST();
        g_eax *= 0x29Cu;
        ORDER_GUEST();
        g_esi = guest_read32(g_eax + g_ecx + 0x14u);
        ORDER_GUEST();
        if (g_esi != 0u) {
            g_esp -= 4u;
            guest_write32(g_esp, g_esi);
            ORDER_GUEST();
            g_esp -= 4u;
            guest_write32(g_esp, g_edi);
            ORDER_GUEST();
            g_esp -= 4u;
            guest_write32(g_esp, 0x001B68C9u);
            ORDER_GUEST();
            if (recomp_lookup == NULL) abort();
            game_guest_function sources = recomp_lookup(0x001B67E0u);
            if (sources == NULL) abort();
            sources();
            ORDER_GUEST();
            uint16_t value;
            memcpy(&value, game_host_ptr(g_edi + g_esi * 2u + 0x61Eu), sizeof(value));
            g_edx = (uint32_t)(int32_t)(int16_t)value;
            ORDER_GUEST();
            g_eax += g_edx;
            ORDER_GUEST();
            g_esp += 8u;
            ORDER_GUEST();
            uint32_t positive = (int32_t)g_eax > 0 ? 1u : 0u;
            g_edi = guest_read32(g_esp);
            g_esp += 4u;
            ORDER_GUEST();
            g_eax = positive;
            g_esi = guest_read32(g_esp);
            g_esp += 4u;
            ORDER_GUEST();
            g_esp += 4u;
            return;
        }
    }
    g_edi = guest_read32(g_esp);
    g_esp += 4u;
    ORDER_GUEST();
    g_eax = 0u;
    g_esi = guest_read32(g_esp);
    g_esp += 4u;
    ORDER_GUEST();
    g_esp += 4u;
}

#undef ORDER_GUEST
