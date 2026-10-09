/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Byte-order swap helpers (T77 range 0x002C0000-0x00470000). Three measured variants of the
 * same operation: two are register based and one swaps through the caller's argument slot.
 * Register residue is reproduced exactly, including the
 * upper half of eax the 16 bit originals leave untouched.
 */
#include "game_replace.h"

static uint32_t game_swap16(uint32_t value)
{
    return ((value & 0xFFu) << 8) | ((value >> 8) & 0xFFu);
}

static uint32_t game_swap32(uint32_t value)
{
    return (value << 24) | ((value & 0xFF00u) << 8) | ((value >> 8) & 0xFF00u) | (value >> 24);
}

/* 0x003BD2B0 (cdecl): swap of the low word of the argument. Only ax is written, ecx = swapped,
 * edx = the original high byte of the word. */
static void game_swap16_register(void)
{
    const uint32_t word = game_stack_arg(0u) & 0xFFFFu;
    const uint32_t swapped = game_swap16(word);
    g_ecx = swapped;
    g_edx = word >> 8;
    g_eax = (g_eax & 0xFFFF0000u) | swapped;
}

GAME_REPLACE_EXACT(003BD2B0, cdecl, 1, u32, game_swap16_register)
{
    game_swap16_register();
}

/* 0x003BD2D0 (cdecl): full 32 bit swap. ecx = eax = result, edx = argument >> 24. */
static void game_swap32_register(void)
{
    const uint32_t value = game_stack_arg(0u);
    g_ecx = game_swap32(value);
    g_edx = value >> 24;
    g_eax = g_ecx;
}

GAME_REPLACE_EXACT(003BD2D0, cdecl, 1, u32, game_swap32_register)
{
    game_swap32_register();
}

/* 0x003BD300 (cdecl): swaps the low word of the argument IN the caller's argument slot
 * (bytes 4 and 5 above the return address) and returns it in ax. Upper eax is preserved. */
static void game_swap16_in_slot(void)
{
    const uint32_t word = game_stack_arg(0u) & 0xFFFFu;
    const uint32_t swapped = game_swap16(word);
    guest_write8(g_esp + 4u, (uint8_t)(word >> 8));
    guest_write8(g_esp + 5u, (uint8_t)word);
    g_eax = (g_eax & 0xFFFF0000u) | swapped;
}

GAME_REPLACE_EXACT(003BD300, cdecl, 1, u32, game_swap16_in_slot)
{
    game_swap16_in_slot();
}
