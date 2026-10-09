/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Independent hand-written native implementation; no generated lift/CPU engine. */
#include "game_replace.h"
#include <stdio.h>
#include <stdlib.h>
extern __thread uint32_t g_ebx; /* Existing runtime TLS, used by the exact fixture. */
GAME_REPLACE_EXACT(00010000, cdecl, 0, u32, custom_byte_table)
{
    g_eax = guest_read8(0x20000u);
    g_ecx = guest_read32(0x20004u);
    g_edx = guest_read32(g_ecx);
    g_eax = guest_read32(0x20010u + 4u * g_eax);
    guest_write32(0xDC0004u, g_edx);
}
/* A real guest CALL write and callee RET read, rather than a native C ABI call. */
static void custom_helper(void)
{
    g_ecx = guest_read32(0x20004u);
    g_edx = guest_read32(g_ecx);
    g_eax = guest_read32(0x20010u);
}
GAME_REPLACE_EXACT(00010080, cdecl, 0, u32, custom_direct_call)
{
    g_esp -= 4u;
    guest_write32(g_esp, 0x10085u);
    custom_helper();
    uint32_t consumed = guest_read32(g_esp);
    g_esp += 4u;
    /* The frozen positive domain supplies this exact continuation. Unexpected
       targets are unsupported, never an invented guest fault or restarted root. */
    if (consumed != 0x10085u) {
        fputs("FATAL t1550-unsupported-continuation\n", stdout);
        fflush(stdout);
        _Exit(86); /* Infrastructure refusal, never an invented guest fault. */
    }
    g_esi = guest_read32(0x20008u);
    g_ebx = guest_read32(0x2000Cu);
    guest_write32(0xDC0008u, g_ebx);
    g_eax = guest_read32(0x20018u);
}
GAME_REPLACE_EXACT(000100F0, stdcall, 2, u32, custom_ret8)
{
    g_eax = guest_read32(g_esp + 4u);
    g_ecx = guest_read32(g_esp + 8u);
    guest_write32(0xDC000Cu, g_eax);
}
/* Genuine invalid guest RET-slot read, not a synthetic host signal. */
GAME_REPLACE_EXACT(00010260, cdecl, 0, u32, custom_unreadable_ret)
{
    g_esp = 0u;
    (void)guest_read32(g_esp);
}
