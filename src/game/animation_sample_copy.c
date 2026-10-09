/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"

extern __thread uint32_t g_esi, g_edi;
extern __thread int g_df;

#define ORDER_GUEST() __asm__ volatile("" ::: "memory")

static uint16_t copy_read16(uint32_t address)
{
    uint16_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return value;
}

static void copy_write16(uint32_t address, uint16_t value)
{
    memcpy(game_host_ptr(address), &value, sizeof value);
}

/* Source bit1 selects a two-byte prefix; REP movement honors incoming guest DF.
 * This is an ordered copy, not memmove. Small prefixed counts wrap unchanged. */
GAME_REPLACE_EXACT(000591D0, cdecl, 3, void, game_animation_sample_copy)
{
    g_eax = game_stack_arg(1u);
    ORDER_GUEST();
    const int prefix = (g_eax & 2u) != 0u;
    g_ecx = game_stack_arg(0u);
    ORDER_GUEST();
    guest_write32(g_esp - 4u, g_esi);
    g_esp -= 4u;
    ORDER_GUEST();
    guest_write32(g_esp - 4u, g_edi);
    g_esp -= 4u;
    ORDER_GUEST();
    g_edi = g_ecx;
    g_esi = g_eax;
    ORDER_GUEST();
    if (prefix) {
        g_edx = (g_edx & 0xFFFF0000u) | copy_read16(g_eax);
        ORDER_GUEST();
        g_esi = g_eax + 2u;
        g_eax = guest_read32(g_esp + 0x14u);
        ORDER_GUEST();
        g_eax -= 2u;
        copy_write16(g_ecx, (uint16_t)g_edx);
        ORDER_GUEST();
        g_edi = g_ecx + 2u;
        guest_write32(g_esp + 0x14u, g_eax);
        ORDER_GUEST();
    }
    g_ecx = guest_read32(g_esp + 0x14u);
    ORDER_GUEST();
    g_eax = g_ecx;
    g_ecx >>= 2;
    ORDER_GUEST();
    while (g_ecx != 0u) {
        const uint32_t word = guest_read32(g_esi);
        ORDER_GUEST();
        guest_write32(g_edi, word);
        ORDER_GUEST();
        const uint32_t step = g_df != 0 ? 0xFFFFFFFCu : 4u;
        g_esi += step;
        g_edi += step;
        --g_ecx;
        ORDER_GUEST();
    }
    g_ecx = g_eax & 3u;
    ORDER_GUEST();
    while (g_ecx != 0u) {
        const uint8_t byte = guest_read8(g_esi);
        ORDER_GUEST();
        guest_write8(g_edi, byte);
        ORDER_GUEST();
        const uint32_t step = g_df != 0 ? 0xFFFFFFFFu : 1u;
        g_esi += step;
        g_edi += step;
        --g_ecx;
        ORDER_GUEST();
    }
    g_edi = guest_read32(g_esp);
    g_esp += 4u;
    ORDER_GUEST();
    g_esi = guest_read32(g_esp);
    g_esp += 4u;
}

#undef ORDER_GUEST
