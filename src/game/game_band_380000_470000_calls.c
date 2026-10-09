/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1480 CRT classifiers: signed locale gate, original live frames and read widths. */
#include "game_replace.h"

static uint32_t read_word(uint32_t address)
{
    uint16_t value;
    memcpy(&value, game_host_ptr(address), sizeof value);
    return value;
}

static void classify(uint32_t mask, uint32_t return_pc, unsigned word_fallback)
{
    const uint32_t value = game_stack_arg(0u);
    if ((int32_t)guest_read32(0x0054D7F8u) > 1) {
        const uint32_t arguments[2] = {value, mask};
        if (game_guest_call(0x003C9C4Eu, GAME_CC_cdecl, return_pc,
                            0u, 0u, 0u, arguments, 2u) != GAME_GUEST_CALL_OK) {
            __builtin_trap();
        }
        /* Original pop ecx; pop ecx observes the second live argument word. */
        g_ecx = guest_read32(g_esp - 4u);
    } else {
        g_eax = value;
        g_ecx = guest_read32(0x0054D7F0u);
        const uint32_t address = g_ecx + g_eax * 2u;
        g_eax = (word_fallback ? read_word(address) : guest_read8(address)) & mask;
    }
}

GAME_REPLACE_EXACT(003C887E, cdecl, 1, u32, game_isalpha)
{
    classify(0x103u, 0x003C8895u, 1u);
}

GAME_REPLACE_EXACT(003C88D5, cdecl, 1, u32, game_isspace)
{
    classify(8u, 0x003C88E9u, 0u);
}

GAME_REPLACE_EXACT(003C88FE, cdecl, 1, u32, game_isalnum)
{
    classify(0x107u, 0x003C8915u, 1u);
}

GAME_REPLACE_EXACT(003C892C, cdecl, 1, u32, game_isprint)
{
    classify(0x157u, 0x003C8943u, 1u);
}
