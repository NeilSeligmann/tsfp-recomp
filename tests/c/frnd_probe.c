/* SPDX-License-Identifier: GPL-3.0-or-later
 * Loaded by tests/test_frnd_register_oracle.py. It is linked with src/game/crt_math_leaves.c so
 * the test runs the REAL 0x003CC790 replacement adapter against the original's registers. */
#include "game_replace.h"

__thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_fs_base;
ptrdiff_t g_xbox_mem_offset;
__thread double g_fp_stack[8];
__thread int g_fp_top;
__thread uint16_t g_fp_control_word;
__thread uint32_t g_flag_bridge_eflags, g_flag_bridge_mask;
static uint8_t memory[0x1000];

void sub_003CC790(void);

/* One cdecl call with the double at esp + 4 (low) and esp + 8 (high). */
void probe_frnd(uint32_t low, uint32_t high, uint32_t control, uint32_t *regs, uint64_t *top,
                uint32_t *pops, uint32_t *bridge_mask)
{
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    g_esp = 0x100u;
    guest_write32(g_esp, 0x12345678u);
    guest_write32(g_esp + 4u, low);
    guest_write32(g_esp + 8u, high);
    g_eax = 0x11111111u;
    g_ecx = 0x22222222u;
    g_edx = 0x33333333u;
    g_fp_top = 0;
    g_fp_control_word = (uint16_t)control;
    g_flag_bridge_mask = 0u;
    sub_003CC790();
    regs[0] = g_eax;
    regs[1] = g_ecx;
    regs[2] = g_edx;
    __builtin_memcpy(top, &g_fp_stack[g_fp_top & 7], sizeof *top);
    *pops = g_esp - 0x100u;
    *bridge_mask = g_flag_bridge_mask;
}
