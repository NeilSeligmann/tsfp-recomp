/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * PsCreateSystemThreadEx is the first kernel call the guest makes and cannot be
 * stubbed: returning a status without writing the out-parameter faults the guest
 * dereferencing 0x20. So the thing to pin is that the out-parameter is actually
 * written, and that a failure path never hands back a handle to a half-built record.
 */

#include "kernel_object.h"
#include "kernel_thread.h"

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_sync.h"
#include "kernel_clock.h"
#include "nt_status.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures;
static int log_lines;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        uint32_t a_ = (uint32_t)(actual);                                                \
        uint32_t e_ = (uint32_t)(expected);                                              \
        if (a_ != e_) {                                                                   \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__,          \
                   #actual, (unsigned)a_, (unsigned)e_);                                 \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

static int counting_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    (void)format;
    va_end(args);
    log_lines++;
    return 0;
}

/* A guest scratch page to hold the synthetic argument frame and the out-parameters. */
static uint32_t scratch_base(void)
{
    guest_region_request request = {0};
    request.bytes = 0x4000u;
    request.state = MEM_COMMIT;
    request.protect = PAGE_READWRITE;
    nt_status status = STATUS_SUCCESS;
    kernel_guest_ptr base = guest_region_alloc(&request, &status);
    if (base == 0u) {
        printf("FAIL could not allocate guest scratch (status %#x)\n", (unsigned)status);
        failures++;
        return 0u;
    }
    return (uint32_t)base;
}

static void test_out_parameter_is_written(void)
{
    kernel_hle_init();
    kernel_thread_reset();
    CHECK_EQ_U32(kernel_thread_register(), 10u);

    uint32_t base = scratch_base();
    if (base == 0u) {
        return;
    }
    const uint32_t handle_slot = base;
    const uint32_t id_slot = base + 4u;
    const uint32_t frame_buf = base + 0x100u;

    const uint32_t args[10] = {
        handle_slot, 0u, 0x4000u, 0u, id_slot, 0x00123456u, 0xCAFEu, 0u, 0u, 0x00654321u,
    };
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    CHECK(kernel_frame_build(&frame, frame_buf, 0x100u, args, 10u));

    CHECK_EQ_U32(kernel_hle_call(255u, &frame), STATUS_SUCCESS);

    /* The whole point: the guest dereferences this immediately. */
    uint32_t handle = 0u;
    CHECK(kernel_guest_read_u32(handle_slot, &handle));
    CHECK(handle != 0u);

    uint32_t id = 0u;
    CHECK(kernel_guest_read_u32(id_slot, &id));
    CHECK(id != 0u);

    const kernel_thread_record *record = kernel_thread_find(handle);
    CHECK(record != NULL);
    if (record != NULL) {
        CHECK_EQ_U32(record->start_routine, 0x00123456u);
        CHECK_EQ_U32(record->start_context, 0xCAFEu);
        CHECK_EQ_U32(record->system_routine, 0x00654321u);
        /* Not started, and that must be visible rather than implied. */
        CHECK(!record->started);
    }
    CHECK_EQ_U32(kernel_thread_unstarted_count(), 1u);
}

static void test_argument_positions_are_not_off_by_one(void)
{
    /* An off-by-one in an argument index is silent, because the neighbouring value
     * is also a plausible pointer. Distinct sentinels make a shift detectable. */
    kernel_hle_init();
    kernel_thread_reset();
    (void)kernel_thread_register();

    uint32_t base = scratch_base();
    if (base == 0u) {
        return;
    }
    const uint32_t args[10] = {
        base, 0x11u, 0x22u, 0x33u, 0u, 0xAAAA0000u, 0xBBBB0000u, 0u, 0u, 0xCCCC0000u,
    };
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    CHECK(kernel_frame_build(&frame, base + 0x100u, 0x100u, args, 10u));
    CHECK_EQ_U32(kernel_hle_call(255u, &frame), STATUS_SUCCESS);

    uint32_t handle = 0u;
    CHECK(kernel_guest_read_u32(base, &handle));
    const kernel_thread_record *record = kernel_thread_find(handle);
    CHECK(record != NULL);
    if (record != NULL) {
        CHECK_EQ_U32(record->kernel_stack_size, 0x22u);
        CHECK_EQ_U32(record->tls_data_size, 0x33u);
        CHECK_EQ_U32(record->start_routine, 0xAAAA0000u);
        CHECK_EQ_U32(record->start_context, 0xBBBB0000u);
        CHECK_EQ_U32(record->system_routine, 0xCCCC0000u);
    }
}

/*
 * A FAILED CREATION MUST NOT LEAK ITS HANDLE. PsCreateSystemThreadEx issues the handle
 * before it can know the guest's out-parameter is writable, so a bad ThreadHandle pointer
 * used to strand one object-table entry per call. 300 failures is past the table's 256
 * slots, so a leak shows up as the final good call failing rather than as a count nobody
 * reads. 0x1000 is in range but unmapped, which the accessor now reports instead of
 * crashing the host.
 */
static void test_a_failed_creation_withdraws_its_handle(void)
{
    kernel_hle_init();
    kernel_thread_reset();
    kernel_object_reset(); /* earlier cases leave live handles; the count below must start at 0 */
    (void)kernel_thread_register();

    const uint32_t base = scratch_base();
    if (base == 0u) {
        return;
    }
    const uint32_t args_bad[10] = {0x1000u, 0u, 0x4000u, 0u, 0u, 0x123456u, 0u, 0u, 0u, 0x654321u};
    for (unsigned i = 0u; i < 300u; i++) {
        kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
        CHECK(kernel_frame_build(&frame, base + 0x100u, 0x100u, args_bad, 10u));
        CHECK_EQ_U32(kernel_hle_call(255u, &frame), STATUS_INVALID_PARAMETER);
    }
    CHECK_EQ_U32(kernel_object_live_count(), 0u);

    const uint32_t args_good[10] = {base, 0u, 0x4000u, 0u, 0u, 0x123456u, 0u, 0u, 0u, 0x654321u};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    CHECK(kernel_frame_build(&frame, base + 0x100u, 0x100u, args_good, 10u));
    CHECK_EQ_U32(kernel_hle_call(255u, &frame), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_object_live_count(), 1u);
}

static void test_a_null_out_pointer_is_refused(void)
{
    kernel_hle_init();
    kernel_thread_reset();
    (void)kernel_thread_register();
    kernel_hle_set_log(counting_log);
    log_lines = 0;

    uint32_t base = scratch_base();
    if (base == 0u) {
        kernel_hle_set_log(NULL);
        return;
    }
    const uint32_t args[10] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    CHECK(kernel_frame_build(&frame, base, 0x100u, args, 10u));

    CHECK_EQ_U32(kernel_hle_call(255u, &frame), STATUS_INVALID_PARAMETER);
    CHECK(log_lines > 0);
    /* No record may be consumed by a refused call. */
    CHECK_EQ_U32(kernel_thread_unstarted_count(), 0u);
    kernel_hle_set_log(NULL);
}

static void test_handles_are_distinct_and_synthetic_looking(void)
{
    kernel_hle_init();
    kernel_thread_reset();
    (void)kernel_thread_register();

    uint32_t base = scratch_base();
    if (base == 0u) {
        return;
    }
    uint32_t first = 0u;
    uint32_t second = 0u;
    for (unsigned i = 0u; i < 2u; i++) {
        const uint32_t slot = base + i * 4u;
        const uint32_t args[10] = {slot, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
        kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
        CHECK(kernel_frame_build(&frame, base + 0x200u + i * 0x100u, 0x100u, args, 10u));
        CHECK_EQ_U32(kernel_hle_call(255u, &frame), STATUS_SUCCESS);
    }
    CHECK(kernel_guest_read_u32(base, &first));
    CHECK(kernel_guest_read_u32(base + 4u, &second));
    CHECK(first != second);
    /* A handle must not look like a small integer or an array index. */
    CHECK(first > 0x10000u);
    CHECK_EQ_U32(kernel_thread_unstarted_count(), 2u);
}

/* ------------------------------------------------------------------------- *
 * Ordinals 231 NtSuspendThread, 224 NtResumeThread, 143 KeSetBasePriorityThread and
 * 144 KeSetDisableBoostThread. Every number below is a LITERAL: the ordinals, the NTSTATUS
 * values (nt_status.h has none of the three thread ones, so they are written out) and the
 * counts. Reading expectations through the module's own macros would move the assertion with
 * the code.
 * ------------------------------------------------------------------------- */

#define LIT_INVALID_HANDLE 0xC0000008u
#define LIT_ACCESS_VIOLATION 0xC0000005u
#define LIT_NOT_IMPLEMENTED 0xC0000002u
#define LIT_OBJECT_TYPE_MISMATCH 0xC0000024u
#define LIT_THREAD_IS_TERMINATING 0xC000004Bu

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_cond = PTHREAD_COND_INITIALIZER;
static unsigned fake_entered;
static bool fake_block;
static bool fake_release;

static bool fake_has_code(uint32_t guest_va)
{
    return guest_va >= 0x10000u;
}

static void fake_enter(const kernel_thread_launch *launch)
{
    (void)launch;
    pthread_mutex_lock(&fake_lock);
    fake_entered++;
    pthread_cond_broadcast(&fake_cond);
    while (fake_block && !fake_release) {
        (void)pthread_cond_wait(&fake_cond, &fake_lock);
    }
    pthread_mutex_unlock(&fake_lock);
}

static const kernel_thread_host_ops FAKE_OPS = {
    .has_code = fake_has_code,
    .enter = fake_enter,
    .terminate = NULL,
};

static void fake_reset(bool block)
{
    pthread_mutex_lock(&fake_lock);
    fake_entered = 0u;
    fake_block = block;
    fake_release = false;
    pthread_mutex_unlock(&fake_lock);
}

static void fake_release_all(void)
{
    pthread_mutex_lock(&fake_lock);
    fake_release = true;
    pthread_cond_broadcast(&fake_cond);
    pthread_mutex_unlock(&fake_lock);
}

static unsigned fake_entered_now(void)
{
    pthread_mutex_lock(&fake_lock);
    const unsigned entered = fake_entered;
    pthread_mutex_unlock(&fake_lock);
    return entered;
}

static bool wait_for_entries(unsigned count)
{
    struct timespec deadline;
    if (timespec_get(&deadline, TIME_UTC) != TIME_UTC) {
        return false;
    }
    deadline.tv_sec += 5;
    pthread_mutex_lock(&fake_lock);
    while (fake_entered < count) {
        if (pthread_cond_timedwait(&fake_cond, &fake_lock, &deadline) != 0) {
            break;
        }
    }
    const bool reached = fake_entered >= count;
    pthread_mutex_unlock(&fake_lock);
    return reached;
}

static uint32_t scratch;

static void begin_ordinal_case(void)
{
    kernel_hle_init();
    kernel_hle_set_log(counting_log);
    log_lines = 0;
    kernel_object_reset();
    CHECK(kernel_thread_set_host_ops(NULL));
    CHECK(kernel_thread_reset());
    CHECK_EQ_U32(kernel_thread_register(), 10u);
    scratch = scratch_base();
}

static void end_ordinal_case(void)
{
    fake_release_all();
    (void)kernel_thread_join_all(5000u);
    CHECK(kernel_thread_set_host_ops(NULL));
    CHECK(kernel_thread_reset());
    kernel_hle_set_log(NULL);
}

/* Create a thread through ordinal 255 and return its handle. */
static uint32_t make_thread(bool suspended)
{
    const uint32_t handle_slot = scratch + 0x2000u;
    const uint32_t args[10] = {handle_slot, 0u,        0x10000u,        0u, 0u,
                               0x003801D9u, 0u,        suspended ? 1u : 0u, 0u, 0x0037FE1Du};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    CHECK(kernel_frame_build(&frame, scratch + 0x2100u, 0x100u, args, 10u));
    CHECK_EQ_U32(kernel_hle_call(255u, &frame), STATUS_SUCCESS);
    uint32_t handle = 0u;
    CHECK(kernel_guest_read_u32(handle_slot, &handle));
    return handle;
}

/* Two-argument call. `out` is where the result pointer argument points. */
static uint32_t call2(unsigned ordinal, uint32_t first, uint32_t second)
{
    const uint32_t args[2] = {first, second};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    CHECK(kernel_frame_build(&frame, scratch + 0x300u, 0x100u, args, 2u));
    return kernel_hle_call(ordinal, &frame);
}

static void test_the_four_ordinals_are_bound(void)
{
    begin_ordinal_case();
    const unsigned ordinals[4] = {143u, 144u, 224u, 231u};
    for (unsigned i = 0u; i < 4u; i++) {
        const kernel_entry *entry = kernel_hle_entry(ordinals[i]);
        CHECK(entry != NULL);
        if (entry != NULL) {
            CHECK(entry->state == KERNEL_ENTRY_IMPLEMENTED);
        }
    }
    end_ordinal_case();
}

static void test_resume_starts_a_suspended_thread_exactly_once(void)
{
    begin_ordinal_case();
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
    fake_reset(false);

    const uint32_t handle = make_thread(true);
    kernel_thread_record record;
    CHECK(kernel_thread_get(handle, &record));
    CHECK_EQ_U32(record.suspend_count, 1u);
    CHECK(!record.started);
    CHECK_EQ_U32(fake_entered_now(), 0u);

    const uint32_t out = scratch + 0x40u;
    CHECK(kernel_guest_write_u32(out, 0x77777777u));
    CHECK_EQ_U32(call2(224u, handle, out), 0u);
    CHECK(wait_for_entries(1u));
    uint32_t previous = 0u;
    CHECK(kernel_guest_read_u32(out, &previous));
    CHECK_EQ_U32(previous, 1u);
    CHECK(kernel_thread_get(handle, &record));
    CHECK(record.started);
    CHECK_EQ_U32(record.suspend_count, 0u);
    CHECK_EQ_U32(kernel_thread_started_count(), 1u);

    /* A second resume of a thread that is not suspended reports 0 and starts nothing. */
    CHECK(kernel_guest_write_u32(out, 0x77777777u));
    CHECK_EQ_U32(call2(224u, handle, out), 0u);
    CHECK(kernel_guest_read_u32(out, &previous));
    CHECK_EQ_U32(previous, 0u);
    CHECK_EQ_U32(kernel_thread_started_count(), 1u);
    CHECK_EQ_U32(kernel_thread_join_all(5000u), 0u);
    CHECK_EQ_U32(fake_entered_now(), 1u);
    end_ordinal_case();
}

/* The guest's wrapper passes the address of its OWN argument slot as the out pointer:
 * `lea eax,[ebp+8]; push eax; push [ebp+8]`. The handle must be read before the count is
 * written over it. */
static void test_the_out_pointer_may_alias_the_handle_argument(void)
{
    begin_ordinal_case();
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
    fake_reset(false);

    const uint32_t handle = make_thread(true);
    const uint32_t frame_base = scratch + 0x300u;
    const uint32_t args[2] = {handle, frame_base + 4u};
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    CHECK(kernel_frame_build(&frame, frame_base, 0x100u, args, 2u));
    CHECK_EQ_U32(kernel_hle_call(224u, &frame), 0u);
    uint32_t slot = 0u;
    CHECK(kernel_guest_read_u32(frame_base + 4u, &slot));
    CHECK_EQ_U32(slot, 1u);
    CHECK(wait_for_entries(1u));
    end_ordinal_case();
}

static void test_a_null_out_pointer_is_allowed(void)
{
    begin_ordinal_case();
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
    fake_reset(false);
    const uint32_t handle = make_thread(true);
    CHECK_EQ_U32(call2(224u, handle, 0u), 0u);
    CHECK(wait_for_entries(1u));
    const uint32_t other = make_thread(true);
    CHECK_EQ_U32(call2(231u, other, 0u), 0u);
    kernel_thread_record record;
    CHECK(kernel_thread_get(other, &record));
    CHECK_EQ_U32(record.suspend_count, 2u);
    end_ordinal_case();
}

static void test_suspend_of_an_unstarted_thread_counts_and_resume_unwinds(void)
{
    begin_ordinal_case();
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
    fake_reset(false);
    const uint32_t handle = make_thread(true);
    const uint32_t out = scratch + 0x40u;
    uint32_t previous = 0u;

    CHECK_EQ_U32(call2(231u, handle, out), 0u);
    CHECK(kernel_guest_read_u32(out, &previous));
    CHECK_EQ_U32(previous, 1u);

    /* Count 2 -> 1: still suspended, so nothing may start. */
    CHECK_EQ_U32(call2(224u, handle, out), 0u);
    CHECK(kernel_guest_read_u32(out, &previous));
    CHECK_EQ_U32(previous, 2u);
    CHECK_EQ_U32(fake_entered_now(), 0u);
    CHECK_EQ_U32(kernel_thread_started_count(), 0u);

    /* Count 1 -> 0: starts. */
    CHECK_EQ_U32(call2(224u, handle, out), 0u);
    CHECK(kernel_guest_read_u32(out, &previous));
    CHECK_EQ_U32(previous, 1u);
    CHECK(wait_for_entries(1u));
    end_ordinal_case();
}

static void test_suspend_of_a_running_thread_is_refused_and_counted(void)
{
    begin_ordinal_case();
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
    fake_reset(true);
    const uint32_t handle = make_thread(false);
    CHECK(wait_for_entries(1u));

    const uint32_t out = scratch + 0x40u;
    CHECK(kernel_guest_write_u32(out, 0x77777777u));
    log_lines = 0;
    CHECK_EQ_U32(call2(231u, handle, out), LIT_NOT_IMPLEMENTED);
    CHECK(log_lines > 0);
    uint32_t untouched = 0u;
    CHECK(kernel_guest_read_u32(out, &untouched));
    CHECK_EQ_U32(untouched, 0x77777777u);
    CHECK_EQ_U32(kernel_thread_unhonoured_suspend_count(), 1u);
    kernel_thread_record record;
    CHECK(kernel_thread_get(handle, &record));
    CHECK_EQ_U32(record.suspend_count, 0u);

    CHECK_EQ_U32(call2(231u, handle, 0u), LIT_NOT_IMPLEMENTED);
    CHECK_EQ_U32(kernel_thread_unhonoured_suspend_count(), 2u);
    end_ordinal_case();
}

static void test_a_finished_thread_is_reported(void)
{
    begin_ordinal_case();
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
    fake_reset(false);
    const uint32_t handle = make_thread(false);
    CHECK(wait_for_entries(1u));
    CHECK_EQ_U32(kernel_thread_join_all(5000u), 0u);

    const uint32_t out = scratch + 0x40u;
    CHECK(kernel_guest_write_u32(out, 0x77777777u));
    log_lines = 0;
    CHECK_EQ_U32(call2(231u, handle, out), LIT_THREAD_IS_TERMINATING);
    CHECK(log_lines > 0);
    uint32_t value = 0u;
    CHECK(kernel_guest_read_u32(out, &value));
    CHECK_EQ_U32(value, 0x77777777u);

    log_lines = 0;
    CHECK_EQ_U32(call2(224u, handle, out), 0u);
    CHECK(log_lines > 0);
    CHECK(kernel_guest_read_u32(out, &value));
    CHECK_EQ_U32(value, 0u);
    CHECK_EQ_U32(fake_entered_now(), 1u);
    end_ordinal_case();
}

static void test_unknown_and_non_thread_handles_are_refused(void)
{
    begin_ordinal_case();
    const uint32_t out = scratch + 0x40u;
    CHECK(kernel_guest_write_u32(out, 0x77777777u));
    log_lines = 0;
    CHECK_EQ_U32(call2(231u, 0x12345678u, out), LIT_INVALID_HANDLE);
    CHECK_EQ_U32(call2(224u, 0x12345678u, out), LIT_INVALID_HANDLE);
    CHECK(log_lines >= 2);
    uint32_t value = 0u;
    CHECK(kernel_guest_read_u32(out, &value));
    CHECK_EQ_U32(value, 0x77777777u);

    /* A live handle of another kind must not be treated as a thread. */
    const uint32_t file_handle = kernel_object_create(KERNEL_OBJECT_FILE, 0u);
    CHECK(file_handle != 0u);
    CHECK_EQ_U32(call2(231u, file_handle, out), LIT_OBJECT_TYPE_MISMATCH);
    CHECK_EQ_U32(call2(224u, file_handle, out), LIT_OBJECT_TYPE_MISMATCH);

    /* 143 and 144 return a plain value, not a status, so a refusal is 0 and counts nothing. */
    CHECK_EQ_U32(call2(143u, 0x12345678u, 5u), 0u);
    CHECK_EQ_U32(call2(144u, file_handle, 1u), 0u);
    CHECK_EQ_U32(kernel_thread_priority_ignored_count(), 0u);
    end_ordinal_case();
}

static void test_an_unwritable_out_pointer_is_refused_before_acting(void)
{
    begin_ordinal_case();
    CHECK(kernel_thread_set_host_ops(&FAKE_OPS));
    fake_reset(false);
    const uint32_t handle = make_thread(true);
    /* In range but unmapped. */
    CHECK_EQ_U32(call2(224u, handle, 0x1000u), LIT_ACCESS_VIOLATION);
    CHECK_EQ_U32(call2(231u, handle, 0x1000u), LIT_ACCESS_VIOLATION);
    kernel_thread_record record;
    CHECK(kernel_thread_get(handle, &record));
    CHECK_EQ_U32(record.suspend_count, 1u);
    CHECK(!record.started);
    CHECK_EQ_U32(fake_entered_now(), 0u);
    end_ordinal_case();
}

static void test_resume_without_host_ops_reports_and_does_not_start(void)
{
    begin_ordinal_case();
    const uint32_t handle = make_thread(true);
    const uint32_t out = scratch + 0x40u;
    log_lines = 0;
    CHECK_EQ_U32(call2(224u, handle, out), 0u);
    CHECK(log_lines > 0);
    uint32_t previous = 0u;
    CHECK(kernel_guest_read_u32(out, &previous));
    CHECK_EQ_U32(previous, 1u);
    kernel_thread_record record;
    CHECK(kernel_thread_get(handle, &record));
    CHECK_EQ_U32(record.suspend_count, 0u);
    CHECK(!record.started);
    CHECK_EQ_U32(kernel_thread_running_count(), 0u);
    end_ordinal_case();
}

static void test_priority_and_boost_are_recorded_and_announced(void)
{
    begin_ordinal_case();
    const uint32_t handle = make_thread(true);
    kernel_thread_record record;
    CHECK(kernel_thread_get(handle, &record));
    CHECK(!record.base_priority_set);
    CHECK(!record.boost_disabled);

    CHECK_EQ_U32(kernel_object_register(), 7u);
    const uint32_t out = scratch + 0x2400u;
    const uint32_t ref_args[3] = {handle, 0u, out};
    kernel_call_frame ref_frame = {0};
    CHECK(kernel_frame_build(&ref_frame, scratch + 0x2500u, 16u, ref_args, 3u));
    CHECK_EQ_U32(kernel_hle_call(246u, &ref_frame), STATUS_SUCCESS);
    uint32_t body = 0u;
    CHECK(kernel_guest_read_u32(out, &body));
    CHECK_EQ_U32(body, record.control_base + KERNEL_THREAD_KTHREAD_OFFSET);
    CHECK_EQ_U32(call2(143u, handle, 99u), 0u);
    CHECK(kernel_thread_get(handle, &record));
    CHECK(!record.base_priority_set);
    log_lines = 0;
    CHECK_EQ_U32(call2(143u, body, 2u), 0u);
    CHECK(log_lines > 0);
    CHECK(kernel_thread_get(handle, &record));
    CHECK(record.base_priority_set);
    CHECK_EQ_U32((uint32_t)record.base_priority_increment, 2u);

    /* The previous increment comes back, and a negative one survives the round trip. */
    CHECK_EQ_U32(call2(143u, body, 0xFFFFFFF0u), 2u);
    CHECK(kernel_thread_get(handle, &record));
    CHECK_EQ_U32((uint32_t)record.base_priority_increment, 0xFFFFFFF0u);

    /* BOOLEAN is the low byte only: 0x101 disables, 0x100 does not. */
    CHECK_EQ_U32(call2(144u, body, 0x101u), 0u);
    CHECK(kernel_thread_get(handle, &record));
    CHECK(record.boost_disabled);
    CHECK_EQ_U32(call2(144u, body, 0x100u), 1u);
    CHECK(kernel_thread_get(handle, &record));
    CHECK(!record.boost_disabled);
    CHECK_EQ_U32(kernel_thread_priority_ignored_count(), 4u);
    kernel_frame_set_registers(&ref_frame, body, 0u);
    (void)kernel_hle_call(250u, &ref_frame);
    end_ordinal_case();
}

static void test_too_few_arguments_are_refused(void)
{
    begin_ordinal_case();
    const uint32_t handle = make_thread(true);
    const unsigned ordinals[4] = {143u, 144u, 224u, 231u};
    for (unsigned i = 0u; i < 4u; i++) {
        /* One argument, and a limit that ends right after it. */
        const uint32_t args[1] = {handle};
        kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
        CHECK(kernel_frame_build(&frame, scratch + 0x300u, 8u, args, 1u));
        const uint32_t result = kernel_hle_call(ordinals[i], &frame);
        if (ordinals[i] == 224u || ordinals[i] == 231u) {
            CHECK_EQ_U32(result, 0xC000000Du);
        } else {
            CHECK_EQ_U32(result, 0u);
        }
    }
    kernel_thread_record record;
    CHECK(kernel_thread_get(handle, &record));
    CHECK_EQ_U32(record.suspend_count, 1u);
    CHECK(!record.base_priority_set);
    CHECK(!record.boost_disabled);
    CHECK_EQ_U32(kernel_thread_priority_ignored_count(), 0u);
    end_ordinal_case();
}

typedef struct {
    kernel_fn handler;
    bool raised;
    bool okay;
} yield_worker;

static void *run_yield_worker(void *context)
{
    yield_worker *worker = context;
    kernel_sync_reset();
    if (worker->raised) {
        (void)kernel_hle_entry(129u)->handler(NULL);
    }
    worker->okay = true;
    for (unsigned i = 0u; i < 64u; i++) {
        const uint32_t expected = worker->raised ? STATUS_NOT_IMPLEMENTED
                                                 : STATUS_NO_YIELD_PERFORMED;
        if (worker->handler((void *)(uintptr_t)1u) != expected ||
            kernel_sync_current_irql() != (worker->raised ? KERNEL_IRQL_DISPATCH
                                                          : KERNEL_IRQL_PASSIVE)) {
            worker->okay = false;
        }
    }
    return NULL;
}

static void test_yield_declines_handoff_without_reading_a_frame(void)
{
    begin_ordinal_case();
    kernel_sync_reset();
    kernel_clock_reset();
    (void)kernel_clock_read();
    const uint64_t clock_before = kernel_clock_peek();
    const unsigned running_before = kernel_thread_running_count();
    const unsigned refused_before = kernel_thread_unhonoured_suspend_count();
    const kernel_entry *entry = kernel_hle_entry(238u);
    CHECK(entry && entry->state == KERNEL_ENTRY_IMPLEMENTED);
    kernel_call_frame untouched = {scratch, 0u, 0xA5A5u, 0x5A5Au, true,
                                   0xDEADBEEFu, true};
    kernel_call_frame before;
    memcpy(&before, &untouched, sizeof(before));
    CHECK_EQ_U32(kernel_hle_call(238u, &untouched), STATUS_NO_YIELD_PERFORMED);
    CHECK(memcmp(&before, &untouched, sizeof(before)) == 0);
    CHECK(kernel_guest_write_u32(scratch, 0xA55AA55Au));
    /* A deliberately invalid context proves the zero-argument handler does not
     * dereference a frame. Repeated calls must not manufacture clock progress. */
    for (unsigned i = 0u; i < 32u; i++) {
        CHECK_EQ_U32(kernel_hle_call(238u, (void *)(uintptr_t)1u),
                     STATUS_NO_YIELD_PERFORMED);
    }
    CHECK(kernel_clock_peek() == clock_before);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_PASSIVE);
    CHECK_EQ_U32(kernel_thread_running_count(), running_before);
    CHECK_EQ_U32(kernel_thread_unhonoured_suspend_count(), refused_before);
    (void)kernel_sync_register();
    CHECK_EQ_U32(kernel_hle_call(129u, NULL), KERNEL_IRQL_PASSIVE);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_DISPATCH);
    CHECK_EQ_U32(kernel_hle_call(238u, NULL), STATUS_NOT_IMPLEMENTED);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_DISPATCH);
    CHECK(kernel_clock_peek() == clock_before);
    kernel_sync_reset();
    pthread_t workers[4];
    yield_worker outcomes[4];
    bool started[4];
    for (unsigned i = 0u; i < 4u; i++) {
        outcomes[i] = (yield_worker){entry->handler, i == 3u, false};
        started[i] = pthread_create(&workers[i], NULL, run_yield_worker,
                                    &outcomes[i]) == 0;
        CHECK(started[i]);
    }
    for (unsigned i = 0u; i < 4u; i++) {
        if (started[i]) {
            CHECK(pthread_join(workers[i], NULL) == 0);
            CHECK(outcomes[i].okay);
        }
    }
    uint32_t sentinel = 0u;
    CHECK(kernel_guest_read_u32(scratch, &sentinel));
    CHECK_EQ_U32(sentinel, 0xA55AA55Au);
    CHECK(kernel_clock_peek() == clock_before);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_PASSIVE);
    end_ordinal_case();
}

int main(void)
{
    test_yield_declines_handoff_without_reading_a_frame();
    test_out_parameter_is_written();
    test_argument_positions_are_not_off_by_one();
    test_a_failed_creation_withdraws_its_handle();
    test_a_null_out_pointer_is_refused();
    test_handles_are_distinct_and_synthetic_looking();
    test_the_four_ordinals_are_bound();
    test_resume_starts_a_suspended_thread_exactly_once();
    test_the_out_pointer_may_alias_the_handle_argument();
    test_a_null_out_pointer_is_allowed();
    test_suspend_of_an_unstarted_thread_counts_and_resume_unwinds();
    test_suspend_of_a_running_thread_is_refused_and_counted();
    test_a_finished_thread_is_reported();
    test_unknown_and_non_thread_handles_are_refused();
    test_an_unwritable_out_pointer_is_refused_before_acting();
    test_resume_without_host_ops_reports_and_does_not_start();
    test_priority_and_boost_are_recorded_and_announced();
    test_too_few_arguments_are_refused();

    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("kernel_thread: all checks passed\n");
    return 0;
}
