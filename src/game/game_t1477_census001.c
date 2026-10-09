/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
extern __thread uint32_t g_ebx, g_ebp, g_edi;
static inline void save_reg(uint32_t value)
{
    uint32_t address=g_esp-4u;
    guest_write32(address,value);
    g_esp=address;
}
static inline uint32_t restore_reg(void)
{
    uint32_t value=guest_read32(g_esp);
    g_esp+=4u;
    return value;
}
GAME_REPLACE_EXACT(0010E9A0, cdecl, 2, u32, game_record_set_vec3_0x98_flag_0x4000)
{
    g_eax=game_stack_arg(0u);
    if(g_eax)
    {
        g_ecx=game_stack_arg(1u);
        g_edx=guest_read32(g_ecx);
        guest_write32(g_eax+0x98u,g_edx);
        g_edx=guest_read32(g_ecx+4u);
        guest_write32(g_eax+0x9Cu,g_edx);
        g_ecx=guest_read32(g_ecx+8u);
        guest_write32(g_eax+0xA0u,g_ecx);
        guest_write32(g_eax+4u,guest_read32(g_eax+4u)|0x4000u);
    }
}
GAME_REPLACE_EXACT(0010E9D0, cdecl, 1, u32, game_record_7ad28c_set_vec3_0x98_flag_0x4000)
{
    g_ecx=guest_read32(0x7AD28Cu);
    if(g_ecx)
    {
        g_eax=game_stack_arg(0u);
        g_edx=guest_read32(g_eax);
        guest_write32(g_ecx+0x98u,g_edx);
        g_ecx=guest_read32(g_eax+4u);
        g_edx=guest_read32(0x7AD28Cu);
        guest_write32(g_edx+0x9Cu,g_ecx);
        g_eax=guest_read32(g_eax+8u);
        g_ecx=guest_read32(0x7AD28Cu);
        guest_write32(g_ecx+0xA0u,g_eax);
        g_eax=guest_read32(0x7AD28Cu);
        guest_write32(g_eax+4u,guest_read32(g_eax+4u)|0x4000u);
    }
}
GAME_REPLACE_EXACT(0010EBD0, cdecl, 1, u32, game_object_is_field_0x50_equal_2)
{
    g_eax=game_stack_arg(0u);
    if(g_eax)
    {
        g_edx=guest_read32(g_eax+0x50u);
        g_ecx=g_edx==2u;
        g_eax=g_ecx;
    }
    else g_eax=0u;
}
GAME_REPLACE_EXACT(00115EA0, cdecl, 0, u32, game_global_73d96c_clear_unless_6b7aa8)
{
    g_eax=guest_read32(0x6B7AA8u);
    if(!g_eax)guest_write32(0x73D96Cu,0u);
}
GAME_REPLACE_EXACT(0012B960, cdecl, 1, u32, game_ring_742510_set_prev_entry_color_channels_divided_by_3)
{
    g_ecx=guest_read32(0x7450D0u)-1u;
    if((int32_t)g_ecx<0)g_ecx+=200u;
    save_reg(g_ebx);
    g_ecx*=56u;
    g_ebx=game_stack_arg(1u);
    g_edx=(g_ebx>>8)&255u;
    uint64_t product=(uint64_t)0xAAAAAAABu*g_edx;
    g_eax=(uint32_t)product;
    g_edx=(uint32_t)(product>>32)>>1;
    save_reg(g_esi);
    g_esi=g_edx;
    g_ebx&=255u;
    product=(uint64_t)0xAAAAAAABu*g_ebx;
    g_eax=(uint32_t)product;
    g_edx=((uint32_t)(product>>32)>>1)<<8;
    g_esi=(g_esi&255u)|g_edx;
    g_edx=(game_stack_arg(2u)>>16)&255u;
    product=(uint64_t)0xAAAAAAABu*g_edx;
    g_eax=(uint32_t)product;
    g_edx=((uint32_t)(product>>32)>>1)&255u;
    g_esi=((g_esi<<8)|g_edx)<<8;
    guest_write32(g_ecx+0x742544u,g_esi);
    g_esi=restore_reg();
    g_ebx=restore_reg();
}
GAME_REPLACE_EXACT(00158D40, cdecl, 4, u32, game_object_ext_clear_state_flag_masks_0x28_0x2c_and_bit_0x80_when_mask_0xc000000_only_in_first_arg)
{
    g_eax=game_stack_arg(0u);
    g_eax=guest_read32(g_eax+0x7Cu);
    g_ecx=guest_read32(g_eax+0x2Cu)&0xFFFFFBDEu;
    g_edx=guest_read32(g_eax+0x28u)&0xFFFBB7FCu;
    guest_write32(g_eax+0x2Cu,g_ecx);
    g_ecx=game_stack_arg(3u);
    save_reg(g_esi);
    guest_write32(g_eax+0x28u,g_edx);
    g_esi=g_edx;
    g_ecx&=0x0C000000u;
    g_edx=0u;
    if(g_ecx)
    {
        g_ecx=game_stack_arg(2u)&0x0C000000u;
        if(!g_ecx)
        {
            g_esi&=0xFFFFFF7Fu;
            guest_write32(g_eax+0x28u,g_esi);
        }
    }
    g_esi=restore_reg();
}
GAME_REPLACE_EXACT(00160E80, cdecl, 1, u32, game_table_7a2d84_any_masked_entry_nonzero)
{
    g_edx=game_stack_arg(0u);
    g_ecx=0u;
    g_eax=0x7A2D84u;
    save_reg(g_esi);
    do
    {
        g_esi=1u<<(g_ecx&31u);
        if((g_edx&g_esi)&&guest_read32(g_eax))
        {
            g_eax=1u;
            g_esi=restore_reg();
            return;
        }
        g_eax+=96u;
        g_ecx++;
    }
    while((int32_t)g_eax<0x7A3504);
    g_eax=0u;
    g_esi=restore_reg();
}
GAME_REPLACE_EXACT(001656B0, cdecl, 1, u32, game_table_74c46c_get_dword_by_id16)
{
    g_eax=game_stack_arg(0u);
    g_ecx=guest_read32(0x74C46Cu);
    g_eax&=0xFFFFu;
    g_eax=guest_read32(g_ecx+g_eax*4u);
}
GAME_REPLACE_EXACT(00167A10, cdecl, 1, u32, game_entry_74c458_addr_field_0x134)
{
    g_eax=game_stack_arg(0u);
    g_ecx=guest_read32(0x74C458u);
    g_eax=((g_eax&0xFFFFu)<<9)+g_ecx+0x134u;
}
