/* SPDX-License-Identifier: GPL-3.0-or-later
 * Original 0x001CF640: update a keyed row, or its preceding fallback word.
 * ECX supplies an incoming index; three stack arguments remain caller-owned.
 */
#include "game_replace.h"

extern __thread uint32_t g_ebx, g_ebp, g_esi, g_edi;

#define KEY_TABLE_SCALE 0x007087C4u
#define KEY_TABLE_ROWS 0x0075AF1Cu

static void key_save(uint32_t value)
{
    const uint32_t next = g_esp - 4u;
    guest_write32(next, value);
    g_esp = next;
}

static uint32_t key_restore(void)
{
    uint32_t value = guest_read32(g_esp);
    g_esp += 4u;
    return value;
}

static void key_restore_base_frame(void)
{
    g_esi = key_restore();
    g_ebp = key_restore();
}

GAME_REPLACE_EXACT(001CF640, cdecl, 3, void, game_update_key_table)
{
    g_eax = guest_read32(KEY_TABLE_SCALE);
    g_eax *= game_stack_arg(0);
    key_save(g_ebx);
    key_save(g_ebp);
    g_ebp = guest_read32(g_esp + 16u);
    g_eax += g_ecx;
    key_save(g_esi);
    g_esi = guest_read32(KEY_TABLE_ROWS);
    g_edx = guest_read32(g_esi + g_eax * 8u);
    g_ebx = UINT32_MAX;
    g_ecx = 0u;

    if (g_edx == 0u) {
        g_ecx = guest_read32(g_esi + g_eax * 8u + 4u);
        g_edx = guest_read32(g_esp + 24u);
        key_restore_base_frame();
        guest_write32(g_ecx + g_ebx * 8u + 4u, g_edx);
        g_ebx = key_restore();
        return;
    }

    key_save(g_edi);
    g_edi = guest_read32(g_esi + g_eax * 8u + 4u);
    for (;;) {
        if (guest_read32(g_edi) == g_ebp) {
            /* Reload the row pointer as the original does, even if its bytes
             * overlap an earlier guest frame save or a searched key. */
            g_edx = guest_read32(g_esi + g_eax * 8u + 4u);
            g_eax = guest_read32(g_esp + 28u);
            g_edi = key_restore();
            key_restore_base_frame();
            guest_write32(g_edx + g_ecx * 8u + 4u, g_eax);
            g_ebx = key_restore();
            return;
        }
        ++g_ecx;
        g_edi += 8u;
        if (g_ecx >= g_edx) break;
    }

    g_eax = guest_read32(g_esi + g_eax * 8u + 4u);
    g_ecx = guest_read32(g_esp + 28u);
    g_edi = key_restore();
    key_restore_base_frame();
    guest_write32(g_eax + g_ebx * 8u + 4u, g_ecx);
    g_ebx = key_restore();
}
