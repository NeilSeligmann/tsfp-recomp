/* SPDX-License-Identifier: GPL-3.0-or-later
 * Unregistered draft; native-chunk-predecl.md documents the original contract.
 */
#include "game_replace.h"
extern __thread uint32_t g_esi, g_edi;

void t1479_candidate_32a5b0(void)
{
    g_ecx = game_stack_arg(0u);
    if (guest_read32(g_ecx) != 0xC77667u ||
        guest_read32(g_ecx + 4u) != 0x12Du) {
        g_eax = 0u;
        return;
    }
    guest_write32(g_esp - 4u, g_esi);
    guest_write32(g_esp - 8u, g_edi);
    g_eax = g_ecx + 8u;
    g_edi = g_ecx + 0x7FE0u;
    if (g_eax < g_edi) {
        g_esi = game_stack_arg(1u);
        do {
            g_ecx = g_eax;
            g_edx = guest_read32(g_ecx);
            g_eax += 8u;
            if (g_edx == g_esi) goto finished;
            if (g_edx == 0xC7766Au) break;
            g_eax += guest_read32(g_ecx + 4u);
        } while (g_eax < g_edi);
    }
    g_eax = 0u;
finished:
    g_edi = guest_read32(g_esp - 8u);
    g_esi = guest_read32(g_esp - 4u);
}
