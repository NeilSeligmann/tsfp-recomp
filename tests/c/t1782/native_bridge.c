/* SPDX-License-Identifier: GPL-3.0-or-later
 * Standalone native adapter replay. Inputs and expectations come from retail
 * instructions under Unicorn, including guest-stack alias states.
 */
#include "game_replace.h"
__thread uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi, g_fs_base;
__thread int g_df;
ptrdiff_t g_xbox_mem_offset;
static unsigned char memory[0x01000000u];
unsigned char *native_memory(void) { g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)memory; return memory; }
int native_execute(uint32_t va, uint32_t *regs, int df)
{
    g_eax=regs[0]; g_ecx=regs[1]; g_edx=regs[2]; g_ebx=regs[3];
    g_esp=regs[4]; g_ebp=regs[5]; g_esi=regs[6]; g_edi=regs[7]; g_df=df;
    size_t count=0u;
    const game_replacement *table=game_replacement_table(&count);
    const game_replacement *entry=NULL;
    for (size_t i=0u;i<count;++i) if(table[i].va==va) entry=&table[i];
    if (entry==NULL) return 1;
    entry->adapter();
#ifdef SYNTH_MUTANT_VA
    if (va == SYNTH_MUTANT_VA) g_eax ^= 1u;
#endif
    regs[0]=g_eax;regs[1]=g_ecx;regs[2]=g_edx;regs[3]=g_ebx;
    regs[4]=g_esp;regs[5]=g_ebp;regs[6]=g_esi;regs[7]=g_edi;
    return 0;
}
