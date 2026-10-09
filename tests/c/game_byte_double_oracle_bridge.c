/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
__thread uint32_t g_eax, g_ecx, g_edx, g_esp;
ptrdiff_t g_xbox_mem_offset;
void sub_0032D9B0(void);
/* A complete stack page, relocated through the production guest-memory offset. */
void byte_double_run(uint32_t registers[8], uint8_t stack[4096])
{
    g_xbox_mem_offset = (ptrdiff_t)((uintptr_t)stack - (uintptr_t)registers[7]);
    g_eax = registers[0]; g_ecx = registers[1];
    g_edx = registers[2]; g_esp = registers[7];
    sub_0032D9B0();
    registers[0] = g_eax; registers[1] = g_ecx;
    registers[2] = g_edx; registers[7] = g_esp;
}
