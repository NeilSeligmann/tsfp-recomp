/* SPDX-License-Identifier: GPL-3.0-or-later
 * Original 0x00311C00: collect spatial direction and component masks.
 * The original's three jump tables map the bounded direction/type cases below.
 */
#include "game_replace.h"
#include <stdatomic.h>

extern __thread uint32_t g_ebx, g_ebp, g_esi, g_edi;

static void mask_save(uint32_t value)
{
    uint32_t next = g_esp - 4u;
    guest_write32(next, value);
    atomic_signal_fence(memory_order_seq_cst);
    g_esp = next;
    atomic_signal_fence(memory_order_seq_cst);
}

static uint32_t mask_restore(void)
{
    uint32_t value = guest_read32(g_esp);
    g_esp += 4u;
    return value;
}

static void mask_clear_read_eax(uint32_t address)
{
    g_eax = 0u;
    g_eax = guest_read8(address);
}

static void mask_clear_read_ecx(uint32_t address)
{
    g_ecx = 0u;
    g_ecx = guest_read8(address);
}

static void mask_one_direction(uint32_t direction)
{
    switch (direction) {
    case 0u:
        mask_clear_read_eax(g_edx);
        mask_clear_read_ecx(g_edx - 2u);
        g_eax *= 3u;
        g_ecx += g_eax * 4u;
        break;
    case 1u:
        mask_clear_read_eax(g_edx);
        mask_clear_read_ecx(g_edx - 1u);
        g_eax *= 3u;
        g_eax *= 4u;
        g_ecx += g_eax + 3u;
        break;
    case 2u:
        g_edi = guest_read32(g_ebp + 0x00529524u);
        mask_clear_read_eax(g_edx);
        mask_clear_read_ecx(g_edx + 1u);
        g_eax *= 3u;
        g_edi -= g_ecx;
        g_ecx = g_edi + g_eax * 4u;
        mask_clear_read_eax(g_edx - 2u);
        g_ecx -= g_eax;
        g_ecx += 6u;
        break;
    case 3u:
        g_edi = guest_read32(g_ebp + 0x00529528u);
        mask_clear_read_ecx(g_edx - 1u);
        mask_clear_read_eax(g_edx + 1u);
        g_edi -= g_ecx;
        g_edi -= g_eax;
        mask_clear_read_eax(g_edx);
        g_ecx = g_eax * 3u;
        g_ecx = g_edi + g_ecx * 4u + 9u;
        break;
    }
    g_eax = 1u;
    g_eax <<= g_ecx & 31u;
    g_esi |= g_eax;
}

static void mask_direction_range(uint32_t direction)
{
    g_edi = guest_read8(g_edx + 1u);
    if (g_edi == 0u) return;

    switch (direction) {
    case 0u:
        g_eax = guest_read8(g_edx);
        g_ecx = g_eax * 3u;
        g_eax = guest_read8(g_edx - 2u);
        g_ecx = g_eax + g_ecx * 4u;
        g_eax = g_edi;
        break;
    case 1u:
        g_eax = guest_read8(g_edx);
        g_ecx = guest_read8(g_edx - 1u);
        g_eax *= 3u;
        g_ecx += g_eax * 4u + 3u;
        g_eax = g_edi;
        break;
    case 2u:
        g_eax = guest_read8(g_edx);
        g_ecx = g_eax * 3u;
        g_eax = guest_read32(g_ebp + 0x00529524u);
        g_ecx = g_eax + g_ecx * 4u;
        g_eax = guest_read8(g_edx - 2u);
        g_ecx -= g_eax;
        g_ecx += g_edi + 4u;
        break;
    case 3u:
        g_ecx = guest_read8(g_edx - 1u);
        g_eax = guest_read32(g_ebp + 0x00529528u);
        g_eax -= g_ecx;
        g_ecx = g_eax;
        g_eax = guest_read8(g_edx);
        g_eax *= 3u;
        g_ecx += g_edi;
        g_ecx += g_eax * 4u + 7u;
        break;
    }

    if (direction < 2u) {
        do {
            g_edi = 1u;
            g_edi <<= g_ecx & 31u;
            g_esi |= g_edi;
            ++g_ecx;
            --g_eax;
        } while (g_eax != 0u);
    } else {
        do {
            g_eax = 1u;
            g_eax <<= g_ecx & 31u;
            g_esi |= g_eax;
            --g_ecx;
            --g_edi;
        } while (g_edi != 0u);
    }
}

GAME_REPLACE_EXACT(00311C00, cdecl, 1, u32, game_collect_region_mask)
{
    g_eax = game_stack_arg(0);
    mask_save(g_ebp);
    g_ebp = (g_eax + g_eax * 4u) * 64u;
    g_eax = guest_read32(g_ebp + 0x00529554u);
    mask_save(g_esi);
    g_esi = 0u;

    if ((int32_t)g_eax > 0) {
        mask_save(g_ebx);
        mask_save(g_edi);
        g_edx = g_ebp + 0x0052955Bu;
        g_ebx = g_eax;
        do {
            g_eax = (g_eax & 0xFFFFFF00u) | guest_read8(g_edx - 3u);
            uint32_t type = g_eax & 255u;
            if (type == 5u) {
                g_esi |= 0x02000000u;
            } else if (type == 6u) {
                g_esi |= 0x01000000u;
            } else {
                g_ecx = (g_ecx & 0xFFFFFF00u) | guest_read8(g_edx + 2u);
                uint32_t mode = g_ecx & 255u;
                if (mode == 2u || mode == 3u) {
                    g_eax = type;
                    --g_eax;
                    if (g_eax <= 3u) {
                        if (mode == 2u) mask_one_direction(g_eax);
                        else mask_direction_range(g_eax);
                    }
                }
            }
            g_edx += 6u;
            --g_ebx;
        } while (g_ebx != 0u);
        g_edi = mask_restore();
        g_ebx = mask_restore();
    }

    g_eax = guest_read32(g_ebp + 0x00529618u);
    if ((int32_t)g_eax > 0) {
        g_ecx = g_ebp + 0x00529620u;
        g_edx = g_eax;
        do {
            g_eax = guest_read32(g_ecx - 4u);
            if (g_eax == 0u) {
                g_eax = guest_read32(g_ecx);
                switch (g_eax) {
                case 0u: g_esi |= 0x04000000u; break;
                case 1u: g_esi |= 0x08000000u; break;
                case 2u: g_esi |= 0x10000000u; break;
                case 3u: g_esi |= 0x20000000u; break;
                case 4u: g_esi |= 0x40000000u; break;
                case 5u: g_esi |= 0x80000000u; break;
                }
            } else if (guest_read32(g_ecx) == 1u) {
                g_esi |= 0x08000000u;
            }
            g_ecx += 8u;
            --g_edx;
        } while (g_edx != 0u);
    }

    g_eax = g_esi;
    g_esi = mask_restore();
    g_ebp = mask_restore();
}
