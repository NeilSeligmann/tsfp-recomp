/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Retried T77 candidates from retail XBE range 0x00180000-0x00470000 whose first proof
 * could not reach their interesting paths with the default 128 cases. They are proven with a
 * large case count (--cases-per-function 40000), see
 * docs/t77-round4-high.md for the recipe. Every body is register-exact
 * (GAME_REPLACE_EXACT). Only measured behaviour is named, no game meaning is assigned to
 * fields or globals.
 */
#include "game_replace.h"

#define RECORD_C4_BASE 0x00524F5Au
#define RECORD_C4_STRIDE 0xC4u
#define RECT_TABLE_BASE 0x00524F68u
#define RECT_TABLE_ROW 7u
#define RECT_TABLE_STRIDE 0x1Cu
#define GLOBAL_525640 0x00525640u
#define TABLE_52CE00 0x0052CE00u
#define COLOR_TABLE_783A80 0x00783A80u

static uint32_t game_word_at(uint32_t address)
{
    return (uint32_t)guest_read8(address) | ((uint32_t)guest_read8(address + 1u) << 8);
}

/* 0x002F7DA0: true when the 16 bit word at field 0x524F5A of the 0xC4 byte record index a0
 * is 0x22. eax = ecx = result. */
GAME_REPLACE_EXACT(002F7DA0, cdecl, 1, u32, game_record_c4_type_is_0x22)
{
    const uint32_t index = game_stack_arg(0u);
    g_ecx = game_word_at(RECORD_C4_BASE + index * RECORD_C4_STRIDE) == 0x22u ? 1u : 0u;
    g_eax = g_ecx;
}

/* 0x002F7DC0: as 0x002F7DA0 for the value 0x21. */
GAME_REPLACE_EXACT(002F7DC0, cdecl, 1, u32, game_record_c4_type_is_0x21)
{
    const uint32_t index = game_stack_arg(0u);
    g_ecx = game_word_at(RECORD_C4_BASE + index * RECORD_C4_STRIDE) == 0x21u ? 1u : 0u;
    g_eax = g_ecx;
}

/* 0x00308AA0: true when the dword at 0x525640 equals entry a0 of the dword table at
 * 0x52CE00. ecx = a0, edx = the global. */
GAME_REPLACE_EXACT(00308AA0, cdecl, 1, u32, game_global_525640_equals_table_entry)
{
    g_ecx = game_stack_arg(0u);
    g_edx = guest_read32(GLOBAL_525640);
    g_eax = (g_edx == guest_read32(TABLE_52CE00 + g_ecx * 4u)) ? 1u : 0u;
}

/* 0x003194C0: entry a0 of the dword table at 0x783A80 masked with 0xFF7F7F00, doubled,
 * plus 0xFF. ecx = the masked value. */
GAME_REPLACE_EXACT(003194C0, cdecl, 1, u32, game_color_entry_masked_doubled_plus_ff)
{
    g_ecx = guest_read32(COLOR_TABLE_783A80 + game_stack_arg(0u) * 4u) & 0xFF7F7F00u;
    g_eax = g_ecx + g_ecx + 0xFFu;
}

/* 0x0033DD80: dword at 0x524F68 + (a1 * 7 + a0) * 0x1C. ecx = a0. */
GAME_REPLACE_EXACT(0033DD80, cdecl, 2, u32, game_table_entry_row7_stride_1c)
{
    const uint32_t column = game_stack_arg(0u);
    const uint32_t row = game_stack_arg(1u);
    g_ecx = column;
    g_eax = guest_read32(RECT_TABLE_BASE + (row * RECT_TABLE_ROW + column) * RECT_TABLE_STRIDE);
}
