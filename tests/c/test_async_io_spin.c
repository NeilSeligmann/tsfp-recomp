/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T821: the spin rule of src/host/async_io_spin.c and kernel_async_io_complete_next (src/xbox/kernel_async_io.c).
 *
 * A guest thread that passes `threshold` cooperative safepoints with no HLE dispatch of its own while an overlapped
 * read is pending completes the EARLIEST pending read, the virtual clock advancing to its due time and never past it.
 * Synthetic: reads are queued straight into the model (no file, no title), safepoints are `async_io_spin_note` calls,
 * dispatches are `thunk_trace_append` calls (the same entry every real dispatch makes). Each group says what breaks it.
 */

#include "async_io_spin.h"

#include "guest_mem.h"
#include "kernel_async_io.h"
#include "kernel_call.h"
#include "kernel_clock.h"
#include "kernel_hle.h"
#include "kernel_object.h"
#include "nt_status.h"
#include "recomp_abi.h"
#include "thunk_trace.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the thunk library reads the guest registers, this suite defines them */
TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

static int failures;
static int checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        checks++;                                                                      \
        if (!(cond)) {                                                                 \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                     \
            failures++;                                                                \
        }                                                                              \
    } while (0)

#define CHECK_EQ(actual, expected)                                                     \
    do {                                                                               \
        checks++;                                                                      \
        const unsigned long long a_ = (unsigned long long)(actual);                    \
        const unsigned long long e_ = (unsigned long long)(expected);                  \
        if (a_ != e_) {                                                                \
            printf("FAIL %s:%d  %s == %llu, expected %llu\n", __FILE__, __LINE__,      \
                   #actual, a_, e_);                                                   \
            failures++;                                                                \
        }                                                                              \
    } while (0)

#define IOSB_A 0x0000u
#define IOSB_B 0x0040u
#define BUFFER_A 0x1000u
#define BUFFER_B 0x3000u
#define BYTES 0x2000u

static kernel_guest_ptr scratch;
static uint32_t queued_event;

static uint32_t read32(kernel_guest_ptr address)
{
    uint32_t value = 0u;
    CHECK(kernel_guest_read_u32(address, &value));
    return value;
}

static void setup(void)
{
    kernel_hle_init();
    kernel_object_reset();
    queued_event = 0u;
    kernel_clock_reset();
    guest_mem_reset();
    kernel_async_io_reset();
    kernel_async_io_set_enabled(true);
    async_io_spin_configure(0u);
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = 0x10000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    if (scratch == 0u) {
        printf("FATAL could not allocate scratch\n");
        exit(EXIT_FAILURE);
    }
}

static void teardown(void)
{
    async_io_spin_configure(0u);
    kernel_async_io_set_enabled(false);
    kernel_async_io_reset();
    guest_mem_reset();
}

/* Queue one read of BYTES at `iosb`/`buffer`, as the model's own queue holds it. */
static void queue(uint32_t iosb, uint32_t buffer, uint8_t fill)
{
    uint8_t *copy = malloc(BYTES);
    CHECK(copy != NULL);
    memset(copy, fill, BYTES);
    kernel_async_read request = {
        .file_handle = 0u,
        .event_handle = queued_event,
        .io_status = scratch + iosb,
        .buffer = scratch + buffer,
        .data = copy,
        .length = BYTES,
        .requested = BYTES,
        .volume = KERNEL_ASYNC_IO_VOLUME_DISC,
    };
    CHECK(kernel_guest_write_u32(scratch + iosb, STATUS_PENDING));
    CHECK(kernel_async_io_submit(&request));
}

static void spin(unsigned safepoints)
{
    for (unsigned i = 0u; i < safepoints; i++) {
        async_io_spin_note();
    }
}

static void *dispatch_from_another_thread(void *unused)
{
    (void)unused;
    for (int i = 0; i < 20; i++) {
        (void)thunk_trace_append(THUNK_KIND_ORDINAL, 2u, 0u, 0u, 0u, true);
    }
    return NULL;
}

static void other_thread_dispatches(void)
{
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, dispatch_from_another_thread, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
}

/* complete_next: nothing pending is false, otherwise the earliest request completes at its due time exactly. */
static void test_complete_next_advances_to_the_earliest_due_time_and_no_further(void)
{
    setup();
    CHECK(!kernel_async_io_complete_next()); /* nothing pending */
    queue(IOSB_A, BUFFER_A, 0xA1);
    queue(IOSB_B, BUFFER_B, 0xB2);
    const uint64_t due_a = kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_DISC, BYTES);
    CHECK(due_a > 0u);
    CHECK_EQ(kernel_async_io_pending(), 2u);
    const uint64_t before = kernel_clock_peek();
    CHECK(before < due_a);
    CHECK(kernel_async_io_complete_next());
    CHECK_EQ(kernel_async_io_pending(), 1u); /* only the first */
    CHECK_EQ(read32(scratch + IOSB_A), STATUS_SUCCESS);
    CHECK_EQ(read32(scratch + IOSB_B), STATUS_PENDING);
    uint8_t first = 0u;
    CHECK(kernel_guest_read_u8(scratch + BUFFER_A, &first));
    CHECK_EQ(first, 0xA1);
    CHECK(kernel_clock_peek() >= due_a);
    CHECK(kernel_clock_peek() < 2u * due_a); /* the second request (due later on the serial drive) is not reached */
    CHECK_EQ(kernel_async_io_get_stats().spin_completions, 1u);
    CHECK(kernel_async_io_complete_next());
    CHECK_EQ(read32(scratch + IOSB_B), STATUS_SUCCESS);
    CHECK(!kernel_async_io_complete_next());
    /* the model off refuses to complete what is queued */
    queue(IOSB_A, BUFFER_A, 0u);
    kernel_async_io_set_enabled(false);
    CHECK(!kernel_async_io_complete_next());
    CHECK_EQ(read32(scratch + IOSB_A), STATUS_PENDING);
    teardown();
}

/* The rule: threshold consecutive safepoints without an own dispatch complete the earliest read, fewer do not, a dispatch
 * restarts the count, no pending read counts nothing, and off (0) does nothing. */
static void test_the_rule_counts_safepoints_without_an_own_dispatch(void)
{
    setup();
    queue(IOSB_A, BUFFER_A, 1u);
    spin(100u); /* off */
    CHECK_EQ(read32(scratch + IOSB_A), STATUS_PENDING);
    async_io_spin_configure(10u);
    CHECK_EQ(async_io_spin_threshold(), 10u);
    async_io_spin_note();
    spin(8u);
    CHECK_EQ(read32(scratch + IOSB_A), STATUS_PENDING); /* 9 safepoints in the run: one short */
    (void)thunk_trace_append(THUNK_KIND_ORDINAL, 1u, 0u, 0u, 0u, true); /* a dispatch of this thread restarts the run */
    async_io_spin_note();
    spin(8u);
    CHECK_EQ(read32(scratch + IOSB_A), STATUS_PENDING);
    spin(1u);
    CHECK_EQ(read32(scratch + IOSB_A), STATUS_PENDING); /* the note after the dispatch only re-armed the count */
    spin(1u);
    CHECK_EQ(read32(scratch + IOSB_A), STATUS_SUCCESS);
    CHECK_EQ(kernel_async_io_get_stats().spin_completions, 1u);
    /* with nothing pending nothing is counted and nothing is completed later */
    spin(50u);
    queue(IOSB_B, BUFFER_B, 2u);
    spin(5u);
    CHECK_EQ(read32(scratch + IOSB_B), STATUS_PENDING);
    spin(5u);
    CHECK_EQ(read32(scratch + IOSB_B), STATUS_SUCCESS);
    teardown();
}

/* Dispatches of another thread do not restart this thread's run, this thread's own do (the counter is per thread). */
static void test_the_count_is_per_thread(void)
{
    setup();
    async_io_spin_configure(6u);
    queue(IOSB_A, BUFFER_A, 3u);
    async_io_spin_note(); /* arms the run: this thread made dispatches since the configure */
    spin(3u);
    /* a different thread making dispatches changes the global total but not this thread's own count */
    other_thread_dispatches();
    spin(2u);
    CHECK_EQ(read32(scratch + IOSB_A), STATUS_PENDING); /* 5 of 6 */
    spin(1u);
    CHECK_EQ(read32(scratch + IOSB_A), STATUS_SUCCESS);
    teardown();
}

/* Actual time, not safepoint count: identical timestamps must never complete
 * a future read. Literal epoch-to-clock units also catch conversion drift. */
static void test_elapsed_cutoff_signal_and_no_count_completion(void)
{
    setup();
    CHECK_EQ(kernel_object_create_event(1u,0u,scratch+0x80u),STATUS_SUCCESS);
    queued_event=read32(scratch+0x80u);
    queue(IOSB_A,BUFFER_A,0x39u);
    const uint64_t due=kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_DISC,BYTES);
    const uint64_t ns=(due*UINT64_C(1000000000)+KERNEL_CLOCK_FREQUENCY_HZ-1u)/KERNEL_CLOCK_FREQUENCY_HZ;
    CHECK_EQ(kernel_async_io_service_elapsed(100u),0u);
    for(unsigned i=0;i<1000u;i++) CHECK_EQ(kernel_async_io_service_elapsed(100u),0u);
    CHECK_EQ(kernel_clock_peek(),0u);
    CHECK_EQ(kernel_async_io_service_elapsed(100u+ns-1u),0u);
    CHECK_EQ(read32(scratch+IOSB_A),STATUS_PENDING);
    bool signalled=true;
    CHECK(kernel_object_event_signaled(queued_event,&signalled) && !signalled);
    const uint64_t before=kernel_clock_peek();
    CHECK_EQ(kernel_async_io_service_elapsed(99u),0u);
    CHECK_EQ(kernel_clock_peek(),before);
    CHECK_EQ(kernel_async_io_get_stats().elapsed_backwards,1u);
    CHECK_EQ(kernel_async_io_service_elapsed(100u+ns),1u);
    CHECK_EQ(read32(scratch+IOSB_A),STATUS_SUCCESS);
    CHECK_EQ(read32(scratch+IOSB_A+4u),BYTES);
    uint8_t buffer[BYTES]; CHECK(kernel_guest_read_bytes(scratch+BUFFER_A,buffer,sizeof buffer));
    for(size_t i=0;i<sizeof buffer;i++) CHECK_EQ(buffer[i],0x39u);
    CHECK(kernel_object_event_signaled(queued_event,&signalled) && signalled);
    CHECK_EQ(kernel_async_io_get_stats().elapsed_completions,1u);
    const uint64_t stopped=kernel_clock_peek();
    CHECK_EQ(kernel_async_io_service_elapsed(UINT64_C(9000000000)),0u);
    CHECK_EQ(kernel_clock_peek(),stopped); /* empty queue does not move time */
    teardown();
}

static void test_elapsed_serial_runahead_and_new_epoch(void)
{
    setup();
    queue(IOSB_A,BUFFER_A,1u); queue(IOSB_B,BUFFER_B,2u);
    const uint64_t due=kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_DISC,BYTES);
    const uint64_t ns=(due*UINT64_C(1000000000)+KERNEL_CLOCK_FREQUENCY_HZ-1u)/KERNEL_CLOCK_FREQUENCY_HZ;
    CHECK_EQ(kernel_async_io_service_elapsed(1000u),0u);
    CHECK_EQ(kernel_async_io_service_elapsed(1000u+ns),1u);
    CHECK_EQ(read32(scratch+IOSB_B),STATUS_PENDING);
    CHECK_EQ(kernel_async_io_service_elapsed(1000u+2u*ns),1u);
    (void)kernel_clock_advance_to(UINT64_C(100)*KERNEL_CLOCK_FREQUENCY_HZ);
    queue(IOSB_A,BUFFER_A,3u);
    CHECK_EQ(kernel_async_io_service_elapsed(UINT64_C(1000000000)),0u);
    CHECK_EQ(kernel_async_io_service_elapsed(UINT64_C(1000000000)+ns),1u);
    CHECK_EQ(read32(scratch+IOSB_A),STATUS_SUCCESS);
    queue(IOSB_A,BUFFER_A,4u);
    CHECK_EQ(kernel_async_io_service_elapsed(UINT64_C(2000000000)),0u);
    const uint64_t epoch=kernel_clock_peek();
    (void)kernel_clock_advance_to(epoch+due/2u);
    CHECK_EQ(kernel_async_io_service_elapsed(UINT64_C(2000000000)+ns/4u),0u);
    CHECK_EQ(kernel_clock_peek(),epoch+due/2u); /* elapsed is not added twice */
    CHECK_EQ(kernel_async_io_service_elapsed(UINT64_C(2000000000)+ns),1u);
    teardown();
}

static void test_elapsed_continuous_queue_rebases_after_external_clock_jump(void)
{
    setup(); queue(IOSB_A,BUFFER_A,8u);
    CHECK_EQ(kernel_async_io_service_elapsed(0u),0u);
    (void)kernel_clock_advance_to(KERNEL_CLOCK_FREQUENCY_HZ);
    queue(IOSB_B,BUFFER_B,9u); /* queue never drained; this read starts at model1s */
    CHECK_EQ(kernel_async_io_service_elapsed(UINT64_C(1000000)),1u);
    CHECK_EQ(read32(scratch+IOSB_B),STATUS_PENDING);
    CHECK_EQ(kernel_clock_peek(),KERNEL_CLOCK_FREQUENCY_HZ);
    const uint64_t due=kernel_async_io_service_ticks(KERNEL_ASYNC_IO_VOLUME_DISC,BYTES);
    const uint64_t ns=(due*UINT64_C(1000000000)+KERNEL_CLOCK_FREQUENCY_HZ-1u)/KERNEL_CLOCK_FREQUENCY_HZ;
    CHECK_EQ(kernel_async_io_service_elapsed(UINT64_C(1000000)+ns-1u),0u);
    CHECK_EQ(kernel_async_io_service_elapsed(UINT64_C(1000000)+ns),1u);
    CHECK_EQ(read32(scratch+IOSB_B),STATUS_SUCCESS);
    teardown();
}

static void test_elapsed_disabled_reset_and_saturated_target(void)
{
    setup(); queue(IOSB_A,BUFFER_A,5u);
    kernel_async_io_set_enabled(false);
    CHECK_EQ(kernel_async_io_service_elapsed(UINT64_C(1000000000)),0u);
    CHECK_EQ(kernel_clock_peek(),0u);
    CHECK_EQ(kernel_async_io_get_stats().elapsed_calls,0u);
    kernel_async_io_set_enabled(true);
    CHECK_EQ(kernel_async_io_service_elapsed(UINT64_C(2000000000)),0u);
    CHECK_EQ(kernel_async_io_service_elapsed(UINT64_C(3000000000)),1u);
    CHECK_EQ(kernel_clock_peek(),KERNEL_CLOCK_FREQUENCY_HZ); /* exact second */
    kernel_async_io_reset(); kernel_clock_reset();
    (void)kernel_clock_advance_to(UINT64_MAX/2u);
    queue(IOSB_A,BUFFER_A,6u);
    CHECK_EQ(kernel_async_io_service_elapsed(0u),0u);
    CHECK_EQ(kernel_async_io_service_elapsed(UINT64_MAX),1u);
    CHECK_EQ(kernel_clock_peek(),UINT64_MAX); /* no wrap in epoch addition */
    kernel_async_io_reset(); kernel_clock_reset();
    queue(IOSB_A,BUFFER_A,7u);
    CHECK_EQ(kernel_async_io_service_elapsed(1u),0u);
    CHECK_EQ(kernel_clock_peek(),0u); /* old epoch did not survive reset */
    teardown();
}

int main(void)
{
    test_complete_next_advances_to_the_earliest_due_time_and_no_further();
    test_the_rule_counts_safepoints_without_an_own_dispatch();
    test_the_count_is_per_thread();
    test_elapsed_cutoff_signal_and_no_count_completion();
    test_elapsed_serial_runahead_and_new_epoch();
    test_elapsed_continuous_queue_rebases_after_external_clock_jump();
    test_elapsed_disabled_reset_and_saturated_target();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
