/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1543: the two admitted call-bearing roots of the T1534 list-004 worklist (docs/
 * t-call-draft-list004.md, 18 other drafts failed the reach gates and are archived under
 * docs/evidence/t1543). Both adapters are register-exact: the original's saved-register words are
 * written below the entry ESP, nested calls go through game_guest_call with the original return
 * PC, the callee's own EAX/ECX/EDX stay visible, and ESI/EDI are placed in the guest registers
 * around each call and restored from the saved copies. Drafted by Claude (Sonnet) from the
 * disassembly, read against it instruction by instruction, proven by tools.replace prove
 * (--live-call-closure, O0 and O3, seeds 20261001 and 20261006). */
#include "game_replace.h"

extern __thread uint32_t g_esi, g_edi;

static void list004_call(uint32_t target, uint32_t return_pc, uint32_t caller_bytes,
                         const uint32_t *arguments, unsigned count)
{
    if (game_guest_call(target, GAME_CC_cdecl, return_pc, caller_bytes, 0u, 0u, arguments,
                        count) != GAME_GUEST_CALL_OK)
        __builtin_trap();
}

/* One probe block of 0026CC80 and 0026CD50: selects 0 when the object's record [+0x204] is
 * absent or has no flag 1 in its 0x29C-byte row of [0x4F9BAC], else 1, then asks the callee
 * with (object, word at table[selected]). ESI = the record owner, EDI = the object. */
static void list004_probe_block(uint32_t table, uint32_t callee, uint32_t return_pc)
{
    g_eax = guest_read32(g_esi + 0x204u);
    if (g_eax == 0xFFFFFFFFu) {
        g_eax = 1u;
    } else {
        g_ecx = guest_read32(0x004F9BACu);
        g_eax *= 0x29Cu;
        g_eax = (guest_read8(g_eax + g_ecx + 0xCu) & 1u) != 0u ? 1u : 0u;
    }
    g_edx = guest_read32(g_eax * 4u + table);
    const uint32_t arguments[2] = {g_edi, g_edx};
    list004_call(callee, return_pc, 8u, arguments, 2u);
}

/* 0026CC80: cdecl(object). Returns 1 when 00059D70(object, word) is nonzero for any of the
 * three tables 0x5202F8, 0x520308, 0x5202E8, else 0. */
GAME_REPLACE_EXACT(0026CC80, cdecl, 1, u32, game_object_any_probe_hits_a)
{
    const uint32_t entry = g_esp;
    const uint32_t saved_esi = g_esi;
    const uint32_t saved_edi = g_edi;
    guest_write32(entry - 4u, g_esi);
    guest_write32(entry - 8u, g_edi);
    g_edi = game_stack_arg(0u);
    g_esi = guest_read32(g_edi + 0x7Cu);
    list004_probe_block(0x005202F8u, 0x00059D70u, 0x0026CCBEu);
    if (g_eax == 0u) {
        list004_probe_block(0x00520308u, 0x00059D70u, 0x0026CCFAu);
        if (g_eax == 0u) {
            list004_probe_block(0x005202E8u, 0x00059D70u, 0x0026CD36u);
            if (g_eax == 0u) {
                g_edi = saved_edi;
                g_esi = saved_esi;
                return;
            }
        }
    }
    g_edi = saved_edi;
    g_eax = 1u;
    g_esi = saved_esi;
}

/* 0026CD50: same shape as 0026CC80 with 00059D20 and the tables 0x520338, 0x520328, 0x520318. */
GAME_REPLACE_EXACT(0026CD50, cdecl, 1, u32, game_object_any_probe_hits_b)
{
    const uint32_t entry = g_esp;
    const uint32_t saved_esi = g_esi;
    const uint32_t saved_edi = g_edi;
    guest_write32(entry - 4u, g_esi);
    guest_write32(entry - 8u, g_edi);
    g_edi = game_stack_arg(0u);
    g_esi = guest_read32(g_edi + 0x7Cu);
    list004_probe_block(0x00520338u, 0x00059D20u, 0x0026CD8Eu);
    if (g_eax == 0u) {
        list004_probe_block(0x00520328u, 0x00059D20u, 0x0026CDCAu);
        if (g_eax == 0u) {
            list004_probe_block(0x00520318u, 0x00059D20u, 0x0026CE06u);
            if (g_eax == 0u) {
                g_edi = saved_edi;
                g_esi = saved_esi;
                return;
            }
        }
    }
    g_edi = saved_edi;
    g_eax = 1u;
    g_esi = saved_esi;
}
