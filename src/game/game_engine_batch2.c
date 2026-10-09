/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1787 batch2: nine register-exact admissions; descriptive names INFERRED.
 * Five synth-v2 and four default-domain proofs; frozen receipts in docs/. */
#include "game_replace.h"
GAME_REPLACE_EXACT(00045D60, cdecl, 1, u32, game_audio_global_mask_bit_test_batch2)
{ g_ecx=guest_read32(g_esp+4); g_eax=1u<<(g_ecx&31); g_eax&=guest_read32(0x4CB54C); g_eax=(g_eax!=0); }
GAME_REPLACE_EXACT(00059D70, cdecl, 2, u32, game_anim_fieldd4_equals_arg_batch2)
{ g_eax=guest_read32(g_esp+4); g_ecx=guest_read32(g_eax+4); g_edx=guest_read32(g_ecx+0xD4); g_ecx=guest_read32(g_esp+8); g_eax=(g_edx==g_ecx); }
GAME_REPLACE_EXACT(000A4C70, cdecl, 1, u32, game_object_wrapping_capacity_signed_test_batch2)
{ g_edx=guest_read32(0x7B65B8);g_eax=guest_read32(0x7B4FAC);g_ecx=guest_read32(g_esp+4);g_eax-=g_edx;g_edx=g_eax+g_ecx+4;g_ecx=guest_read32(0x72F5E4);g_eax=((int32_t)g_edx<(int32_t)g_ecx); }
GAME_REPLACE_EXACT(000B0F90, cdecl, 1, u32, game_object_nullable_extension_state0_batch2)
{ g_eax=guest_read32(g_esp+4); if(g_eax){g_eax=guest_read32(g_eax+0x7C);g_edx=guest_read32(g_eax+0x10);g_ecx=(g_edx==0);g_eax=g_ecx;} }
GAME_REPLACE_EXACT(000FF490, cdecl, 2, u32, game_effect_nullable_xyz_copy20_batch2)
{ g_eax=guest_read32(g_esp+4);if(g_eax){g_ecx=guest_read32(g_esp+8);g_edx=guest_read32(g_ecx);guest_write32(g_eax+0x20,g_edx);g_edx=guest_read32(g_ecx+4);guest_write32(g_eax+0x24,g_edx);g_ecx=guest_read32(g_ecx+8);guest_write32(g_eax+0x28,g_ecx);} }
GAME_REPLACE_EXACT(00274330, cdecl, 1, u32, game_object_index_extension_mask4_batch2)
{ g_eax=guest_read32(g_esp+4); g_ecx=guest_read32(g_eax*8u+0x7A2BE4); g_edx=guest_read32(g_ecx+0x7C); g_eax=guest_read32(g_edx+0xA0)&4; }
GAME_REPLACE_EXACT(00274350, cdecl, 1, u32, game_object_flag_slot_extension_mask4_batch2)
{ g_ecx=guest_read32(g_esp+4);g_eax=(g_ecx==1);g_ecx=guest_read32(g_eax*8u+0x7A2BEC);g_edx=guest_read32(g_ecx+0x7C);++g_eax;g_eax=guest_read32(g_edx+0xA0)&4; }
GAME_REPLACE_EXACT(00274380, cdecl, 1, u32, game_object_index_slot_bit7_clear_batch2)
{ g_eax=guest_read32(g_esp+4); g_ecx=guest_read32(g_eax*8u+0x7A2BE4); g_eax=guest_read32(g_ecx+0x28); g_eax=(~(g_eax>>7))&1; }
GAME_REPLACE_EXACT(002744C0, cdecl, 1, u32, game_object_flag_slot_extension_field3c_batch2)
{ g_edx=guest_read32(g_esp+4);g_eax=(g_edx==1);g_ecx=guest_read32(g_eax*8u+0x7A2BEC);g_edx=guest_read32(g_ecx+0x7C);++g_eax;g_eax=guest_read32(g_edx+0x3C); }
