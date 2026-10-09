/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "xnet_offline.h"
#include "xnet_hle.h"
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
    return xnet_hle_call(address, &frame);
}
static bool call_refuses(uint32_t address, uint32_t arg0)
{
    volatile bool returned = false;
    kernel_call_frame frame = frame_with(arg0, 0u);
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        (void)xnet_hle_call(address, &frame);
        returned = true;
    }
    const bool refused = !returned && host_run_result()->reason == HOST_STOP_XDK_UNIMPLEMENTED &&
                         host_run_result()->guest_address == address;
    host_run_disarm();
    return refused;
}
static uint32_t slot_value;
static bool slot_readable;
static bool reader(uint32_t *out)
{
    if (!slot_readable) return false;
    *out = slot_value;
    return true;
}
int main(void)
{
    const xnet_surface_entry rows[] = {{XNET_NTOHS_ENTRY, "ntohs", 15u},
                                       {XNET_HTONL_ENTRY, "htonl", 8u},
                                       {XNET_WSAGETLASTERROR_ENTRY, "WSAGetLastError", 11u},
                                       {0x0043188Fu, "sendto", 12u}};
    CHECK(xnet_hle_init(rows, 4u));
    xnet_hle_set_fatal(fatal);
    CHECK(xnet_hle_implemented_count() == 0u);
    CHECK(xnet_offline_register() == 3u);
    CHECK(xnet_hle_implemented_count() == 3u);
    CHECK(xnet_hle_entry(0x0043188Fu)->state == XNET_ENTRY_STUB); /* sendto stays a stop */
    xnet_offline_reset();

    guest_region_request request = {0};
    request.bytes = 0x1000u; request.protect = PAGE_READWRITE; request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    base = guest_region_alloc(&request, &status);
    CHECK(base != 0u);

    /* Byte swaps: the original bytes (tests/test_xnet_oracle.py) give the same answers. */
    CHECK(xnet_swap16(0x1234u) == 0x3412u && xnet_swap32(0x11223344u) == 0x44332211u);
    CHECK(call(XNET_NTOHS_ENTRY, 0x1234u, 0u) == 0x3412u);
    CHECK(call(XNET_NTOHS_ENTRY, 0xABCD1234u, 0u) == 0x3412u); /* only the low word is read */
    CHECK(call(XNET_NTOHS_ENTRY, 0x0050u, 0u) == 0x5000u);
    CHECK(call(XNET_HTONL_ENTRY, 0x7F000001u, 0u) == 0x0100007Fu);
    CHECK(call(XNET_HTONL_ENTRY, 0x01020304u, 0u) == 0x04030201u);

    /* WSAGetLastError returns the guest slot, and refuses with no readable slot. */
    CHECK(call_refuses(XNET_WSAGETLASTERROR_ENTRY, 0u)); /* no reader installed */
    xnet_offline_set_last_error_reader(reader);
    slot_readable = false;
    CHECK(call_refuses(XNET_WSAGETLASTERROR_ENTRY, 0u));
    slot_readable = true;
    slot_value = 0x2751u;
    CHECK(call(XNET_WSAGETLASTERROR_ENTRY, 0u, 0u) == 0x2751u);
    slot_value = 0u;
    CHECK(call(XNET_WSAGETLASTERROR_ENTRY, 0u, 0u) == 0u);
    xnet_offline_stats stats = xnet_offline_stats_get();
    CHECK(stats.ntohs_calls == 3u && stats.htonl_calls == 2u && stats.last_error_calls == 2u);

    /* An unregistered XNET address is not answered by the registry's silent default path
     * in production, the thunk stops first: here it only reports and returns 0. */
    CHECK(xnet_hle_entry(0x0043188Fu)->state == XNET_ENTRY_STUB);

    /* The production dispatcher routes the XNET section and pops the declared arguments. */
    const xdk_dispatch_entry dispatch_rows[] = {
        {XNET_NTOHS_ENTRY, "ntohs", XDK_MODULE_XNET},
        {XNET_HTONL_ENTRY, "htonl", XDK_MODULE_XNET},
        {XNET_WSAGETLASTERROR_ENTRY, "WSAGetLastError", XDK_MODULE_XNET},
        {0x0043188Fu, "sendto", XDK_MODULE_XNET},
    };
    CHECK(xdk_module_for_section("XNET") == XDK_MODULE_XNET);
    CHECK(xdk_thunk_init(dispatch_rows, sizeof(dispatch_rows) / sizeof(dispatch_rows[0])));
    CHECK(xdk_thunk_declare_abi(XNET_NTOHS_ENTRY, XDK_CC_STDCALL, 1u, 0u));
    CHECK(xnet_offline_register() == 3u);
    xnet_offline_reset();
    CHECK(kernel_guest_write_u32(base, 0x12345678u) && kernel_guest_write_u32(base + 4u, 0x00001234u));
    g_esp = base;
    g_eax = g_ecx = g_edx = 0u;
    xdk_thunk_dispatch_at(XNET_NTOHS_ENTRY);
    CHECK(g_eax == 0x3412u && g_esp == base + 8u);
    CHECK(xdk_thunk_module_call_count(XDK_MODULE_XNET) == 1u);
    xdk_thunk_shutdown();
    xnet_hle_shutdown();
    printf("xnet offline: %u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
