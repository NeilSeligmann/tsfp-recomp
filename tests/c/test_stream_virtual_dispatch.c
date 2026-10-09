/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "xdk_thunk.h"
#include "dsound_hle.h"
#include "guest_mem.h"
#include "kernel_call.h"
#include "host_runtime.h"
#include "thunk_trace.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
TSFP_RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp, g_fs_base;
#define ADDRESS 0x0040733Bu
#define CALLER 0x00029B72u
static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)
static uint32_t marker_missing, marker_wrong;
static uint32_t dispatch_missing, dispatch_wrong;
#ifndef STREAM_VIRTUAL_NO_MARKERS
int recomp_has_stop_boundary(uint32_t address)
{
    if (address == marker_missing) return 0;
    return address == marker_wrong ? 2 : 1;
}
int recomp_has_dispatch_boundary(uint32_t address)
{
    if (address == dispatch_missing) return 0;
    return address == dispatch_wrong ? 2 : 1;
}
#endif
static uint32_t stack;
static volatile unsigned char *guard;
static unsigned behavior;
static _Thread_local unsigned depth;
static _Thread_local bool returned, configuration_refused;
static _Thread_local host_stop stopped;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool entered, release_handler;
static uint32_t typed_handler(uint32_t stream)
{
    configuration_refused = !xdk_thunk_set_stream_virtual_handler(NULL) &&
                            !xdk_thunk_set_stream_virtual_handler(typed_handler);
    if (stream != 0x12345678u)
        host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, ADDRESS, 0u, "not owned");
    if (behavior == 1u || (behavior == 4u && depth != 0u))
        host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, ADDRESS, 0u, "typed refusal");
    if (behavior == 2u) *guard = 1u;
    if ((behavior == 4u || behavior == 5u) && depth == 0u) {
        const uint32_t saved_eax = g_eax, saved_esp = g_esp;
        depth++;
        recomp_func_t function = recomp_lookup_manual(ADDRESS);
        function();
        depth--;
        g_eax = saved_eax;
        g_esp = saved_esp;
    }
    if (behavior == 3u) {
        pthread_mutex_lock(&gate);
        entered = true;
        pthread_cond_broadcast(&condition);
        while (!release_handler) (void)pthread_cond_wait(&condition, &gate);
        pthread_mutex_unlock(&gate);
    }
    return 0u;
}
static void prepare(uint32_t pointer, uint32_t caller, uint32_t argument)
{
    const uint32_t words[2] = {caller, argument};
    CHECK(kernel_guest_write_bytes(pointer, words, sizeof(words)));
    g_eax = 0xABCD0011u; g_ecx = 0xABCD0022u; g_edx = 0xABCD0033u;
    g_ebx = 0xABCD0044u; g_esi = 0xABCD0055u; g_edi = 0xABCD0066u;
    g_ebp = 0xABCD0077u; g_fs_base = 0xABCD0088u; g_esp = pointer;
}
static void route(bool normal)
{
    returned = false;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        if (normal) xdk_thunk_stop_at(ADDRESS, "StreamDiscontinuity", "compiled direct stop");
        else {
            recomp_func_t function = recomp_lookup_manual(ADDRESS);
            if (function != NULL) function();
            else xdk_thunk_stop_at(ADDRESS, "StreamDiscontinuity", "compiled fallback stop");
        }
        returned = true;
    }
    stopped = *host_run_result();
    host_run_disarm();
    xdk_thunk_stream_virtual_cancel_pending();
}
static void preserved(uint32_t esp, bool success)
{
    CHECK(g_eax == (success ? 0u : 0xABCD0011u));
    CHECK(g_esp == esp + (success ? 8u : 0u));
    CHECK(g_ecx == 0xABCD0022u && g_edx == 0xABCD0033u);
    CHECK(g_ebx == 0xABCD0044u && g_esi == 0xABCD0055u);
    CHECK(g_edi == 0xABCD0066u && g_ebp == 0xABCD0077u && g_fs_base == 0xABCD0088u);
    uint32_t words[2];
    CHECK(kernel_guest_read_bytes(esp, words, sizeof(words)));
    CHECK(words[0] == CALLER && words[1] == 0x12345678u);
    CHECK(xdk_thunk_stream_virtual_active_count() == 0u);
    CHECK(xdk_thunk_stream_virtual_pending_count() == 0u);
    CHECK(host_run_scope_depth() == 0u);
}
static void readiness(const xdk_dispatch_entry *entries)
{
    const uint32_t stops[14] = {0x384859u,0x40686Cu,0x406879u,0x40688Au,0x40723Fu,
        0x407286u,0x4072D4u,ADDRESS,0x407388u,0x4073D3u,0x407424u,0x407883u,0x40788Du,0x4093ADu};
    for (unsigned i = 0u; i < 14u; i++) {
        marker_missing = stops[i];
        CHECK(!xdk_thunk_set_stream_virtual_handler(typed_handler));
        marker_missing = 0u; marker_wrong = stops[i];
        CHECK(!xdk_thunk_set_stream_virtual_handler(typed_handler));
        marker_wrong = 0u;
    }
    for (unsigned i = 0u; i < 41u; i++) {
        dispatch_missing = entries[i].address;
        CHECK(!xdk_thunk_set_stream_virtual_handler(typed_handler));
        dispatch_missing = 0u; dispatch_wrong = entries[i].address;
        CHECK(!xdk_thunk_set_stream_virtual_handler(typed_handler));
        dispatch_wrong = 0u;
    }
    CHECK(xdk_thunk_init(entries, 40u));
    CHECK(!xdk_thunk_set_stream_virtual_handler(typed_handler));
    CHECK(xdk_thunk_init(entries, 41u));
}
static void *worker(void *unused)
{
    (void)unused;
    const uint32_t words[2] = {CALLER, 0x12345678u};
    (void)kernel_guest_write_bytes(stack + 64u, words, sizeof(words));
    g_esp = stack + 64u; g_eax = 0x11u;
    route(false);
    return NULL;
}
static void *pending_worker(void *unused)
{
    (void)unused;
    (void)recomp_lookup_manual(ADDRESS);
    pthread_mutex_lock(&gate);
    entered = true;
    pthread_cond_broadcast(&condition);
    while (!release_handler) (void)pthread_cond_wait(&condition, &gate);
    pthread_mutex_unlock(&gate);
    xdk_thunk_stream_virtual_cancel_pending();
    return NULL;
}
static void pending_is_thread_local(void)
{
    entered = false; release_handler = false;
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, pending_worker, NULL) == 0);
    pthread_mutex_lock(&gate);
    while (!entered) (void)pthread_cond_wait(&condition, &gate);
    pthread_mutex_unlock(&gate);
    CHECK(xdk_thunk_stream_virtual_pending_count() == 1u);
    xdk_thunk_stream_virtual_cancel_pending();
    CHECK(xdk_thunk_stream_virtual_pending_count() == 1u);
    CHECK(!xdk_thunk_set_stream_virtual_handler(NULL));
    pthread_mutex_lock(&gate); release_handler = true;
    pthread_cond_broadcast(&condition); pthread_mutex_unlock(&gate);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(xdk_thunk_stream_virtual_pending_count() == 0u);
    CHECK(xdk_thunk_set_stream_virtual_handler(NULL));
    CHECK(xdk_thunk_set_stream_virtual_handler(typed_handler));
}
static void concurrent(void)
{
    behavior = 3u; entered = false; release_handler = false;
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, worker, NULL) == 0);
    pthread_mutex_lock(&gate);
    while (!entered) (void)pthread_cond_wait(&condition, &gate);
    pthread_mutex_unlock(&gate);
    CHECK(xdk_thunk_stream_virtual_active_count() == 1u);
    CHECK(!xdk_thunk_set_stream_virtual_handler(NULL));
    CHECK(!xdk_thunk_set_stream_virtual_handler(typed_handler));
    /* A foreign thread cannot cancel the worker's active reservation. */
    xdk_thunk_stream_virtual_cancel_pending();
    CHECK(xdk_thunk_stream_virtual_active_count() == 1u);
    pthread_mutex_lock(&gate); release_handler = true;
    pthread_cond_broadcast(&condition); pthread_mutex_unlock(&gate);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(xdk_thunk_stream_virtual_active_count() == 0u);
    behavior = 0u;
}
int main(void)
{
    dsound_hle_init();
    size_t count;
    const dsound_entry *sound = dsound_hle_table(&count);
    xdk_dispatch_entry entries[41];
    CHECK(count == 41u);
    for (unsigned i = 0u; i < 41u; i++)
        entries[i] = (xdk_dispatch_entry){sound[i].address, sound[i].name, XDK_MODULE_DSOUND};
    CHECK(!xdk_thunk_set_stream_virtual_handler(typed_handler));
    CHECK(xdk_thunk_init(entries, 41u));
    guest_region_request request = {0};
    request.bytes = 4096u; request.state = MEM_COMMIT; request.protect = PAGE_READWRITE;
    nt_status status = STATUS_SUCCESS;
    stack = guest_region_alloc(&request, &status);
    CHECK(stack != 0u);
    guard = mmap(NULL, 4096u, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK((const void *)guard != MAP_FAILED);
    prepare(stack, CALLER, 0x12345678u);
    route(false);
    CHECK(!returned && stopped.reason == HOST_STOP_XDK_UNIMPLEMENTED);
    preserved(stack, false);
#ifdef STREAM_VIRTUAL_NO_MARKERS
    CHECK(!xdk_thunk_set_stream_virtual_handler(typed_handler));
    if (marker_missing == 0u) goto cleanup;
#endif
    readiness(entries);
    CHECK(xdk_thunk_set_stream_virtual_handler(typed_handler));
    prepare(stack, CALLER, 0x12345678u);
    recomp_func_t reserved = recomp_lookup_manual(ADDRESS);
    CHECK(reserved != NULL && xdk_thunk_stream_virtual_pending_count() == 1u);
    CHECK(!xdk_thunk_set_stream_virtual_handler(NULL));
    CHECK(!xdk_thunk_init(entries, 41u));
    xdk_thunk_stream_virtual_cancel_pending();
    xdk_thunk_stream_virtual_cancel_pending();
    CHECK(xdk_thunk_stream_virtual_pending_count() == 0u);
    CHECK(xdk_thunk_set_stream_virtual_handler(NULL));
    CHECK(xdk_thunk_set_stream_virtual_handler(typed_handler));
    const xdk_stop_override explicit_stop = {ADDRESS, "explicit stop", "explicit priority"};
    CHECK(xdk_thunk_stop_override_adopt(&explicit_stop, 1u));
    route(false);
    CHECK(!returned && strcmp(stopped.detail, "explicit priority") == 0);
    CHECK(xdk_thunk_stream_virtual_call_count() == 0u);
    xdk_thunk_stop_override_reset();
    for (unsigned policy = 0u; policy < 2u; policy++) {
        xdk_thunk_set_stop_on_missing(policy != 0u);
        prepare(stack, CALLER, 0x12345678u); thunk_trace_reset();
        behavior = 0u; route(false);
        CHECK(returned && configuration_refused); preserved(stack, true);
        size_t length;
        const thunk_trace_entry *trace = thunk_trace_entries(&length);
        CHECK(length == 1u && trace[0].implemented && trace[0].result_known);
        CHECK(trace[0].address == ADDRESS && trace[0].return_address == CALLER && trace[0].result == 0u);
        CHECK(xdk_thunk_call_count() == 0u && xdk_thunk_module_call_count(XDK_MODULE_DSOUND) == 0u);
        prepare(stack, CALLER, 0x12345678u); route(true);
        CHECK(!returned && strcmp(stopped.detail, "compiled direct stop") == 0);
        preserved(stack, false);
    }
    for (unsigned mode = 1u; mode <= 5u; mode++) {
        if (mode == 3u) continue;
        prepare(stack, CALLER, 0x12345678u); thunk_trace_reset();
        behavior = mode; depth = 0u; route(false);
        CHECK(returned == (mode == 5u)); preserved(stack, mode == 5u);
        size_t length;
        const thunk_trace_entry *trace = thunk_trace_entries(&length);
        CHECK(length == (mode >= 4u ? 2u : 1u));
        CHECK(trace[0].implemented && trace[0].result_known == (mode == 5u));
        if (mode == 2u) CHECK(stopped.reason == HOST_STOP_FAULT && stopped.signal_number == SIGSEGV && stopped.fault_address == (uintptr_t)guard);
        else if (mode != 5u) CHECK(strcmp(stopped.detail, "typed refusal") == 0);
        CHECK(xdk_thunk_set_stream_virtual_handler(typed_handler));
    }
    behavior = 0u;
    prepare(stack, CALLER + 4u, 0x12345678u); route(false);
    CHECK(!returned && g_eax == 0xABCD0011u && g_esp == stack);
    /* T602: sub_000299C0 calls the method too (return 0x29A5A), its neighbours are not callers. */
    prepare(stack, 0x29A5Au, 0x12345678u); route(false);
    CHECK(returned && g_eax == 0u && g_esp == stack + 8u);
    CHECK(g_ecx == 0xABCD0022u && g_edx == 0xABCD0033u && g_ebx == 0xABCD0044u && g_esi == 0xABCD0055u);
    const uint32_t neighbours[] = {0x29A57u, 0x29A59u, 0x29A5Bu, 0x29A51u};
    for (unsigned i = 0u; i < 4u; i++) {
        prepare(stack, neighbours[i], 0x12345678u); route(false);
        CHECK(!returned && g_eax == 0xABCD0011u && g_esp == stack);
    }
    prepare(stack, CALLER, 0xDEADBEEFu); route(false);
    CHECK(!returned && strcmp(stopped.detail, "not owned") == 0 && g_esp == stack);
    prepare(stack + 4088u, CALLER, 0x12345678u); route(false);
    CHECK(returned); preserved(stack + 4088u, true);
    prepare(stack + 4088u, CALLER, 0x12345678u); g_esp = stack + 4092u;
    route(false); CHECK(!returned && g_esp == stack + 4092u && g_eax == 0xABCD0011u);
    concurrent();
    pending_is_thread_local();
    CHECK(xdk_thunk_stream_virtual_refused_count() >= 6u);
#ifdef STREAM_VIRTUAL_NO_MARKERS
cleanup:
#endif
    CHECK(xdk_thunk_set_stream_virtual_handler(NULL));
    CHECK(guest_region_free(stack));
    CHECK(munmap((void *)guard, 4096u) == 0);
    xdk_thunk_shutdown();
    printf("stream virtual: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
