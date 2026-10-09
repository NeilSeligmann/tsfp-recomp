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
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static bool released[8];
static unsigned entered;
static pthread_key_t destructor_key;
static bool destructor_entered, destructor_release;
static host_stop target_stops[8];
static volatile unsigned char *guard;
static uint32_t scratch;
static _Thread_local unsigned target_mode;
static const kernel_thread_host_ops ops;

typedef struct {
    uint32_t handle, frame_address, timeout_address, mode, alertable, irql;
    uint32_t status;
    host_stop stop;
    kernel_thread_wait_refusal refusal;
    bool hook_unlocked;
    bool returned;
} waiter;
static _Thread_local waiter *current_waiter;
static void pause_tick(void)
{
    const struct timespec tick = {0, 1000000};
    (void)nanosleep(&tick, NULL);
}
static bool wait_pins(unsigned count)
{
    for (unsigned i = 0u; i < 5000u; i++) {
        if (kernel_thread_active_wait_count() == count) { return true; }
        pause_tick();
    }
    return false;
}
static bool has_code(uint32_t address) { return address == 0x123456u; }
static void terminate(uint32_t status)
{
    (void)status;
    host_run_stop(HOST_STOP_THREAD_EXITED, 0u, 0u, "wait target terminated");
}
static void enter(const kernel_thread_launch *launch)
{
    target_mode = launch->start_context;
    if (target_mode == 6u) { (void)pthread_setspecific(destructor_key, (void *)(uintptr_t)1u); }
    pthread_mutex_lock(&gate_lock);
    entered++;
    pthread_cond_broadcast(&gate_cond);
    while (!released[target_mode]) { (void)pthread_cond_wait(&gate_cond, &gate_lock); }
    pthread_mutex_unlock(&gate_lock);
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        if (target_mode == 1u) { *guard = 1u; }
        if (target_mode == 2u) {
            const uint32_t args[4] = {launch->handle, 1u, 0u, 0u};
            kernel_call_frame frame = {0};
            (void)kernel_frame_build(&frame, launch->stack_low, 20u, args, 4u);
            (void)kernel_hle_call(234u, &frame); /* Self wait must stop. */
        } else {
            const uint32_t status = 0xCAFE0000u + target_mode;
            kernel_call_frame frame = {0};
            (void)kernel_frame_build(&frame, launch->stack_low, 8u, &status, 1u);
            (void)kernel_hle_call(258u, &frame);
        }
    }
    target_stops[target_mode] = *host_run_result();
    host_run_disarm();
}
static bool confirm(const kernel_thread_launch *launch)
{
    (void)launch;
    return host_run_result()->reason == HOST_STOP_THREAD_EXITED;
}
static void refused(uint32_t handle, kernel_thread_wait_refusal reason)
{
    if (current_waiter) {
        current_waiter->refusal = reason;
        kernel_thread_record record;
        /* Safe reentry demonstrates pins and locks were released before hook. */
        current_waiter->hook_unlocked = kernel_thread_active_wait_count() == 0u &&
            kernel_thread_get(handle, &record);
    }
    if (reason == KERNEL_THREAD_WAIT_TERMINAL_HOST_FAILURE) {
        kernel_thread_record record;
        if (kernel_thread_get(handle, &record)) {
            host_run_rethrow(&target_stops[record.start_context]);
        }
    }
    host_run_stop(HOST_STOP_KERNEL_UNIMPLEMENTED, 0x38004Cu, 234u,
                  "unsupported bounded thread wait");
}
static const kernel_thread_host_ops ops = {
    .has_code = has_code, .enter = enter, .terminate = terminate,
    .termination_confirmed = confirm, .wait_refused = refused,
};
static uint32_t create(unsigned mode, bool suspended)
{
    const uint32_t out = scratch + 0x800u + mode * 4u;
    const uint32_t args[10] = {out, 0u, 0u, 0u, 0u, 0x123456u, mode,
                              suspended ? 1u : 0u, 0u, 0u};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch + 0x100u, 44u, args, 10u));
    CHECK(kernel_hle_call(255u, &frame) == STATUS_SUCCESS);
    uint32_t handle = 0u;
    CHECK(kernel_guest_read_u32(out, &handle));
    return handle;
}
static void release(unsigned mode)
{
    pthread_mutex_lock(&gate_lock);
    released[mode] = true;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_lock);
}
static void *run_wait(void *context)
{
    waiter *w = context;
    current_waiter = w;
    kernel_sync_reset();
    if (w->irql != 0u) { (void)kernel_hle_entry(129u)->handler(NULL); }
    const uint32_t args[4] = {w->handle, w->mode, w->alertable, w->timeout_address};
    kernel_call_frame frame = {0};
    if (!kernel_frame_build(&frame, w->frame_address, 20u, args, 4u)) { return NULL; }
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        w->status = kernel_hle_call(234u, &frame);
        w->returned = true;
    }
    w->stop = *host_run_result();
    host_run_disarm();
    current_waiter = NULL;
    return NULL;
}
static waiter make_waiter(uint32_t handle, unsigned index)
{
    return (waiter){.handle = handle, .frame_address = scratch + 0x1000u + index * 64u,
                    .mode = 1u};
}
static void reset_case(void)
{
    CHECK(kernel_thread_reset());
    kernel_object_reset();
    memset(released, 0, sizeof(released));
    memset(target_stops, 0, sizeof(target_stops));
    entered = 0u;
    CHECK(kernel_thread_set_host_ops(&ops));
}
static void test_multiple_retained_waiters(void)
{
    reset_case();
    const uint32_t handle = create(0u, false);
    const uint64_t before = kernel_clock_peek();
    waiter zero = make_waiter(handle, 0u);
    zero.timeout_address = scratch + 0x900u;
    const uint64_t timeout = 0u;
    CHECK(kernel_guest_write_bytes(zero.timeout_address, &timeout, 8u));
    (void)run_wait(&zero);
    CHECK(zero.returned && zero.status == 0x102u);
    waiter high_byte = make_waiter(handle, 5u);
    high_byte.alertable = 0x100u;
    high_byte.timeout_address = zero.timeout_address;
    (void)run_wait(&high_byte);
    CHECK(high_byte.returned && high_byte.status == 0x102u);
    waiter raised = make_waiter(handle, 5u);
    raised.irql = 2u;
    (void)run_wait(&raised);
    CHECK(!raised.returned && raised.refusal == KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE);
    CHECK(kernel_sync_current_irql() == 2u);
    kernel_sync_reset();
    waiter a = make_waiter(handle, 1u), b = make_waiter(handle, 2u);
    pthread_t wa, wb;
    CHECK(pthread_create(&wa, NULL, run_wait, &a) == 0);
    CHECK(pthread_create(&wb, NULL, run_wait, &b) == 0);
    CHECK(wait_pins(2u));
    CHECK(!kernel_thread_reset());
    CHECK(!kernel_thread_set_host_ops(NULL));
    kernel_object_entry snapshot;
    CHECK(kernel_object_get_copy(handle, &snapshot));
    CHECK(snapshot.handle == handle && snapshot.kind == KERNEL_OBJECT_THREAD);
    CHECK(kernel_object_release(handle));
    kernel_object_entry unchanged = snapshot;
    CHECK(!kernel_object_get_copy(handle, &snapshot));
    CHECK(memcmp(&snapshot, &unchanged, sizeof(snapshot)) == 0);
    /* Finishing another target broadcasts table_cond, a real spurious wake for a/b. */
    const uint32_t other = create(3u, false);
    CHECK(other != handle);
    kernel_object_entry replacement;
    CHECK(kernel_object_get_copy(other, &replacement));
    CHECK(replacement.handle == other && replacement.owner_tag != unchanged.owner_tag);
    CHECK(!kernel_object_get_copy(handle, &snapshot));
    release(3u);
    for (unsigned i = 0u; i < 5000u; i++) {
        kernel_thread_record record;
        if (kernel_thread_get(other, &record) && record.finished) { break; }
        pause_tick();
    }
    CHECK(kernel_thread_active_wait_count() == 2u);
    waiter closed = make_waiter(handle, 3u);
    (void)run_wait(&closed);
    CHECK(closed.returned && closed.status == STATUS_INVALID_HANDLE);
    release(0u);
    CHECK(pthread_join(wa, NULL) == 0);
    CHECK(pthread_join(wb, NULL) == 0);
    CHECK(a.returned && a.status == STATUS_SUCCESS);
    CHECK(b.returned && b.status == STATUS_SUCCESS);
    CHECK(kernel_thread_active_wait_count() == 0u);
    CHECK(kernel_thread_join_all(5000u) == 0u);
    CHECK(kernel_clock_peek() == before);
    waiter done = make_waiter(other, 4u);
    (void)run_wait(&done);
    CHECK(done.returned && done.status == STATUS_SUCCESS);
}
static void test_fault_and_unsupported(void)
{
    reset_case();
    const uint32_t bad = create(1u, false);
    waiter fault = make_waiter(bad, 0u);
    pthread_t fault_waiter;
    CHECK(pthread_create(&fault_waiter, NULL, run_wait, &fault) == 0);
    CHECK(wait_pins(1u));
    release(1u);
    CHECK(pthread_join(fault_waiter, NULL) == 0);
    CHECK(kernel_thread_join_all(5000u) == 0u);
    CHECK(!fault.returned && fault.refusal == KERNEL_THREAD_WAIT_TERMINAL_HOST_FAILURE);
    CHECK(fault.hook_unlocked);
    CHECK(fault.stop.reason == HOST_STOP_FAULT);
    CHECK(fault.stop.signal_number == SIGSEGV && fault.stop.fault_address == (uintptr_t)guard);
    CHECK(kernel_thread_active_wait_count() == 0u);
    waiter unsupported = make_waiter(bad, 1u);
    unsupported.alertable = 1u;
    (void)run_wait(&unsupported);
    CHECK(!unsupported.returned && unsupported.refusal == KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE);
    unsupported = make_waiter(bad, 1u);
    unsupported.mode = 0u;
    (void)run_wait(&unsupported);
    CHECK(!unsupported.returned);
    unsupported = make_waiter(bad, 1u);
    unsupported.timeout_address = scratch + 0x900u;
    const uint64_t finite = UINT64_MAX;
    CHECK(kernel_guest_write_bytes(unsupported.timeout_address, &finite, 8u));
    (void)run_wait(&unsupported);
    CHECK(!unsupported.returned);
    kernel_call_frame unreadable = {0};
    unreadable.stack_ptr = (uint32_t)(uintptr_t)guard;
    unreadable.stack_limit = unreadable.stack_ptr + 20u;
    CHECK(kernel_hle_call(234u, &unreadable) == STATUS_INVALID_PARAMETER);
    CHECK(kernel_hle_call(234u, NULL) == STATUS_INVALID_PARAMETER);
    unsupported = make_waiter(bad, 1u);
    unsupported.timeout_address = (uint32_t)(uintptr_t)guard;
    (void)run_wait(&unsupported);
    CHECK(unsupported.returned && unsupported.status == STATUS_ACCESS_VIOLATION);
    const uint32_t nonthread = kernel_object_create(KERNEL_OBJECT_EVENT, 42u);
    unsupported = make_waiter(nonthread, 1u);
    (void)run_wait(&unsupported);
    CHECK(!unsupported.returned);
    const uint32_t suspended = create(4u, true);
    unsupported = make_waiter(suspended, 1u);
    (void)run_wait(&unsupported);
    CHECK(!unsupported.returned);
    const uint32_t self = create(2u, false);
    release(2u);
    CHECK(kernel_thread_join_all(5000u) == 0u);
    kernel_thread_record record;
    CHECK(kernel_thread_get(self, &record));
    CHECK(record.finished && !record.terminated);
    CHECK(target_stops[2].reason == HOST_STOP_KERNEL_UNIMPLEMENTED);
}
static void join_destructor(void *value)
{
    (void)value;
    pthread_mutex_lock(&gate_lock);
    destructor_entered = true;
    pthread_cond_broadcast(&gate_cond);
    while (!destructor_release) { (void)pthread_cond_wait(&gate_cond, &gate_lock); }
    pthread_mutex_unlock(&gate_lock);
}
static bool reset_result;
static void *run_reset(void *context)
{
    (void)context;
    reset_result = kernel_thread_reset();
    return NULL;
}
static void test_reset_join_excludes_creation(void)
{
    reset_case();
    destructor_entered = false;
    destructor_release = false;
    CHECK(pthread_key_create(&destructor_key, join_destructor) == 0);
    (void)create(6u, false);
    release(6u);
    pthread_mutex_lock(&gate_lock);
    while (!destructor_entered) { (void)pthread_cond_wait(&gate_cond, &gate_lock); }
    pthread_mutex_unlock(&gate_lock);
    CHECK(kernel_thread_running_count() == 0u);
    pthread_t resetter;
    CHECK(pthread_create(&resetter, NULL, run_reset, NULL) == 0);
    /* This changes to false only once reset has entered its unlocked join phase. */
    bool resetting = false;
    for (unsigned i = 0u; i < 5000u; i++) {
        if (!kernel_thread_set_host_ops(&ops)) { resetting = true; break; }
        pause_tick();
    }
    CHECK(resetting);
    CHECK(!kernel_thread_reset());
    const uint32_t out = scratch + 0x800u;
    CHECK(kernel_guest_write_u32(out, 0xDEADBEEFu));
    const uint32_t args[10] = {out, 0u, 0u, 0u, 0u, 0x123456u, 0u, 0u, 0u, 0u};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch + 0x100u, 44u, args, 10u));
    CHECK(kernel_hle_call(255u, &frame) == STATUS_NOT_IMPLEMENTED);
    uint32_t unchanged = 0u;
    CHECK(kernel_guest_read_u32(out, &unchanged) && unchanged == 0xDEADBEEFu);
    pthread_mutex_lock(&gate_lock);
    destructor_release = true;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_lock);
    CHECK(pthread_join(resetter, NULL) == 0);
    CHECK(reset_result);
    CHECK(pthread_key_delete(destructor_key) == 0);
}

static void returning_refused(uint32_t handle, kernel_thread_wait_refusal reason)
{
    (void)handle;
    (void)reason;
}
static void test_missing_hook_aborts(void)
{
    for (unsigned variant = 0u; variant < 2u; variant++) {
        reset_case();
        kernel_thread_host_ops absent = ops;
        absent.wait_refused = variant == 0u ? NULL : returning_refused;
        CHECK(kernel_thread_set_host_ops(&absent));
        const pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            waiter w = make_waiter(kernel_object_create(KERNEL_OBJECT_EVENT, 0u), 0u);
            (void)run_wait(&w);
            _exit(0);
        }
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    }
}
static uint32_t call_233(uint32_t handle, uint32_t alertable, uint32_t timeout_address,
                         uint32_t slot)
{
    const uint32_t args[3] = {handle, alertable, timeout_address};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch + 0x1800u + slot * 64u, 16u, args, 3u));
    return kernel_hle_call(233u, &frame);
}
static uint32_t call_99(uint32_t mode, uint32_t alertable, uint32_t interval_address)
{
    const uint32_t args[3] = {mode, alertable, interval_address};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch + 0x1C00u, 16u, args, 3u));
    return kernel_hle_call(99u, &frame);
}
static void test_wait_233_and_delay_99(void)
{
    reset_case();
    /* 233 (Handle, Alertable, Timeout) shares the 234 policy with mode forced to 1. */
    const uint32_t handle = create(0u, false);
    const uint32_t zero_address = scratch + 0x900u;
    const uint64_t zero = 0u;
    CHECK(kernel_guest_write_bytes(zero_address, &zero, 8u));
    CHECK(call_233(handle, 0u, zero_address, 0u) == 0x102u);
    CHECK(call_233(0xDEAD0u, 0u, zero_address, 1u) == STATUS_INVALID_HANDLE);
    release(0u);
    CHECK(kernel_thread_join_all(5000u) == 0u);
    CHECK(call_233(handle, 0u, 0u, 2u) == STATUS_SUCCESS);

    /* 99 (WaitMode, Alertable, Interval*): measured mode 1, relative interval. */
    const uint32_t interval_address = scratch + 0x940u;
    int64_t interval = -80000; /* 8 ms, the measured Sleep(8)-style value. */
    CHECK(kernel_guest_write_bytes(interval_address, &interval, 8u));
    struct timespec begin, end;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &begin) == 0);
    CHECK(call_99(1u, 0u, interval_address) == STATUS_SUCCESS);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    const long elapsed_ns = (end.tv_sec - begin.tv_sec) * 1000000000L +
                            (end.tv_nsec - begin.tv_nsec);
    CHECK(elapsed_ns >= 8000000L);
    CHECK(call_99(1u, 1u, interval_address) == STATUS_SUCCESS); /* alertable, no APC */
    CHECK(kernel_thread_active_wait_count() == 0u);
    interval = 0;
    CHECK(kernel_guest_write_bytes(interval_address, &interval, 8u));
    CHECK(call_99(1u, 0u, interval_address) == STATUS_SUCCESS);
    CHECK(call_99(1u, 0u, 0u) == STATUS_ACCESS_VIOLATION);
    CHECK(call_99(1u, 0u, (uint32_t)(uintptr_t)guard) == STATUS_ACCESS_VIOLATION);

    /* Only the low byte of WaitMode is the mode (the wrapper pushes a byte), so 0x101 is mode 1. */
    interval = -80000;
    CHECK(kernel_guest_write_bytes(interval_address, &interval, 8u));
    CHECK(call_99(0x101u, 0u, interval_address) == STATUS_SUCCESS);

    /* A delay of a second or more sleeps the whole seconds AND the remainder: 1.05 s. The upper
     * bound is generous (5 s) so only a wrongly scaled sleep, not a loaded host, trips it. */
    interval = -10500000;
    CHECK(kernel_guest_write_bytes(interval_address, &interval, 8u));
    CHECK(clock_gettime(CLOCK_MONOTONIC, &begin) == 0);
    CHECK(call_99(1u, 0u, interval_address) == STATUS_SUCCESS);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
    const long long_elapsed_ns = (end.tv_sec - begin.tv_sec) * 1000000000L +
                                 (end.tv_nsec - begin.tv_nsec);
    CHECK(long_elapsed_ns >= 1050000000L);
    CHECK(long_elapsed_ns < 5000000000L);

    /* 233 with an argument frame that cannot be read is a parameter error, never a wait. */
    kernel_call_frame unreadable = {0};
    unreadable.stack_ptr = (uint32_t)(uintptr_t)guard;
    unreadable.stack_limit = unreadable.stack_ptr + 16u;
    CHECK(kernel_hle_call(233u, &unreadable) == STATUS_INVALID_PARAMETER);
    CHECK(kernel_hle_call(233u, NULL) == STATUS_INVALID_PARAMETER);
}
static void test_delay_99_refusals(void)
{
    const uint32_t interval_address = scratch + 0x940u;
    /* Cases 0-3 are bad intervals (positive, INT64_MIN, far over the cap, and EXACTLY one unit
     * over the cap, -(DELAY_MAX_100NS + 1), the first refused value), case 4 is the wrong
     * WaitMode, case 5 is a good call at a raised IRQL. */
    const int64_t cases[4] = {5, INT64_MIN, -INT64_C(200000000), -INT64_C(100000001)};
    for (unsigned i = 0u; i < 6u; i++) {
        reset_case();
        const pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            int64_t interval = i < 4u ? cases[i] : -80000;
            (void)kernel_guest_write_bytes(interval_address, &interval, 8u);
            waiter probe = make_waiter(0u, 0u);
            current_waiter = &probe;
            if (i == 5u) { (void)kernel_hle_entry(129u)->handler(NULL); }
            if (sigsetjmp(*host_run_jmp(), 1) == 0) {
                host_run_arm();
                (void)call_99(i == 4u ? 0u : 1u, 0u, interval_address);
                _exit(2); /* Must have been refused. */
            }
            _exit(host_run_result()->reason == HOST_STOP_KERNEL_UNIMPLEMENTED &&
                  probe.refusal == KERNEL_THREAD_WAIT_UNSUPPORTED_SCOPE ? 0 : 3);
        }
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}
int main(void)
{
    kernel_hle_init();
    CHECK(kernel_thread_register() == 10u);
    (void)kernel_sync_register();
    guest_region_request request = {0};
    request.bytes = 0x4000u; request.state = MEM_COMMIT; request.protect = PAGE_READWRITE;
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    CHECK(scratch != 0u);
    request.bytes = 4096u;
    const uint32_t guard_address = guest_region_alloc(&request, &status);
    CHECK(guard_address != 0u);
    guard = (volatile unsigned char *)(uintptr_t)guard_address;
    CHECK(mprotect((void *)(uintptr_t)guard_address, 4096u, PROT_NONE) == 0);
    test_multiple_retained_waiters();
    test_fault_and_unsupported();
    test_reset_join_excludes_creation();
    test_missing_hook_aborts();
    test_wait_233_and_delay_99();
    test_delay_99_refusals();
    CHECK(kernel_thread_reset());
    CHECK(kernel_thread_set_host_ops(NULL));
    CHECK(guest_region_free(guard_address));
    CHECK(guest_region_free(scratch));
    printf("thread wait: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
