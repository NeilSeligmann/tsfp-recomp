/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1789 original-instruction hand translations. Fixed default-domain gates passed.
 * MEASURED harness evidence; remaining limits: docs/t-prove-campaign-mapmaker.md.
 * Register exact; guest stack stores and alias-sensitive pops retained. */
#include "game_replace.h"
extern __thread uint32_t g_edi;
GAME_REPLACE_EXACT(00336850, cdecl, 0, u32, game_mapmaker_336850)
{ g_ecx=guest_read32(0x765C30); g_eax=(g_ecx!=0); }
GAME_REPLACE_EXACT(00308690, cdecl, 0, u32, game_mapmaker_308690)
{
 g_esp-=4; guest_write32(g_esp,g_edi); g_eax=0; g_ecx=0x2D; g_edi=0x783F40;
 while(g_ecx){guest_write32(g_edi,g_eax); g_edi+=4; --g_ecx;}
 guest_write32(0x783FF4,g_eax); g_edi=guest_read32(g_esp); g_esp+=4;
}
GAME_REPLACE_EXACT(002FE450, cdecl, 1, u32, game_mapmaker_2fe450)
{
 g_ecx=guest_read32(g_esp+4); g_eax=guest_read32(g_ecx+0x84);
 if((int32_t)g_eax>=0x35 && ((int32_t)g_eax<=0x37 || g_eax==0x46))g_eax=1;
 else {g_edx=guest_read32(g_ecx+0x80); g_eax=(g_edx==11);}
}
