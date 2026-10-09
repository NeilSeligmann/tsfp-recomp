/* SPDX-License-Identifier: GPL-3.0-or-later
* T1479 census list 001: original-instruction proofs for six fixed tuples.
* EXACT bodies preserve caller-visible register outputs and ordered guest stores.
* Rejected drafts and all outcomes: docs/t1479-census-batches.md.
*/
#include "game_replace.h"
/* 0x0031d2f0: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(0031D2F0, cdecl, 1, u32,
    game_is_arg_5_inverted_when_global_byte_equals_7)
{
    uint32_t mode = guest_read8(0x7de455u);
    g_ecx = game_stack_arg(0u);
    g_eax = mode == 7u ? (uint32_t)(g_ecx == 5u) : (uint32_t)(g_ecx != 5u);
}
/* 0x0031df50: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(0031DF50, cdecl, 1, u32,
    game_mapedit_reset_active_record_and_menu_globals_set_flag_1_and_store_arg)
{
    g_eax = 0u;
    guest_write32(0x762f98u,g_eax);
    guest_write32(0x778d3cu,g_eax);
    guest_write32(0x77d840u,g_eax);
    g_eax = game_stack_arg(0u);
    guest_write32(0x783a1cu,1u);
    guest_write32(0x76307cu,g_eax);
}
/* 0x00349e20: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(00349E20, cdecl, 1, u32,
    game_set_field_0x3c_of_all_four_stride_0x68_records_from_arg)
{
    g_eax = game_stack_arg(0u);
    guest_write32(0x77883cu,g_eax);
    guest_write32(0x7788a4u,g_eax);
    guest_write32(0x77890cu,g_eax);
    guest_write32(0x778974u,g_eax);
}
/* 0x0034c0d0: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(0034C0D0, cdecl, 2, u32,
    game_hud_transition_effect_set_two_params_from_args)
{
    g_eax = game_stack_arg(1u);
    g_ecx = game_stack_arg(0u);
    guest_write32(0x7676dcu,g_eax);
    guest_write32(0x7676c8u,g_ecx);
}
/* 0x003533e0: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(003533E0, cdecl, 0, u32,
    game_is_global_76b11c_nonzero)
{
    g_ecx = guest_read32(0x76b11cu);
    g_eax = (uint32_t)(g_ecx != 0u);
}
/* 0x00358e40: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(00358E40, cdecl, 0, u32,
    game_is_global_76b1a0_equal_1)
{
    g_ecx = guest_read32(0x76b1a0u);
    g_eax = (uint32_t)(g_ecx == 1u);
}
/* 0x00358e50: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(00358E50, cdecl, 0, u32,
    game_is_global_76b1a0_equal_2)
{
    g_ecx = guest_read32(0x76b1a0u);
    g_eax = (uint32_t)(g_ecx == 2u);
}
/* 0x0035fd70: original leaf, EXACT registers and guest stores. */
GAME_REPLACE_EXACT(0035FD70, cdecl, 1, u32,
    game_get_selected_online_record_index_in_mode_0x65_else_field_0x38_of_arg)
{
    if (guest_read32(0x79094cu) == 0x65u) g_eax = guest_read32(0x545d80u);
    else {
        g_eax = game_stack_arg(0u);
        g_eax = guest_read32(g_eax + 0x38u);
    }
}
