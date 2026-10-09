/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Small call-free leaves of retail XBE range 0x00180000-0x00470000 that read or write
 * guest globals (T77 follow-up). Only measured behaviour is named: no meaning is
 * assigned to the globals. Every body is register-exact (GAME_REPLACE_EXACT), so the
 * guest registers the original leaves behind are reproduced as well.
 * See docs/t77-leaf-batch-180000-470000.md for proofs and rejects.
 */
#include "game_replace.h"

static void game_store16(uint32_t address, uint16_t value)
{
    guest_write8(address, (uint8_t)value);
    guest_write8(address + 1u, (uint8_t)(value >> 8));
}

/* Store the single cdecl argument in a global, leaving eax = argument. */
static void game_store_arg_global(uint32_t address)
{
    const uint32_t value = game_stack_arg(0u);
    guest_write32(address, value);
    g_eax = value;
}

/* 0x0023EF30: arg-th bit of the mask global 0x78CD54. eax = bit & mask, ecx = arg. */
GAME_REPLACE_EXACT(0023EF30, cdecl, 1, u32, game_mask_test_bit)
{
    g_ecx = game_stack_arg(0u);
    g_eax = (1u << (g_ecx & 31u)) & guest_read32(0x78CD54u);
}

/* 0x0023EF70, 0x00344270, 0x002A62E0, 0x002A8370, 0x0024A170, 0x0025FA40, 0x00227740,
 * 0x00311900: `[global] = arg`, eax = arg. */
GAME_REPLACE_EXACT(0023EF70, cdecl, 1, u32, game_set_global_78cd54)
{
    game_store_arg_global(0x78CD54u);
}
GAME_REPLACE_EXACT(00344270, cdecl, 1, u32, game_set_global_765e58)
{
    game_store_arg_global(0x765E58u);
}
GAME_REPLACE_EXACT(002A62E0, cdecl, 1, u32, game_set_global_78b0ec)
{
    game_store_arg_global(0x78B0ECu);
}
GAME_REPLACE_EXACT(002A8370, cdecl, 1, u32, game_set_global_78b094)
{
    game_store_arg_global(0x78B094u);
}
GAME_REPLACE_EXACT(0024A170, cdecl, 1, u32, game_set_global_78b700)
{
    game_store_arg_global(0x78B700u);
}
GAME_REPLACE_EXACT(0025FA40, cdecl, 1, u32, game_set_global_75f5e0)
{
    game_store_arg_global(0x75F5E0u);
}
GAME_REPLACE_EXACT(00227740, cdecl, 1, u32, game_set_global_78ced0)
{
    game_store_arg_global(0x78CED0u);
}
GAME_REPLACE_EXACT(00311900, cdecl, 1, u32, game_set_global_78402c)
{
    game_store_arg_global(0x78402Cu);
}

/* 0x00227BE0: `[0x75C148] = a0; [0x75C150] = a1`, eax = a0, ecx = a1. */
GAME_REPLACE_EXACT(00227BE0, cdecl, 2, u32, game_set_global_pair_75c148)
{
    g_eax = game_stack_arg(0u);
    g_ecx = game_stack_arg(1u);
    guest_write32(0x75C148u, g_eax);
    guest_write32(0x75C150u, g_ecx);
}

/* 0x00334700: zero the first two dwords of the record argument. eax = 0, ecx = record. */
GAME_REPLACE_EXACT(00334700, cdecl, 1, u32, game_zero_two_words)
{
    g_ecx = game_stack_arg(0u);
    g_eax = 0u;
    guest_write32(g_ecx, 0u);
    guest_write32(g_ecx + 4u, 0u);
}

/* 0x00331CA0: store 0x8008 into the three 16 bit slots at the argument. ecx = 0xFFFF8008,
 * eax = argument. */
GAME_REPLACE_EXACT(00331CA0, cdecl, 1, u32, game_fill_three_halfwords_8008)
{
    g_eax = game_stack_arg(0u);
    g_ecx = 0xFFFF8008u;
    game_store16(g_eax, 0x8008u);
    game_store16(g_eax + 2u, 0x8008u);
    game_store16(g_eax + 4u, 0x8008u);
}

/* 0x00311A80 / 0x00311A90: arg == 2 / arg == 3. ecx = arg. */
GAME_REPLACE_EXACT(00311A80, cdecl, 1, u32, game_is_two)
{
    g_ecx = game_stack_arg(0u);
    g_eax = (g_ecx == 2u) ? 1u : 0u;
}
GAME_REPLACE_EXACT(00311A90, cdecl, 1, u32, game_is_three)
{
    g_ecx = game_stack_arg(0u);
    g_eax = (g_ecx == 3u) ? 1u : 0u;
}

/* 0x0031D2C0: record `[p+0x34] == 5`. ecx = p, edx = [p+0x34]. */
GAME_REPLACE_EXACT(0031D2C0, cdecl, 1, u32, game_record_34_is_five)
{
    g_ecx = game_stack_arg(0u);
    g_edx = guest_read32(g_ecx + 0x34u);
    g_eax = (g_edx == 5u) ? 1u : 0u;
}

/* 0x0022DF10: record `[p+0xA0]` has bit (a1 & 31) set. eax is 0/1, ecx = p. */
GAME_REPLACE_EXACT(0022DF10, cdecl, 2, u32, game_record_a0_has_bit)
{
    const uint32_t bit = 1u << (game_stack_arg(1u) & 31u);
    g_ecx = game_stack_arg(0u);
    g_eax = (bit & guest_read32(g_ecx + 0xA0u)) ? 1u : 0u;
}
