/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include "guest_mem.h"
#include "kernel_call.h"
#include "recomp_abi.h"
#include "xdk_thunk.h"
#include "xgrph_hle.h"
#include "xgrph_object_lifetime.h"
TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
TSFP_RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp, g_fs_base;
int main(void)
{
    guest_region_request request = {0};
    request.bytes = 4096u;
    request.fixed_base = 0x00D18000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status;
    if (guest_region_alloc(&request, &status) != request.fixed_base) return 3;
    const xgrph_surface_entry row = {0x003EF236u, "independent reset", 1u};
    const xdk_dispatch_entry dispatch = {0x003EF236u, "independent reset", XDK_MODULE_XGRPH};
    if (!xgrph_hle_init(&row, 1u) || xgrph_object_lifetime_register() != 1u ||
        !xdk_thunk_init(&dispatch, 1u) ||
        !xdk_thunk_declare_abi(0x003EF236u, XDK_CC_THISCALL, 0u, 1u)) return 3;
    g_eax = 0xA5A51234u;
    g_ecx = 0x00D18020u;
    g_edx = 0xB6B62345u;
    g_esp = 0x00D18800u;
    if (!kernel_guest_write_u32(g_esp, 0x003EF236u)) return 3;
    xdk_thunk_dispatch_at(0x003EF236u);
    uint32_t word;
    if (!kernel_guest_read_u32(g_ecx, &word)) return 3;
    printf("%08x %08x %08x %08x %08x\n", g_eax, g_ecx, g_edx, g_esp, word);
    xdk_thunk_shutdown();
    xgrph_hle_shutdown();
    guest_mem_reset();
    return 0;
}
