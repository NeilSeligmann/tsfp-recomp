/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "kernel_thread.h"
#include "nt_status.h"
#include "host_runtime.h"
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

static unsigned checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %d: %s\n", __LINE__, #c); } } while (0)
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_cond = PTHREAD_COND_INITIALIZER;
static unsigned arrived;
static bool release_gate;
static volatile unsigned char *fault_page;
static _Thread_local unsigned mode;
static _Thread_local bool confirmed;
static uint32_t handler_results[8];

static bool has_code(uint32_t address) { return address == 0x123456u; }
static void terminate_hook(uint32_t status)
{
    (void)status;
    if (mode == 2u) { return; }
    if (mode == 3u) { *fault_page = 1u; }
    host_run_stop(HOST_STOP_THREAD_EXITED, 0u, 0u, "lifecycle termination");
}
static bool termination_confirmed(const kernel_thread_launch *launch)
{
    (void)launch;
    return confirmed || host_run_result()->reason == HOST_STOP_THREAD_EXITED;
}
static void enter(const kernel_thread_launch *launch)
{
    mode = launch->start_context;
    confirmed = false;
    pthread_mutex_lock(&gate_lock);
    arrived++;
    pthread_cond_broadcast(&gate_cond);
    while (!release_gate) { (void)pthread_cond_wait(&gate_cond, &gate_lock); }
    pthread_mutex_unlock(&gate_lock);
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        if (mode == 1u) { *fault_page = 1u; } /* Real host fault, no request. */
        if (mode == 5u) { confirmed = true; host_run_disarm(); return; }
        kernel_call_frame frame = {0};
        const uint32_t status = 0xCAFE0000u + mode;
        if (mode != 4u) {
            (void)kernel_frame_build(&frame, launch->stack_low, 8u, &status, 1u);
        }
        handler_results[mode] = kernel_hle_call(258u, &frame);
    }
    host_run_disarm();
}
static const kernel_thread_host_ops ops = {
    .has_code = has_code, .enter = enter, .terminate = terminate_hook,
    .termination_confirmed = termination_confirmed,
};
static uint32_t create(uint32_t scratch, unsigned index)
{
    uint32_t args[10] = {scratch + 0x800u + index * 4u, 0u, 0u, 0u, 0u,
                         0x123456u, index, 0u, 0u, 0u};
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch + index * 64u, 44u, args, 10u));
    CHECK(kernel_hle_call(255u, &frame) == STATUS_SUCCESS);
    uint32_t handle = 0u;
    CHECK(kernel_guest_read_u32(args[0], &handle));
    return handle;
}
static void run_batch(uint32_t scratch, bool missing_hook, bool missing_confirmation)
{
    CHECK(kernel_thread_reset());
    kernel_thread_host_ops selected = ops;
    if (missing_hook) { selected.terminate = NULL; }
    if (missing_confirmation) { selected.termination_confirmed = NULL; }
    CHECK(kernel_thread_set_host_ops(&selected));
    arrived = 0u;
    release_gate = false;
    const unsigned count = missing_hook || missing_confirmation ? 1u : 6u;
    uint32_t handles[6];
    for (unsigned i = 0u; i < count; i++) { handles[i] = create(scratch, i); }
    pthread_mutex_lock(&gate_lock);
    while (arrived != count) { (void)pthread_cond_wait(&gate_cond, &gate_lock); }
    release_gate = true;
    pthread_cond_broadcast(&gate_cond);
    pthread_mutex_unlock(&gate_lock);
    CHECK(kernel_thread_join_all(5000u) == 0u);
    for (unsigned i = 0u; i < count; i++) {
        kernel_thread_record record;
        CHECK(kernel_thread_get(handles[i], &record));
        CHECK(record.finished);
        const bool requested = !missing_hook && (i == 0u || i == 3u);
        CHECK(record.termination_requested == requested);
        CHECK(record.terminated == (!missing_hook && !missing_confirmation && i == 0u));
        CHECK(record.exit_eax == (requested ? 0xCAFE0000u + i : 0u));
    }
    if (missing_hook) { CHECK(handler_results[0] == STATUS_NOT_IMPLEMENTED); }
    else if (!missing_confirmation) {
        CHECK(handler_results[2] == STATUS_NOT_IMPLEMENTED);
        CHECK(handler_results[4] == STATUS_INVALID_PARAMETER);
    }
}
int main(void)
{
    const long page_size = sysconf(_SC_PAGESIZE);
    CHECK(page_size > 0);
    fault_page = mmap(NULL, (size_t)page_size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK((const void *)fault_page != MAP_FAILED);
    kernel_hle_init();
    CHECK(kernel_thread_register() == 10u);
    guest_region_request request = {0};
    request.bytes = 4096u;
    request.state = MEM_COMMIT;
    request.protect = PAGE_READWRITE;
    nt_status allocation_status = STATUS_SUCCESS;
    const uint32_t scratch = guest_region_alloc(&request, &allocation_status);
    CHECK(scratch != 0u && allocation_status == STATUS_SUCCESS);
    /* Calling from an unmanaged host thread must not update any owned record. */
    CHECK(kernel_thread_set_host_ops(&ops));
    const uint32_t status = 0xDEADBEEFu;
    kernel_call_frame frame = {0};
    CHECK(kernel_frame_build(&frame, scratch, 8u, &status, 1u));
    CHECK(kernel_hle_call(258u, &frame) == STATUS_NOT_IMPLEMENTED);
    for (unsigned i = 0u; i < 8u; i++) { run_batch(scratch, false, false); }
    run_batch(scratch, true, false);
    run_batch(scratch, false, true);
    CHECK(kernel_thread_set_host_ops(NULL));
    CHECK(kernel_thread_reset());
    CHECK(guest_region_free(scratch));
    CHECK(munmap((void *)fault_page, (size_t)page_size) == 0);
    printf("thread lifecycle: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
