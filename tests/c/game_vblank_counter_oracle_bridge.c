/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#include <string.h>
__thread uint32_t g_eax,g_ecx,g_edx,g_esp;
ptrdiff_t g_xbox_mem_offset;
static uint8_t memory[0x01000000];
void sub_00022020(void);
void sub_00022030(void);
void vblank_counter_run(uint32_t entry,uint32_t registers[8],uint8_t stack[4096],uint8_t counter[12])
{
    g_xbox_mem_offset=(ptrdiff_t)(uintptr_t)memory;
    memcpy(memory+registers[7],stack,4096u);memcpy(memory+0x563914u,counter,12u);
    g_eax=registers[0];g_ecx=registers[1];g_edx=registers[2];g_esp=registers[7];
    if(entry==0x22020u)sub_00022020();else sub_00022030();
    registers[0]=g_eax;registers[1]=g_ecx;registers[2]=g_edx;registers[7]=g_esp;
    memcpy(stack,memory+registers[7]-4u,4096u);memcpy(counter,memory+0x563914u,12u);
}
