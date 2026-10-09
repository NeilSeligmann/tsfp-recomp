/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#include <stdlib.h>
__thread uint32_t g_eax, g_ecx, g_edx, g_esp;
ptrdiff_t g_xbox_mem_offset;
void sub_00081480(void);
/* Test-only bridge: two complete guest pages and all eight integer registers. */
int byte_transform_run(uint32_t registers[8], uint8_t pages[8192])
{
    static uint8_t *memory;
    if (!memory) memory = calloc(1u, 0x800000u);
    if (!memory) return 0;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory;
    memcpy(memory + 0x749000u, pages, 4096u);
    memcpy(memory + 0x7BA000u, pages + 4096u, 4096u);
    g_eax = registers[0]; g_ecx = registers[1];
    g_edx = registers[2]; g_esp = registers[7];
    sub_00081480();
    registers[0] = g_eax; registers[1] = g_ecx;
    registers[2] = g_edx; registers[7] = g_esp;
    memcpy(pages, memory + 0x749000u, 4096u);
    memcpy(pages + 4096u, memory + 0x7BA000u, 4096u);
    return 1;
}
