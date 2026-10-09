/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
/* T392: the thunk layer route for the five indirect movie stream methods. The handler pair is a
 * fixture, the module behind it is tested in test_dsound_movie_stream.c. This pins the routing: a
 * disabled route changes nothing, an owned stream is answered and popped, anything else keeps the
 * route it had (the startup virtual route, then the compiled stop), and explicit stops still win. */
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
#define ADD_REF 0x40723Fu
#define RELEASE 0x407286u
#define STATUS 0x4073D3u
#define PROCESS 0x407424u
#define DISCONT 0x40733Bu
#define FLUSH 0x407388u
#define GET_INFO 0x4072D4u
#define OWNED 0x12345678u
#define COMPLETION_OWNED 0x22222222u
#define STRANGER 0x00BADBADu
#define SENTINEL 0xABCD0011u
#define CALLER 0x445128u
static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL %d %s\n", __LINE__, #x); } } while (0)

static uint32_t missing_stop;
int recomp_has_stop_boundary(uint32_t address) { return address == missing_stop ? 0 : 1; }
int recomp_has_dispatch_boundary(uint32_t address) { (void)address; return 1; }

static uint32_t stack, out;
static unsigned handler_calls, owner_calls, startup_calls;
static uint32_t last_address, last_sp;
static unsigned behavior;
static bool owns(uint32_t sp)
{
    uint32_t stream = 0u;
    owner_calls++;
    return kernel_guest_read_u32(sp + 4u, &stream) && stream == OWNED;
}
static bool handle(uint32_t address, uint32_t sp, uint32_t *result, uint32_t *pop)
{
    handler_calls++;
    last_address = address;
    last_sp = sp;
    if (behavior == 1u) host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, address, 0u, "movie handler refusal");
    *result = 0x1000u + address % 0x100u;
    *pop = address == PROCESS ? 12u : address == STATUS ? 8u : 4u;
    return true;
}
static unsigned completion_handler_calls, completion_behavior;
static bool completion_owns(uint32_t sp)
{
    uint32_t stream = 0u;
    return kernel_guest_read_u32(sp + 4u, &stream) && stream == COMPLETION_OWNED;
}
static bool completion_handle(uint32_t address, uint32_t sp, uint32_t *result, uint32_t *pop)
{
    (void)sp;
    completion_handler_calls++;
    if (completion_behavior == 1u) return false;
    *result = 0x2000u + address % 0x100u;
    *pop = address == PROCESS ? 12u : 8u;
    return true;
}
static uint32_t startup_discontinuity(uint32_t stream) { startup_calls++; return stream == STRANGER ? 7u : 0u; }
static uint32_t startup_status(uint32_t stream, uint32_t output)
{
    startup_calls++;
    return kernel_guest_write_u32(output, 1u) && stream == STRANGER ? 0u : 9u;
}

static _Thread_local bool returned;
static _Thread_local host_stop stopped;
static void prepare(uint32_t caller, uint32_t stream)
{
    const uint32_t words[4] = {caller, stream, out, 0u};
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
    xdk_thunk_stream_virtual_cancel_pending();
}
static void untouched(void)
{
    CHECK(g_eax == SENTINEL && g_esp == stack);
    CHECK(g_ecx == 2u && g_edx == 3u && g_ebx == 4u && g_esi == 5u && g_edi == 6u && g_ebp == 7u);
    CHECK(host_run_scope_depth() == 0u);
}
static unsigned pops(uint32_t address) { return address == PROCESS ? 12u : address == STATUS ? 8u : 4u; }

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
    out = stack + 4096u;
    const uint32_t methods[] = {ADD_REF, RELEASE, STATUS, PROCESS, DISCONT, FLUSH};

    /* Disabled by default: no route, exactly as before this existed. */
    for (unsigned i = 0u; i < 6u; i++) CHECK(recomp_lookup_manual(methods[i]) == NULL);
    /* Enabling needs both callbacks, a matched pair, and every compiled stream stop. */
    CHECK(!xdk_thunk_set_movie_method_handler(owns, NULL));
    CHECK(!xdk_thunk_set_movie_method_handler(NULL, handle));
    missing_stop = PROCESS;
    CHECK(!xdk_thunk_set_movie_method_handler(owns, handle));
    missing_stop = 0u;
    for (unsigned i = 0u; i < 6u; i++) CHECK(recomp_lookup_manual(methods[i]) == NULL);
    CHECK(xdk_thunk_set_movie_method_handler(owns, handle));
    CHECK(xdk_thunk_set_movie_method_handler(NULL, NULL));
    for (unsigned i = 0u; i < 6u; i++) CHECK(recomp_lookup_manual(methods[i]) == NULL);
    CHECK(xdk_thunk_set_movie_method_handler(owns, handle));
    CHECK(recomp_lookup_manual(GET_INFO) == NULL);  /* GetInfo is not a movie method */

    /* An owned stream is answered, popped (return address plus the stdcall arguments) and traced. */
    for (unsigned i = 0u; i < 6u; i++) {
        thunk_trace_reset();
        const uint64_t before = xdk_thunk_movie_method_call_count();
        handler_calls = owner_calls = 0u;
        prepare(CALLER, OWNED);
        route(methods[i]);
        CHECK(returned);
        CHECK(handler_calls == 1u && owner_calls == 1u && last_address == methods[i] && last_sp == stack);
        CHECK(g_eax == 0x1000u + methods[i] % 0x100u);
        CHECK(g_esp == stack + 4u + pops(methods[i]));
        CHECK(g_ecx == 2u && g_edx == 3u && g_ebx == 4u && g_esi == 5u && g_edi == 6u && g_ebp == 7u);
        CHECK(xdk_thunk_movie_method_call_count() == before + 1u);
        size_t entries_seen;
        const thunk_trace_entry *trace = thunk_trace_entries(&entries_seen);
        CHECK(entries_seen == 1u);
        if (entries_seen == 1u) {
            CHECK(trace[0].kind == THUNK_KIND_XDK && trace[0].address == methods[i] && trace[0].return_address == CALLER);
            CHECK(trace[0].result_known && trace[0].result == g_eax);
        }
    }
    /* A stranger on AddRef, Release and Process takes the compiled stop. */
    const uint32_t only_stops[] = {ADD_REF, RELEASE, PROCESS};
    for (unsigned i = 0u; i < 3u; i++) {
        thunk_trace_reset();
        handler_calls = 0u;
        const uint64_t before = xdk_thunk_movie_method_call_count();
        prepare(CALLER, STRANGER);
        route(only_stops[i]);
        CHECK(!returned);
        CHECK(handler_calls == 0u);
        CHECK(stopped.guest_address == only_stops[i]);
        CHECK(strstr(stopped.detail, "Unrecovered stream or shared reference method") != NULL);
        untouched();
        CHECK(xdk_thunk_movie_method_call_count() == before);
        size_t entries_seen;
        const thunk_trace_entry *trace = thunk_trace_entries(&entries_seen);
        CHECK(entries_seen == 1u);
        if (entries_seen == 1u) CHECK(trace[0].address == only_stops[i] && !trace[0].result_known);
    }
    /* GetStatus and Discontinuity on a stranger keep the startup virtual route when it is enabled. */
    CHECK(xdk_thunk_set_stream_virtual_handlers(startup_discontinuity, startup_status));
    startup_calls = 0u;
    const uint32_t startup_words[3] = {0x29CEAu, STRANGER, out + 16u};
    CHECK(kernel_guest_write_bytes(stack, startup_words, sizeof(startup_words)));
    g_esp = stack;
    g_eax = SENTINEL;
    route(STATUS);
    CHECK(returned && startup_calls == 1u && g_eax == 0u && g_esp == stack + 12u);
    uint32_t written = 0u;
    CHECK(kernel_guest_read_u32(out + 16u, &written) && written == 1u);
    const uint32_t discont_words[2] = {0x29B72u, STRANGER};
    CHECK(kernel_guest_write_bytes(stack, discont_words, sizeof(discont_words)));
    g_esp = stack;
    g_eax = SENTINEL;
    route(DISCONT);
    CHECK(returned && startup_calls == 2u && g_eax == 7u && g_esp == stack + 8u);
    CHECK(xdk_thunk_stream_virtual_pending_count() == 0u && xdk_thunk_stream_virtual_active_count() == 0u);
    /* The movie handler still wins for its own stream while the startup route is on. */
    startup_calls = 0u;
    prepare(CALLER, OWNED);
    route(STATUS);
    CHECK(returned && startup_calls == 0u && g_eax == 0x1000u + STATUS % 0x100u && g_esp == stack + 12u);
    /* Without the startup route the same stranger stops. */
    CHECK(xdk_thunk_set_stream_virtual_handlers(NULL, NULL));
    prepare(CALLER, STRANGER);
    route(STATUS);
    CHECK(!returned && stopped.guest_address == STATUS);
    untouched();
    /* An explicit stop override wins over the movie route, as it does over the startup route. */
    const xdk_stop_override explicit_stop = {ADD_REF, "explicit addref", "explicit priority"};
    CHECK(xdk_thunk_stop_override_adopt(&explicit_stop, 1u));
    handler_calls = 0u;
    prepare(CALLER, OWNED);
    route(ADD_REF);
    CHECK(!returned && handler_calls == 0u && strcmp(stopped.detail, "explicit priority") == 0);
    untouched();
    xdk_thunk_stop_override_reset();
    /* A handler that stops propagates the stop with the registers it was given. */
    behavior = 1u;
    prepare(CALLER, OWNED);
    route(PROCESS);
    CHECK(!returned && stopped.guest_address == PROCESS && strcmp(stopped.detail, "movie handler refusal") == 0);
    untouched();
    behavior = 0u;
    /* T681: the completion route answers GetStatus, Process and GetInfo of the streams it owns, after the movie model. */
    CHECK(recomp_lookup_manual(GET_INFO) == NULL);
    CHECK(!xdk_thunk_set_completion_method_handler(completion_owns, NULL));
    CHECK(!xdk_thunk_set_completion_method_handler(NULL, completion_handle));
    missing_stop = PROCESS;
    CHECK(!xdk_thunk_set_completion_method_handler(completion_owns, completion_handle));
    missing_stop = 0u;
    CHECK(recomp_lookup_manual(GET_INFO) == NULL);
    CHECK(xdk_thunk_set_completion_method_handler(completion_owns, completion_handle));
    CHECK(recomp_lookup_manual(GET_INFO) != NULL);
    const uint32_t completion_methods[] = {STATUS, PROCESS, GET_INFO};
    for (unsigned i = 0u; i < 3u; i++) {
        thunk_trace_reset();
        const uint64_t before = xdk_thunk_completion_method_call_count();
        const uint64_t movie_before = xdk_thunk_movie_method_call_count();
        completion_handler_calls = handler_calls = 0u;
        prepare(CALLER, COMPLETION_OWNED);
        route(completion_methods[i]);
        CHECK(returned && completion_handler_calls == 1u && handler_calls == 0u);
        CHECK(g_eax == 0x2000u + completion_methods[i] % 0x100u);
        CHECK(g_esp == stack + 4u + (completion_methods[i] == PROCESS ? 12u : 8u));
        CHECK(g_ecx == 2u && g_edx == 3u && g_ebx == 4u && g_esi == 5u && g_edi == 6u && g_ebp == 7u);
        CHECK(xdk_thunk_completion_method_call_count() == before + 1u && xdk_thunk_movie_method_call_count() == movie_before);
        size_t entries_seen;
        const thunk_trace_entry *trace = thunk_trace_entries(&entries_seen);
        CHECK(entries_seen == 1u);
        if (entries_seen == 1u)
            CHECK(trace[0].address == completion_methods[i] && trace[0].return_address == CALLER && trace[0].result_known && trace[0].result == g_eax);
    }
    /* The movie model still answers its own stream first, and a stranger takes the compiled stop. */
    completion_handler_calls = handler_calls = 0u;
    prepare(CALLER, OWNED);
    route(PROCESS);
    CHECK(returned && handler_calls == 1u && completion_handler_calls == 0u);
    prepare(CALLER, STRANGER);
    route(PROCESS);
    CHECK(!returned && completion_handler_calls == 0u && strstr(stopped.detail, "Unrecovered stream") != NULL);
    untouched();
    /* A handler that declines leaves the call to the old route: here the compiled stop, nothing popped or counted. */
    completion_behavior = 1u;
    thunk_trace_reset();
    completion_handler_calls = 0u;
    const uint64_t declined_before = xdk_thunk_completion_method_call_count();
    prepare(CALLER, COMPLETION_OWNED);
    route(STATUS);
    CHECK(!returned && completion_handler_calls == 1u && strstr(stopped.detail, "Unrecovered stream") != NULL);
    untouched();
    CHECK(xdk_thunk_completion_method_call_count() == declined_before);
    size_t declined_entries;
    (void)thunk_trace_entries(&declined_entries);
    CHECK(declined_entries == 1u);  /* only the compiled stop's own pending entry */
    completion_behavior = 0u;
    /* Disabling restores the old lookup. */
    CHECK(xdk_thunk_set_completion_method_handler(NULL, NULL));
    CHECK(recomp_lookup_manual(GET_INFO) == NULL);
    /* Disabling restores the old lookup and shutdown clears the registration. */
    CHECK(xdk_thunk_set_movie_method_handler(NULL, NULL));
    for (unsigned i = 0u; i < 6u; i++) CHECK(recomp_lookup_manual(methods[i]) == NULL);
    CHECK(xdk_thunk_set_movie_method_handler(owns, handle));
    xdk_thunk_shutdown();
    for (unsigned i = 0u; i < 6u; i++) CHECK(recomp_lookup_manual(methods[i]) == NULL);
    CHECK(guest_region_free(stack));
    printf("movie method dispatch: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
