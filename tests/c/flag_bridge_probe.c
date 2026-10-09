/* SPDX-License-Identifier: GPL-3.0-or-later
 * Loaded by tests/test_flag_bridge_oracle.py. It is linked with src/game/x87_helpers.c, so the
 * test runs the REAL 0x003CCE80 replacement adapter (and the exact flag function it includes)
 * against the original's registers and EFLAGS, not a copy of either. */
#include "game_replace.h"
#include "x87_flags.h"

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
ptrdiff_t g_xbox_mem_offset;
__thread double g_fp_stack[8];
__thread int g_fp_top;
__thread uint16_t g_fp_control_word;
__thread uint32_t g_flag_bridge_eflags, g_flag_bridge_mask;
static uint8_t memory[0x1000];

void sub_003CCE80(void);

uint32_t probe_cmp_flags(uint32_t left, uint32_t right)
{
    return x87_cmp_flags(left, right);
}

/* One call of the replacement as a cdecl call with two argument words at esp + 4 and esp + 8. */
void probe_provider(uint32_t high, uint32_t *eax, uint32_t *flags, uint32_t *mask, uint32_t *pops)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    g_esp = 0x100u;
    guest_write32(g_esp, 0x12345678u);
    guest_write32(g_esp + 4u, 0x12345678u);
    guest_write32(g_esp + 8u, high);
    g_eax = 0x11111111u;
    g_ecx = 0x22222222u;
    g_edx = 0x33333333u;
    g_flag_bridge_eflags = 0xFFFFFFFFu;
    g_flag_bridge_mask = 0u;
    sub_003CCE80();
    *eax = g_eax;
    *flags = g_flag_bridge_eflags;
    *mask = g_flag_bridge_mask;
    *pops = g_esp - 0x100u;
}
