/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include "xdk_thunk.h"
#include "xgrph_hle.h"
#include "xgrph_texture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(value) do { checks++; if (!(value)) { \
    fprintf(stderr, "texture route failed at %d: %s\n", __LINE__, #value); abort(); } } while (0)
static uint32_t allocate(uint32_t fixed)
{
    guest_region_request request = {0};
    request.bytes = 4096u;
    request.fixed_base = fixed;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status;
    uint32_t result = guest_region_alloc(&request, &status);
    CHECK(result != 0u);
    return result;
}
int main(void)
{
    const uint32_t metadata = allocate(0x402000u);
    const uint32_t stack = allocate(0u);
    const uint32_t header = allocate(0u);
    CHECK(kernel_guest_write_u8(0x402D76u, 0xA1u));
    const xdk_dispatch_entry row = {0x3E6684u, "XGSetTextureHeader", XDK_MODULE_XGRPH};
    const xgrph_surface_entry utility = {0x3E6684u, "XGSetTextureHeader", 5u};
    CHECK(xdk_thunk_init(&row, 1u));
    CHECK(xdk_thunk_declare_abi(row.address, XDK_CC_STDCALL, 9u, 0u));
    CHECK(xgrph_hle_init(&utility, 1u));
    CHECK(xgrph_texture_register() == 1u);
    for (unsigned route = 0u; route < 2u; route++) {
        uint32_t frame[10] = {0x18638u, 4096u, 1u, 1u, 0u, 6u, 0u, header, 0u, 16384u};
        uint8_t before[34], after[34];
        memset(before, 0xA5, sizeof(before));
        CHECK(kernel_guest_write_bytes(header, before, sizeof(before)));
        CHECK(kernel_guest_write_bytes(stack, frame, sizeof(frame)));
        g_eax = 1u; g_ecx = 2u; g_edx = 3u; g_ebx = 4u;
        g_esi = 5u; g_edi = 6u; g_ebp = 7u; g_esp = stack;
        thunk_trace_reset();
        recomp_func_t function = route == 0u ? recomp_lookup(row.address) : recomp_lookup_manual(row.address);
        CHECK(function != NULL);
        if (sigsetjmp(*host_run_jmp(), 1) == 0) {
            host_run_arm();
            function();
            CHECK(g_eax == 0x4000u && g_esp == stack + 40u);
            CHECK(g_ecx == 2u && g_edx == 3u && g_ebx == 4u);
            CHECK(g_esi == 5u && g_edi == 6u && g_ebp == 7u);
        } else { CHECK(false); }
        host_run_disarm();
        CHECK(kernel_guest_read_bytes(header, after, sizeof(after)));
        const uint32_t expected[5] = {0x40001u, 0u, 0u, 0x00C10629u, 0u};
        CHECK(memcmp(after, expected, sizeof(expected)) == 0);
        CHECK(memcmp(after + 20u, before + 20u, 14u) == 0);
        uint32_t saved_frame[10];
        CHECK(kernel_guest_read_bytes(stack, saved_frame, sizeof(saved_frame)));
        CHECK(memcmp(frame, saved_frame, sizeof(frame)) == 0);
        size_t count;
        const thunk_trace_entry *trace = thunk_trace_entries(&count);
        CHECK(count == 1u && trace[0].address == row.address);
        CHECK(trace[0].return_address == frame[0] && trace[0].implemented);
        CHECK(trace[0].result_known && trace[0].result == 0x4000u);
    }
    xdk_thunk_shutdown(); xgrph_hle_shutdown();
    CHECK(guest_region_free(header));
    CHECK(guest_region_free(stack));
    CHECK(guest_region_free(metadata));
    printf("compiled texture routes: %u checks passed\n", checks);
    return 0;
}
