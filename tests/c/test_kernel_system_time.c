/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ordinal 228, NtSetSystemTime(PLARGE_INTEGER NewTime, PLARGE_INTEGER OldTime OPTIONAL),
 * stdcall, TWO arguments.
 *
 * THE ONE MEASURED SITE is the XONLINE wrapper 0x00425813 (`push 0; lea eax,[esp+8]; push eax;
 * call [0x4759C0]; ret 8`), so the wrapper takes the 8-byte time BY VALUE as its two stack
 * words and hands the kernel its address with OldTime NULL. Its two callers (0x00427241 and
 * 0x0042758F, in the XONLINE reply handlers) build the value with RtlTimeFieldsToTime from the
 * TIME_FIELDS of a server reply (a block that is all zero, or exactly 1970-01-01, becomes 0),
 * call the wrapper unconditionally and ignore its result. So the measured modes are: OldTime
 * NULL, and a NewTime that can be zero.
 *
 * THE MODEL (INFERRED, NT contract): the guest wall clock is the host clock plus an offset, and
 * KeQuerySystemTime (128) reads through it. A set REBASES to the host (it does not stack on
 * an earlier set) and restarts the clock's monotonic guard so a backward set does not stall.
 * Zero and negative times are STATUS_INVALID_PARAMETER, a non-NULL OldTime is REFUSED
 * (STATUS_NOT_IMPLEMENTED, nothing changes), an unreadable NewTime is STATUS_ACCESS_VIOLATION.
 *
 * The host clock cannot be pinned, so every time check is a window of a few seconds around the
 * value just set, wide enough for a slow CI host and narrow enough that an unapplied offset,
 * a doubled offset or a swapped half is hundreds of years away.
 */

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_event.h"
#include "kernel_hle.h"
#include "nt_status.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        checks++;                                                                        \
        if (!(cond)) {                                                                   \
            failures++;                                                                  \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                     \
        }                                                                                \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                   \
    do {                                                                                 \
        checks++;                                                                        \
        const uint32_t a_ = (uint32_t)(actual);                                          \
        const uint32_t e_ = (uint32_t)(expected);                                        \
        if (a_ != e_) {                                                                  \
            failures++;                                                                  \
            printf("  FAIL %s:%d  %s == %#x, want %#x\n", __FILE__, __LINE__, #actual,   \
                   (unsigned)a_, (unsigned)e_);                                          \
        }                                                                                \
    } while (0)

static char captured[16384];
static size_t captured_len;

static int capture_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(captured + captured_len, sizeof(captured) - captured_len,
                                  format, args);
    va_end(args);
    if (written > 0) {
        captured_len += (size_t)written;
        if (captured_len >= sizeof(captured)) {
            captured_len = sizeof(captured) - 1u;
        }
    }
    return written;
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

#define ORD_KE_QUERY_SYSTEM_TIME 128u
#define ORD_NT_SET_SYSTEM_TIME 228u
#define STATUS_ACCESS_VIOLATION_CODE 0xC0000005u

#define SCRATCH_BYTES 0x1000u
#define FRAME_AT 0x000u
#define FRAME_BYTES 0x100u
#define NEW_TIME_AT 0x200u
#define OLD_TIME_AT 0x210u
#define OUT_AT 0x220u

/* 10^7 100ns units per second. */
#define SECOND 10000000ull
/* Windows-epoch times, derived independently of the module: 2005-06-01 00:00:00 UTC and
 * 1999-01-01 00:00:00 UTC are (days since 1601 * 86400) * 10^7. */
#define TIME_2005 ((152ull * 365ull + 38ull) * 86400ull * 10000000ull)
#define TIME_FUTURE (TIME_2005 + 900ull * 86400ull * SECOND)
#define TIME_PAST (TIME_2005 - 3000ull * 86400ull * SECOND)
/* How far past the value just set a read may be: a generous five seconds. */
#define WINDOW (5ull * SECOND)

static kernel_guest_ptr scratch;

static void setup(void)
{
    kernel_hle_init();
    kernel_event_reset();
    guest_mem_reset();
    /* ELEVEN: original 228 plus the genuine 150 periodic-timer binding. A dropped binding leaves a stub returning 0, which reads as
     * STATUS_SUCCESS with no clock change, so the count and the effect are both asserted. */
    CHECK_EQ_U32(kernel_event_register(), 11u);
    kernel_hle_set_log(capture_printer);
    captured[0] = '\0';
    captured_len = 0u;
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = SCRATCH_BYTES;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    request.contiguous = true;
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    if (scratch == 0u) {
        printf("  FATAL: could not allocate scratch (status %#x)\n", (unsigned)status);
        exit(EXIT_FAILURE);
    }
}

static void teardown(void)
{
    kernel_event_reset();
    kernel_hle_set_log(NULL);
    guest_mem_reset();
    scratch = 0u;
}

static uint32_t call_ordinal(unsigned ordinal, const uint32_t *args, unsigned count)
{
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (!kernel_frame_build(&frame, scratch + FRAME_AT, FRAME_BYTES, args, count)) {
        printf("  FATAL: could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    /* Clamped to exactly the arguments built, so a read of one more FAILS. */
    frame.stack_limit = scratch + FRAME_AT + (count + 1u) * 4u;
    return kernel_hle_call(ordinal, &frame);
}

static void write_time(uint32_t offset, uint64_t value)
{
    CHECK(kernel_guest_write_u32(scratch + offset, (uint32_t)(value & 0xFFFFFFFFu)));
    CHECK(kernel_guest_write_u32(scratch + offset + 4u, (uint32_t)(value >> 32)));
}

/* NtSetSystemTime(&value, NULL), the one measured shape. */
static uint32_t set_time(uint64_t value)
{
    write_time(NEW_TIME_AT, value);
    const uint32_t args[2] = {scratch + NEW_TIME_AT, 0u};
    return call_ordinal(ORD_NT_SET_SYSTEM_TIME, args, 2u);
}

/* KeQuerySystemTime through the real handler, the way the guest reads it. */
static uint64_t guest_time(void)
{
    const uint32_t args[1] = {scratch + OUT_AT};
    (void)call_ordinal(ORD_KE_QUERY_SYSTEM_TIME, args, 1u);
    uint32_t low = 0u;
    uint32_t high = 0u;
    CHECK(kernel_guest_read_u32(scratch + OUT_AT, &low));
    CHECK(kernel_guest_read_u32(scratch + OUT_AT + 4u, &high));
    return (uint64_t)low | ((uint64_t)high << 32);
}

static bool near_after(uint64_t actual, uint64_t set_value)
{
    return actual >= set_value && actual - set_value < WINDOW;
}

/* 228 is registered by name as implemented. MUTATION: drop the binding. */
static void test_registered_by_name(void)
{
    setup();
    const kernel_entry *entry = kernel_hle_entry(ORD_NT_SET_SYSTEM_TIME);
    CHECK(entry != NULL);
    CHECK(entry != NULL && entry->name != NULL && strcmp(entry->name, "NtSetSystemTime") == 0);
    CHECK(entry != NULL && entry->state == KERNEL_ENTRY_IMPLEMENTED);
    teardown();
}

/*
 * THE MEASURED SHAPE moves the guest clock. A forward set puts the next read just after the
 * set value, the clock keeps counting up, and 128 and the C accessor agree.
 *
 * MUTATION: return success without storing the offset, apply it with the wrong sign, swap the
 * two dwords of NewTime, or drop the offset from the accessor, and this fails.
 */
static void test_a_set_moves_the_guest_wall_clock(void)
{
    setup();
    const uint64_t before = guest_time();
    CHECK(before > TIME_2005 + 3000ull * 86400ull * SECOND);
    CHECK_EQ_U32(set_time(TIME_FUTURE), STATUS_SUCCESS);
    const uint64_t first = guest_time();
    CHECK(near_after(first, TIME_FUTURE));
    const uint64_t second = guest_time();
    CHECK(second > first);
    CHECK(second - first < WINDOW);
    CHECK(near_after(kernel_event_system_time(), TIME_FUTURE));
    CHECK_EQ_U32(kernel_event_system_time_set_count(), 1u);
    CHECK(captured_contains("NtSetSystemTime"));
    teardown();
}

/*
 * A BACKWARD SET MUST NOT STALL THE CLOCK. The old monotonic guard clamps a reading to
 * last + 1, so without a restart a clock set back by years would crawl at one unit per read
 * for years. A set also REBASES (offset = new - host) and does not stack on an earlier one.
 *
 * MUTATION: keep the monotonic floor across a set (the read after a backward set equals
 * last + 1 and not the new time), or add the new offset to the old one, and this fails.
 */
static void test_a_backward_set_does_not_stall_and_a_second_set_rebases(void)
{
    setup();
    CHECK_EQ_U32(set_time(TIME_FUTURE), STATUS_SUCCESS);
    const uint64_t high_read = guest_time();
    CHECK(near_after(high_read, TIME_FUTURE));
    CHECK_EQ_U32(set_time(TIME_PAST), STATUS_SUCCESS);
    const uint64_t low_read = guest_time();
    CHECK(near_after(low_read, TIME_PAST));
    CHECK(low_read < high_read);
    CHECK(near_after(guest_time(), TIME_PAST));
    CHECK_EQ_U32(set_time(TIME_2005), STATUS_SUCCESS);
    CHECK(near_after(guest_time(), TIME_2005));
    CHECK_EQ_U32(kernel_event_system_time_set_count(), 3u);
    teardown();
}

/*
 * A SET THAT IS NOT A TIME CHANGES NOTHING. Zero (the XONLINE converter produces it for a
 * reply with no time, and the caller passes it on), a negative value (top bit set) are
 * INVALID_PARAMETER. 2^63 - 1 is the largest accepted.
 *
 * MUTATION: accept zero, accept the top bit, or reject the largest positive, and this fails.
 */
static void test_zero_and_negative_times_are_invalid_and_change_nothing(void)
{
    setup();
    CHECK_EQ_U32(set_time(TIME_2005), STATUS_SUCCESS);
    CHECK_EQ_U32(set_time(0u), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(set_time(0x8000000000000000ull), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(set_time(0xFFFFFFFFFFFFFFFFull), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(set_time(0x8000000000000001ull), STATUS_INVALID_PARAMETER);
    CHECK(near_after(guest_time(), TIME_2005));
    CHECK_EQ_U32(kernel_event_system_time_set_count(), 1u);
    CHECK_EQ_U32(set_time(0x7FFFFFFFFFFFFFFFull - 100ull * SECOND), STATUS_SUCCESS);
    CHECK(guest_time() > 0x7FFFFFFFFFFFFFFFull - 105ull * SECOND);
    CHECK_EQ_U32(kernel_event_system_time_set_count(), 2u);
    teardown();
}

/*
 * OldTime is NOT a measured mode (the one site passes NULL), so a non-NULL one is REFUSED
 * with nothing written and the clock untouched, judged before the time is even read.
 *
 * MUTATION: honour or ignore OldTime (the clock moves, or the buffer is written), or judge
 * it after the set, and this fails.
 */
static void test_a_non_null_old_time_is_refused(void)
{
    setup();
    CHECK_EQ_U32(set_time(TIME_2005), STATUS_SUCCESS);
    write_time(OLD_TIME_AT, 0x1122334455667788ull);
    write_time(NEW_TIME_AT, TIME_FUTURE);
    const uint32_t args[2] = {scratch + NEW_TIME_AT, scratch + OLD_TIME_AT};
    CHECK_EQ_U32(call_ordinal(ORD_NT_SET_SYSTEM_TIME, args, 2u), STATUS_NOT_IMPLEMENTED);
    CHECK(near_after(guest_time(), TIME_2005));
    uint32_t low = 0u;
    uint32_t high = 0u;
    CHECK(kernel_guest_read_u32(scratch + OLD_TIME_AT, &low));
    CHECK(kernel_guest_read_u32(scratch + OLD_TIME_AT + 4u, &high));
    CHECK_EQ_U32(low, 0x55667788u);
    CHECK_EQ_U32(high, 0x11223344u);
    CHECK_EQ_U32(kernel_event_system_time_set_count(), 1u);
    CHECK(captured_contains("OldTime"));
    teardown();
}

/* Argument faults: a NULL or unreadable NewTime is an ACCESS_VIOLATION, a frame too short
 * for either argument is INVALID_PARAMETER, and the clock never moves. MUTATION: skip the NULL
 * check, treat an unreadable pointer as zero, or read fewer arguments. */
static void test_argument_faults_change_nothing(void)
{
    setup();
    CHECK_EQ_U32(set_time(TIME_2005), STATUS_SUCCESS);
    uint32_t args[2] = {0u, 0u};
    CHECK_EQ_U32(call_ordinal(ORD_NT_SET_SYSTEM_TIME, args, 2u), STATUS_ACCESS_VIOLATION_CODE);
    args[0] = 0x10u;
    CHECK_EQ_U32(call_ordinal(ORD_NT_SET_SYSTEM_TIME, args, 2u), STATUS_ACCESS_VIOLATION_CODE);
    /* The high dword falls past the end of the scratch region: half a time is not a time. */
    args[0] = scratch + SCRATCH_BYTES - 4u;
    CHECK_EQ_U32(call_ordinal(ORD_NT_SET_SYSTEM_TIME, args, 2u), STATUS_ACCESS_VIOLATION_CODE);
    CHECK_EQ_U32(call_ordinal(ORD_NT_SET_SYSTEM_TIME, args, 1u), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(call_ordinal(ORD_NT_SET_SYSTEM_TIME, args, 0u), STATUS_INVALID_PARAMETER);
    CHECK_EQ_U32(kernel_hle_call(ORD_NT_SET_SYSTEM_TIME, NULL), STATUS_INVALID_PARAMETER);
    CHECK(near_after(guest_time(), TIME_2005));
    CHECK_EQ_U32(kernel_event_system_time_set_count(), 1u);
    teardown();
}

/* A reset returns the host clock and the counter. MUTATION: leave either out of reset. */
static void test_reset_restores_the_host_clock(void)
{
    setup();
    CHECK_EQ_U32(set_time(TIME_PAST), STATUS_SUCCESS);
    CHECK(guest_time() < TIME_2005);
    kernel_event_reset();
    CHECK(guest_time() > TIME_2005 + 3000ull * 86400ull * SECOND);
    CHECK_EQ_U32(kernel_event_system_time_set_count(), 0u);
    CHECK(kernel_event_system_time_offset() == 0);
    teardown();
}

int main(void)
{
    printf("NtSetSystemTime (228) tests\n");
    test_registered_by_name();
    test_a_set_moves_the_guest_wall_clock();
    test_a_backward_set_does_not_stall_and_a_second_set_rebases();
    test_zero_and_negative_times_are_invalid_and_change_nothing();
    test_a_non_null_old_time_is_refused();
    test_argument_faults_change_nothing();
    test_reset_restores_the_host_clock();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
