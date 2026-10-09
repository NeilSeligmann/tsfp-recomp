/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1576: the typed tail adapter for a replacement whose original ends (or branches) in an
 * out-of-body `jmp <exact function entry>`.
 *
 * A tail jmp is not a call. The original leaves its own return address and arguments on the
 * stack, the target runs on the live register file (EAX..EDI, including the callee-saved
 * EBX/EBP/ESI/EDI and ESP) and returns straight to the original's caller, popping whatever its
 * own convention pops. `game_guest_call` cannot express that: it invents a return VA, pushes
 * its own frame and restores ESP. This adapter changes none of the guest state itself:
 *
 *   - no return VA is invented, no frame is built, no register is saved or restored;
 *   - the target is reached through the same dispatch table (`recomp_lookup`) as any lifted
 *     call, so a registered replacement of the target is honoured;
 *   - on return ESP must be exactly entry ESP + 4 (+ 4 * stack_args when the target pops its
 *     arguments). Anything else is GAME_GUEST_CALL_BAD_STACK and the mismatched ESP is kept,
 *     so the enclosing exact replacement cannot hide the defect;
 *   - on success ESP is reset to the entry value because the enclosing GAME_REPLACE_EXACT
 *     adapter performs the final `g_esp += 4 + 4 * pops` itself.
 *
 * The enclosing registration must be GAME_REPLACE_EXACT* (no register excluded from the
 * comparison) and its own convention pop must equal the target's. The proof pipeline
 * (tools/replace/tail_adapter.py) checks both against the ORIGINAL bytes, requires the literal
 * target VA to equal the original's exact jmp target, and compares the full register file.
 */
#ifndef TSFP_GAME_GUEST_TAIL_H
#define TSFP_GAME_GUEST_TAIL_H

#include "game_replace.h"

static inline game_guest_call_status game_guest_tail(
    uint32_t target_va, game_convention convention, unsigned stack_args)
{
    if (stack_args > GAME_GUEST_CALL_MAX_STACK_ARGS || (int)convention < (int)GAME_CC_cdecl ||
        convention > GAME_CC_fastcall) {
        return GAME_GUEST_CALL_BAD_FRAME;
    }
    if (recomp_lookup == NULL) {
        return GAME_GUEST_CALL_NOT_FOUND;
    }
    game_guest_function function = recomp_lookup(target_va);
    if (function == NULL) {
        return GAME_GUEST_CALL_NOT_FOUND;
    }
    const uint32_t entry_esp = g_esp;
    function();
    uint32_t expected_esp = entry_esp + 4u;
    if (convention != GAME_CC_cdecl) {
        expected_esp += 4u * stack_args;
    }
    if (g_esp != expected_esp) {
        return GAME_GUEST_CALL_BAD_STACK;
    }
    g_esp = entry_esp;
    return GAME_GUEST_CALL_OK;
}

#endif /* TSFP_GAME_GUEST_TAIL_H */
