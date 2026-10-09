/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

/* The lifted runtime also exposes these preserved guest registers. */
extern __thread uint32_t g_edi, g_ebp;

/* Exact ordered state for the 45-byte body at 0x003CA410. The original uses
 * forward and reverse REPNE SCASB scans, saving EDI/EBP on the guest stack. */
GAME_REPLACE_EXACT(003CA410, cdecl, 2, u32, game_strrchr)
{
    /* push ebp; mov ebp, esp; push edi */
    guest_write32(g_esp - 4u, g_ebp);
    g_esp -= 4u;
    g_ebp = g_esp;
    guest_write32(g_esp - 4u, g_edi);
    g_esp -= 4u;

    /* mov edi, [ebp+8] precedes xor eax,eax in the original. */
    g_edi = guest_read32(g_ebp + 8u);
    g_eax = 0u;

    /* or ecx, -1; repne scasb through the terminating NUL. */
    g_ecx = UINT32_MAX;
    for (;;) {
        const uint8_t byte = guest_read8(g_edi);
        g_edi += 1u;
        g_ecx -= 1u;
        if (byte == 0u) {
            break;
        }
    }

    /* add ecx,1; neg ecx; sub edi,1; mov al,[ebp+0xc] */
    g_ecx = 0u - (g_ecx + 1u);
    g_edi -= 1u;
    g_eax = guest_read8(g_ebp + 0x0Cu);

    /* The original sets DF and scans backward, including the terminating NUL.
     * Model its address/count updates directly; the host C ABI keeps its own DF clear. */
    while (g_ecx != 0u) {
        const uint8_t byte = guest_read8(g_edi);
        g_edi -= 1u;
        g_ecx -= 1u;
        if (byte == (uint8_t)g_eax) {
            break;
        }
    }
    g_edi += 1u;

    /* The final CMP re-reads the found byte (or the first byte on a miss). */
    if (guest_read8(g_edi) == (uint8_t)g_eax) {
        g_eax = g_edi;
    } else {
        g_eax = 0u;
    }

    /* The original CLDs before pop edi; leave; ret. Keep every stack access in original order so
     * faults retain the same committed pushes and guest ESP value. */
    g_edi = guest_read32(g_esp);
    g_esp += 4u;
    g_esp = g_ebp;
    g_ebp = guest_read32(g_esp);
    g_esp += 4u;
    (void)guest_read32(g_esp);
}
