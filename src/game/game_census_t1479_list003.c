/* T1479 list003 frozen original-instruction drafts; see docs/t1479-census-batches.md. */
#include "game_replace.h"
extern __thread uint32_t g_esi, g_edi, g_ebx;
/* 0x002a9b70: EXACT registers and ordered guest memory. */
GAME_REPLACE_EXACT(002A9B70, cdecl, 4, u32,
    game_grid_cell_pair_fetch_stride_0x28_offset_0x10_0x14_cmp_limit_2)
{
    g_edx = game_stack_arg(1u);
    uint32_t selection = (int32_t) guest_read32(g_edx) >= 2 ? 4u : 0u;
    guest_write32(g_esp - 4u, g_esi);
    g_ecx = guest_read32(g_edx + 4u);
    g_eax = selection * 7u;
    g_ecx += g_eax;
    guest_write32(g_esp - 8u, g_ebx);
    uint32_t destination = game_stack_arg(2u);
    g_ecx *= 5u;
    guest_write32(g_esp - 12u, g_edi);
    uint32_t table = game_stack_arg(0u);
    g_ecx = guest_read32(table + g_ecx * 8u + 16u);
    guest_write32(destination, g_ecx);
    g_ecx = guest_read32(g_edx + 4u) + g_eax;
    g_eax = g_ecx * 5u;
    g_ecx = guest_read32(table + g_eax * 8u + 20u);
    g_eax = game_stack_arg(3u);
    guest_write32(destination + 4u, g_ecx);
    guest_write32(g_eax, selection);
    g_ecx = guest_read32(g_edx + 4u);
    guest_write32(g_eax + 4u, g_ecx);
    g_edx = guest_read32(g_edx);
    g_edi = guest_read32(g_esp - 12u);
    g_ebx = guest_read32(g_esp - 8u);
    g_eax = (int32_t) selection > (int32_t) g_edx ? 0u : selection == g_edx ? 0xffffffffu : 2u;
    g_esi = guest_read32(g_esp - 4u);
}
/* 0x002a9be0: EXACT registers and ordered guest memory. */
GAME_REPLACE_EXACT(002A9BE0, cdecl, 4, u32,
    game_grid_cell_pair_fetch_stride_0x118_offset_0x100_0x104_cmp_limit_6)
{
    g_eax = game_stack_arg(1u);
    g_edx = guest_read32(g_eax) * 0x118u;
    g_ecx = game_stack_arg(0u);
    guest_write32(g_esp - 4u, g_esi);
    uint32_t first = guest_read32(g_edx + g_ecx + 256u);
    g_edx = game_stack_arg(2u);
    guest_write32(g_edx, first);
    uint32_t row = guest_read32(g_eax) * 0x118u;
    g_ecx = guest_read32(row + g_ecx + 260u);
    guest_write32(g_edx + 4u, g_ecx);
    g_edx = guest_read32(g_eax);
    g_ecx = game_stack_arg(3u);
    guest_write32(g_ecx, g_edx);
    guest_write32(g_ecx + 4u, 6u);
    g_eax = guest_read32(g_eax + 4u);
    g_esi = guest_read32(g_esp - 4u);
    if ((int32_t) g_eax < 6) g_eax = 3u;
    else {
        g_ecx = (int32_t) g_eax > 6 ? 1u : 0xffffffffu;
        g_eax = g_ecx;
    }
}
/* 0x002a9c40: EXACT registers and ordered guest memory. */
GAME_REPLACE_EXACT(002A9C40, cdecl, 4, u32,
    game_record_pair_fetch_stride_0x28_offset_0x240_0x244_cmp_limit_2)
{
    g_eax = game_stack_arg(1u);
    g_ecx = guest_read32(g_eax + 4u) * 5u;
    g_edx = game_stack_arg(0u);
    g_ecx = guest_read32(g_edx + g_ecx * 8u + 0x240u);
    guest_write32(g_esp - 4u, g_esi);
    uint32_t destination = game_stack_arg(2u);
    guest_write32(destination, g_ecx);
    g_ecx = guest_read32(g_eax + 4u) * 5u;
    g_edx = guest_read32(g_edx + g_ecx * 8u + 0x244u);
    g_ecx = game_stack_arg(3u);
    guest_write32(destination + 4u, g_edx);
    guest_write32(g_ecx, 2u);
    g_edx = guest_read32(g_eax + 4u);
    guest_write32(g_ecx + 4u, g_edx);
    g_eax = guest_read32(g_eax);
    g_esi = guest_read32(g_esp - 4u);
    if ((int32_t) g_eax < 2) g_eax = 0u;
    else {
        g_ecx = g_eax == 2u ? 0xffffffffu : 2u;
        g_eax = g_ecx;
    }
}
/* 0x002a9ca0: EXACT registers and ordered guest memory. */
GAME_REPLACE_EXACT(002A9CA0, cdecl, 4, u32,
    game_grid_cell_pair_fetch_stride_0x118_offset_0x88_0x8c_cmp_limit_3)
{
    g_eax = game_stack_arg(1u);
    g_edx = guest_read32(g_eax) * 0x118u;
    g_ecx = game_stack_arg(0u);
    guest_write32(g_esp - 4u, g_esi);
    uint32_t first = guest_read32(g_edx + g_ecx + 136u);
    g_edx = game_stack_arg(2u);
    guest_write32(g_edx, first);
    uint32_t row = guest_read32(g_eax) * 0x118u;
    g_ecx = guest_read32(row + g_ecx + 140u);
    guest_write32(g_edx + 4u, g_ecx);
    g_edx = guest_read32(g_eax);
    g_ecx = game_stack_arg(3u);
    guest_write32(g_ecx, g_edx);
    guest_write32(g_ecx + 4u, 3u);
    g_eax = guest_read32(g_eax + 4u);
    g_esi = guest_read32(g_esp - 4u);
    if ((int32_t) g_eax < 3) g_eax = 3u;
    else {
        g_ecx = (int32_t) g_eax > 3 ? 1u : 0xffffffffu;
        g_eax = g_ecx;
    }
}
/* 0x002a9e10: EXACT registers and ordered guest memory. */
GAME_REPLACE_EXACT(002A9E10, cdecl, 2, u32,
    game_compare_int_pairs_lexicographic_ordering_code)
{
    g_edx = game_stack_arg(1u);
    g_eax = guest_read32(g_edx);
    guest_write32(g_esp - 4u, g_esi);
    uint32_t left = game_stack_arg(0u);
    g_ecx = guest_read32(left);
    if (g_eax == g_ecx) {
        g_eax = guest_read32(g_edx + 4u);
        g_edx = guest_read32(left + 4u);
        g_ecx = (int32_t) g_eax < (int32_t) g_edx ? 3u : 1u;
        g_esi = guest_read32(g_esp - 4u);
        g_eax = g_ecx;
        return;
    }
    g_edx = guest_read32(g_edx + 4u);
    if (g_edx == guest_read32(left + 4u)) {
        g_edx = (int32_t) g_eax < (int32_t) g_ecx ? 0u : 2u;
        g_esi = guest_read32(g_esp - 4u);
        g_eax = g_edx;
        return;
    }
    g_eax = 0xffffffffu;
    g_esi = guest_read32(g_esp - 4u);
}
/* 0x002be600: EXACT registers and ordered guest memory. */
GAME_REPLACE_EXACT(002BE600, cdecl, 2, u32,
    game_find_record_30_index_by_key_pair)
{
    guest_write32(g_esp - 4u, g_ebx);
    guest_write32(g_esp - 8u, g_esi);
    uint32_t tag = game_stack_arg(1u);
    guest_write32(g_esp - 12u, g_edi);
    uint32_t key = game_stack_arg(0u);
    g_eax = 0u;
    g_ecx = 0u;
    for (;;) {
        if (guest_read32(g_ecx + 0x521a1cu) == key) {
            if ((int32_t) tag < 0) break;
            g_edx = (g_edx & 0xffffff00u) | guest_read8(g_ecx + 0x521a19u);
            if ((uint32_t)(int32_t)(int8_t) g_edx == tag || (uint8_t) g_edx == 255u) break;
        }
        g_ecx += 48u;
        ++ g_eax;
        if (g_ecx >= 0x1c20u) {
            g_eax = 0xffffffffu;
            break;
        }
    }
    g_edi = guest_read32(g_esp - 12u);
    g_esi = guest_read32(g_esp - 8u);
    g_ebx = guest_read32(g_esp - 4u);
}
/* 0x002fbb10: EXACT registers and ordered guest memory. */
GAME_REPLACE_EXACT(002FBB10, cdecl, 0, u32,
    game_count_nodes_of_list_78400c_with_flag_bit1_of_byte_14)
{
    g_ecx = guest_read32(0x78400cu);
    g_eax = 0u;
    while (g_ecx) {
        g_edx = guest_read8(g_ecx + 20u);
        g_ecx = guest_read32(g_ecx + 64u);
        g_edx = (g_edx >> 1u) & 1u;
        g_eax += g_edx;
    }
}
/* 0x0031dd10: EXACT registers and ordered guest memory. */
GAME_REPLACE_EXACT(0031DD10, cdecl, 2, u32,
    game_unpack_bitmask_into_flag_struct_fields_8_c_10_4)
{
    g_eax = guest_read8(g_esp + 8u);
    g_ecx = game_stack_arg(0u);
    g_edx = g_eax & 1u;
    guest_write32(g_ecx + 8u, g_edx);
    g_edx = (g_eax >> 1u) & 1u;
    guest_write32(g_ecx + 12u, g_edx);
    g_edx = (g_eax >> 2u) & 1u;
    g_eax &= 8u;
    guest_write32(g_ecx + 16u, g_edx);
    guest_write32(g_ecx + 4u, g_eax);
}
/* 0x00359f40: EXACT registers and ordered guest memory. */
GAME_REPLACE_EXACT(00359F40, cdecl, 2, u32,
    game_net_handle_received_slot_counter_update_2fc_if_greater)
{
    guest_write32(g_esp - 4u, g_esi);
    uint32_t data = game_stack_arg(0u);
    if (data) {
        g_eax = game_stack_arg(1u);
        g_ecx = (g_ecx & 0xffffff00u) | guest_read8(g_eax);
        g_edx = (g_edx & 0xffffff00u) | guest_read8(g_eax + 1u);
        g_eax += 2u;
        guest_write32(g_esp - 8u, g_ebx);
        uint32_t slot = guest_read8(g_eax);
        guest_write8(g_esp + 4u, (uint8_t) g_edx);
        g_edx = guest_read32(g_eax + 1u);
        g_eax = slot * 0x30cu;
        g_ecx &= 255u;
        uint32_t old = guest_read32(data + g_ecx * 4u + 0x2fcu);
        g_eax += 0x774180u;
        g_ebx = guest_read32(g_esp - 8u);
        if (g_edx > old) {
            guest_write32(g_eax + g_ecx * 4u + 0x2fcu, g_edx);
            g_eax = guest_read32(g_eax) * 0x30cu + 0x774180u;
            g_edx = g_ecx;
            if (g_edx)-- g_edx;
            if (g_edx == 0u) {
                g_edx = (g_edx & 0xffffff00u) | guest_read8(g_esp + 4u);
                guest_write8(g_eax + g_ecx + 0x2fau, (uint8_t) g_edx);
            }
        }
    }
    g_esi = guest_read32(g_esp - 4u);
}
/* 0x0036e8c0: EXACT registers and ordered guest memory. */
GAME_REPLACE_EXACT(0036E8C0, cdecl, 8, u32,
    game_terminal_button_widget_init_rect_and_callback_fields)
{
    g_eax = game_stack_arg(0u);
    g_ecx = game_stack_arg(1u);
    g_edx = guest_read32(g_ecx);
    guest_write32(g_eax, g_edx);
    g_edx = guest_read32(g_ecx + 8u);
    guest_write32(g_eax + 8u, g_edx);
    g_edx = guest_read32(g_ecx + 4u);
    guest_write32(g_eax + 4u, g_edx);
    g_ecx = guest_read32(g_ecx + 12u);
    guest_write32(g_eax + 12u, g_ecx);
    g_edx = guest_read32(g_eax);
    g_ecx = guest_read32(g_eax + 4u);
    guest_write32(g_eax + 32u, g_edx);
    g_edx = guest_read32(g_eax + 8u);
    guest_write32(g_eax + 36u, g_ecx);
    g_ecx = guest_read32(g_eax + 12u);
    guest_write32(g_eax + 40u, g_edx);
    g_edx = game_stack_arg(2u);
    guest_write32(g_eax + 44u, g_ecx);
    g_ecx = game_stack_arg(3u);
    guest_write32(g_eax + 64u, g_edx);
    g_edx = game_stack_arg(4u);
    guest_write32(g_eax + 68u, g_ecx);
    g_ecx = game_stack_arg(5u);
    guest_write32(g_eax + 80u, g_edx);
    g_edx = game_stack_arg(6u);
    guest_write32(g_eax + 84u, g_ecx);
    g_ecx = game_stack_arg(7u);
    guest_write32(g_eax + 72u, g_edx);
    guest_write32(g_eax + 76u, g_ecx);
}
/* 0x00377730: EXACT registers and ordered guest memory. */
GAME_REPLACE_EXACT(00377730, cdecl, 2, u32,
    game_table_548f5c_replace_key_of_entry_matching_arg_with_second_arg)
{
    g_ecx = guest_read32(0x548f58u);
    g_eax = 0u;
    if (! g_ecx) return;
    g_ecx = game_stack_arg(0u);
    for (;;) {
        if (guest_read32(0x548f5cu + g_eax * 8u) == g_ecx) {
            g_ecx = game_stack_arg(1u);
            guest_write32(0x548f5cu + g_eax * 8u, g_ecx);
            return;
        }
        g_edx = guest_read32(0x548f60u + g_eax * 8u);
        ++ g_eax;
        if (! g_edx) return;
    }
}
