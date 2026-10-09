/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
/* T421: the thunk layer route for IDirectSound::SynchPlayback (0x407A4C). The handler is a fixture, the module
 * behind it is tested in test_dsound_movie_stream.c and test_dsound_synch_routes.c. This pins the routing: a
 * disabled route changes nothing, enabling needs the compiled stop boundary, an enabled call is answered,
 * popped (return address plus one argument) and traced, a refusing handler stops the run with the registers it
 * was given, an explicit stop override wins and nothing else address is claimed. */
#include "xdk_thunk.h"
#include "dsound_hle.h"
#include "guest_mem.h"
#include "kernel_call.h"
#include "host_runtime.h"
#include "thunk_trace.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_fs_base;
#define SYNCH 0x407A4Cu
#define OTHER_STOP 0x40686Cu
#define DEVICE 0x20000008u
#define SENTINEL 0xABCD0011u
#define CALLER 0x44571Bu
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %d %s\n", __LINE__, #x); } } while (0)

static uint32_t missing_stop;
int recomp_has_stop_boundary(uint32_t address) { return address == missing_stop ? 0 : 1; }
int recomp_has_dispatch_boundary(uint32_t address) { (void)address; return 1; }

static uint32_t stack;
static unsigned handler_calls;
static uint32_t last_sp, last_interface;
static unsigned behavior;
static bool handle(uint32_t sp, uint32_t *result)
{
    handler_calls++;
    last_sp = sp;
    CHECK(kernel_guest_read_u32(sp + 4u, &last_interface));
    if (behavior == 1u) host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, SYNCH, 0u, "synch handler refusal");
    if (behavior == 2u) return false;
    *result = 0x1234u;
    return true;
}

static _Thread_local bool returned;
static _Thread_local host_stop stopped;
static void prepare(uint32_t caller, uint32_t interface)
{
    const uint32_t words[3] = {caller, interface, 0xDEADBEEFu};
    CHECK(kernel_guest_write_bytes(stack, words, sizeof(words)));
    g_esp = stack;
    g_eax = SENTINEL;
    g_ecx = 2u;
    g_edx = 3u;
    g_ebx = 4u;
    g_esi = 5u;
    g_edi = 6u;
    g_ebp = 7u;
}
static void route(uint32_t address)
{
    returned = false;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        recomp_func_t function = recomp_lookup_manual(address);
        if (function == NULL) xdk_thunk_stop_at(address, "no route", "lookup returned NULL");
        function();
        returned = true;
    }
    stopped = *host_run_result();
    host_run_disarm();
}
static void untouched(void)
{
    CHECK(g_eax == SENTINEL && g_esp == stack);
    CHECK(g_ecx == 2u && g_edx == 3u && g_ebx == 4u && g_esi == 5u && g_edi == 6u && g_ebp == 7u);
    CHECK(host_run_scope_depth() == 0u);
}

int main(void)
{
    dsound_hle_init();
    size_t count;
    const dsound_entry *sound = dsound_hle_table(&count);
    CHECK(count == 41u);
    xdk_dispatch_entry entries[41];
    for (unsigned i = 0u; i < 41u; i++) entries[i] = (xdk_dispatch_entry){sound[i].address, sound[i].name, XDK_MODULE_DSOUND};
    CHECK(xdk_thunk_init(entries, 41u));
    guest_region_request request = {0};
    request.bytes = 8192u;
    request.state = MEM_COMMIT;
    request.protect = PAGE_READWRITE;
    nt_status status = STATUS_SUCCESS;
    stack = guest_region_alloc(&request, &status);
    CHECK(stack != 0u);

    /* The capability query follows the compiled marker and nothing else. */
    CHECK(!xdk_thunk_synch_route_enabled());
    CHECK(xdk_thunk_synch_stop_ready());
    missing_stop = SYNCH;
    CHECK(!xdk_thunk_synch_stop_ready());
    CHECK(!xdk_thunk_set_synch_handler(handle));
    CHECK(recomp_lookup_manual(SYNCH) == NULL);
    missing_stop = OTHER_STOP;  /* a different boundary missing does not matter */
    CHECK(xdk_thunk_synch_stop_ready());
    missing_stop = 0u;
    /* Disabled by default: no route, the lifted table's compiled stop is the answer. */
    CHECK(recomp_lookup_manual(SYNCH) == NULL);
    CHECK(xdk_thunk_set_synch_handler(handle));
    CHECK(xdk_thunk_synch_route_enabled());
    CHECK(recomp_lookup_manual(SYNCH) != NULL);
    CHECK(xdk_thunk_set_synch_handler(NULL));
    CHECK(!xdk_thunk_synch_route_enabled());
    CHECK(recomp_lookup_manual(SYNCH) == NULL);
    CHECK(xdk_thunk_set_synch_handler(handle));
    CHECK(recomp_lookup_manual(SYNCH + 1u) == NULL);  /* only the one address */
    CHECK(recomp_lookup_manual(SYNCH - 1u) == NULL);

    /* An enabled call is answered, popped (return address plus one argument) and traced. */
    thunk_trace_reset();
    const uint64_t before = xdk_thunk_synch_call_count();
    prepare(CALLER, DEVICE);
    route(SYNCH);
    CHECK(returned);
    CHECK(handler_calls == 1u && last_sp == stack && last_interface == DEVICE);
    CHECK(g_eax == 0x1234u);
    CHECK(g_esp == stack + 8u);
    CHECK(g_ecx == 2u && g_edx == 3u && g_ebx == 4u && g_esi == 5u && g_edi == 6u && g_ebp == 7u);
    CHECK(xdk_thunk_synch_call_count() == before + 1u);
    size_t entries_seen;
    const thunk_trace_entry *trace = thunk_trace_entries(&entries_seen);
    CHECK(entries_seen == 1u);
    if (entries_seen == 1u) {
        CHECK(trace[0].kind == THUNK_KIND_XDK && trace[0].address == SYNCH && trace[0].return_address == CALLER);
        CHECK(trace[0].result_known && trace[0].result == 0x1234u);
    }
    /* A handler that stops propagates the stop and answers nothing. */
    behavior = 1u;
    prepare(CALLER, DEVICE);
    route(SYNCH);
    CHECK(!returned && stopped.guest_address == SYNCH && strcmp(stopped.detail, "synch handler refusal") == 0);
    untouched();
    CHECK(xdk_thunk_synch_call_count() == before + 1u);
    /* An explicit stop override wins over the route, as it does for the movie methods. */
    behavior = 0u;
    const xdk_stop_override explicit_stop = {SYNCH, "explicit synch", "explicit priority"};
    CHECK(xdk_thunk_stop_override_adopt(&explicit_stop, 1u));
    handler_calls = 0u;
    prepare(CALLER, DEVICE);
    route(SYNCH);
    CHECK(!returned && handler_calls == 0u && strcmp(stopped.detail, "explicit priority") == 0);
    untouched();
    xdk_thunk_stop_override_reset();
    /* Disabling restores the old lookup and shutdown clears the registration. */
    CHECK(xdk_thunk_set_synch_handler(NULL));
    CHECK(recomp_lookup_manual(SYNCH) == NULL);
    CHECK(xdk_thunk_set_synch_handler(handle));
    xdk_thunk_shutdown();
    CHECK(recomp_lookup_manual(SYNCH) == NULL);
    CHECK(xdk_thunk_synch_call_count() == 0u);
    CHECK(checks > 40u);
    CHECK(guest_region_free(stack));
    printf("synch dispatch: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
