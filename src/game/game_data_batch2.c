/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
#include <string.h>
extern __thread uint32_t g_edi, g_ebx, g_ebp;
extern __thread int g_df;
static uint16_t b2_read16(uint32_t a) { uint16_t v; memcpy(&v, game_host_ptr(a), 2); return v; }
static void b2_push(uint32_t v) { g_esp-=4; guest_write32(g_esp,v); }
static uint32_t b2_pop(void) { uint32_t v=guest_read32(g_esp); g_esp+=4; return v; }
GAME_REPLACE_EXACT(0009A260, cdecl, 0, u32, game_data_b2_reset_715f2c)
{ g_ecx=guest_read32(0x6B7AA8); g_eax=0; if (!g_ecx) { guest_write32(0x715F2C,0); guest_write32(0x715F30,0); guest_write32(0x715F34,0); guest_write32(0x715F38,0); } }
GAME_REPLACE_EXACT(00058240, cdecl, 3, u32, game_data_b2_store_cutscene_pair)
{ g_eax=guest_read32(g_esp+4); g_ecx=(uint32_t)(int32_t)(int16_t)b2_read16(g_eax*2+0x7E03C0); g_edx=guest_read32(g_esp+8); g_eax=guest_read32(g_esp+12); guest_write32(g_ecx*8+0x7DE660,g_edx); guest_write32(g_ecx*8+0x7DE664,g_eax); }
GAME_REPLACE_EXACT(000698D0, cdecl, 1, u32, game_data_b2_clear_save_record)
{ b2_push(g_edi); g_edi=guest_read32(g_esp+8); if(g_edi && guest_read32(g_edi)) { g_eax=guest_read32(g_edi+0x3C); if(g_eax) guest_write32(g_eax,0); g_ecx=16;g_eax=0;while(g_ecx){guest_write32(g_edi,0);g_edi+=g_df?0xFFFFFFFCu:4u;--g_ecx;} } g_edi=b2_pop(); }
GAME_REPLACE_EXACT(000177D0, cdecl, 0, u32, game_data_b2_clear_54eec8)
{g_ecx=guest_read32(0x6B7AA8);g_eax=0;if(!g_ecx)for(uint32_t a=0x54EEC8;a<=0x54EEE4;a+=4)guest_write32(a,0);}
GAME_REPLACE_EXACT(00085830, cdecl, 1, u32, game_data_b2_rebase_room_lists)
{g_ecx=guest_read32(g_esp+4);g_eax=guest_read32(g_ecx+20);b2_push(g_edi);g_edi=0;if((int32_t)g_eax>0){b2_push(g_ebx);b2_push(g_ebp);b2_push(g_esi);g_esi=0;do{g_eax=guest_read32(g_ecx);g_eax=guest_read32(g_esi+g_eax+0xB8);g_edx=guest_read32(g_eax);if((int32_t)g_edx>0){g_eax+=4;do{g_ebp=guest_read32(g_eax);g_ebx=guest_read32(g_ecx+0x2C);g_ebp+=g_ebx;guest_write32(g_eax,g_ebp);g_eax+=4;--g_edx;}while(g_edx);}g_eax=guest_read32(g_ecx+20);++g_edi;g_esi+=0xB4;}while((int32_t)g_edi<(int32_t)g_eax);g_esi=b2_pop();g_ebp=b2_pop();g_ebx=b2_pop();}g_edi=b2_pop();}
