/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "recomp_abi.h"
#include "thunk_trace.h"
#include "xdk_thunk.h"
#include "d3d8_hle.h"
#include "d3d8_lock.h"
#include "d3d8_resource.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(value) do { checks++; if (!(value)) { \
    fprintf(stderr, "texture lock route failed at %d: %s\n", __LINE__, #value); abort(); } } while (0)
static uint32_t allocate(uint32_t fixed, uint32_t bytes, bool contiguous)
{
    guest_region_request request = {0};
    request.bytes = bytes;
    request.contiguous = contiguous;
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
    const uint32_t metadata = allocate(0x3E1000u, 4096u, false);
    const uint32_t device_slot = allocate(0x3E3000u, 4096u, false);
    const uint32_t stack = allocate(0u, 4096u, false);
    const uint32_t header = allocate(0u, 4096u, false);
    const uint32_t backing = allocate(0u, 16384u, true);
    const uint32_t locked = stack + 512u;
    CHECK(kernel_guest_write_u8(0x3E182Eu, 0xA1u));
    const uint32_t texture[5] = {0x40001u, 0u, 0u, 0x00C10629u, 0u};
    CHECK(kernel_guest_write_bytes(header, texture, sizeof(texture)));
    (void)d3d8_register_resource(header, backing);
    uint32_t registered[5];
    CHECK(kernel_guest_read_bytes(header, registered, sizeof(registered)));
    const xdk_dispatch_entry row = {0x3D4E60u, "D3DTexture_LockRect", XDK_MODULE_D3D8};
    const d3d8_surface_entry utility = {0x3D4E60u, "D3DTexture_LockRect", 1u};
    CHECK(xdk_thunk_init(&row, 1u));
    CHECK(xdk_thunk_declare_abi(row.address, XDK_CC_STDCALL, 5u, 0u));
    CHECK(d3d8_hle_init(&utility, 1u));
    CHECK(d3d8_lock_register() == 1u);
    for (unsigned route = 0u; route < 2u; route++) {
        uint32_t frame[6] = {0x2A39Fu, header, 0u, locked, 0u, 0u};
        uint8_t before[34], after[34];
        memset(before, 0xA5, sizeof(before));
        CHECK(kernel_guest_write_bytes(locked, before, sizeof(before)));
        CHECK(kernel_guest_write_bytes(stack, frame, sizeof(frame)));
        g_eax = 1u; g_ecx = 2u; g_edx = 3u; g_ebx = 4u;
        g_esi = 5u; g_edi = 6u; g_ebp = 7u; g_esp = stack;
        thunk_trace_reset();
        recomp_func_t function = route == 0u ? recomp_lookup(row.address) : recomp_lookup_manual(row.address);
        CHECK(function != NULL);
        if (sigsetjmp(*host_run_jmp(), 1) == 0) {
            host_run_arm();
            function();
            CHECK(g_eax == locked && g_esp == stack + 24u);
            CHECK(g_ecx == 2u && g_edx == 3u && g_ebx == 4u);
            CHECK(g_esi == 5u && g_edi == 6u && g_ebp == 7u);
        } else { CHECK(false); }
        host_run_disarm();
        CHECK(kernel_guest_read_bytes(locked, after, sizeof(after)));
        const uint32_t expected[2] = {16384u, backing};
        CHECK(memcmp(after, expected, sizeof(expected)) == 0);
        CHECK(memcmp(after + 8u, before + 8u, 26u) == 0);
        uint32_t current_header[5];
        CHECK(kernel_guest_read_bytes(header, current_header, sizeof(current_header)));
        CHECK(memcmp(current_header, registered, sizeof(current_header)) == 0);
        uint32_t saved_frame[6];
        CHECK(kernel_guest_read_bytes(stack, saved_frame, sizeof(saved_frame)));
        CHECK(memcmp(frame, saved_frame, sizeof(frame)) == 0);
        size_t count;
        const thunk_trace_entry *trace = thunk_trace_entries(&count);
        CHECK(count == 1u && trace[0].address == row.address);
        CHECK(trace[0].return_address == frame[0] && trace[0].implemented);
        CHECK(trace[0].result_known && trace[0].result == locked);
    }
    xdk_thunk_shutdown(); d3d8_hle_shutdown();
    d3d8_resource_reset();
    CHECK(guest_region_free(backing));
    CHECK(guest_region_free(device_slot));
    CHECK(guest_region_free(header));
    CHECK(guest_region_free(stack));
    CHECK(guest_region_free(metadata));
    printf("compiled texture lock routes: %u checks passed\n", checks);
    return 0;
}
