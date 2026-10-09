/* SPDX-License-Identifier: GPL-3.0-or-later
 * Original 0x002FDF50: count directions across ordinary and linked layers.
 * Guest locals and partial registers remain observable through frame aliases.
 */
#include "game_replace.h"
#include <stdatomic.h>

extern __thread uint32_t g_ebx, g_ebp, g_esi, g_edi;

static uint16_t layer_read_cell(uint32_t address)
{
    uint16_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return value;
}

static void layer_save(uint32_t value)
{
    uint32_t next = g_esp - 4u;
    guest_write32(next, value);
    atomic_signal_fence(memory_order_seq_cst);
    g_esp = next;
    atomic_signal_fence(memory_order_seq_cst);
}

static uint32_t layer_restore(void)
{
    uint32_t value = guest_read32(g_esp);
    g_esp += 4u;
    return value;
}

static void layer_increment_count(void)
{
    uint32_t count = guest_read32(g_esp + 16u);
    guest_write32(g_esp + 16u, count + 1u);
}

static void layer_count_directions(uint32_t cell)
{
    /* Four separate increments preserve original guest read/write order. */
    if (cell & 1u) layer_increment_count();
    if (cell & 4u) layer_increment_count();
    if (cell & 2u) layer_increment_count();
    if (cell & 8u) layer_increment_count();
}

static int layer_link_matches(uint32_t cell, uint32_t previous)
{
    if (!(previous & 0x0F00u)) return 0;
    if ((cell & 1u) && (previous & 0x0200u)) return 1;
    if ((cell & 4u) && (previous & 0x0800u)) return 1;
    if ((cell & 2u) && (previous & 0x0400u)) return 1;
    return (cell & 8u) && (previous & 0x0100u);
}

GAME_REPLACE_EXACT(002FDF50, cdecl, 1, u32, game_count_layer_directions)
{
    g_esp -= 20u;
    g_ecx = guest_read32(g_esp + 24u);
    layer_save(g_ebx);
    g_edx = (g_ecx + g_ecx * 4u) * 64u + 0x00529510u;
    g_eax = guest_read32(g_edx + 20u);
    layer_save(g_ebp);
    g_ebx = 0u;
    layer_save(g_esi);
    layer_save(g_edi);
    guest_write32(g_esp + 16u, 0u);
    guest_write32(g_esp + 24u, g_edx);
    guest_write32(g_esp + 40u, g_eax);

    if ((int32_t)g_eax > 0) {
        g_ebp = guest_read32(g_edx + 24u);
        do {
            if ((int32_t)g_ebp > 0) {
                g_ecx = guest_read32(g_edx + 32u);
                g_edi = g_eax * 4u;
                g_edx = g_ecx + g_ebx * 4u;
                g_esi = g_ebp;
                do {
                    g_ecx = (g_ecx & 0xFFFF0000u) | layer_read_cell(g_edx);
                    if (!(g_ecx & 0x1000u)) {
                        layer_count_directions(g_ecx);
                        g_eax = guest_read32(g_esp + 40u);
                    }
                    g_edx += g_edi;
                    --g_esi;
                } while (g_esi != 0u);
                g_edx = guest_read32(g_esp + 24u);
            }
            ++g_ebx;
        } while ((int32_t)g_ebx < (int32_t)g_eax);
    }

    g_ecx = guest_read32(g_edx + 28u);
    if ((int32_t)g_ecx > 1) {
        --g_ecx;
        g_ebx = g_edx + 32u;
        guest_write32(g_esp + 28u, g_ecx);
        do {
            g_ecx = 0u;
            guest_write32(g_esp + 20u, g_ecx);
            if ((int32_t)g_eax > 0) {
                g_edx = guest_read32(g_edx + 24u);
                guest_write32(g_esp + 32u, g_edx);
                do {
                    if ((int32_t)g_edx > 0) {
                        g_edi = guest_read32(g_ebx + 4u);
                        g_eax *= 4u;
                        g_esi = g_ecx * 4u;
                        g_ebp = g_edx;
                        do {
                            g_ecx = (g_ecx & 0xFFFF0000u) |
                                    layer_read_cell(g_edi + g_esi);
                            if (g_ecx & 0x1000u) {
                                g_edx = guest_read32(g_ebx);
                                g_edx = (g_edx & 0xFFFF0000u) |
                                        layer_read_cell(g_esi + g_edx);
                                if (layer_link_matches(g_ecx, g_edx))
                                    layer_increment_count();
                            } else {
                                layer_count_directions(g_ecx);
                            }
                            g_esi += g_eax;
                            --g_ebp;
                        } while (g_ebp != 0u);
                        g_ecx = guest_read32(g_esp + 20u);
                        g_edx = guest_read32(g_esp + 32u);
                    }
                    g_eax = guest_read32(g_esp + 40u);
                    ++g_ecx;
                    guest_write32(g_esp + 20u, g_ecx);
                } while ((int32_t)g_ecx < (int32_t)g_eax);
                g_edx = guest_read32(g_esp + 24u);
            }
            g_ecx = guest_read32(g_esp + 28u);
            g_ebx += 4u;
            --g_ecx;
            guest_write32(g_esp + 28u, g_ecx);
        } while (g_ecx != 0u);
    }

    g_eax = guest_read32(g_esp + 16u);
    g_edi = layer_restore();
    g_esi = layer_restore();
    g_ebp = layer_restore();
    g_ebx = layer_restore();
    g_esp += 20u;
}
