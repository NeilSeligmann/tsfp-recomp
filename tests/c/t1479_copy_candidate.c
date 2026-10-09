/* SPDX-License-Identifier: GPL-3.0-or-later
 * Unregistered T1479 draft; native-copy-predecl.md.
 */
#include "game_replace.h"
#include <string.h>
extern __thread uint32_t g_ebx, g_esi, g_edi;
extern __thread int g_df;

static void copy_word(uint32_t address, uint16_t value)
{
    memcpy(game_host_ptr(address), &value, sizeof value);
}

static uint16_t copy_read_word(uint32_t address)
{
    uint16_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return value;
}

void t1479_candidate_3081e0(void)
{
    guest_write32(g_esp - 4u, g_ebx);
    g_ebx = guest_read32(0x762CC8u);
    if (g_ebx == 0u) goto empty;
    g_eax = guest_read32(g_ebx + 0x14u);
    guest_write32(0x762CC8u, g_eax);
    g_eax = 0xFFFFFFFFu;
    copy_word(g_ebx + 0xCu, 0xFFFFu);
    copy_word(g_ebx + 0xEu, 0xFFFFu);
    copy_word(g_ebx + 0x10u, 0xFFFFu);
    guest_write32(g_ebx, g_eax);
    guest_write32(g_ebx + 4u, g_eax);
    guest_write32(0x762CC0u, guest_read32(0x762CC0u) - 1u);
    g_eax = game_stack_arg(3u);
    if (g_eax == 0u) goto empty;
    g_edx = (g_edx & 0xFFFF0000u) | copy_read_word(g_esp + 12u);
    guest_write32(g_esp - 8u, g_esi);
    guest_write32(g_esp - 12u, g_edi);
    g_esi = g_eax; g_ecx = 15u; g_edi = g_ebx;
    while (g_ecx != 0u) {
        const uint32_t value = guest_read32(g_esi);
        guest_write32(g_edi, value);
        const uint32_t step = g_df != 0 ? 0xFFFFFFFCu : 4u;
        g_esi += step; g_edi += step; g_ecx--;
    }
    g_ecx = game_stack_arg(1u);
    g_edi = game_stack_arg(0u);
    guest_write32(g_ebx + 0xCu, g_ecx);
    copy_word(g_ebx + 0x10u, (uint16_t)g_edx);
    g_eax = guest_read32(g_eax + 8u);
    g_ecx = g_eax < 0x80000000u ? g_eax : 0u;
    g_esi = g_ebx;
    guest_write32(g_ebx + 8u, g_ecx);
    g_eax = guest_read32(g_edi + 0x3Cu);
    if (game_guest_call(0x308070u, GAME_CC_cdecl, 0x308254u, 12u,
                        0u, 0u, NULL, 0u) != GAME_GUEST_CALL_OK)
        __builtin_trap();
    guest_write32(g_edi + 0x3Cu, g_eax);
    g_edi = guest_read32(g_esp - 12u);
    g_esi = guest_read32(g_esp - 8u);
    g_eax = g_ebx;
    g_ebx = guest_read32(g_esp - 4u);
    return;
empty:
    g_eax = 0u;
    g_ebx = guest_read32(g_esp - 4u);
}
