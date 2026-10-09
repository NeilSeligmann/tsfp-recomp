/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
/* T92/T234: the opt-in retained XMV exports. Fixture bodies stand in for the generated
 * chunk. They own registers and stack like an original and never touch disc data. */
#include "xmv_original.h"
#include "xdk_original.h"
#include "xdk_thunk.h"
#include "host_runtime.h"
#include "thunk_trace.h"
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
TSFP_RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp, g_fs_base;
static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)
static const uint32_t addresses[XMV_ORIGINAL_EXPORT_COUNT] = {
    0x444A2Du, 0x444F71u, 0x445055u, 0x4450C2u, 0x445241u, 0x445252u, 0x44525Du,
};
static _Thread_local bool returned;
static _Thread_local host_stop stopped;
static void route(uint32_t address)
{
    returned = false;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        xdk_thunk_dispatch_at(address);
        returned = true;
    }
    stopped = *host_run_result();
    host_run_disarm();
}
#ifndef XMV_ORIGINAL_NO_CAPABILITY
static const char *identity = "xmv-original-v1";
static unsigned missing_index = 99u;
/* Bytes the original pops above its return address (stdcall RET n), index order. */
static const uint32_t pops[XMV_ORIGINAL_EXPORT_COUNT] = {12u, 4u, 8u, 20u, 8u, 4u, 16u};
static atomic_uint calls;
static unsigned behavior;
static volatile unsigned char *guard;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool body_entered, body_release;
static _Thread_local bool reentrant_refused;
static void body(unsigned index)
{
    (void)atomic_fetch_add(&calls, 1u);
    g_eax = 0xBEEF0000u + index;
    g_ecx = 0xAA550000u + index;
    g_esp += 4u + pops[index];
    reentrant_refused = !xmv_original_configure(false);
    if (index != 6u) { return; }
    if (behavior == 1u) {
        host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, 0x40967Cu, 0u, "original nested boundary");
    } else if (behavior == 2u) {
        *guard = 1u;
    } else if (behavior == 3u) {
        pthread_mutex_lock(&gate_lock);
        body_entered = true;
        pthread_cond_broadcast(&gate_cond);
        while (!body_release) { (void)pthread_cond_wait(&gate_cond, &gate_lock); }
        pthread_mutex_unlock(&gate_lock);
    }
}
static void body0(void) { body(0u); }
static void body1(void) { body(1u); }
static void body2(void) { body(2u); }
static void body3(void) { body(3u); }
static void body4(void) { body(4u); }
static void body5(void) { body(5u); }
static void body6(void) { body(6u); }
const char *recomp_xmv_profile_identity(void) { return identity; }
recomp_func_t recomp_xmv_lookup_original(uint32_t address)
{
    static const recomp_func_t functions[XMV_ORIGINAL_EXPORT_COUNT] = {
        body0, body1, body2, body3, body4, body5, body6,
    };
    for (unsigned i = 0u; i < XMV_ORIGINAL_EXPORT_COUNT; i++) {
        if (addresses[i] == address) { return missing_index == i ? NULL : functions[i]; }
    }
    return NULL;
}
static void test_table_is_the_seven_exports(void)
{
    for (unsigned i = 0u; i < XMV_ORIGINAL_EXPORT_COUNT; i++) {
        CHECK(xmv_original_address(i) == addresses[i]);
    }
    CHECK(xmv_original_address(XMV_ORIGINAL_EXPORT_COUNT) == 0u);
    CHECK(XMV_ORIGINAL_EXPORT_COUNT == 7u);
}
static void test_readiness_is_pure_and_off_by_default(void)
{
    const uint64_t calls_before = xdk_thunk_call_count();
    const uint64_t trace_before = thunk_trace_total();
    g_eax = 0x1234u; g_ecx = 0x5678u; g_esp = 0xDE00u;
    CHECK(xmv_original_ready());
    CHECK(g_eax == 0x1234u && g_ecx == 0x5678u && g_esp == 0xDE00u);
    CHECK(xdk_thunk_call_count() == calls_before && thunk_trace_total() == trace_before);
    for (unsigned i = 0u; i < XMV_ORIGINAL_EXPORT_COUNT; i++) {
        CHECK(!xmv_original_dispatch(addresses[i]));
    }
    CHECK(atomic_load(&calls) == 0u);
}
static void test_capability_fails_closed(void)
{
    CHECK(xmv_original_configure(true));
    for (unsigned i = 0u; i < XMV_ORIGINAL_EXPORT_COUNT; i++) {
        missing_index = i;
        CHECK(!xmv_original_ready());
        CHECK(!xmv_original_configure(true));
        route(addresses[i]);
        CHECK(returned); /* The failed reconfiguration kept every old pointer. */
    }
    missing_index = 99u;
    const char *bad_ids[] = {NULL, "", "xmv-original-v2", "xmv-original-v1-extra", "shader-assembler-v2"};
    for (unsigned i = 0u; i < 5u; i++) {
        identity = bad_ids[i];
        CHECK(!xmv_original_ready());
        CHECK(!xmv_original_configure(true));
    }
    identity = "xmv-original-v1";
    CHECK(xmv_original_configure(false));
    CHECK(!xmv_original_dispatch(addresses[0]));
}
static void test_disabled_is_untouched_and_enabled_owns_state(void)
{
    CHECK(xmv_original_configure(false));
    const unsigned disabled_baseline = atomic_load(&calls);
    xdk_thunk_set_stop_on_missing(true);
    for (unsigned i = 0u; i < XMV_ORIGINAL_EXPORT_COUNT; i++) {
        g_eax = 0xABCDu; g_esp = 0xF1234000u;
        route(addresses[i]);
        /* Disabled: the normal XMV row has no module, so the dispatcher stops loudly. */
        CHECK(!returned && stopped.reason == HOST_STOP_XDK_UNROUTED);
        CHECK(g_eax == 0xABCDu && g_esp == 0xF1234000u);
    }
    CHECK(atomic_load(&calls) == disabled_baseline);
    CHECK(xmv_original_configure(true));
    const unsigned enabled_baseline = atomic_load(&calls);
    const uint64_t before = xdk_thunk_call_count();
    const uint64_t trace_before = thunk_trace_total();
    for (unsigned i = 0u; i < XMV_ORIGINAL_EXPORT_COUNT; i++) {
        g_eax = 0u; g_ecx = 0u; g_esp = 0xF1234000u;
        g_edx = 0xA1A2u; g_ebx = 0xB1B2u; g_esi = 0xC1C2u;
        g_edi = 0xD1D2u; g_ebp = 0xE1E2u; g_fs_base = 0x1234u;
        const unsigned seen = atomic_load(&calls);
        route(addresses[i]);
        CHECK(returned && reentrant_refused);
        CHECK(atomic_load(&calls) == seen + 1u);
        CHECK(g_eax == 0xBEEF0000u + i && g_ecx == 0xAA550000u + i);
        CHECK(g_esp == 0xF1234000u + 4u + pops[i]);
        CHECK(g_edx == 0xA1A2u && g_ebx == 0xB1B2u && g_esi == 0xC1C2u);
        CHECK(g_edi == 0xD1D2u && g_ebp == 0xE1E2u && g_fs_base == 0x1234u);
        CHECK(host_run_scope_depth() == 0u);
    }
    CHECK(atomic_load(&calls) == enabled_baseline + XMV_ORIGINAL_EXPORT_COUNT);
    /* Retained originals bypass the HLE counters and the ordered trace. */
    CHECK(xdk_thunk_call_count() == before);
    CHECK(thunk_trace_total() == trace_before);
    /* Only the seven. DSOUND, the shader profile and arbitrary addresses are not routed. */
    const uint32_t others[] = {0u, 0x40967Cu, 0x3EE2B3u, 0x444A2Cu, 0x444A2Eu, 0x44525Eu, 0xFE000001u};
    for (unsigned i = 0u; i < 7u; i++) {
        CHECK(!xmv_original_dispatch(others[i]));
    }
}
static void test_stop_fault_and_depth(void)
{
    for (unsigned selected = 1u; selected <= 2u; selected++) {
        behavior = selected;
        g_esp = 0xF1234000u;
        route(addresses[6]);
        CHECK(!returned);
        CHECK(host_run_scope_depth() == 0u);
        CHECK(g_eax == 0xBEEF0006u); /* No state restoration on failure. */
        if (selected == 1u) {
            CHECK(stopped.reason == HOST_STOP_XDK_UNIMPLEMENTED && stopped.guest_address == 0x40967Cu);
            CHECK(strcmp(stopped.detail, "original nested boundary") == 0);
        } else {
            CHECK(stopped.reason == HOST_STOP_FAULT && stopped.signal_number == SIGSEGV);
            CHECK(stopped.fault_address == (uintptr_t)guard);
        }
        /* Bookkeeping was released, so reconfiguration works and the route is reusable. */
        CHECK(xmv_original_configure(false));
        CHECK(xmv_original_configure(true));
        behavior = 0u;
        route(addresses[0]);
        CHECK(returned);
    }
}
static void test_unarmed_execution_aborts(void)
{
    CHECK(xmv_original_configure(true));
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)xmv_original_dispatch(addresses[0]);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
static bool worker_ok;
static void *blocked_worker(void *context)
{
    (void)context;
    route(addresses[6]);
    worker_ok = returned && reentrant_refused && host_run_scope_depth() == 0u;
    return NULL;
}
static void test_concurrent_configuration_is_refused(void)
{
    behavior = 3u; body_entered = false; body_release = false;
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, blocked_worker, NULL) == 0);
    struct timespec deadline;
    (void)clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    pthread_mutex_lock(&gate_lock);
    while (!body_entered && pthread_cond_timedwait(&gate_cond, &gate_lock, &deadline) == 0) {}
    const bool entered = body_entered;
    pthread_mutex_unlock(&gate_lock);
    CHECK(entered); /* A bounded wait: a routing mistake fails here instead of hanging. */
    if (!entered) {
        (void)pthread_join(thread, NULL);
        behavior = 0u;
        return;
    }
    CHECK(xmv_original_ready());
    CHECK(!xmv_original_configure(false));
    CHECK(!xmv_original_configure(true));
    route(addresses[0]); /* Another guest thread may still execute an original. */
    CHECK(returned && reentrant_refused);
    pthread_mutex_lock(&gate_lock);
    body_release = true;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_lock);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(worker_ok);
    CHECK(xmv_original_configure(false));
    behavior = 0u;
}
#endif
/* T394: the movie substitution spec "FROM=TO" is validated before it is stored. A bad spec changes
 * nothing, NULL clears. The rewrite itself is covered by the movie boot test. */
static void test_substitute_spec_validation(void)
{
    CHECK(xmv_original_set_substitute(NULL));
    CHECK(xmv_original_set_substitute("eag_e=frd"));
    CHECK(xmv_original_set_substitute("a=b"));
    CHECK(xmv_original_set_substitute("Eag_E9=attract"));
    CHECK(xmv_original_set_substitute("abcdefghijklmnopqrstuvwxyz012345=x"));  /* 32 characters */
    CHECK(!xmv_original_set_substitute("abcdefghijklmnopqrstuvwxyz0123456=x"));  /* 33 */
    CHECK(!xmv_original_set_substitute("x=abcdefghijklmnopqrstuvwxyz0123456"));
    CHECK(!xmv_original_set_substitute(""));
    CHECK(!xmv_original_set_substitute("noequals"));
    CHECK(!xmv_original_set_substitute("=frd"));
    CHECK(!xmv_original_set_substitute("eag_e="));
    CHECK(!xmv_original_set_substitute("eag_e=frd=x"));
    CHECK(!xmv_original_set_substitute("eag_e=fr d"));
    CHECK(!xmv_original_set_substitute("d:\\xmv\\eag_e=frd"));
    CHECK(!xmv_original_set_substitute("eag.e=frd"));
    CHECK(!xmv_original_set_substitute("eag-e=frd"));
    CHECK(xmv_original_set_substitute(NULL));
}

int main(void)
{
    xdk_dispatch_entry entries[XMV_ORIGINAL_EXPORT_COUNT];
    for (unsigned i = 0u; i < XMV_ORIGINAL_EXPORT_COUNT; i++) {
        entries[i] = (xdk_dispatch_entry){addresses[i], NULL, XDK_MODULE_NONE};
    }
    CHECK(xdk_thunk_init(entries, XMV_ORIGINAL_EXPORT_COUNT));
    CHECK(!xmv_original_dispatch(addresses[0]));
    test_substitute_spec_validation();
#ifdef XMV_ORIGINAL_NO_CAPABILITY
    CHECK(!xmv_original_ready());
    CHECK(!xmv_original_configure(true));
    CHECK(xmv_original_configure(false));
    route(addresses[0]);
    CHECK(!returned && stopped.reason == HOST_STOP_XDK_UNROUTED);
    /* The shader profile is independent and equally absent here. */
    CHECK(!xdk_original_dispatch(addresses[0]));
#else
    guard = mmap(NULL, 4096u, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK((const void *)guard != MAP_FAILED);
    test_table_is_the_seven_exports();
    test_readiness_is_pure_and_off_by_default();
    test_capability_fails_closed();
    test_disabled_is_untouched_and_enabled_owns_state();
    test_stop_fault_and_depth();
    test_unarmed_execution_aborts();
    test_concurrent_configuration_is_refused();
    CHECK(munmap((void *)guard, 4096u) == 0);
#endif
    CHECK(xmv_original_configure(false));
    xdk_thunk_shutdown();
    printf("original XMV: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
