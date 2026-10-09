/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "guest_mem.h"
#include "kernel_call.h"
#include "recomp_abi.h"
#include "xdk_thunk.h"
#include "thunk_trace.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_state.h"
#include "d3d8_pushbuffer.h"
TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
TSFP_RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp, g_fs_base;
static jmp_buf escape;
static uint32_t fatal_at, mode, rolls, observation[8][6];
static bool protect_page(uint32_t address, uint32_t protection)
{
    kernel_guest_probe_change_begin();
    int mode = protection == PAGE_NOACCESS ? PROT_NONE :
               protection == PAGE_READONLY ? PROT_READ : PROT_READ | PROT_WRITE;
    const bool ok = mprotect((void *)(uintptr_t)address, 4096u, mode) == 0;
    kernel_guest_probe_cache_flush();
    return ok;
}
static void refuse(uint32_t address, const char *message)
{
    (void)message; fatal_at = address; longjmp(escape, 1);
}
static void consumer(void *context, uint32_t begin, uint32_t end)
{
    (void)context; (void)begin;
    if (rolls >= 8u) exit(3);
    uint32_t *o = observation[rolls++];
    o[0] = end; o[1] = d3d8_guest_load32(0x003E3EECu);
    o[2] = d3d8_guest_load32(0x003E3EF0u); o[3] = d3d8_guest_load32(0x003E3EF4u);
    o[4] = d3d8_guest_load32(g_esp + 4u); o[5] = d3d8_guest_load32(0x003E3AB8u);
    if (mode == 1u || mode == 2u) {
        d3d8_guest_store32(g_esp + 4u, 0xD1230000u + rolls);
        d3d8_guest_store32(0x003E3EECu, 0xF1230000u + rolls);
        d3d8_guest_store32(0x003E3EF0u, 0xB1230000u + rolls);
        d3d8_guest_store32(0x003E3EF4u, rolls & 1u);
    }
    if (mode == 2u && !protect_page((g_esp + 4u) & ~4095u, PAGE_NOACCESS)) exit(3);
}
int main(void)
{
    uint32_t config[6], count, ranges[16][2], entry;
    if (fread(config, sizeof(config), 1, stdin) != 1 || fread(&count, 4, 1, stdin) != 1 || count > 16u) return 2;
    entry = config[0]; mode = config[2];
    if (fread(ranges, 8, count, stdin) != count) return 2;
    for (uint32_t n = 0; n < count; n++) {
        guest_region_request request = {0}; nt_status status;
        request.bytes = ranges[n][1]; request.fixed_base = ranges[n][0];
        request.protect = PAGE_READWRITE; request.state = MEM_COMMIT;
        if (guest_region_alloc(&request, &status) != request.fixed_base) return 3;
        void *memory = kernel_guest_at(ranges[n][0], ranges[n][1]);
        if (!memory || fread(memory, 1, ranges[n][1], stdin) != ranges[n][1]) return 3;
    }
    const d3d8_surface_entry surface[] = {
        {0x003D7380u, "front", 1u}, {0x003D73D0u, "back", 1u}, {0x003D7430u, "lighting", 1u}
    };
    const xdk_dispatch_entry dispatch[] = {
        {0x003D7380u, "front", XDK_MODULE_D3D8},
        {0x003D73D0u, "back", XDK_MODULE_D3D8},
        {0x003D7430u, "lighting", XDK_MODULE_D3D8}
    };
    d3d8_guest_reset(); d3d8_pushbuffer_reset();
    if (!d3d8_hle_init(surface, 3u) || d3d8_state_register() != 3u || !xdk_thunk_init(dispatch, 3u)) return 3;
    for (unsigned n = 0; n < 3u; n++)
        if (!xdk_thunk_declare_abi(dispatch[n].address, XDK_CC_STDCALL, 1u, 0u)) return 3;
    d3d8_hle_set_fatal(refuse); d3d8_pushbuffer_set_consumer(consumer, NULL);
    g_eax = 0xA1020304u; g_ecx = 0xC1020304u; g_edx = 0xD1020304u;
    g_ebx = 0xB1020304u; g_ebp = 0xB5020304u; g_esi = 0x51020304u; g_edi = 0xD5020304u; g_esp = config[1];
    if (config[3] && !protect_page(config[3], config[4])) return 3;
    const int fault = setjmp(escape);
    if (!fault) {
        recomp_func_t fn = recomp_lookup_manual(entry);
        if (!fn) return 3;
        fn();
    }
    if (config[3] && !protect_page(config[3], PAGE_READWRITE)) return 3;
    if (mode == 2u && !protect_page((config[1] + 4u) & ~4095u, PAGE_READWRITE)) return 3;
    size_t traces = 0; const thunk_trace_entry *trace = thunk_trace_entries(&traces);
    if (traces != 1u || xdk_thunk_module_call_count(XDK_MODULE_D3D8) != 1u) return 3;
    uint32_t header[] = {(uint32_t)fault, fatal_at, g_eax, g_ecx, g_edx, g_ebx, g_ebp, g_esi, g_edi, g_esp,
                         rolls, trace[0].result, (uint32_t)d3d8_hle_entry(entry)->call_count};
    if (fwrite(header, sizeof(header), 1, stdout) != 1 || fwrite(observation, sizeof(observation), 1, stdout) != 1) return 3;
    for (uint32_t n = 0; n < count; n++) {
        const void *memory = kernel_guest_at(ranges[n][0], ranges[n][1]);
        if (!memory || fwrite(memory, 1, ranges[n][1], stdout) != ranges[n][1]) return 3;
    }
    xdk_thunk_shutdown(); d3d8_hle_shutdown(); guest_mem_reset(); return 0;
}
