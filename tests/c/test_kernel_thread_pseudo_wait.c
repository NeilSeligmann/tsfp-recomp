/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "kernel_clock.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "kernel_sync.h"
#include "kernel_thread.h"
#include "nt_status.h"
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define WORKERS 4u
static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)
static uint32_t scratch;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static unsigned entered;
static bool release_workers;
static pthread_t worker_ids[WORKERS];
static _Thread_local unsigned current_case;
static volatile sig_atomic_t signals_seen;
typedef struct {
    uint32_t status;
    uint64_t elapsed, min_elapsed, clock_before, clock_after;
    bool frame_same, stopped, unlocked;
    unsigned sleep_calls, interrupts;
    struct timespec first_deadline;
    bool deadline_same, absolute;
    kernel_thread_wait_refusal refusal;
} result;
static result results[24];
/* Observe the real sleep without replacing its result or requested deadline. */
int __real_clock_nanosleep(clockid_t clock, int flags,
                           const struct timespec *request, struct timespec *remaining);
int __wrap_clock_nanosleep(clockid_t clock, int flags,
                           const struct timespec *request, struct timespec *remaining)
{
    result *r = &results[current_case];
    if (r->sleep_calls == 0u) {
        r->first_deadline = *request;
        r->deadline_same = true;
        r->absolute = true;
    } else if (current_case < WORKERS &&
               (r->first_deadline.tv_sec != request->tv_sec ||
                r->first_deadline.tv_nsec != request->tv_nsec)) r->deadline_same = false;
    r->absolute = r->absolute && clock == CLOCK_MONOTONIC && flags == TIMER_ABSTIME;
    r->sleep_calls++;
    const int error = __real_clock_nanosleep(clock, flags, request, remaining);
    if (error == EINTR) r->interrupts++;
    return error;
}
static uint64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0u;
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
static void signal_handler(int signal_number) { (void)signal_number; signals_seen = 1; }
static bool has_code(uint32_t address) { return address == 0x123456u; }
static void refused(uint32_t handle, kernel_thread_wait_refusal reason)
{
    (void)handle;
    results[current_case].refusal = reason;
    results[current_case].unlocked = kernel_thread_active_wait_count() == 0u;
    host_run_stop(HOST_STOP_KERNEL_UNIMPLEMENTED, 0x38004Cu, 234u, "pseudo wait refused");
}
static void enter(const kernel_thread_launch *launch)
{
    current_case = launch->start_context;
    if (current_case < WORKERS) {
        pthread_mutex_lock(&gate);
        worker_ids[current_case] = pthread_self();
        entered++;
        pthread_cond_broadcast(&condition);
        while (!release_workers) (void)pthread_cond_wait(&condition, &gate);
        pthread_mutex_unlock(&gate);
    }
    const uint32_t address = launch->stack_low + 128u;
    const uint32_t timeout_address = address + 64u;
    int64_t timeout = current_case < WORKERS && (current_case & 1u) != 0u ? -160000 : -80000;
    if (current_case == 4u) timeout = -70000;
    if (current_case == 9u) timeout = 0;
    if (current_case == 10u) timeout = 80000;
    /* T577: -160000 (the loader thread's 16 ms, MEASURED from 0x00030160) is admitted, and the
     * sleep follows the requested interval. Cases 14 and 15 repeat the call and check the
     * elapsed lower bound; scheduler delay is not a deadline-scaling failure. Neighbours of the
     * two measured intervals stay refused. */
    if (current_case == 15u) timeout = -160000;
    if (current_case == 16u) timeout = -240000;
    if (current_case == 17u) timeout = -160001;
    if (current_case == 18u) timeout = -159999;
    if (current_case == 19u) timeout = INT64_MIN;
    if (current_case == 20u) timeout = -80001;
    (void)kernel_guest_write_bytes(timeout_address, &timeout, sizeof(timeout));
    uint32_t args[4] = {0xFFFFFFFEu, 1u, 0u, timeout_address};
    if (current_case == 5u) args[2] = 1u;
    if (current_case == 6u) args[1] = 0u;
    if (current_case == 8u) args[3] = 0u;
    if (current_case == 11u) args[3] = 0xFFFFF000u;
    if (current_case == 12u) args[2] = 0x100u; /* BOOLEAN uses the low byte. */
    kernel_call_frame frame = {0};
    (void)kernel_frame_build(&frame, address, 20u, args, 4u);
    unsigned char before[72], after[72];
    (void)kernel_guest_read_bytes(address, before, sizeof(before));
    if (current_case == 7u) (void)kernel_hle_entry(129u)->handler(NULL);
    result *r = &results[current_case];
    r->clock_before = kernel_clock_peek();
    const unsigned repeats = current_case == 14u || current_case == 15u ? 5u : 1u;
    const uint64_t start = monotonic_ns();
    for (unsigned repeat = 0u; repeat < repeats && !r->stopped; repeat++) {
        const uint64_t call_start = monotonic_ns();
        if (sigsetjmp(*host_run_jmp(), 1) == 0) {
            host_run_arm();
            r->status = kernel_hle_call(234u, &frame);
        } else r->stopped = true;
        host_run_disarm(); /* each repeat arms again */
        const uint64_t call_elapsed = monotonic_ns() - call_start;
        if (repeat == 0u || call_elapsed < r->min_elapsed) r->min_elapsed = call_elapsed;
    }
    r->elapsed = monotonic_ns() - start;
    r->clock_after = kernel_clock_peek();
    host_run_disarm();
    (void)kernel_guest_read_bytes(address, after, sizeof(after));
    r->frame_same = memcmp(before, after, sizeof(before)) == 0;
    kernel_sync_reset();
}
static const kernel_thread_host_ops ops = {.has_code = has_code, .enter = enter,
                                          .wait_refused = refused};
static uint32_t create(unsigned index)
{
    const uint32_t out = scratch + 512u + index * 4u;
    const uint32_t args[10] = {out, 0u, 0u, 0u, 0u, 0x123456u, index, 0u, 0u, 0u};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch, 44u, args, 10u));
    CHECK(kernel_hle_call(255u, &frame) == STATUS_SUCCESS);
    uint32_t handle = 0u;
    CHECK(kernel_guest_read_u32(out, &handle));
    return handle;
}
int main(void)
{
    kernel_hle_init();
    CHECK(kernel_thread_register() == 10u);
    (void)kernel_sync_register();
    guest_region_request request = {0};
    request.bytes = 4096u; request.state = MEM_COMMIT; request.protect = PAGE_READWRITE;
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    CHECK(scratch != 0u);
    CHECK(kernel_thread_set_host_ops(&ops));
    struct sigaction action = {0}, previous;
    action.sa_handler = signal_handler;
    (void)sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1, &action, &previous) == 0);
    uint32_t handles[WORKERS];
    for (unsigned i = 0u; i < WORKERS; i++) handles[i] = create(i);
    pthread_mutex_lock(&gate);
    while (entered != WORKERS) (void)pthread_cond_wait(&condition, &gate);
    /* Closing the issued handle does not invalidate the retained TLS identity. */
    CHECK(kernel_object_release(handles[0]));
    kernel_object_entry closed;
    CHECK(!kernel_object_get_copy(handles[0], &closed));
    CHECK(kernel_object_release(handles[1]));
    CHECK(!kernel_object_get_copy(handles[1], &closed));
    release_workers = true;
    pthread_cond_broadcast(&condition);
    pthread_mutex_unlock(&gate);
    bool pinned = false;
    for (unsigned i = 0u; i < 1000u; i++) {
        if (kernel_thread_active_wait_count() == WORKERS) { pinned = true; break; }
        const struct timespec tick = {0, 10000};
        (void)nanosleep(&tick, NULL);
    }
    CHECK(pinned);
    CHECK(!kernel_thread_reset());
    CHECK(!kernel_thread_set_host_ops(NULL));
    /* Signals interrupt absolute sleeps, but never satisfy or shorten the wait. */
    for (unsigned j = 0u; j < 3u; j++) {
        for (unsigned i = 0u; i < WORKERS; i++) CHECK(pthread_kill(worker_ids[i], SIGUSR1) == 0);
        const struct timespec tick = {0, 500000};
        (void)nanosleep(&tick, NULL);
    }
    CHECK(kernel_thread_join_all(5000u) == 0u);
    CHECK(signals_seen != 0);
    for (unsigned i = 0u; i < WORKERS; i++) {
        result *r = &results[i];
        CHECK(!r->stopped && r->status == 0x102u);
        CHECK(r->elapsed >= ((i & 1u) != 0u ? UINT64_C(16000000) : UINT64_C(8000000)));
        CHECK(r->frame_same && r->clock_before == r->clock_after);
        CHECK(r->absolute && r->deadline_same);
        /* Signal delivery need not coincide with the syscall on a loaded host. */
        printf("worker %u: sleep calls %u, observed EINTR %u\n", i, r->sleep_calls, r->interrupts);
        kernel_thread_record record;
        CHECK(kernel_thread_get(handles[i], &record));
        CHECK(record.finished && !record.terminated);
    }
    CHECK(kernel_thread_active_wait_count() == 0u);
    CHECK(kernel_thread_reset());
    CHECK(kernel_thread_set_host_ops(&ops));
    for (unsigned i = 4u; i <= 20u; i++) {
        if (i == 13u) continue; /* the no-identity case below runs on this thread */
        (void)create(i);
        CHECK(kernel_thread_join_all(5000u) == 0u);
        result *r = &results[i];
        if (i == 11u) CHECK(!r->stopped && r->status == STATUS_ACCESS_VIOLATION);
        else if (i == 12u) {
            CHECK(!r->stopped && r->status == 0x102u);
            CHECK(r->elapsed >= UINT64_C(8000000));
        } else if (i == 14u || i == 15u) {
            /* -80000 sleeps 8 ms and -160000 sleeps 16 ms: the length follows the interval. */
            const uint64_t requested = i == 14u ? UINT64_C(8000000) : UINT64_C(16000000);
            CHECK(!r->stopped && r->status == 0x102u);
            CHECK(r->min_elapsed >= requested);
            CHECK(r->absolute && r->sleep_calls >= 5u);
            CHECK(r->elapsed >= 5u * requested);
        } else CHECK(r->stopped && r->refusal == KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE && r->unlocked);
        CHECK(r->frame_same && r->clock_before == r->clock_after);
        CHECK(kernel_thread_active_wait_count() == 0u);
    }
    /* No guest-thread TLS identity: exact pseudo arguments must still stop. */
    current_case = 13u;
    const int64_t timeout = -80000;
    CHECK(kernel_guest_write_bytes(scratch + 128u, &timeout, sizeof(timeout)));
    const uint32_t args[4] = {0xFFFFFFFEu, 1u, 0u, scratch + 128u};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch, 20u, args, 4u));
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        (void)kernel_hle_call(234u, &frame);
        CHECK(false);
    } else CHECK(host_run_result()->ordinal == 234u);
    host_run_disarm();
    CHECK(results[13].unlocked);
    CHECK(sigaction(SIGUSR1, &previous, NULL) == 0);
    CHECK(kernel_thread_reset());
    CHECK(kernel_thread_set_host_ops(NULL));
    CHECK(guest_region_free(scratch));
    printf("pseudo wait: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
