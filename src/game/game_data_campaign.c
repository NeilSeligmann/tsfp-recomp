/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1786 hand-written register-exact data candidates. Eight default-domain admissions; see docs/t-prove-campaign-data.md.
 * Preserve original memory order, guest stack aliases, and direction flag. */
#include "game_replace.h"
extern __thread uint32_t g_edi;
extern __thread int g_df;

GAME_REPLACE_EXACT(000665C0, cdecl, 0, u32, game_global_6f11bc_is_zero)
{
    g_ecx = guest_read32(0x6F11BC); g_eax = g_ecx == 0;
}
GAME_REPLACE_EXACT(00246D10, cdecl, 0, u32, game_input_driven_object_75eaf8_is_null)
{
    g_ecx = guest_read32(0x75EAF8); g_eax = g_ecx == 0;
}
GAME_REPLACE_EXACT(00162060, cdecl, 0, u32, game_clear_table_7a2d80_0x780_bytes)
{
    g_esp -= 4; guest_write32(g_esp, g_edi);
    g_ecx = 0x1E0; g_eax = 0; g_edi = 0x7A2D80;
    while (g_ecx) {
        guest_write32(g_edi, g_eax); g_edi += g_df ? 0xFFFFFFFCu : 4u; --g_ecx;
    }
    g_edi = guest_read32(g_esp); g_esp += 4;
}
GAME_REPLACE_EXACT(00378510, cdecl, 1, u32, game_pointer_arg_field_0xc_equals_76db78)
{
    g_ecx = guest_read32(g_esp + 4); g_edx = guest_read32(g_ecx + 0xC);
    g_eax = g_edx == 0x76DB78;
}
GAME_REPLACE_EXACT(000BDB80, cdecl, 0, u32, game_reset_ptr_732e60_if_6b7aa8_zero)
{
    g_eax = guest_read32(0x6B7AA8);
    if (!g_eax) guest_write32(0x732E60, 0);
}
GAME_REPLACE_EXACT(000D6A50, cdecl, 0, u32, game_reset_ptr_7356d8_if_6b7aa8_zero)
{
    g_eax = guest_read32(0x6B7AA8);
    if (!g_eax) guest_write32(0x7356D8, 0);
}
GAME_REPLACE_EXACT(00158D90, cdecl, 0, u32, game_clear_table_7a3580_and_count_74c37c)
{
    g_esp -= 4; guest_write32(g_esp, g_edi);
    g_ecx = 0x20; g_eax = 0; g_edi = 0x7A3580;
    while (g_ecx) {
        guest_write32(g_edi, g_eax); g_edi += g_df ? 0xFFFFFFFCu : 4u; --g_ecx;
    }
    guest_write32(0x74C37C, 0);
    g_edi = guest_read32(g_esp); g_esp += 4;
}
GAME_REPLACE_EXACT(000827D0, cdecl, 1, u32, game_memory_unit_slot_7b979c_set_and_flag_4d9658_when_minus_one)
{
    g_eax = guest_read32(g_esp + 4);
    if (g_eax == 0xFFFFFFFFu) guest_write32(0x4D9658, 1);
    guest_write32(0x7B979C, g_eax);
}
