#include "game_replace.h"
extern __thread uint32_t g_esi, g_edi, g_ebx;
extern __thread int g_df;
static uint16_t guest_read16(uint32_t address) { uint16_t value; memcpy(&value, game_host_ptr(address), sizeof value); return value; }
static void guest_write16(uint32_t address, uint16_t value) { memcpy(game_host_ptr(address), &value, sizeof value); }


/* 0x002ed7b0: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(002ED7B0, cdecl, 0, u32, game_clear_state_block_761c58)
{
g_eax=0u; guest_write32(0x761c58u,0u);guest_write32(0x7625f4u,0u);guest_write32(0x761c7cu,1u);guest_write32(0x761c68u,0u);guest_write32(0x761c6cu,0u);guest_write32(0x761c70u,0u);guest_write32(0x761c74u,0u);guest_write32(0x761bdcu,0u);guest_write32(0x761be0u,0u);guest_write32(0x761be4u,0u);guest_write32(0x761be8u,0u);
}

/* 0x00346250: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(00346250, cdecl, 3, u32, game_hud_queue_overlay_request_12_bytes_max_0x20)
{
g_ecx=guest_read32(0x7670e0u);if((int32_t)g_ecx>=32)return;g_edx=game_stack_arg(0u);g_eax=g_ecx*12u;guest_write32(g_eax+0x766f60u,g_edx);g_edx=game_stack_arg(1u)&0xffffff00u;guest_write32(g_eax+0x766f64u,g_edx);g_edx=game_stack_arg(2u);++g_ecx;guest_write32(g_eax+0x766f68u,g_edx);guest_write32(0x7670e0u,g_ecx);
}

/* 0x00350d40: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(00350D40, cdecl, 1, u32, game_type_table_dword_by_object_word_0x48_when_mode_0xa_or_flag_bit19)
{
uint32_t mode=guest_read8(0x7de455u);g_eax=game_stack_arg(0u);g_eax=(uint32_t)(int32_t)(int16_t)guest_read16(g_eax+0x48u);if(mode!=10u && !(guest_read32(0x7de458u)&0x80000u)){g_eax=0u;return;}g_ecx=g_eax*5u;g_eax=guest_read32(g_ecx*4u+0x53c508u);
}

/* 0x00350e70: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(00350E70, cdecl, 2, u32, game_type_table_flag_mask_test_by_object_word_0x48)
{
g_ecx=game_stack_arg(1u);g_eax=game_stack_arg(0u);g_eax=(uint32_t)(int32_t)(int16_t)guest_read16(g_eax+0x48u);if(g_ecx&1u){g_edx=g_eax*5u;if(guest_read32(g_edx*4u+0x53c508u)&g_ecx){g_eax=1u;return;}}if(guest_read8(0x7de455u)!=10u && !(guest_read32(0x7de458u)&0x80000u)){g_eax=0u;return;}g_eax*=5u;g_eax=guest_read32(g_eax*4u+0x53c508u)&g_ecx;
}

/* 0x00356020: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(00356020, cdecl, 3, u32, game_net_player_slot_30c_claim_by_index)
{
g_ecx=game_stack_arg(0u);if((int32_t)g_ecx<0 || (int32_t)g_ecx>=16)return;g_eax=g_ecx*0x30cu;guest_write32(g_esp-4u,g_ebx);uint32_t flag=guest_read8(g_eax+0x7741b4u);guest_write32(g_eax+0x774180u,g_ecx);g_ecx=game_stack_arg(1u);g_edx=(g_edx&0xffffff00u)|1u;guest_write32(g_eax+0x774184u,g_ecx);g_ecx=game_stack_arg(2u);guest_write8(g_eax+0x7741b4u,(uint8_t)(flag|1u));guest_write32(g_eax+0x7741a0u,g_ecx);guest_write8(g_eax+0x774188u,1u);guest_write8(g_eax+0x774189u,0u);guest_write16(g_eax+0x7741b6u,0u);
g_ebx = guest_read32(g_esp - 4u);
}

/* 0x0035e590: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(0035E590, cdecl, 2, u32, game_net_is_slot_limit_reached_76bbbc)
{
g_eax=guest_read32(0x76bbbcu);g_ecx=game_stack_arg(1u);if((int32_t)g_eax>=(int32_t)g_ecx){g_eax=1u;return;}g_edx=game_stack_arg(0u);if(g_edx){g_eax=0u;return;}g_edx=guest_read32(0x76bbc4u)+g_eax;g_eax=(int32_t)g_edx>=(int32_t)g_ecx;
}

/* 0x0037a6f0: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(0037A6F0, cdecl, 2, u32, game_object_type_8_copy_vec3_from_ext_child_0xaa0_fields_0x14_0x18_0x1c)
{
g_eax=game_stack_arg(0u);if(guest_read32(g_eax+32u)!=8u)return;g_eax=guest_read32(g_eax+0x7cu);if(!g_eax)return;g_eax=guest_read32(g_eax+0xaa0u);if(!g_eax)return;g_edx=guest_read32(g_eax+20u);g_ecx=game_stack_arg(1u);guest_write32(g_ecx,g_edx);g_edx=guest_read32(g_eax+24u);guest_write32(g_ecx+4u,g_edx);g_eax=guest_read32(g_eax+28u);guest_write32(g_ecx+8u,g_eax);
}
