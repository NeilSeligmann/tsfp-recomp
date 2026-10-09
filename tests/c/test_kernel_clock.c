/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The deterministic virtual time-stamp counter: `src/xbox/kernel_clock.c`.
 *
 * WHAT THIS SUITE EXISTS TO PROVE. A clock policy fails quietly in four ways, and each has
 * a test below that names the mutation which breaks it:
 *
 *   1. IT CAN CHANGE WHAT ALREADY WORKS. With no frame reported the counter must be the old
 *      one exactly (1000 per read from a first reading of 1000): every baseline in the repo
 *      was measured against it.
 *   2. IT CAN DRIFT. After `refresh` frames the floor must be exactly 733,333,333 ticks, for
 *      60 AND for 50, which do not divide the frequency. A truncating division loses 20
 *      ticks per second at 60 Hz and the loss is invisible until it is compared with a
 *      second clock.
 *   3. IT CAN HANG A POLL. A guest that spins on the clock with no frame must still reach any
 *      deadline, so the creep must survive the floor.
 *   4. IT CAN GO BACKWARDS. Across threads, and across a frame that lands behind a reading
 *      that has already crept past the floor.
 *
 * KeStallExecutionProcessor (151) adds a fifth failure, tested at the end of this file: a
 * stall that SPINS in real time, loses its sub-tick remainder, starts below the frame floor
 * (and so advances nothing), or accepts a garbage argument that jumps the clock by hours.
 *
 * FREE OF LIFTED CODE, THE XBE AND ANY DISC. Each case resets the clock first.
 */

#define _POSIX_C_SOURCE 200809L

#include "kernel_clock.h"

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

#define CHECK_EQ_U64(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                       \
        unsigned long long a_ = (unsigned long long)(actual);                           \
        unsigned long long e_ = (unsigned long long)(expected);                         \
        if (a_ != e_) {                                                                 \
            printf("FAIL %s:%d  %s == %llu, expected %llu\n", __FILE__, __LINE__,        \
                   #actual, a_, e_);                                                     \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

/* The old counter: 1000, 2000, 3000.
 * BREAKS THIS: changing KERNEL_CLOCK_READ_STEP_TICKS; starting from 0 instead of one step;
 * letting a read return before it has advanced. */
static void test_with_no_frame_it_is_the_old_counter(void)
{
    kernel_clock_reset();
    for (uint64_t index = 1u; index <= 5u; index++) {
        CHECK_EQ_U64(kernel_clock_read(), index * 1000u);
    }
    CHECK_EQ_U64(kernel_clock_peek(), 5000u);
}

/* Exactly one second after `refresh` frames, at both rates the title uses.
 * BREAKS THIS: dropping the remainder carry (60 Hz ends 20 ticks short, 50 Hz 33 short);
 * carrying against the wrong denominator; adding a rate instead of a period. */
static void test_a_second_of_frames_is_exactly_one_second(void)
{
    static const unsigned rates[] = {60u, 50u, 30u, 25u, 1u, 1000u, 7u};
    for (size_t index = 0; index < sizeof(rates) / sizeof(rates[0]); index++) {
        kernel_clock_reset();
        for (unsigned frame = 0; frame < rates[index]; frame++) {
            CHECK(kernel_clock_frame(rates[index]));
        }
        CHECK_EQ_U64(kernel_clock_peek(), KERNEL_CLOCK_FREQUENCY_HZ);
        CHECK_EQ_U64(kernel_clock_tick_count_ms(), 1000u);
    }
}

/* A frame raises the floor and a later read honours it, then creeps from there.
 * BREAKS THIS: dropping the floor comparison in kernel_clock_read (the read returns 1000,
 * ignoring the frame). */
static void test_the_floor_is_honoured_and_reads_creep_from_it(void)
{
    kernel_clock_reset();
    CHECK(kernel_clock_frame(60u));
    const uint64_t frame = KERNEL_CLOCK_FREQUENCY_HZ / 60u;
    CHECK_EQ_U64(kernel_clock_read(), frame);
    CHECK_EQ_U64(kernel_clock_read(), frame + 1000u);
    CHECK_EQ_U64(kernel_clock_read(), frame + 2000u);
}

/* Creep that has already run past the next floor is not pulled back, and the frame after it
 * does not move the clock backwards.
 * BREAKS THIS: assigning the floor instead of taking the maximum. */
static void test_a_frame_never_moves_the_clock_backwards(void)
{
    kernel_clock_reset();
    const uint64_t frame = KERNEL_CLOCK_FREQUENCY_HZ / 60u;
    uint64_t last = 0u;
    for (uint64_t reads = 0; reads < frame / 1000u + 50u; reads++) {
        last = kernel_clock_read();
    }
    CHECK(last > frame);
    CHECK(kernel_clock_frame(60u));
    const uint64_t after = kernel_clock_read();
    CHECK(after > last);
}

/* A spin on the clock with NO frame reaches a one second deadline: 733,333 reads, then it
 * stops. A mutation that stopped the creep (a floor-only clock) makes this loop forever, so
 * it is bounded by a read budget rather than trusted to terminate.
 * BREAKS THIS: any change that makes a read not advance the clock when no frame is pending. */
static void test_a_poll_with_no_frame_reaches_its_deadline(void)
{
    kernel_clock_reset();
    const uint64_t start = kernel_clock_read();
    const uint64_t budget = 2000000u;
    uint64_t reads = 0u;
    while (reads < budget && kernel_clock_read() - start < KERNEL_CLOCK_FREQUENCY_HZ) {
        reads++;
    }
    CHECK(reads < budget);
    CHECK_EQ_U64(reads, (KERNEL_CLOCK_FREQUENCY_HZ + 999u) / 1000u - 1u);
}

/* T372: the floor variant reports the raised floor, not the creeping counter, and a refused
 * rate reports nothing.
 * BREAKS THIS: returning the peek (the counter above the floor) instead of floor_ticks. */
static void test_frame_floor_reports_the_floor_not_the_counter(void)
{
    kernel_clock_reset();
    const uint64_t frame = KERNEL_CLOCK_FREQUENCY_HZ / 60u;
    uint64_t floor_after = 0u;
    CHECK(kernel_clock_frame_floor(60u, &floor_after));
    CHECK_EQ_U64(floor_after, frame);
    CHECK(kernel_clock_stall_us(100000u) > 0u); /* the counter runs well past the floor */
    CHECK(kernel_clock_peek() > 2u * frame);
    CHECK(kernel_clock_frame_floor(60u, &floor_after));
    CHECK_EQ_U64(floor_after, 2u * frame);
    CHECK(kernel_clock_frame_floor(60u, NULL));
    floor_after = 0xABCDu;
    CHECK(!kernel_clock_frame_floor(0u, &floor_after));
    CHECK_EQ_U64(floor_after, 0xABCDu);
}

/* A nonsense rate is refused and changes nothing, rather than dividing by zero or stalling.
 * BREAKS THIS: removing either bound. */
static void test_a_bad_refresh_rate_is_refused(void)
{
    kernel_clock_reset();
    CHECK(!kernel_clock_frame(0u));
    CHECK(!kernel_clock_frame(1001u));
    CHECK(!kernel_clock_frame(0xFFFFFFFFu));
    CHECK_EQ_U64(kernel_clock_peek(), 0u);
    CHECK_EQ_U64(kernel_clock_read(), 1000u);
}

/* Changing the refresh rate mid-run does not carry the old denominator's remainder.
 * 59 frames at 60 Hz leave a carry of 47 sixtieths. Switching to 50 Hz must start from
 * nothing, so its FIRST frame adds exactly the truncated period (47 + 33 would otherwise
 * cross 50 and add one tick early), and 50 frames still add exactly one second.
 * BREAKS THIS: not resetting the carry when the rate changes. */
static void test_a_rate_change_starts_a_fresh_remainder(void)
{
    kernel_clock_reset();
    for (unsigned frame = 0; frame < 59u; frame++) {
        CHECK(kernel_clock_frame(60u));
    }
    const uint64_t before = kernel_clock_peek();
    CHECK(kernel_clock_frame(50u));
    CHECK_EQ_U64(kernel_clock_peek() - before, KERNEL_CLOCK_FREQUENCY_HZ / 50u);
    for (unsigned frame = 1; frame < 50u; frame++) {
        CHECK(kernel_clock_frame(50u));
    }
    CHECK_EQ_U64(kernel_clock_peek() - before, KERNEL_CLOCK_FREQUENCY_HZ);
}

static void test_the_frequency_is_the_titles_constant(void)
{
    CHECK_EQ_U64(kernel_clock_frequency(), 733333333u);
    CHECK_EQ_U64(KERNEL_CLOCK_FREQUENCY_HZ, 0x2BB5C755u);
}

#define THREADS 4
#define READS_PER_THREAD 20000

static void *reader(void *unused)
{
    (void)unused;
    uint64_t previous = 0u;
    for (int index = 0; index < READS_PER_THREAD; index++) {
        const uint64_t reading = kernel_clock_read();
        if (reading <= previous) {
            return (void *)1;
        }
        previous = reading;
    }
    return NULL;
}

/* Guest threads are host threads. Every read advances by exactly one step, so the total after
 * N reads is N steps: a lost update shows as a short total, and a thread that saw time stand
 * still or go back returns non-NULL.
 * BREAKS THIS: removing the mutex (lost updates; run it a few times). */
static void test_concurrent_readers_lose_no_update_and_never_see_it_go_back(void)
{
    pthread_t threads[THREADS];
    kernel_clock_reset();
    for (int index = 0; index < THREADS; index++) {
        CHECK(pthread_create(&threads[index], NULL, reader, NULL) == 0);
    }
    for (int index = 0; index < THREADS; index++) {
        void *result = (void *)0;
        pthread_join(threads[index], &result);
        CHECK(result == NULL);
    }
    CHECK_EQ_U64(kernel_clock_peek(), (uint64_t)THREADS * READS_PER_THREAD * 1000u);
}

/* ------------------------------------------------------------------------------------------
 * KeStallExecutionProcessor (151).
 * --------------------------------------------------------------------------------------- */

static char captured[4096];
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

static void reset_capture(void)
{
    captured[0] = '\0';
    captured_len = 0u;
}

static kernel_guest_ptr stall_stack;

static void stall_setup(void)
{
    kernel_hle_init();
    guest_mem_reset();
    kernel_clock_reset();
    CHECK_EQ_U64(kernel_clock_register(), 3u);
    kernel_hle_set_log(capture_printer);
    reset_capture();
    const guest_region_request request = {
        .bytes = 4096u, .alignment = 4096u, .protect = PAGE_READWRITE, .state = MEM_COMMIT,
    };
    nt_status status = STATUS_SUCCESS;
    stall_stack = guest_region_alloc(&request, &status);
    CHECK(stall_stack != 0u);
}

static void stall_teardown(void)
{
    kernel_hle_set_log(NULL);
    guest_mem_reset();
}

/* One KeStallExecutionProcessor(microseconds) through the real dispatcher, as a stdcall
 * guest frame: return address, then the single argument. */
static uint32_t dispatch_stall(uint32_t microseconds)
{
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    const uint32_t args[1] = {microseconds};
    CHECK(kernel_frame_build(&frame, stall_stack, 0x200u, args, 1u));
    return kernel_hle_call(151u, &frame);
}

/* Exact conversion, rounded DOWN with the remainder carried: 733333333 ticks per second is
 * 733.333333 ticks per microsecond, so 1 us is 733 ticks and a third.
 * BREAKS THIS: rounding up per call (1 us becomes 734), a wrong frequency, or microseconds
 * treated as milliseconds. */
static void test_stall_converts_microseconds_to_ticks_exactly(void)
{
    kernel_clock_reset();
    CHECK_EQ_U64(kernel_clock_stall_us(0u), 0u);
    CHECK_EQ_U64(kernel_clock_peek(), 0u);
    CHECK_EQ_U64(kernel_clock_stall_us(1u), 733u);
    CHECK_EQ_U64(kernel_clock_stall_us(1000000u), KERNEL_CLOCK_FREQUENCY_HZ);
    CHECK_EQ_U64(kernel_clock_peek(), 733u + KERNEL_CLOCK_FREQUENCY_HZ);
    kernel_clock_reset();
    CHECK_EQ_U64(kernel_clock_stall_us(10000u), 7333333u);
    CHECK_EQ_U64(kernel_clock_peek(), 7333333u);
}

/* The carry: a million one-microsecond stalls are exactly one second, not 733 million ticks
 * short of it by truncation (333,333 ticks) nor over it by rounding up (666,667).
 * BREAKS THIS: dropping the remainder carry, rounding each call up. */
static void test_stall_carries_the_sub_tick_remainder(void)
{
    kernel_clock_reset();
    for (unsigned count = 0u; count < 1000000u; count++) {
        (void)kernel_clock_stall_us(1u);
    }
    CHECK_EQ_U64(kernel_clock_peek(), KERNEL_CLOCK_FREQUENCY_HZ);
    kernel_clock_reset();
    for (unsigned count = 0u; count < 100000u; count++) {
        (void)kernel_clock_stall_us(10u);
    }
    CHECK_EQ_U64(kernel_clock_peek(), KERNEL_CLOCK_FREQUENCY_HZ);
}

/* A reset forgets a half-accumulated remainder, so a replay is bit-identical.
 * BREAKS THIS: not clearing the carry in kernel_clock_reset. */
static void test_stall_remainder_does_not_survive_a_reset(void)
{
    kernel_clock_reset();
    (void)kernel_clock_stall_us(1u);
    (void)kernel_clock_stall_us(1u);
    kernel_clock_reset();
    CHECK_EQ_U64(kernel_clock_stall_us(1u), 733u);
    CHECK_EQ_U64(kernel_clock_peek(), 733u);
}

/* The point of the policy: a stall costs no host time. A ONE-SECOND stall (the refusal
 * bound) must finish in a small fraction of a real second, where a sleep or a spin would not.
 * BREAKS THIS: implementing the stall as nanosleep or a busy loop on CLOCK_MONOTONIC. */
static void test_stall_takes_no_real_time(void)
{
    kernel_clock_reset();
    struct timespec before;
    struct timespec after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    (void)kernel_clock_stall_us(KERNEL_CLOCK_STALL_MAX_US);
    for (unsigned count = 0u; count < 100u; count++) {
        (void)kernel_clock_stall_us(10000u);
    }
    clock_gettime(CLOCK_MONOTONIC, &after);
    const long elapsed_ms = (after.tv_sec - before.tv_sec) * 1000l +
                            (after.tv_nsec - before.tv_nsec) / 1000000l;
    CHECK(elapsed_ms < 200l);
    /* ... while two virtual seconds really passed. */
    CHECK_EQ_U64(kernel_clock_peek() / KERNEL_CLOCK_FREQUENCY_HZ, 2u);
}

/* A stall begun below the frame floor still adds its full time ON TOP of the floor. Without
 * the lift the next read's max(now, floor) swallows it and the guest's delay is a no-op.
 * BREAKS THIS: advancing now_ticks without first raising it to floor_ticks. */
static void test_stall_starts_from_the_frame_floor(void)
{
    kernel_clock_reset();
    CHECK(kernel_clock_frame(60u));
    const uint64_t frame = KERNEL_CLOCK_FREQUENCY_HZ / 60u;
    CHECK_EQ_U64(kernel_clock_peek(), frame);
    (void)kernel_clock_stall_us(1000u);
    CHECK_EQ_U64(kernel_clock_peek(), frame + 733333u);
    /* and the next read creeps from there, never back to the floor */
    CHECK_EQ_U64(kernel_clock_read(), frame + 733333u + 1000u);
}

/* A guest that waits for a hardware flag with stall(10) in a bounded loop and then reads the
 * clock sees the whole wait, and a later frame never pulls the clock back below it.
 * BREAKS THIS: a stall that does not advance the shared counter. */
static void test_stall_then_frame_never_goes_backwards(void)
{
    kernel_clock_reset();
    for (unsigned count = 0u; count < 6000u; count++) {
        (void)kernel_clock_stall_us(10000u); /* a minute of virtual time */
    }
    const uint64_t after_stalls = kernel_clock_peek();
    CHECK_EQ_U64(after_stalls, 60u * (uint64_t)KERNEL_CLOCK_FREQUENCY_HZ);
    CHECK(kernel_clock_frame(60u));
    CHECK(kernel_clock_peek() >= after_stalls);
    CHECK(kernel_clock_read() > after_stalls);
}

/* Through the dispatcher: the argument is read from the guest stack, the export returns
 * VOID as 0, and the clock advances by exactly the stalled time with NO read creep.
 * BREAKS THIS: reading the wrong stack slot (the return address is the slot below it),
 * advancing by a read step as well, registering the wrong ordinal. */
static void test_dispatch_reads_the_stack_argument_and_advances_the_clock(void)
{
    stall_setup();
    CHECK_EQ_U64(dispatch_stall(10u), 0u);
    CHECK_EQ_U64(kernel_clock_peek(), 7333u);
    CHECK_EQ_U64(dispatch_stall(0x2710u), 0u);
    CHECK_EQ_U64(kernel_clock_peek(), 7333u + 7333333u);
    CHECK_EQ_U64((uint64_t)captured_len, 0u);
    const kernel_entry *entry = kernel_hle_entry(151u);
    CHECK(entry != NULL && entry->state == KERNEL_ENTRY_IMPLEMENTED);
    CHECK(entry != NULL && strcmp(entry->name, "KeStallExecutionProcessor") == 0);
    stall_teardown();
}

/* The largest accepted request is the bound itself, one past it is refused: nothing advances
 * and the report names the ordinal, the value and the bound.
 * BREAKS THIS: dropping the bound, or an off-by-one in it (> versus >=). */
static void test_dispatch_refuses_an_unmeasured_magnitude_loudly(void)
{
    stall_setup();
    CHECK_EQ_U64(dispatch_stall(KERNEL_CLOCK_STALL_MAX_US), 0u);
    const uint64_t at_bound = kernel_clock_peek();
    CHECK_EQ_U64(at_bound, KERNEL_CLOCK_FREQUENCY_HZ);
    CHECK_EQ_U64((uint64_t)captured_len, 0u);

    CHECK_EQ_U64(dispatch_stall(KERNEL_CLOCK_STALL_MAX_US + 1u), 0u);
    CHECK_EQ_U64(kernel_clock_peek(), at_bound);
    CHECK(strstr(captured, "KeStallExecutionProcessor") != NULL);
    CHECK(strstr(captured, "1000001") != NULL);
    CHECK(strstr(captured, "refused") != NULL);

    reset_capture();
    CHECK_EQ_U64(dispatch_stall(0xFFFFFFFFu), 0u);
    CHECK_EQ_U64(kernel_clock_peek(), at_bound);
    CHECK(strstr(captured, "refused") != NULL);
    stall_teardown();
}

/* A frame the guest stack cannot supply an argument for is reported and changes nothing,
 * as is a call with no frame at all. A stall of unknown length must not pass for stall(0).
 * BREAKS THIS: defaulting the argument to 0, or dereferencing a NULL context. */
static void test_dispatch_with_no_readable_argument_is_reported(void)
{
    stall_setup();
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    frame.stack_ptr = 0xFFFFFFF0u;
    CHECK_EQ_U64(kernel_hle_call(151u, &frame), 0u);
    CHECK_EQ_U64(kernel_clock_peek(), 0u);
    CHECK(strstr(captured, "KeStallExecutionProcessor") != NULL);

    reset_capture();
    CHECK_EQ_U64(kernel_hle_call(151u, NULL), 0u);
    CHECK_EQ_U64(kernel_clock_peek(), 0u);
    CHECK(strstr(captured, "KeStallExecutionProcessor") != NULL);
    stall_teardown();
}

/* The stall publishes KeTickCount like any other advance: 10 ms of stalls is 10 ms.
 * BREAKS THIS: advancing now_ticks without publish_tick_count. */
static void test_stall_publishes_the_tick_count(void)
{
    stall_setup();
    CHECK(kernel_clock_bind_tick_count(stall_stack + 16u));
    for (unsigned count = 0u; count < 100u; count++) {
        (void)dispatch_stall(10000u);
    }
    uint32_t published = 0u;
    CHECK(kernel_guest_read_u32(stall_stack + 16u, &published));
    CHECK_EQ_U64(published, 1000u);
    CHECK(kernel_clock_bind_tick_count(0u));
    stall_teardown();
}

/* Concurrent stalls lose no update: N threads each stalling 3 us M times add
 * exactly N*M*3 us of ticks, with the remainder carry intact across threads.
 * BREAKS THIS: touching the counter or the carry outside the mutex. */
static void *staller(void *unused)
{
    (void)unused;
    for (int index = 0; index < 20000; index++) {
        (void)kernel_clock_stall_us(3u);
    }
    return NULL;
}

static void test_concurrent_stalls_lose_no_update(void)
{
    pthread_t threads[THREADS];
    kernel_clock_reset();
    for (int index = 0; index < THREADS; index++) {
        CHECK(pthread_create(&threads[index], NULL, staller, NULL) == 0);
    }
    for (int index = 0; index < THREADS; index++) {
        pthread_join(threads[index], NULL);
    }
    /* 80,000 stalls of 3 us = 240,000 us = 175,999,999.92 ticks, floored once. */
    CHECK_EQ_U64(kernel_clock_peek(), (uint64_t)KERNEL_CLOCK_FREQUENCY_HZ * 240000u / 1000000u);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_with_no_frame_it_is_the_old_counter();
    test_a_second_of_frames_is_exactly_one_second();
    test_the_floor_is_honoured_and_reads_creep_from_it();
    test_a_frame_never_moves_the_clock_backwards();
    test_a_poll_with_no_frame_reaches_its_deadline();
    test_a_bad_refresh_rate_is_refused();
    test_frame_floor_reports_the_floor_not_the_counter();
    test_a_rate_change_starts_a_fresh_remainder();
    test_the_frequency_is_the_titles_constant();
    test_concurrent_readers_lose_no_update_and_never_see_it_go_back();
    test_stall_converts_microseconds_to_ticks_exactly();
    test_stall_carries_the_sub_tick_remainder();
    test_stall_remainder_does_not_survive_a_reset();
    test_stall_takes_no_real_time();
    test_stall_starts_from_the_frame_floor();
    test_stall_then_frame_never_goes_backwards();
    test_dispatch_reads_the_stack_argument_and_advances_the_clock();
    test_dispatch_refuses_an_unmeasured_magnitude_loudly();
    test_dispatch_with_no_readable_argument_is_reported();
    test_stall_publishes_the_tick_count();
    test_concurrent_stalls_lose_no_update();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
