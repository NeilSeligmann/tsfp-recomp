/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1477 continuation, batch 4: two scalar call-bearing roots (0x12D1E0 was refused by selection, see the docs) in the 0x100000-0x180000 band
 * that the 2026-10-08 re-screen newly reports as live-call closed. Each keeps the original
 * nested-call frames (saved-register words, argument slot, return PC) and leaves the callee's own
 * EAX/ECX/EDX behind, so every adapter is register-exact. */
#include "game_replace.h"

extern __thread uint32_t g_ebx, g_ebp, g_esi, g_edi;

static void call_guest(uint32_t target, uint32_t return_pc, uint32_t caller_bytes,
                       const uint32_t *arguments, unsigned count)
{
    if (game_guest_call(target, GAME_CC_cdecl, return_pc, caller_bytes, 0u, 0u, arguments,
                        count) != GAME_GUEST_CALL_OK)
        __builtin_trap();
}

/* 0010E220: cdecl(object, set_bits, clear_bits). A null object returns 0. Otherwise the flag word
 * at object+4 becomes (flags & ~clear_bits) | set_bits; when set_bits contains 0x10000 the
 * object is passed to cdecl 0010E000 (return PC 0x10E24A) after the store. */
GAME_REPLACE_EXACT(0010E220, cdecl, 3, void, game_record_modify_flags_0x4_notify)
{
    const uint32_t entry = g_esp;
    const uint32_t object = game_stack_arg(0u);
    g_eax = object;
    if (object == 0u)
        return;
    g_edx = ~game_stack_arg(2u);
    g_ecx = guest_read32(object + 4u);
    g_edx &= g_ecx;
    g_ecx = game_stack_arg(1u);
    g_edx |= g_ecx;
    guest_write32(object + 4u, g_edx);
    if ((g_ecx & 0x10000u) == 0u)
        return;
    const uint32_t arguments[1] = {object};
    call_guest(0x0010E000u, 0x0010E24Au, 0u, arguments, 1u);
    g_ecx = guest_read32(entry - 4u);
}

/* 0015C4E0: cdecl(void). Walks the (count [0x7A2958] + signed [0x790950]) 0xEA0-byte records of
 * the table at [0x7356D8]. A record is skipped when bit 2 of its byte +0x2C is set, or when its
 * 64-bit mask pair (offsets 0x10/0x14 when the mode byte [0x7DE455] is 10, else 0x18/0x1C) is
 * zero and its signed +8 is not below [0x790950]. Every other record's first dword is passed to
 * cdecl 0015C3D0 (return PC 0x15C535); the count and table base are reloaded after each call. */
GAME_REPLACE_EXACT(0015C4E0, cdecl, 0, void, game_net_slot_release_each_active_slot_without_flag_0x2)
{
    const uint32_t entry = g_esp;
    guest_write32(entry - 4u, g_ebp);
    guest_write32(entry - 8u, g_edi);
    const uint32_t saved_ebp = g_ebp;
    const uint32_t saved_edi = g_edi;
    g_eax = guest_read32(0x007A2958u);
    g_edi = guest_read32(0x00790950u);
    g_eax += g_edi;
    g_ebp = 0u;
    if ((int32_t)g_eax > 0) {
        g_eax = guest_read32(0x007356D8u);
        guest_write32(entry - 12u, g_esi);
        const uint32_t saved_esi = g_esi;
        g_esi = 0u;
        do {
            if ((guest_read8(g_esi + g_eax + 0x2Cu) & 2u) == 0u) {
                if (guest_read8(0x007DE455u) == 0x0Au) {
                    g_ecx = guest_read32(g_esi + g_eax + 0x10u);
                    g_edx = guest_read32(g_esi + g_eax + 0x14u);
                } else {
                    g_ecx = guest_read32(g_esi + g_eax + 0x18u);
                    g_edx = guest_read32(g_esi + g_eax + 0x1Cu);
                }
                g_ecx |= g_edx;
                if (g_ecx != 0u || (int32_t)guest_read32(g_esi + g_eax + 8u) < (int32_t)g_edi) {
                    g_ecx = guest_read32(g_esi + g_eax);
                    const uint32_t arguments[1] = {g_ecx};
                    call_guest(0x0015C3D0u, 0x0015C535u, 12u, arguments, 1u);
                    g_edi = guest_read32(0x00790950u);
                    g_eax = guest_read32(0x007356D8u);
                }
            }
            g_edx = guest_read32(0x007A2958u);
            g_ebp++;
            g_edx += g_edi;
            g_esi += 0xEA0u;
        } while ((int32_t)g_ebp < (int32_t)g_edx);
        g_esi = saved_esi;
    }
    g_edi = saved_edi;
    g_ebp = saved_ebp;
}
