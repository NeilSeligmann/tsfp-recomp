/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include "guest_mem.h"
#include "kernel_call.h"
#include "recomp_abi.h"
#include "xdk_thunk.h"
#include "thunk_trace.h"
#include "xgrph_hle.h"
#include "xgrph_object_lifetime.h"
TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
TSFP_RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp, g_fs_base;
static jmp_buf escape;
static void refuse(uint32_t address, const char *message)
{
    (void)message;
    if (address != 0x003EF236u) exit(3);
    longjmp(escape, 1);
}
static uint32_t neighbor(void *context)
{
    (void)context;
    return 0x87654321u;
}
int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    const uint32_t object = (uint32_t)strtoul(argv[1], NULL, 0);
    const uint32_t protection = (uint32_t)strtoul(argv[2], NULL, 0);
    const uint32_t eax = (uint32_t)strtoul(argv[3], NULL, 0);
    const int mode = atoi(argv[4]);
    const uint32_t base = 0x00D18000u;
    guest_region_request request = {0};
    request.bytes = 4096u;
    request.fixed_base = base;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status;
    if (guest_region_alloc(&request, &status) != base) return 3;
    unsigned char page[4096];
    if (fread(page, 1, sizeof(page), stdin) != sizeof(page) ||
        !kernel_guest_write_bytes(base, page, sizeof(page)) ||
        !guest_region_set_protect(base, 4096u, protection)) return 3;
    const xgrph_surface_entry rows[] = {
        {0x003EF236u, "measured reset", 1u},
        {0x003EF3B7u, "neighbor result", 1u}
    };
    const xdk_dispatch_entry dispatch[] = {
        {0x003EF236u, "measured reset", XDK_MODULE_XGRPH},
        {0x003EF3B7u, "neighbor result", XDK_MODULE_XGRPH}
    };
    if (!xgrph_hle_init(rows, 2u) || xgrph_object_lifetime_register() != 2u ||
        !xdk_thunk_init(dispatch, 2u) ||
        !xdk_thunk_declare_abi(0x003EF236u, XDK_CC_THISCALL, 0u, 1u) ||
        !xdk_thunk_declare_abi(0x003EF3B7u, XDK_CC_STDCALL, 2u, 0u)) return 3;
    if (mode && !xgrph_hle_register(mode == 2 ? 0x003EF236u : 0x003EF3B7u, neighbor))
        return 3;
    xgrph_hle_set_fatal(refuse);
    g_eax = eax; g_ecx = object; g_edx = 0xA5A51234u + 142u;
    g_ebx = 0xA5A51234u + 213u; g_ebp = 0xA5A51234u + 284u;
    g_esi = 0xA5A51234u + 355u; g_edi = 0xA5A51234u + 426u;
    g_esp = base + 2048u;
    int fault = setjmp(escape);
    if (!fault) xdk_thunk_dispatch_at(mode == 1 ? 0x003EF3B7u : 0x003EF236u);
    if (!fault) {
        size_t count = 0u;
        const thunk_trace_entry *trace = thunk_trace_entries(&count);
        if (count != 1u || trace[0].result != g_eax) return 3;
    }
    if (!guest_region_set_protect(base, 4096u, PAGE_READWRITE) ||
        !kernel_guest_read_bytes(base, page, sizeof(page))) return 3;
    uint32_t header[] = {(uint32_t)fault, g_eax, g_ecx, g_edx, g_ebx,
                         g_ebp, g_esi, g_edi, g_esp};
    if (fwrite(header, sizeof(header), 1, stdout) != 1 ||
        fwrite(page, 1, sizeof(page), stdout) != sizeof(page)) return 3;
    xdk_thunk_shutdown(); xgrph_hle_shutdown(); guest_mem_reset();
    return 0;
}
