/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1787 handwritten register-exact engine leaves.
 * Six admissions pass the frozen default-domain gates; descriptive roles INFERRED. */
#include "game_replace.h"
static uint16_t campaign_read16(uint32_t a) { uint16_t v; memcpy(&v,game_host_ptr(a),2); return v; }
static void campaign_write16(uint32_t a,uint16_t v) { memcpy(game_host_ptr(a),&v,2); }
GAME_REPLACE_EXACT(00063BF0, cdecl, 1, u32, game_audio_pair_cursor_equal_campaign)
{ g_ecx=guest_read32(g_esp+4); g_eax=guest_read32(g_ecx); g_eax-=guest_read32(g_ecx+4); g_eax=(g_eax==0); }
GAME_REPLACE_EXACT(000942D0, cdecl, 2, u32, game_object_flags_clear_word_campaign)
{ g_ecx=guest_read32(g_esp+8); g_eax=guest_read32(g_esp+4); g_ecx=~g_ecx; campaign_write16(g_eax+0x1F6,campaign_read16(g_eax+0x1F6)&(uint16_t)g_ecx); }
GAME_REPLACE_EXACT(00242EC0, cdecl, 1, u32, game_object_extension_state1_test_campaign)
{ g_eax=guest_read32(g_esp+4); g_ecx=guest_read32(g_eax+0x7C); g_edx=guest_read32(g_ecx+0x10); g_eax=(g_edx==1); }
GAME_REPLACE_EXACT(002744F0, cdecl, 1, u32, game_object_slot_from_flag_campaign)
{ g_ecx=guest_read32(g_esp+4); g_eax=(g_ecx==1); ++g_eax; g_eax=guest_read32(g_eax*8+0x7A2BE4); }
GAME_REPLACE_EXACT(001B63B0, cdecl, 2, u32, game_object_fielde8_equals_arg_campaign)
{ g_ecx=guest_read32(g_esp+4); g_edx=guest_read32(g_ecx+0xE8); g_ecx=guest_read32(g_esp+8); g_eax=(g_edx==g_ecx); }
GAME_REPLACE_EXACT(0003EA20, cdecl, 0, u32, game_memory_clear_pair_when_idle_campaign)
{ g_ecx=guest_read32(0x6B7AA8); g_eax=0; if(g_ecx==0){guest_write32(0x6B83C8,g_eax);guest_write32(0x6B83CC,g_eax);} }
