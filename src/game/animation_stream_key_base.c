/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

extern __thread uint32_t g_ebx, g_esi, g_edi;

/* Keep guest effects ordered even when metadata aliases saved stack words. */
#define ORDER_GUEST() __asm__ volatile("" ::: "memory")
#define SAVE_GUEST(reg)                      \
    do {                                     \
        guest_write32(g_esp - 4u, (reg));      \
        g_esp -= 4u;                         \
        ORDER_GUEST();                       \
    } while (0)
#define RESTORE_GUEST(reg)                   \
    do {                                     \
        (reg) = guest_read32(g_esp);           \
        g_esp += 4u;                         \
        ORDER_GUEST();                       \
    } while (0)

/* Original 0x60390: four ready windows, first pending match uses resident base.
 * Operational role is inferred; arithmetic, read order and save writes are exact.
 * Caller arithmetic EFLAGS are audited separately by the unchanged proof gate. */
GAME_REPLACE_EXACT(00060390, cdecl, 1, u32, game_animation_stream_key_base)
{
    g_edx = game_stack_arg(0u);
    ORDER_GUEST();
    if (g_edx == 0u) {
        g_ecx = guest_read32(0x006D7818u);
        ORDER_GUEST();
        g_eax = guest_read32(g_ecx + 4u);
        ORDER_GUEST();
        g_eax += g_ecx;
        return;
    }

    SAVE_GUEST(g_esi);
    g_esi = guest_read32(0x006D7834u);
    ORDER_GUEST();
    SAVE_GUEST(g_ebx);
    g_eax = 0u;
    ORDER_GUEST();
    SAVE_GUEST(g_edi);
    do {
        g_ecx = guest_read32(0x007DE540u + g_eax * 4u);
        ORDER_GUEST();
        const uint32_t start = guest_read32(g_esi + g_ecx * 8u);
        ORDER_GUEST();
        if ((int32_t)g_edx >= (int32_t)start) {
            g_edi = guest_read32(g_esi + g_ecx * 8u + 4u);
            ORDER_GUEST();
            g_ebx = guest_read32(g_esi + g_ecx * 8u);
            ORDER_GUEST();
            g_ecx = g_esi + g_ecx * 8u;
            g_edi += g_ebx;
            ORDER_GUEST();
            if ((int32_t)g_edx < (int32_t)g_edi) {
                g_ecx = guest_read32(0x007DE520u + g_eax * 4u);
                ORDER_GUEST();
                if (g_ecx == 0u) {
                    g_ecx = guest_read32(0x006D7818u);
                    ORDER_GUEST();
                    g_ecx = guest_read32(g_ecx + 0x14u);
                    ORDER_GUEST();
                    g_ecx *= g_eax;
                    g_eax = guest_read32(0x006D7838u);
                    ORDER_GUEST();
                    g_eax = guest_read32(g_eax + g_edx * 4u);
                    ORDER_GUEST();
                    RESTORE_GUEST(g_edi);
                    g_eax += g_ecx;
                    g_ecx = guest_read32(0x007DE530u);
                    ORDER_GUEST();
                    RESTORE_GUEST(g_ebx);
                    RESTORE_GUEST(g_esi);
                    g_eax += g_ecx;
                    return;
                }
                break;
            }
        }
        ++g_eax;
    } while (g_eax < 4u);

    g_ecx = guest_read32(0x006D7818u);
    ORDER_GUEST();
    g_eax = guest_read32(g_ecx + 4u);
    ORDER_GUEST();
    RESTORE_GUEST(g_edi);
    RESTORE_GUEST(g_ebx);
    RESTORE_GUEST(g_esi);
    g_eax += g_ecx;
}

#undef RESTORE_GUEST
#undef SAVE_GUEST
#undef ORDER_GUEST
