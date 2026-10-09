/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "game_replace.h"
extern __thread uint32_t g_ebx, g_ebp, g_edi;
extern __thread int g_df;
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
GAME_REPLACE_EXACT(0010E760, cdecl, 2, u32, game_record_set_vec3_0x2c)
{
    g_eax=game_stack_arg(0u);
    if(g_eax)
    {
        g_ecx=game_stack_arg(1u);
        g_edx=guest_read32(g_ecx);
        guest_write32(g_eax+0x2Cu,g_edx);
        g_edx=guest_read32(g_ecx+4u);
        guest_write32(g_eax+0x30u,g_edx);
        g_ecx=guest_read32(g_ecx+8u);
        guest_write32(g_eax+0x34u,g_ecx);
    }
}
GAME_REPLACE_EXACT(001286D0, cdecl, 0, u32, game_tables_742360_7abfa0_clear_and_reset)
{
    save_reg(g_edi);
    g_eax=0u;
    g_ecx=108u;
    g_edi=0x742360u;
    while(g_ecx)
    {
        guest_write32(g_edi,g_eax);
        g_edi+=g_df ? (uint32_t)-4 : 4u;
        g_ecx--;
    }
    g_ecx=380u;
    g_edi=0x7ABFA0u;
    while(g_ecx)
    {
        guest_write32(g_edi,g_eax);
        g_edi+=g_df ? (uint32_t)-4 : 4u;
        g_ecx--;
    }
    guest_write32(0x74235Cu,g_eax);
    g_eax=guest_read32(0x7497A8u);
    guest_write32(0x7ABF80u,1u);
    guest_write32(0x742358u,g_eax);
    g_edi=restore_reg();
}
GAME_REPLACE_EXACT(001553E0, cdecl, 4, u32, game_globals_4e7958_set_four_dwords)
{
    g_eax=game_stack_arg(0u);
    g_ecx=game_stack_arg(1u);
    g_edx=game_stack_arg(2u);
    guest_write32(0x4E7958u,g_eax);
    g_eax=game_stack_arg(3u);
    guest_write32(0x4E795Cu,g_ecx);
    guest_write32(0x4E7960u,g_edx);
    guest_write32(0x4E7964u,g_eax);
}
GAME_REPLACE_EXACT(0015BCA0, cdecl, 1, u32, game_array_7a3580_append_obj_field_0x7c)
{
    g_eax=game_stack_arg(0u);
    g_ecx=guest_read32(g_eax+0x7Cu);
    g_eax=guest_read32(0x74C37Cu);
    guest_write32(g_eax*4u+0x7A3580u,g_ecx);
    g_eax++;
    guest_write32(0x74C37Cu,g_eax);
}
GAME_REPLACE_EXACT(0015BFB0, cdecl, 1, u32, game_table_7a2900_append_arg_to_entry_by_7a295c_count_capped_10)
{
    g_eax=guest_read32(0x7A295Cu);
    g_ecx=g_eax*44u;
    g_eax*=11u;
    g_edx=guest_read32(g_ecx+0x7A2900u);
    g_ecx+=0x7A2900u;
    g_eax+=g_edx;
    g_edx=game_stack_arg(0u);
    guest_write32(g_eax*4u+0x7A2904u,g_edx);
    g_eax=guest_read32(g_ecx);
    if((int32_t)g_eax<10)
    {
        g_eax++;
        guest_write32(g_ecx,g_eax);
    }
}
GAME_REPLACE_EXACT(001665A0, cdecl, 0, u32, game_pool_74c468_pop_stride_0x38)
{
    g_eax=guest_read32(0x4E826Cu);
    if((int32_t)g_eax>0)
    {
        g_ecx=guest_read32(0x74C468u);
        g_eax--;
        guest_write32(0x4E826Cu,g_eax);
        g_eax=g_eax*56u+g_ecx;
    }
    else g_eax=0u;
}
GAME_REPLACE_EXACT(00167A70, cdecl, 1, u32, game_entry_74c458_addr_field_0x17c)
{
    g_eax=game_stack_arg(0u);
    g_ecx=guest_read32(0x74C458u);
    g_eax=((g_eax&0xFFFFu)<<9)+g_ecx+0x17Cu;
}
GAME_REPLACE_EXACT(001680B0, cdecl, 1, u32, game_entry_74c458_get_flags_0x1c0)
{
    g_eax=game_stack_arg(0u);
    g_ecx=guest_read32(0x74C458u);
    g_eax=guest_read32(((g_eax&0xFFFFu)<<9)+g_ecx+0x1C0u);
}
GAME_REPLACE_EXACT(00177E90, cdecl, 1, u32, game_object_child_0x7c_fields_0x56c_0x578_0x57c_all_nonnegative)
{
    g_eax=game_stack_arg(0u);
    g_eax=guest_read32(g_eax+0x7Cu);
    g_ecx=guest_read32(g_eax+0x56Cu);
    if((int32_t)g_ecx>=0)
    {
        g_ecx=guest_read32(g_eax+0x578u);
        if((int32_t)g_ecx>=0)
        {
            g_ecx=guest_read32(g_eax+0x57Cu);
            g_eax=(int32_t)g_ecx>=0;
        }
        else g_eax=0u;
    }
    else g_eax=0u;
}
