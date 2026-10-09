/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "xonline_offline.h"
#include "xonline_hle.h"
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "recomp_abi.h"
#include "xdk_thunk.h"
#include <stdio.h>
#include <string.h>
#include "probe_cache_wrap.h"

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)
static void fatal(uint32_t address, const char *message)
{
    host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, address, 0u, message);
}
static uint32_t base;
/* A stdcall frame: [ret][arg0][arg1] at base. */
static kernel_call_frame frame_with(uint32_t arg0, uint32_t arg1)
{
    CHECK(kernel_guest_write_u32(base, 0x1234u) && kernel_guest_write_u32(base + 4u, arg0) &&
          kernel_guest_write_u32(base + 8u, arg1));
    kernel_call_frame frame = {.stack_ptr = base};
    return frame;
}
static uint32_t call(uint32_t address, uint32_t arg0, uint32_t arg1)
{
    kernel_call_frame frame = frame_with(arg0, arg1);
    return xonline_hle_call(address, &frame);
}
static bool call_refuses(uint32_t address, uint32_t arg0, uint32_t arg1)
{
    volatile bool returned = false;
    kernel_call_frame frame = frame_with(arg0, arg1);
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        (void)xonline_hle_call(address, &frame);
        returned = true;
    }
    const bool refused = !returned && host_run_result()->reason == HOST_STOP_XDK_UNIMPLEMENTED &&
                         host_run_result()->guest_address == address;
    host_run_disarm();
    return refused;
}
static bool users_buffer_is(uint32_t users, uint8_t value)
{
    uint8_t bytes[XONLINE_USERS_BYTES];
    if (!kernel_guest_read_bytes(users, bytes, sizeof(bytes))) {
        return false;
    }
    for (size_t i = 0u; i < sizeof(bytes); i++) {
        if (bytes[i] != value) {
            return false;
        }
    }
    return true;
}
int main(void)
{
    const xonline_surface_entry rows[] = {{XONLINE_STARTUP_ENTRY, "XOnlineStartup", 2u},
                                          {XONLINE_GET_USERS_ENTRY, "XOnlineGetUsers", 2u},
                                          {XONLINE_CLEANUP_ENTRY, "XOnlineCleanup", 2u},
                                          {0x00412FBFu, "XOnlineLogon", 1u}};
    CHECK(xonline_hle_init(rows, 4u));
    xonline_hle_set_fatal(fatal);
    CHECK(xonline_hle_implemented_count() == 0u); /* nothing answers before registration */
    CHECK(xonline_offline_register() == 4u); /* T1071: Logon joined the three T904 handlers */
    CHECK(xonline_hle_implemented_count() == 4u);
    CHECK(xonline_hle_entry(0x00412FBFu)->state == XONLINE_ENTRY_IMPLEMENTED);
    xonline_offline_reset();

    guest_region_request request = {0};
    request.bytes = 0x4000u; request.protect = PAGE_READWRITE; request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    base = guest_region_alloc(&request, &status);
    CHECK(base != 0u);
    const uint32_t users = base + 0x1000u, count = base + 0x2000u;
    uint8_t fill[XONLINE_USERS_BYTES];
    memset(fill, 0xA5, sizeof(fill));

    /* Before XOnlineStartup: GetUsers and Cleanup answer 0x80150005 and write nothing. */
    CHECK(kernel_guest_write_bytes(users, fill, sizeof(fill)) && kernel_guest_write_u32(count, 0x77u));
    CHECK(call(XONLINE_GET_USERS_ENTRY, users, count) == XONLINE_E_NOT_INITIALIZED);
    CHECK(users_buffer_is(users, 0xA5));
    uint32_t word = 0u;
    CHECK(kernel_guest_read_u32(count, &word) && word == 0x77u);
    CHECK(call(XONLINE_CLEANUP_ENTRY, 0u, 0u) == XONLINE_E_NOT_INITIALIZED);

    /* Startup (reserved 0): S_OK, GetUsers zeroes the 0x700 bytes and the count, one past the end is untouched. */
    CHECK(call(XONLINE_STARTUP_ENTRY, 0u, 0u) == 0u);
    CHECK(xonline_offline_stats_get().references == 1u);
    CHECK(kernel_guest_write_u8(users + XONLINE_USERS_BYTES, 0x5Au));
    CHECK(call(XONLINE_GET_USERS_ENTRY, users, count) == 0u);
    CHECK(users_buffer_is(users, 0x00));
    uint8_t after = 0u;
    CHECK(kernel_guest_read_u8(users + XONLINE_USERS_BYTES, &after) && after == 0x5Au);
    CHECK(kernel_guest_read_u32(count, &word) && word == 0u);

    /* A second Startup only takes a reference, Cleanup drops one at a time and frees at zero. */
    CHECK(call(XONLINE_STARTUP_ENTRY, 0u, 0u) == 0u);
    CHECK(xonline_offline_stats_get().references == 2u);
    CHECK(call(XONLINE_CLEANUP_ENTRY, 0u, 0u) == 0u);
    CHECK(xonline_offline_stats_get().references == 1u);
    CHECK(call(XONLINE_CLEANUP_ENTRY, 0u, 0u) == 0u);
    CHECK(xonline_offline_stats_get().references == 0u);
    CHECK(call(XONLINE_CLEANUP_ENTRY, 0u, 0u) == XONLINE_E_NOT_INITIALIZED);
    CHECK(xonline_offline_stats_get().references == 0u);
    CHECK(kernel_guest_write_bytes(users, fill, sizeof(fill)));
    CHECK(call(XONLINE_GET_USERS_ENTRY, users, count) == XONLINE_E_NOT_INITIALIZED);
    CHECK(users_buffer_is(users, 0xA5));
    xonline_offline_stats stats = xonline_offline_stats_get();
    CHECK(stats.startups == 2u && stats.cleanups == 4u && stats.user_queries == 3u);

    /* Refusals by name: a nonzero reserved word, an unwritable buffer. */
    CHECK(call_refuses(XONLINE_STARTUP_ENTRY, 0x1000u, 0u));
    CHECK(xonline_offline_stats_get().references == 0u);
    CHECK(call(XONLINE_STARTUP_ENTRY, 0u, 0u) == 0u);
    CHECK(call_refuses(XONLINE_GET_USERS_ENTRY, 0u, count));
    CHECK(kernel_guest_write_bytes(users, fill, sizeof(fill)));
    CHECK(call_refuses(XONLINE_GET_USERS_ENTRY, users, 0u));
    /* Both nonzero readable spans are checked before the offline model starts writing. */
    CHECK(users_buffer_is(users, 0xA5));
    uint32_t hresult = 0u;
    CHECK(!xonline_offline_startup(1u, &hresult));
    CHECK(!xonline_offline_startup(0u, NULL));
    CHECK(!xonline_offline_get_users(users, count, NULL));

    /* T1071: every no-state wrapper answers its measured original value, then refuses after Startup.
     * address, expected no-state return. Kept apart from the table in xonline_offline.c on purpose,
     * the Unicorn oracle test ties that table to the original bytes. */
    static const uint32_t wrappers[][2] = {
        {0x00412E50u, XONLINE_E_NOT_INITIALIZED}, {0x00412E5Bu, XONLINE_E_NOT_INITIALIZED},
        {0x00412F9Eu, XONLINE_E_NOT_INITIALIZED}, {0x00412FA9u, XONLINE_E_NOT_INITIALIZED},
        {0x00412FBFu, XONLINE_E_NOT_INITIALIZED}, {0x00412FCEu, XONLINE_E_NOT_INITIALIZED},
        {0x00412FD9u, 0u},                        {0x00412FE4u, XONLINE_E_NOT_INITIALIZED},
        {0x00412FEFu, XONLINE_E_NOT_INITIALIZED}, {0x00412FFAu, XONLINE_E_NOT_INITIALIZED},
        {0x00413010u, XONLINE_E_NOT_INITIALIZED}, {0x00413034u, XONLINE_E_NOT_INITIALIZED},
        {0x0041303Fu, XONLINE_E_NOT_INITIALIZED}, {0x0041306Cu, XONLINE_E_NOT_INITIALIZED},
        {0x00413077u, XONLINE_E_NOT_INITIALIZED}, {0x00413082u, XONLINE_E_NOT_INITIALIZED},
        {0x0041308Du, 0u},                        {0x00413098u, XONLINE_E_NOT_INITIALIZED},
        {0x004130A3u, XONLINE_E_NOT_INITIALIZED}, {0x004130AEu, XONLINE_E_NOT_INITIALIZED},
        {0x004130CFu, XONLINE_E_NOT_INITIALIZED}, {0x004130F0u, XONLINE_E_NOT_INITIALIZED},
        {0x004130FBu, XONLINE_E_NOT_INITIALIZED}, {0x00413106u, XONLINE_E_NOT_INITIALIZED},
        {0x00413111u, XONLINE_E_NOT_INITIALIZED}, {0x00413120u, XONLINE_E_NOT_INITIALIZED},
    };
    enum { WRAPPER_COUNT = sizeof(wrappers) / sizeof(wrappers[0]) };
    xonline_surface_entry full[WRAPPER_COUNT + 4];
    size_t full_count = 0u;
    for (size_t i = 0u; i < WRAPPER_COUNT; i++) full[full_count++] = (xonline_surface_entry){wrappers[i][0], "wrapper", 1u};
    full[full_count++] = (xonline_surface_entry){XONLINE_STARTUP_ENTRY, "XOnlineStartup", 2u};
    full[full_count++] = (xonline_surface_entry){XONLINE_GET_USERS_ENTRY, "XOnlineGetUsers", 2u};
    full[full_count++] = (xonline_surface_entry){XONLINE_CLEANUP_ENTRY, "XOnlineCleanup", 2u};
    full[full_count++] = (xonline_surface_entry){0x00413005u, "XOnlineTitleIdIsSameTitle", 1u};
    CHECK(xonline_hle_init(full, full_count));
    xonline_hle_set_fatal(fatal);
    xonline_offline_reset();
    CHECK(xonline_offline_register() == 30u);
    CHECK(xonline_hle_implemented_count() == 30u);
    CHECK(xonline_hle_entry(0x00413005u)->state == XONLINE_ENTRY_IMPLEMENTED); /* T1103 actual-state predicate */
    for (size_t i = 0u; i < WRAPPER_COUNT; i++) CHECK(call(wrappers[i][0], 1u, 2u) == wrappers[i][1]);
    CHECK(xonline_offline_stats_get().service_calls == WRAPPER_COUNT);
    CHECK(call(XONLINE_STARTUP_ENTRY, 0u, 0u) == 0u);
    for (size_t i = 0u; i < WRAPPER_COUNT; i++) CHECK(call_refuses(wrappers[i][0], 1u, 2u));
    CHECK(call(XONLINE_CLEANUP_ENTRY, 0u, 0u) == 0u);
    CHECK(call(wrappers[0][0], 1u, 2u) == XONLINE_E_NOT_INITIALIZED); /* state gone, no-state again */
    xonline_offline_reset();

    /* The production dispatcher adopts the XONLINE subset and routes a measured address. */
    const xdk_dispatch_entry dispatch_rows[] = {
        {XONLINE_STARTUP_ENTRY, "XOnlineStartup", XDK_MODULE_XONLINE},
        {XONLINE_GET_USERS_ENTRY, "XOnlineGetUsers", XDK_MODULE_XONLINE},
        {XONLINE_CLEANUP_ENTRY, "XOnlineCleanup", XDK_MODULE_XONLINE},
        {0x00412FBFu, "XOnlineLogon", XDK_MODULE_XONLINE},
    };
    CHECK(xdk_module_for_section("XONLINE") == XDK_MODULE_XONLINE);
    CHECK(xdk_thunk_init(dispatch_rows, sizeof(dispatch_rows) / sizeof(dispatch_rows[0])));
    CHECK(xdk_thunk_declare_abi(XONLINE_STARTUP_ENTRY, XDK_CC_STDCALL, 1u, 0u));
    CHECK(xonline_hle_entry(XONLINE_STARTUP_ENTRY) != NULL);
    CHECK(xonline_offline_register() == 4u);
    xonline_offline_reset();
    CHECK(kernel_guest_write_u32(base, 0x12345678u) && kernel_guest_write_u32(base + 4u, 0u));
    g_esp = base;
    g_eax = g_ecx = g_edx = 0u;
    xdk_thunk_dispatch_at(XONLINE_STARTUP_ENTRY);
    CHECK(g_eax == 0u && g_esp == base + 8u);
    CHECK(xdk_thunk_module_call_count(XDK_MODULE_XONLINE) == 1u);
    CHECK(xonline_offline_stats_get().references == 1u);
    xdk_thunk_shutdown();
    xonline_hle_shutdown();
    printf("xonline offline: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
