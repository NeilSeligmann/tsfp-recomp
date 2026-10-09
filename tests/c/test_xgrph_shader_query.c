/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "xgrph_shader_query.h"
#include "xgrph_hle.h"
#include "guest_mem.h"
#include "kernel_call.h"
#include "host_runtime.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)
static void fatal(uint32_t address, const char *message)
{
    host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, address, 0u, message);
}
static void failed_read(uint32_t input)
{
    uint32_t out = 0xDEADBEEFu;
    CHECK(!xgrph_vertex_shader_length(input, &out));
    CHECK(out == 0xDEADBEEFu);
}
static void handler_refusal(kernel_call_frame *frame)
{
    volatile bool returned = false;
    kernel_call_frame before = {0};
    if (frame) { memcpy(&before, frame, sizeof(before)); }
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        (void)xgrph_hle_call(0x3E9546u, frame);
        returned = true;
    }
    CHECK(!returned);
    CHECK(host_run_result()->reason == HOST_STOP_XDK_UNIMPLEMENTED);
    CHECK(host_run_result()->guest_address == 0x3E9546u);
    if (frame) { CHECK(memcmp(&before, frame, sizeof(before)) == 0); }
    host_run_disarm();
}
int main(void)
{
    const xgrph_surface_entry entry = {0x3E9546u, "XGSUCode_GetVertexShaderLength", 1u};
    CHECK(xgrph_hle_init(&entry, 1u));
    CHECK(xgrph_shader_query_register() == 1u);
    CHECK(xgrph_hle_implemented_count() == 1u);
    xgrph_hle_set_fatal(fatal);
    guest_region_request request = {0};
    request.bytes = 0x4000u; request.protect = PAGE_READWRITE; request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    const uint32_t base = guest_region_alloc(&request, &status);
    CHECK(base != 0u);
    const uint32_t lengths[] = {0u, 1u, 4u, 255u, 256u, 0x7FFFu, 0x8000u, 0xFFFEu, 0xFFFFu};
    for (unsigned offset = 0u; offset < 4u; offset++) {
        for (size_t i = 0u; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
            const uint32_t input = base + 0x1000u + offset;
            const uint32_t header = (lengths[i] << 16) | ((uint32_t)i * 931u);
            CHECK(kernel_guest_write_u32(input, header));
            uint32_t out = 0xDEADBEEFu;
            CHECK(xgrph_vertex_shader_length(input, &out));
            CHECK(out == lengths[i]);
            uint32_t after = 0u;
            CHECK(kernel_guest_read_u32(input, &after) && after == header);
        }
    }
    CHECK(!xgrph_vertex_shader_length(base + 0x1000u, NULL));
    failed_read(0u);
    failed_read(UINT32_MAX);
    failed_read(UINT32_MAX - 1u);
    failed_read(UINT32_MAX - 2u);
    const uint8_t count4[2] = {4u, 0u};
    CHECK(kernel_guest_write_bytes(base + 0x1000u, count4, 2u));
    CHECK(kernel_guest_write_bytes(base + 0x1FFEu, count4, 2u));
    CHECK(mprotect((void *)(uintptr_t)base, 0x1000u, PROT_NONE) == 0);
    CHECK(mprotect((void *)(uintptr_t)(base + 0x2000u), 0x1000u, PROT_NONE) == 0);
    CHECK(mprotect((void *)(uintptr_t)(base + 0x1000u), 0x1000u, PROT_READ) == 0);
    uint32_t out = 0u;
    /* Only the high header word is readable: no speculative first-word read. */
    CHECK(xgrph_vertex_shader_length(base + 0xFFEu, &out) && out == 4u);
    /* Following instruction bytes are protected and must not be inspected. */
    CHECK(xgrph_vertex_shader_length(base + 0x1FFCu, &out) && out == 4u);
    failed_read(base + 0x1FFDu); /* Two-byte read crosses the protected page. */
    failed_read(base + 0x2000u);
    const uint32_t input = base + 0xFFEu;
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, base + 0x3000u, 8u, &input, 1u));
    uint8_t before[16], after[16];
    CHECK(kernel_guest_read_bytes(base + 0x3000u, before, sizeof(before)));
    CHECK(xgrph_hle_call(0x3E9546u, &frame) == 4u);
    CHECK(kernel_guest_read_bytes(base + 0x3000u, after, sizeof(after)));
    CHECK(memcmp(before, after, sizeof(before)) == 0);
    const uint32_t bad_input = base + 0x2000u;
    CHECK(kernel_frame_build(&frame, base + 0x3000u, 8u, &bad_input, 1u));
    handler_refusal(&frame);
    handler_refusal(NULL);
    frame.stack_ptr = base;
    frame.stack_limit = base + 8u;
    handler_refusal(&frame);
    CHECK(munmap((void *)(uintptr_t)(base + 0x1000u), 0x1000u) == 0);
    failed_read(base + 0x1000u);
    CHECK(guest_region_free(base));
    xgrph_hle_shutdown();
    printf("shader length: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
