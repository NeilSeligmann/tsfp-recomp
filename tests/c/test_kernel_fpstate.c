/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * KeSaveFloatingPointState (142) and KeRestoreFloatingPointState (139): the bracket model
 * described in kernel_fpstate.h. Every refusal and every report has a case, and the
 * per-thread ownership has a real second thread.
 */

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_fpstate.h"
#include "kernel_hle.h"
#include "nt_status.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        checks++;                                                                    \
        if (!(cond)) {                                                               \
            failures++;                                                              \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                 \
            fflush(stdout);                                                          \
        }                                                                            \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                               \
    do {                                                                             \
        checks++;                                                                    \
        const uint32_t check_a = (uint32_t)(actual);                                 \
        const uint32_t check_e = (uint32_t)(expected);                               \
        if (check_a != check_e) {                                                    \
            failures++;                                                              \
            printf("  FAIL %s:%d  %s == %s (got %#x, want %#x)\n", __FILE__,         \
                   __LINE__, #actual, #expected, check_a, check_e);                  \
            fflush(stdout);                                                          \
        }                                                                            \
    } while (0)

static char captured[8192];
static size_t captured_len;
static pthread_mutex_t capture_mutex = PTHREAD_MUTEX_INITIALIZER;

static int capture_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    pthread_mutex_lock(&capture_mutex);
    int written = vsnprintf(captured + captured_len, sizeof(captured) - captured_len, format,
                            args);
    if (written > 0) {
        captured_len += (size_t)written;
        if (captured_len >= sizeof(captured)) {
            captured_len = sizeof(captured) - 1u;
        }
    }
    pthread_mutex_unlock(&capture_mutex);
    va_end(args);
    return written;
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

#define SCRATCH_BYTES 0x10000u
#define FRAME_OFFSET 0x100u
/* Save areas live from here on, one KERNEL_FPSTATE_AREA_BYTES slot each. */
#define AREAS_OFFSET 0x1000u

static kernel_guest_ptr scratch;

static kernel_guest_ptr area_at(unsigned index)
{
    return scratch + AREAS_OFFSET + index * KERNEL_FPSTATE_AREA_BYTES;
}

static void setup(void)
{
    kernel_hle_init();
    guest_mem_reset();
    CHECK_EQ_U32(kernel_fpstate_register(), 2u);
    kernel_hle_set_log(capture_printer);
    captured[0] = '\0';
    captured_len = 0u;
    guest_region_request request = {
        .bytes = SCRATCH_BYTES,
        .protect = PAGE_READWRITE,
        .state = MEM_COMMIT,
        .contiguous = true,
    };
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    if (scratch == 0u) {
        printf("  FATAL: no scratch memory\n");
        exit(EXIT_FAILURE);
    }
}

static void teardown(void)
{
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    scratch = 0u;
}

/* Each thread builds its frame in its own slice so two threads never share a stack. */
static uint32_t call_one(unsigned ordinal, uint32_t argument, uint32_t frame_slot)
{
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    if (!kernel_frame_build(&frame, scratch, FRAME_OFFSET + frame_slot * 0x40u, &argument, 1u)) {
        printf("  FATAL: could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    return kernel_hle_call(ordinal, &frame);
}

static uint32_t save(uint32_t area)
{
    return call_one(ORD_KeSaveFloatingPointState, area, 0u);
}

static uint32_t restore(uint32_t area)
{
    return call_one(ORD_KeRestoreFloatingPointState, area, 0u);
}

static void test_register_binds_both(void)
{
    setup();
    CHECK_EQ_U32(kernel_fpstate_depth(), 0u);
    CHECK_EQ_U32(kernel_fpstate_anomaly_count(), 0u);
    /* Registering again is a fresh kernel and forgets every live save. */
    CHECK_EQ_U32(save(area_at(0u)), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_fpstate_total_depth(), 1u);
    kernel_hle_init();
    CHECK_EQ_U32(kernel_fpstate_register(), 2u);
    CHECK_EQ_U32(kernel_fpstate_total_depth(), 0u);
    teardown();
}

static void test_save_then_restore_balances(void)
{
    setup();
    CHECK_EQ_U32(save(area_at(0u)), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_fpstate_depth(), 1u);
    CHECK_EQ_U32(restore(area_at(0u)), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_fpstate_depth(), 0u);
    CHECK_EQ_U32(kernel_fpstate_anomaly_count(), 0u);
    CHECK(captured[0] == '\0');
    teardown();
}

static void test_nesting_in_order(void)
{
    setup();
    CHECK_EQ_U32(save(area_at(0u)), STATUS_SUCCESS);
    CHECK_EQ_U32(save(area_at(1u)), STATUS_SUCCESS);
    CHECK_EQ_U32(save(area_at(2u)), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_fpstate_depth(), 3u);
    restore(area_at(2u));
    CHECK_EQ_U32(kernel_fpstate_depth(), 2u);
    restore(area_at(1u));
    restore(area_at(0u));
    CHECK_EQ_U32(kernel_fpstate_depth(), 0u);
    CHECK_EQ_U32(kernel_fpstate_anomaly_count(), 0u);
    CHECK(captured[0] == '\0');
    teardown();
}

static void test_restore_out_of_order_is_reported_and_honoured(void)
{
    setup();
    save(area_at(0u));
    save(area_at(1u));
    CHECK_EQ_U32(restore(area_at(0u)), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_fpstate_depth(), 1u);
    CHECK_EQ_U32(kernel_fpstate_anomaly_count(), 1u);
    CHECK(captured_contains("not this thread's most recent save"));
    /* The survivor is area 1: restoring it now is in order and silent. */
    captured[0] = '\0';
    captured_len = 0u;
    restore(area_at(1u));
    CHECK_EQ_U32(kernel_fpstate_depth(), 0u);
    CHECK_EQ_U32(kernel_fpstate_anomaly_count(), 1u);
    CHECK(captured[0] == '\0');
    teardown();
}

static void test_restore_without_save_is_reported(void)
{
    setup();
    CHECK_EQ_U32(restore(area_at(0u)), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_fpstate_depth(), 0u);
    CHECK_EQ_U32(kernel_fpstate_anomaly_count(), 1u);
    CHECK(captured_contains("no matching save"));
    /* A restore of a DIFFERENT area than the live save does not consume it. */
    save(area_at(1u));
    restore(area_at(2u));
    CHECK_EQ_U32(kernel_fpstate_depth(), 1u);
    CHECK_EQ_U32(kernel_fpstate_anomaly_count(), 2u);
    teardown();
}

static void test_save_into_live_area_is_recorded_twice_and_reported(void)
{
    setup();
    save(area_at(0u));
    CHECK_EQ_U32(save(area_at(0u)), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_fpstate_depth(), 2u);
    CHECK_EQ_U32(kernel_fpstate_anomaly_count(), 1u);
    CHECK(captured_contains("already holds a live save"));
    restore(area_at(0u));
    restore(area_at(0u));
    CHECK_EQ_U32(kernel_fpstate_depth(), 0u);
    CHECK_EQ_U32(kernel_fpstate_anomaly_count(), 1u);
    teardown();
}

static void test_unreadable_area_is_refused(void)
{
    setup();
    CHECK_EQ_U32(save(0u), STATUS_ACCESS_VIOLATION);
    CHECK_EQ_U32(save(0x2000u), STATUS_ACCESS_VIOLATION);
    CHECK_EQ_U32(kernel_fpstate_depth(), 0u);
    CHECK_EQ_U32(kernel_fpstate_anomaly_count(), 2u);
    CHECK(captured_contains("REFUSED: the save area is not readable"));
    /* Nothing was recorded, so the matching restore finds no save. */
    restore(0x2000u);
    CHECK_EQ_U32(kernel_fpstate_anomaly_count(), 3u);
    teardown();
}

static void test_capacity_is_bounded_and_refused_loudly(void)
{
    setup();
    for (unsigned i = 0u; i < KERNEL_FPSTATE_MAX_ACTIVE; i++) {
        /* All inside the first page of areas, distinct from each other not required. */
        CHECK_EQ_U32(save(area_at(i % 32u)), STATUS_SUCCESS);
    }
    CHECK_EQ_U32(kernel_fpstate_total_depth(), KERNEL_FPSTATE_MAX_ACTIVE);
    const uint32_t before = kernel_fpstate_anomaly_count();
    captured[0] = '\0';
    captured_len = 0u;
    CHECK_EQ_U32(save(area_at(40u)), STATUS_INSUFFICIENT_RESOURCES);
    CHECK_EQ_U32(kernel_fpstate_total_depth(), KERNEL_FPSTATE_MAX_ACTIVE);
    CHECK_EQ_U32(kernel_fpstate_anomaly_count(), before + 1u);
    CHECK(captured_contains("REFUSED"));
    CHECK(captured_contains("live saves"));
    /* One restore frees a slot and the next save succeeds again. */
    restore(area_at(0u));
    CHECK_EQ_U32(kernel_fpstate_total_depth(), KERNEL_FPSTATE_MAX_ACTIVE - 1u);
    CHECK_EQ_U32(save(area_at(40u)), STATUS_SUCCESS);
    teardown();
}

static void test_the_last_byte_of_the_area_is_required(void)
{
    setup();
    /* An area whose 0x20 bytes run off the end of the only mapping: probed by the page of
     * its last byte, so the page after the region (unmapped) refuses it. The region end is
     * page aligned and the test claims nothing about the page behind it being unmapped
     * unless the probe says so: skip when it is readable. */
    const kernel_guest_ptr tail = scratch + SCRATCH_BYTES - 0x10u;
    if (!kernel_guest_range_readable(scratch + SCRATCH_BYTES, 1u)) {
        CHECK_EQ_U32(save(tail), STATUS_ACCESS_VIOLATION);
        CHECK_EQ_U32(save(scratch + SCRATCH_BYTES - KERNEL_FPSTATE_AREA_BYTES), STATUS_SUCCESS);
    }
    teardown();
}

typedef struct {
    uint32_t area_of_other_thread;
    uint32_t depth_seen_by_other;
    uint32_t anomalies_after_foreign_restore;
    uint32_t depth_after_foreign_restore;
} thread_result;

static void *other_thread(void *argument)
{
    thread_result *result = argument;
    result->depth_seen_by_other = kernel_fpstate_depth();
    /* Frame slot 1: a different slice of guest stack from the main thread's. */
    call_one(ORD_KeRestoreFloatingPointState, result->area_of_other_thread, 1u);
    result->anomalies_after_foreign_restore = kernel_fpstate_anomaly_count();
    result->depth_after_foreign_restore = kernel_fpstate_depth();
    call_one(ORD_KeSaveFloatingPointState, area_at(5u), 1u);
    return NULL;
}

static void test_saves_are_per_thread(void)
{
    setup();
    save(area_at(0u));
    thread_result result = {area_at(0u), 99u, 99u, 99u};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, other_thread, &result) == 0);
    pthread_join(thread, NULL);
    /* The other thread has no saves, and its restore of OUR area found nothing. */
    CHECK_EQ_U32(result.depth_seen_by_other, 0u);
    CHECK_EQ_U32(result.depth_after_foreign_restore, 0u);
    CHECK_EQ_U32(result.anomalies_after_foreign_restore, 1u);
    /* Ours is untouched, and the other thread's later save is counted in the total only. */
    CHECK_EQ_U32(kernel_fpstate_depth(), 1u);
    CHECK_EQ_U32(kernel_fpstate_total_depth(), 2u);
    restore(area_at(0u));
    CHECK_EQ_U32(kernel_fpstate_depth(), 0u);
    teardown();
}

static void *saving_thread(void *argument)
{
    (void)argument;
    call_one(ORD_KeSaveFloatingPointState, area_at(6u), 1u);
    return NULL;
}

/* "Most recent save" is per thread: another thread saving later must not make this thread's
 * perfectly ordered restore look out of order. */
static void test_order_is_judged_per_thread(void)
{
    setup();
    save(area_at(0u));
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, saving_thread, NULL) == 0);
    pthread_join(thread, NULL);
    CHECK_EQ_U32(kernel_fpstate_total_depth(), 2u);
    restore(area_at(0u));
    CHECK_EQ_U32(kernel_fpstate_anomaly_count(), 0u);
    CHECK(captured[0] == '\0');
    CHECK_EQ_U32(kernel_fpstate_total_depth(), 1u);
    teardown();
}

static void test_missing_frame_is_reported(void)
{
    setup();
    CHECK_EQ_U32(kernel_hle_call(ORD_KeSaveFloatingPointState, NULL), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(kernel_hle_call(ORD_KeRestoreFloatingPointState, NULL), STATUS_SUCCESS);
    CHECK(captured_contains("no argument frame"));
    CHECK_EQ_U32(kernel_fpstate_depth(), 0u);
    teardown();
}

int main(void)
{
    test_register_binds_both();
    test_save_then_restore_balances();
    test_nesting_in_order();
    test_restore_out_of_order_is_reported_and_honoured();
    test_restore_without_save_is_reported();
    test_save_into_live_area_is_recorded_twice_and_reported();
    test_unreadable_area_is_refused();
    test_capacity_is_bounded_and_refused_loudly();
    test_the_last_byte_of_the_area_is_required();
    test_saves_are_per_thread();
    test_order_is_judged_per_thread();
    test_missing_frame_is_reported();
    printf("kernel_fpstate: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
