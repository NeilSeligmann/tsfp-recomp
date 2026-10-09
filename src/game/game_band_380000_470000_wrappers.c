/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1480 fourth batch: small call-bearing wrappers in 0x380000-0x470000. Each keeps the
 * original spill words, return PCs and nested-call register contract (game_guest_call). */
#include "game_replace.h"

#define GAME_ORIGINAL_CALL_FAILED() __builtin_trap()

/* 003E6486: stdcall(code). Leaf 003E6460 answers AL for 12, 14 and 15; the wrapper
 * widens that to 2 or 0. The pushed argument and return word are live callee frame. */
GAME_REPLACE_EXACT(003E6486, stdcall, 1, u32, game_code_is_12_14_15_flag)
{
    const uint32_t argument[1] = {game_stack_arg(0u)};
    if (game_guest_call(0x003E6460u, GAME_CC_stdcall, 0x003E648Fu, 0u, 0u, 0u,
                        argument, 1u) != GAME_GUEST_CALL_OK)
        GAME_ORIGINAL_CALL_FAILED();
    g_eax = (g_eax & 0xFFu) != 0u ? 2u : 0u;
}

/* 003C1A90: copy at most 0x1F bytes of `source` into the buffer at object+0x55FC.
 * The callee's EAX, ECX and EDX are the wrapper's results. */
GAME_REPLACE_EXACT(003C1A90, cdecl, 2, u32, game_online_friends_set_local_name_field_0x55fc_bounded_copy_31)
{
    const uint32_t source = game_stack_arg(1u);
    const uint32_t buffer = game_stack_arg(0u) + 0x55FCu;
    const uint32_t arguments[3] = {buffer, source, 0x1Fu};
    g_eax = source;
    g_ecx = buffer;
    if (game_guest_call(0x003C8960u, GAME_CC_cdecl, 0x003C1AA7u, 0u, 0u, 0u,
                        arguments, 3u) != GAME_GUEST_CALL_OK)
        GAME_ORIGINAL_CALL_FAILED();
}
