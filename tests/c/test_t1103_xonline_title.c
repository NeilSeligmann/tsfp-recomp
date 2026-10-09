/* SPDX-License-Identifier: GPL-3.0-or-later */
#define main inherited_xonline_test_main
#include "test_xonline_offline.c"
#undef main

int main(void)
{
    const xonline_surface_entry row = {XONLINE_TITLE_ID_ENTRY, "XOnlineTitleIdIsSameTitle", 1u};
    CHECK(xonline_hle_init(&row, 1u));
    xonline_hle_set_fatal(fatal);
    CHECK(xonline_offline_register() == 1u);
    guest_region_request request = {.bytes = 0x10000u, .alignment = 0x1000u,
                                   .state = MEM_COMMIT, .protect = PAGE_READWRITE};
    nt_status status;
    base = guest_region_alloc(&request, &status);
    CHECK(base != 0u);
    CHECK(call_refuses(XONLINE_TITLE_ID_ENTRY, 0u, 0u));
    request.fixed_base = 0x00770000u;
    const uint32_t globals = guest_region_alloc(&request, &status);
    CHECK(globals == request.fixed_base);
    CHECK(kernel_guest_write_u32(XONLINE_STATE_POINTER, base + 0x100u));
    const uint32_t values[] = {0u, 1u, 0x4D53005Cu, 0x80000000u, 0xFFFFFFFFu};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        CHECK(kernel_guest_write_u32(base + 0x104u, values[i]));
        CHECK(call(XONLINE_TITLE_ID_ENTRY, values[i], 0u) == 1u);
        CHECK(call(XONLINE_TITLE_ID_ENTRY, values[i] ^ 0x80000000u, 0u) == 0u);
        uint32_t unchanged;
        CHECK(kernel_guest_read_u32(base + 0x104u, &unchanged) && unchanged == values[i]);
    }
    uint32_t hresult = 0xFFFFFFFFu;
    CHECK(xonline_offline_startup(0u, &hresult) && hresult == 0u);
    CHECK(call(XONLINE_TITLE_ID_ENTRY, 0xFFFFFFFFu, 0u) == 1u);
    CHECK(xonline_offline_cleanup() == 0u);
    CHECK(kernel_guest_write_u32(XONLINE_STATE_POINTER, 0u));
    CHECK(call_refuses(XONLINE_TITLE_ID_ENTRY, 0u, 0u));
    CHECK(kernel_guest_write_u32(XONLINE_STATE_POINTER, 0xFFFFFFFFu));
    CHECK(call_refuses(XONLINE_TITLE_ID_ENTRY, 0u, 0u));
    CHECK(guest_region_free(globals));
    CHECK(guest_region_free(base));
    xonline_hle_shutdown();
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
