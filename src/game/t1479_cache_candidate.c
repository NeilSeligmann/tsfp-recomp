/* SPDX-License-Identifier: GPL-3.0-or-later
 * Unregistered T1479 draft; native-cache-predecl.md.
 */
#include "game_replace.h"
#include <string.h>
extern __thread uint32_t g_ebx, g_esi, g_edi;

static uint32_t cache_word(uint32_t address)
{
    uint16_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return value;
}

void t1479_candidate_350dc0(void)
{
    if ((guest_read8(0x7DE458u) & 0x40u) != 0u) {
        g_eax = 8u;
        return;
    }
    const unsigned option_mode = guest_read8(0x7DE455u) == 0xAu;
    guest_write32(g_esp - 4u, g_ebx);
    g_ebx = game_stack_arg(0u);
    guest_write32(g_esp - 8u, g_esi);
    guest_write32(g_esp - 12u, g_edi);
    if (option_mode) {
        g_eax = guest_read32(0x79094Cu);
        const uint32_t argument[1] = {g_eax};
        if (game_guest_call(0x6A850u, GAME_CC_cdecl, 0x350DEAu, 12u,
                            0u, 0u, argument, 1u) != GAME_GUEST_CALL_OK)
            __builtin_trap();
        g_eax = guest_read32(g_eax + 8u) * 0xA4u + 0x53D2D0u;
        g_esi = guest_read32(g_eax);
        g_ecx = 0u;
        if (g_esi != 0u && g_esi < 0x80000000u) {
            g_edi = (g_edi & 0xFFFF0000u) | cache_word(g_ebx + 0x48u);
            g_edx = g_eax + 4u;
            do {
                if (cache_word(g_edx) == (g_edi & 0xFFFFu)) {
                    g_edx = guest_read8(0x7DE457u);
                    g_edi = guest_read32(g_esp - 12u);
                    g_edx += g_eax;
                    g_eax = guest_read8(g_edx + g_ecx * 8u + 6u);
                    g_esi = guest_read32(g_esp - 8u);
                    g_ebx = guest_read32(g_esp - 4u);
                    return;
                }
                g_ecx++; g_edx += 8u;
            } while (g_ecx < g_esi);
        }
    }
    if ((guest_read32(0x7DE458u) & 0x80000u) != 0u) {
        g_eax = cache_word(g_ebx + 0x48u);
        if (g_eax >= 0x8000u) g_eax |= 0xFFFF0000u;
        g_eax *= 3u;
        g_edi = guest_read32(g_esp - 12u);
        g_eax <<= 4u;
        g_eax = guest_read32(g_eax + 0x53A3DCu);
        g_esi = guest_read32(g_esp - 8u);
        g_ebx = guest_read32(g_esp - 4u);
        return;
    }
    g_ecx = guest_read32(g_ebx + 8u);
    g_edx = guest_read8(g_ecx + 0x7DE434u);
    g_edi = guest_read32(g_esp - 12u);
    g_eax = 5u - g_edx;
    g_esi = guest_read32(g_esp - 8u);
    g_eax *= 3u;
    g_ebx = guest_read32(g_esp - 4u);
}
