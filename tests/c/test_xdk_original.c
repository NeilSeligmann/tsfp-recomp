/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "xdk_original.h"
#include "xdk_thunk.h"
#include "host_runtime.h"
#include "thunk_trace.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
TSFP_RECOMP_TLS uint32_t g_ebx, g_esi, g_edi, g_ebp, g_fs_base;
static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)
static const uint32_t addresses[] = {
    0x3E6714u, 0x3EA301u, 0x3EE2B3u, 0x3F1791u, 0x3F42A0u,
    0x3FC8A8u, 0x402BCFu, 0x402C5Cu, 0x3EFEA7u, 0x3F22B4u, 0x3F36F1u,
    0x400D98u, 0x3F17AEu, 0x3F1786u, 0x3F17D4u,
};
static _Thread_local bool returned;
static _Thread_local host_stop stopped;
static void route(uint32_t address, bool lookup)
{
    returned = false;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        if (lookup) {
            recomp_func_t function = recomp_lookup_manual(address);
            if (function) { function(); }
        } else {
            xdk_thunk_dispatch_at(address);
        }
        returned = true;
    }
    stopped = *host_run_result();
    host_run_disarm();
}
#ifndef XDK_ORIGINAL_NO_CAPABILITY
static uint32_t version = 1u;
static const char *identity = "shader-assembler-v1";
static unsigned missing_index = 99u;
static atomic_uint calls;
static unsigned behavior;
static volatile unsigned char *guard;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool body_entered, body_release;
static _Thread_local bool reentrant_refused;
static atomic_uint inside_count, max_inside;
static void overlap_wait(unsigned index)
{
    unsigned now = atomic_fetch_add(&inside_count, 1u) + 1u;
    unsigned seen = atomic_load(&max_inside);
    while (now > seen && !atomic_compare_exchange_weak(&max_inside, &seen, now)) { }
    /* The lock free entry waits long for a peer and must find one. A serialized entry
     * waits briefly and must not. */
    const unsigned wait_ms = index == 0u ? 2000u : 100u;
    for (unsigned waited = 0u; waited < wait_ms && atomic_load(&inside_count) < 2u; waited++) {
        usleep(1000u);
    }
    (void)atomic_fetch_sub(&inside_count, 1u);
}
static void body(unsigned index)
{
    (void)atomic_fetch_add(&calls, 1u);
    g_eax = 0xCAFE0000u + index;
    g_ecx = 0xAA550000u + index;
    g_esp += 12u;
    reentrant_refused = !xdk_original_configure(false);
    if (behavior == 6u) { overlap_wait(index); return; }
    if (index != 2u) { return; }
    if (behavior == 1u) {
        xdk_thunk_dispatch_at(addresses[0]);
        g_eax += 100u;
    } else if (behavior == 2u) {
        host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, 0x3E9546u, 0u, "original nested boundary");
    } else if (behavior == 3u) {
        *guard = 1u;
    } else if (behavior == 4u) {
        xdk_thunk_dispatch_at(addresses[2]);
    } else if (behavior == 5u) {
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
static void body7(void) { body(7u); }
static void body8(void) { body(8u); }
static void body9(void) { body(9u); }
static void body10(void) { body(10u); }
static void body11(void) { body(11u); }
static void body12(void) { body(12u); }
static void body13(void) { body(13u); }
static void body14(void) { body(14u); }
uint32_t recomp_original_profile_version(void) { return version; }
const char *recomp_original_profile_identity(void) { return identity; }
recomp_func_t recomp_lookup_original(uint32_t address)
{
    static const recomp_func_t functions[] = {
        body0, body1, body2, body3, body4, body5, body6, body7, body8, body9, body10, body11, body12, body13, body14,
    };
    for (unsigned i = 0u; i < 15u; i++) {
        if (addresses[i] == address) { return missing_index == i ? NULL : functions[i]; }
    }
    return NULL;
}
static void test_readiness_is_pure(void)
{
    const uint64_t calls_before = xdk_thunk_call_count();
    const uint64_t trace_before = thunk_trace_total();
    const unsigned originals_before = atomic_load(&calls);
    g_eax = 0x1234u; g_ecx = 0x5678u; g_edx = 0x9ABCu; g_esp = 0xDE00u;
    CHECK(xdk_original_ready());
    CHECK(g_eax == 0x1234u && g_ecx == 0x5678u && g_edx == 0x9ABCu && g_esp == 0xDE00u);
    CHECK(xdk_thunk_call_count() == calls_before && thunk_trace_total() == trace_before);
    CHECK(atomic_load(&calls) == originals_before);
    CHECK(!xdk_original_dispatch(addresses[0]));
}
static void test_capability(void)
{
    CHECK(xdk_original_ready());
    CHECK(xdk_original_configure(true));
    for (unsigned i = 0u; i < 5u; i++) {
        missing_index = i;
        CHECK(!xdk_original_ready());
        CHECK(!xdk_original_configure(true));
        route(addresses[i], false);
        CHECK(returned); /* Failed reconfiguration retained ALL old pointers. */
    }
    missing_index = 99u;
    const uint32_t bad_versions[] = {0u, 7u, UINT32_MAX};
    for (unsigned i = 0u; i < 3u; i++) {
        version = bad_versions[i];
        CHECK(!xdk_original_ready());
        CHECK(!xdk_original_configure(true));
    }
    version = 1u;
    const char *bad_ids[] = {NULL, "", "shader-assembler-v2", "shader-assembler-v1-extra"};
    for (unsigned i = 0u; i < 4u; i++) {
        identity = bad_ids[i];
        CHECK(!xdk_original_ready());
        CHECK(!xdk_original_configure(true));
    }
    identity = "shader-assembler-v1";
    CHECK(xdk_original_configure(false));
    CHECK(!xdk_original_dispatch(addresses[0]));
}
static void test_version2_exact_activation(void)
{
    CHECK(xdk_original_configure(true)); /* v1 remains exactly five. */
    xdk_thunk_set_stop_on_missing(true);
    for (unsigned i = 5u; i < 8u; i++) {
        CHECK(!xdk_original_dispatch(addresses[i]));
        route(addresses[i], true);
        CHECK(!returned && stopped.reason == HOST_STOP_XDK_UNIMPLEMENTED);
    }
    version = 2u; identity = "shader-assembler-v2";
    for (unsigned i = 5u; i < 8u; i++) {
        missing_index = i;
        CHECK(!xdk_original_ready());
        CHECK(!xdk_original_configure(true));
        CHECK(!xdk_original_dispatch(addresses[i])); /* Old v1 still intact. */
        route(addresses[0], false);
        CHECK(returned);
    }
    missing_index = 99u;
    CHECK(xdk_original_ready());
    CHECK(xdk_original_configure(true));
    for (unsigned i = 0u; i < 8u; i++) {
        for (unsigned lookup = 0u; lookup < 2u; lookup++) {
            g_esp = 0xF1234000u;
            route(addresses[i], lookup != 0u);
            CHECK(returned && reentrant_refused);
            CHECK(g_eax == 0xCAFE0000u + i && g_esp == 0xF123400Cu);
        }
    }
    /* An incomplete v2 replacement preserves its previous eight-entry cache. */
    missing_index = 7u;
    CHECK(!xdk_original_configure(true));
    route(addresses[7], false);
    CHECK(returned);
    missing_index = 99u;
    const uint32_t bad_versions[] = {0u, 7u, UINT32_MAX};
    for (unsigned i = 0u; i < 3u; i++) {
        version = bad_versions[i];
        CHECK(!xdk_original_ready());
        CHECK(!xdk_original_configure(true));
    }
    version = 2u; identity = "shader-assembler-v1";
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    version = 1u; identity = "shader-assembler-v2";
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    identity = "shader-assembler-v1";
    CHECK(xdk_original_configure(true));
    for (unsigned i = 5u; i < 8u; i++) { CHECK(!xdk_original_dispatch(addresses[i])); }
    CHECK(xdk_original_configure(false));
}

static void test_version3_exact_activation(void)
{
    CHECK(xdk_original_configure(true)); /* v2 still exposes exactly eight. */
    CHECK(!xdk_original_dispatch(addresses[8]));
    version = 3u; identity = "shader-assembler-v3";
    missing_index = 8u;
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    missing_index = 99u;
    CHECK(xdk_original_ready());
    CHECK(xdk_original_configure(true));
    for (unsigned lookup = 0u; lookup < 2u; lookup++) {
        g_esp = 0xF1234000u;
        route(addresses[8], lookup != 0u);
        CHECK(returned && g_eax == 0xCAFE0008u && g_esp == 0xF123400Cu);
    }
    missing_index = 8u;
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    route(addresses[8], false);
    CHECK(returned); /* Failed reconfiguration preserves the prior v3 snapshot. */
    missing_index = 99u;
    version = 2u; identity = "shader-assembler-v2";
    CHECK(xdk_original_configure(true));
    CHECK(!xdk_original_dispatch(addresses[8]));
    version = 3u; identity = "shader-assembler-v2";
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    version = 3u; identity = "shader-assembler-v3";
    CHECK(xdk_original_configure(false));
}

static void test_version4_exact_activation(void)
{
    version = 3u; identity = "shader-assembler-v3";
    CHECK(xdk_original_configure(true));
    CHECK(!xdk_original_dispatch(addresses[9]));
    version = 4u; identity = "shader-assembler-v4";
    missing_index = 9u;
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    missing_index = 99u;
    CHECK(xdk_original_configure(true));
    for (unsigned lookup = 0u; lookup < 2u; lookup++) {
        g_esp = 0xF1234000u;
        route(addresses[9], lookup != 0u);
        CHECK(returned && g_eax == 0xCAFE0009u && g_esp == 0xF123400Cu);
    }
    missing_index = 9u;
    CHECK(!xdk_original_configure(true));
    route(addresses[9], false);
    CHECK(returned); /* failed replacement retains the prior immutable v4 snapshot */
    missing_index = 99u;
    version = 4u; identity = "shader-assembler-v3";
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    version = 3u; identity = "shader-assembler-v3";
    CHECK(xdk_original_configure(true));
    CHECK(!xdk_original_dispatch(addresses[9]));
    CHECK(xdk_original_configure(false));
}

static void test_version5_exact_activation(void)
{
    version = 4u; identity = "shader-assembler-v4";
    CHECK(xdk_original_configure(true));
    CHECK(!xdk_original_dispatch(addresses[10]));
    version = 5u; identity = "shader-assembler-v5";
    missing_index = 10u;
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    missing_index = 99u;
    CHECK(xdk_original_configure(true));
    for (unsigned lookup = 0u; lookup < 2u; lookup++) {
        g_esp = 0xF1234000u;
        route(addresses[10], lookup != 0u);
        CHECK(returned && g_eax == 0xCAFE000Au && g_esp == 0xF123400Cu);
    }
    missing_index = 10u;
    CHECK(!xdk_original_configure(true));
    route(addresses[10], false);
    CHECK(returned); /* failed replacement retains the prior immutable v5 snapshot */
    missing_index = 99u;
    version = 5u; identity = "shader-assembler-v4";
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    version = 4u; identity = "shader-assembler-v4";
    CHECK(xdk_original_configure(true));
    CHECK(!xdk_original_dispatch(addresses[10]));
    CHECK(xdk_original_configure(false));
}

static void test_version6_exact_activation(void)
{
    version = 5u; identity = "shader-assembler-v5";
    CHECK(xdk_original_configure(true));
    CHECK(!xdk_original_dispatch(addresses[11]));
    version = 6u; identity = "shader-assembler-v6";
    missing_index = 11u;
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    missing_index = 99u;
    CHECK(xdk_original_configure(true));
    for (unsigned lookup = 0u; lookup < 2u; lookup++) {
        g_esp = 0xF1234000u;
        route(addresses[11], lookup != 0u);
        CHECK(returned && g_eax == 0xCAFE000Bu && g_esp == 0xF123400Cu);
    }
    missing_index = 11u;
    CHECK(!xdk_original_configure(true));
    route(addresses[11], false);
    CHECK(returned); /* failed replacement retains the prior immutable v6 snapshot */
    missing_index = 99u;
    version = 6u; identity = "shader-assembler-v5";
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    version = 5u; identity = "shader-assembler-v5";
    CHECK(xdk_original_configure(true));
    CHECK(!xdk_original_dispatch(addresses[11]));
    CHECK(xdk_original_configure(false));
}

static void test_version7_exact_activation(void)
{
    version = 6u; identity = "shader-assembler-v6";
    CHECK(xdk_original_configure(true));
    CHECK(!xdk_original_dispatch(addresses[12]));
    version = 7u; identity = "shader-assembler-v7";
    missing_index = 12u;
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    missing_index = 99u;
    CHECK(xdk_original_configure(true));
    for (unsigned lookup = 0u; lookup < 2u; lookup++) {
        g_esp = 0xF1234000u;
        route(addresses[12], lookup != 0u);
        CHECK(returned && g_eax == 0xCAFE000Cu && g_esp == 0xF123400Cu);
    }
    missing_index = 12u;
    CHECK(!xdk_original_configure(true));
    route(addresses[12], false);
    CHECK(returned); /* failed replacement retains the prior immutable v7 snapshot */
    missing_index = 99u;
    version = 7u; identity = "shader-assembler-v6";
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    version = 6u; identity = "shader-assembler-v6";
    CHECK(xdk_original_configure(true));
    CHECK(!xdk_original_dispatch(addresses[12]));
    CHECK(xdk_original_configure(false));
}

static void test_version8_exact_activation(void)
{
    version = 7u; identity = "shader-assembler-v7";
    CHECK(xdk_original_configure(true));
    CHECK(!xdk_original_dispatch(addresses[13]));
    CHECK(!xdk_original_dispatch(addresses[14]));
    version = 8u; identity = "shader-assembler-v8";
    for (unsigned selected = 13u; selected <= 14u; selected++) {
        missing_index = selected;
        CHECK(!xdk_original_ready());
        CHECK(!xdk_original_configure(true));
    }
    missing_index = 99u;
    CHECK(xdk_original_configure(true));
    for (unsigned selected = 13u; selected <= 14u; selected++) {
        for (unsigned lookup = 0u; lookup < 2u; lookup++) {
            g_esp = 0xF1234000u;
            route(addresses[selected], lookup != 0u);
            CHECK(returned && g_eax == 0xCAFE0000u + selected && g_esp == 0xF123400Cu);
        }
        missing_index = selected;
        CHECK(!xdk_original_configure(true));
        route(addresses[selected], false);
        CHECK(returned); /* failed replacement keeps the full immutable v8 snapshot */
        missing_index = 99u;
    }
    version = 8u; identity = "shader-assembler-v7";
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    version = 7u; identity = "shader-assembler-v7";
    CHECK(xdk_original_configure(true));
    CHECK(!xdk_original_dispatch(addresses[13]));
    CHECK(!xdk_original_dispatch(addresses[14]));
    CHECK(xdk_original_configure(false));
}

static void test_routes_and_state(void)
{
    CHECK(xdk_original_configure(false));
    for (unsigned i = 0u; i < 5u; i++) {
        for (unsigned lookup = 0u; lookup < 2u; lookup++) {
            g_eax = 0xABCDu; g_esp = 0xF1234000u;
            route(addresses[i], lookup != 0u);
            CHECK(!returned && stopped.reason == HOST_STOP_XDK_UNIMPLEMENTED);
            CHECK(g_eax == 0xABCDu && g_esp == 0xF1234000u);
        }
    }
    CHECK(xdk_original_configure(true));
    const uint64_t before = xdk_thunk_call_count();
    const uint64_t trace_before = thunk_trace_total();
    for (unsigned i = 0u; i < 5u; i++) {
        for (unsigned lookup = 0u; lookup < 2u; lookup++) {
            g_eax = 0u; g_ecx = 0u; g_esp = 0xF1234000u;
            g_edx = 0xA1A2u; g_ebx = 0xB1B2u; g_esi = 0xC1C2u;
            g_edi = 0xD1D2u; g_ebp = 0xE1E2u; g_fs_base = 0x1234u;
            route(addresses[i], lookup != 0u);
            CHECK(returned && reentrant_refused);
            CHECK(g_eax == 0xCAFE0000u + i && g_ecx == 0xAA550000u + i);
            CHECK(g_esp == 0xF123400Cu && g_edx == 0xA1A2u && g_ebx == 0xB1B2u);
            CHECK(g_esi == 0xC1C2u && g_edi == 0xD1D2u && g_ebp == 0xE1E2u && g_fs_base == 0x1234u);
            CHECK(host_run_scope_depth() == 0u);
        }
    }
    CHECK(xdk_thunk_call_count() == before);
    CHECK(thunk_trace_total() == trace_before);
    /* Real adopted DSOUND and an independent lifetime stop cannot enter profile. */
    xdk_thunk_set_stop_on_missing(false);
    route(0x40967Cu, false);
    CHECK(!returned && stopped.reason == HOST_STOP_XDK_UNIMPLEMENTED);
    CHECK(!xdk_original_dispatch(0x40723Fu));
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        xdk_thunk_stop_at(0x40723Fu, "stream lifetime", "stop-only independent policy");
    }
    CHECK(host_run_result()->guest_address == 0x40723Fu);
    host_run_disarm();
    CHECK(!xdk_original_dispatch(0u));
    CHECK(!xdk_original_dispatch(0xFE000001u));
    CHECK(!xdk_original_dispatch(0x3E671Eu));
    behavior = 1u;
    g_esp = 0xF1234000u;
    route(addresses[2], true);
    CHECK(returned && g_eax == 0xCAFE0064u && g_esp == 0xF1234018u);
    behavior = 0u;
}
static void test_stop_fault_and_depth(void)
{
    for (unsigned selected = 2u; selected <= 4u; selected++) {
        behavior = selected;
        g_esp = 0xF1234000u;
        route(addresses[2], false);
        CHECK(!returned);
        CHECK(host_run_scope_depth() == 0u);
        CHECK(g_eax == 0xCAFE0002u); /* No state restoration on failure. */
        if (selected == 2u) {
            CHECK(stopped.reason == HOST_STOP_XDK_UNIMPLEMENTED && stopped.guest_address == 0x3E9546u);
            CHECK(stopped.ordinal == 0u && strcmp(stopped.detail, "original nested boundary") == 0);
        } else if (selected == 3u) {
            CHECK(stopped.reason == HOST_STOP_FAULT && stopped.signal_number == SIGSEGV);
            CHECK(stopped.fault_address == (uintptr_t)guard);
        } else {
            CHECK(stopped.reason == HOST_STOP_UNIMPLEMENTED);
            CHECK(strcmp(stopped.detail, "original XDK stop scope unavailable") == 0);
        }
        CHECK(xdk_original_configure(false));
        CHECK(xdk_original_configure(true));
        behavior = 0u;
        route(addresses[0], false);
        CHECK(returned);
    }
}
static bool worker_ok;
static void *blocked_worker(void *context)
{
    (void)context;
    route(addresses[2], false);
    worker_ok = returned && reentrant_refused && host_run_scope_depth() == 0u;
    return NULL;
}
static void test_unarmed_execution_refuses(void)
{
    CHECK(xdk_original_configure(true));
    const pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        (void)xdk_original_dispatch(addresses[0]);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
static void test_concurrent_configuration(void)
{
    behavior = 5u; body_entered = false; body_release = false;
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, blocked_worker, NULL) == 0);
    pthread_mutex_lock(&gate_lock);
    while (!body_entered) { (void)pthread_cond_wait(&gate_cond, &gate_lock); }
    pthread_mutex_unlock(&gate_lock);
    CHECK(xdk_original_ready());
    CHECK(!xdk_original_configure(false));
    CHECK(!xdk_original_configure(true));
    route(addresses[0], true); /* Another guest thread may execute an original. */
    CHECK(returned && reentrant_refused);
    pthread_mutex_lock(&gate_lock);
    body_release = true;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_lock);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(worker_ok);
    CHECK(xdk_original_configure(false));
    behavior = 0u;
}
static pthread_barrier_t start_barrier;
static unsigned overlap_target;
static atomic_bool peer_done, peer_returned;
static void *overlap_worker(void *context)
{
    (void)context;
    (void)pthread_barrier_wait(&start_barrier);
    route(overlap_target, false);
    atomic_store(&peer_returned, returned);
    atomic_store(&peer_done, true);
    return NULL;
}
/* Two guest threads enter the same original at the same instant. */
static unsigned overlap_of(unsigned index)
{
    behavior = 6u;
    atomic_store(&max_inside, 0u);
    atomic_store(&inside_count, 0u);
    atomic_store(&peer_done, false);
    atomic_store(&peer_returned, false);
    overlap_target = addresses[index];
    CHECK(pthread_barrier_init(&start_barrier, NULL, 2u) == 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, overlap_worker, NULL) == 0);
    (void)pthread_barrier_wait(&start_barrier);
    route(overlap_target, false);
    CHECK(returned);
    for (unsigned waited = 0u; waited < 5000u && !atomic_load(&peer_done); waited++) {
        usleep(1000u);
    }
    CHECK(atomic_load(&peer_done) && atomic_load(&peer_returned));
    if (atomic_load(&peer_done)) { CHECK(pthread_join(thread, NULL) == 0); }
    else { (void)pthread_detach(thread); }
    CHECK(pthread_barrier_destroy(&start_barrier) == 0);
    behavior = 0u;
    return atomic_load(&max_inside);
}
static void test_serialized_entries(unsigned count)
{
    CHECK(count == 5u || count == 8u || count == 9u || count == 10u || count == 11u || count == 12u || count == 13u || count == 15u);
    CHECK(xdk_original_configure(true));
    /* Only the read-only getter may run in two threads at once. Every other entry
     * mutates or may mutate the compiler's static data, so a second thread waits. */
    CHECK(overlap_of(0u) == 2u);
    for (unsigned i = 1u; i < count; i++) {
        CHECK(overlap_of(i) == 1u);
    }
}
/* A stop or fault inside a serialized original must not leave the lock held for others. */
static void test_serial_lock_released_after_failure(void)
{
    CHECK(xdk_original_configure(true));
    for (unsigned selected = 2u; selected <= 3u; selected++) {
        behavior = selected;
        g_esp = 0xF1234000u;
        route(addresses[2], false);
        CHECK(!returned);
        behavior = 0u;
        atomic_store(&peer_done, false);
        atomic_store(&peer_returned, false);
        overlap_target = addresses[2];
        CHECK(pthread_barrier_init(&start_barrier, NULL, 2u) == 0);
        pthread_t thread;
        CHECK(pthread_create(&thread, NULL, overlap_worker, NULL) == 0);
        (void)pthread_barrier_wait(&start_barrier);
        for (unsigned waited = 0u; waited < 2000u && !atomic_load(&peer_done); waited++) {
            usleep(1000u);
        }
        CHECK(atomic_load(&peer_done) && atomic_load(&peer_returned));
        if (atomic_load(&peer_done)) { CHECK(pthread_join(thread, NULL) == 0); }
        else { (void)pthread_detach(thread); }
        CHECK(pthread_barrier_destroy(&start_barrier) == 0);
        CHECK(xdk_original_configure(false));
        CHECK(xdk_original_configure(true));
    }
}
#endif
int main(void)
{
    xdk_dispatch_entry entries[16];
    for (unsigned i = 0u; i < 15u; i++) {
        entries[i] = (xdk_dispatch_entry){addresses[i], NULL, XDK_MODULE_XGRPH};
    }
    entries[15] = (xdk_dispatch_entry){0x40967Cu, "DirectSoundCreateStream", XDK_MODULE_DSOUND};
    CHECK(xdk_thunk_init(entries, 16u));
    CHECK(!xdk_original_dispatch(addresses[0]));
#ifdef XDK_ORIGINAL_NO_CAPABILITY
    CHECK(!xdk_original_ready());
    CHECK(!xdk_original_configure(true));
    CHECK(xdk_original_configure(false));
    route(addresses[0], false);
    CHECK(!returned && stopped.reason == HOST_STOP_XDK_UNIMPLEMENTED);
#else
    guard = mmap(NULL, 4096u, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK((const void *)guard != MAP_FAILED);
    test_readiness_is_pure();
    test_capability();
    test_version2_exact_activation();
    test_version3_exact_activation();
    test_version4_exact_activation();
    test_version5_exact_activation();
    test_version6_exact_activation();
    test_version7_exact_activation();
    test_version8_exact_activation();
    test_routes_and_state();
    test_stop_fault_and_depth();
    test_unarmed_execution_refuses();
    test_concurrent_configuration();
    test_serialized_entries(5u);
    test_serial_lock_released_after_failure();
    version = 8u;
    identity = "shader-assembler-v8";
    CHECK(xdk_original_configure(true));
    test_stop_fault_and_depth();
    test_unarmed_execution_refuses();
    test_concurrent_configuration();
    test_serialized_entries(15u);
    test_serial_lock_released_after_failure();
    version = 7u;
    identity = "shader-assembler-v7";
    CHECK(xdk_original_configure(true));
    test_stop_fault_and_depth();
    test_unarmed_execution_refuses();
    test_concurrent_configuration();
    test_serialized_entries(13u);
    test_serial_lock_released_after_failure();
    version = 6u;
    identity = "shader-assembler-v6";
    CHECK(xdk_original_configure(true));
    test_stop_fault_and_depth();
    test_unarmed_execution_refuses();
    test_concurrent_configuration();
    test_serialized_entries(12u);
    test_serial_lock_released_after_failure();
    version = 5u;
    identity = "shader-assembler-v5";
    CHECK(xdk_original_configure(true));
    test_stop_fault_and_depth();
    test_unarmed_execution_refuses();
    test_concurrent_configuration();
    test_serialized_entries(11u);
    test_serial_lock_released_after_failure();
    version = 4u;
    identity = "shader-assembler-v4";
    CHECK(xdk_original_configure(true));
    test_stop_fault_and_depth();
    test_unarmed_execution_refuses();
    test_concurrent_configuration();
    test_serialized_entries(10u);
    test_serial_lock_released_after_failure();
    version = 3u;
    identity = "shader-assembler-v3";
    CHECK(xdk_original_configure(true));
    test_stop_fault_and_depth();
    test_unarmed_execution_refuses();
    test_concurrent_configuration();
    test_serialized_entries(9u);
    test_serial_lock_released_after_failure();
    version = 2u;
    identity = "shader-assembler-v2";
    CHECK(xdk_original_configure(true));
    test_stop_fault_and_depth();
    test_unarmed_execution_refuses();
    test_concurrent_configuration();
    test_serialized_entries(8u);
    test_serial_lock_released_after_failure();
    CHECK(munmap((void *)guard, 4096u) == 0);
#endif
    CHECK(xdk_original_configure(false));
    xdk_thunk_shutdown();
    printf("original XDK: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
