/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Critical sections: ordinals 277 Enter, 294 Leave, 291 Initialize.
 *
 * WHAT THESE TESTS ARE FOR. 294 is the second most-called ordinal in the image and
 * 277 is where the guest's main thread stops, so a plausible-but-wrong
 * implementation here would be felt everywhere and diagnosed nowhere. Two
 * properties in particular are the ones a careless implementation gets wrong, and
 * each has a test named after it:
 *
 *   - A critical section that never passed through ordinal 291 must still work.
 *     Roughly 105 of the 114 Leave calls act on one of the three the XBE
 *     initialises from image data, and the very first critical-section call the
 *     guest makes (0x0037FBF5, on 0x00549148) is one of them.
 *   - A Leave by a thread that does not hold the lock must not pass silently.
 *     Releasing a lock somebody else holds lets a second thread into a region the
 *     first is still inside, and the corruption surfaces arbitrarily far away.
 *
 * DELIBERATELY FREE OF LIFTED CODE AND OF THE XBE. Everything runs against scratch
 * memory from the region allocator. In particular it does NOT use
 * GUEST_CS_VA_DSOUND/XPP/TEXT as critical-section pointers: those are IMAGE
 * addresses, only mapped when an XBE is loaded, so a unit test using one would be
 * dereferencing an unmapped page.
 *
 * THE EXPECTED BYTES ARE WRITTEN OUT LONGHAND HERE, ON PURPOSE. `write_image_cs`
 * and the Initialize assertions use the literal dwords measured in
 * docs/guest-structs.md rather than the GUEST_CS_* macros the implementation
 * composes them from. Sharing the macros would make the test agree with the
 * implementation by construction; spelling the measurement out independently means
 * a change to either one has to be justified against the other.
 */

#include "kernel_critsec.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "guest_mem.h"
#include "guest_structs.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "nt_status.h"

static int failures;
static int log_lines;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        uint32_t a_ = (uint32_t)(actual);                                               \
        uint32_t e_ = (uint32_t)(expected);                                             \
        if (a_ != e_) {                                                                 \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__,         \
                   #actual, (unsigned)a_, (unsigned)e_);                                \
            failures++;                                                                 \
        }                                                                               \
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

/* The scratch page. Frames for two threads, then well-separated critical
 * sections: each test that can leave a lock held uses an address of its own, so
 * one case cannot wedge the next. */
#define SCRATCH_BYTES 4096u
#define FRAME_BYTES 32u
#define FRAME_A_OFF 0u
#define FRAME_B_OFF 32u
#define CS_SLOT_STRIDE 64u
#define CS_SLOT_BASE 128u

static kernel_guest_ptr scratch;

static kernel_guest_ptr frame_a(void)
{
    return (kernel_guest_ptr)(scratch + FRAME_A_OFF);
}

static kernel_guest_ptr frame_b(void)
{
    return (kernel_guest_ptr)(scratch + FRAME_B_OFF);
}

/* Critical section number `n`, 64 bytes apart so a 28-byte object plus a margin
 * never overlaps its neighbour. */
static kernel_guest_ptr cs_slot(unsigned n)
{
    return (kernel_guest_ptr)(scratch + CS_SLOT_BASE + n * CS_SLOT_STRIDE);
}

/* Drive a handler the way the dispatcher does, never by calling it directly. */
static uint32_t call_cs(unsigned ordinal, kernel_guest_ptr frame_base, kernel_guest_ptr cs)
{
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    const uint32_t args[1] = {cs};
    if (!kernel_frame_build(&frame, frame_base, FRAME_BYTES, args, 1u)) {
        printf("  FATAL: could not build a call frame\n");
        exit(EXIT_FAILURE);
    }
    return kernel_hle_call(ordinal, &frame);
}

static uint32_t read_u32(kernel_guest_ptr addr)
{
    uint32_t value = 0xDEADBEEFu;
    if (!kernel_guest_read_u32(addr, &value)) {
        printf("  FATAL: could not read guest memory at %#x\n", (unsigned)addr);
        exit(EXIT_FAILURE);
    }
    return value;
}

/*
 * Lay down the exact bytes the XBE ships for its three static critical sections,
 * WITHOUT going anywhere near ordinal 291.
 *
 * This is the whole scenario the implementation has to survive: a lock that exists
 * and is initialised, that we have never been told about. Measured in
 * docs/guest-structs.md as 7 dwords, byte-identical across all three instances
 * modulo the self-pointer.
 */
static void write_image_cs(kernel_guest_ptr cs)
{
    (void)kernel_guest_write_u32(cs + 0x00u, 0x00040001u); /* Type=1 Abs=0 Size=4 Ins=0 */
    (void)kernel_guest_write_u32(cs + 0x04u, 0x00000000u);
    (void)kernel_guest_write_u32(cs + 0x08u, cs + 0x08u); /* wait list, self-linked */
    (void)kernel_guest_write_u32(cs + 0x0Cu, cs + 0x08u);
    (void)kernel_guest_write_u32(cs + 0x10u, 0xFFFFFFFFu); /* lock_count */
    (void)kernel_guest_write_u32(cs + 0x14u, 0x00000000u);
    (void)kernel_guest_write_u32(cs + 0x18u, 0x00000000u);
}

static void fill(kernel_guest_ptr base, uint32_t bytes, uint32_t pattern)
{
    for (uint32_t off = 0u; off < bytes; off += 4u) {
        (void)kernel_guest_write_u32(base + off, pattern);
    }
}

static void sleep_ms(long ms)
{
    struct timespec request;
    request.tv_sec = ms / 1000L;
    request.tv_nsec = (ms % 1000L) * 1000000L;
    (void)nanosleep(&request, NULL);
}

/*
 * THE WATCHDOG, AND WHY THIS SUITE NEEDS ONE.
 *
 * The characteristic failure of a lock implementation is not a wrong value, it is
 * a deadlock -- and a hung test reports nothing at all. Mutation testing made that
 * concrete: inverting the ownership test in Leave, or deleting the
 * already-held fast path in Enter, both hang the process instead of failing an
 * assertion, so the one piece of information worth having (WHICH property broke)
 * is exactly the piece that is lost.
 *
 * So a watchdog thread names the test that stopped making progress and exits
 * non-zero. It turns every deadlock in this module into an ordinary named failure.
 */
#define WATCHDOG_MS 15000
#define WATCHDOG_POLL_MS 50

static _Atomic(const char *) current_test;
static _Atomic int all_tests_done;

static void *watchdog(void *raw)
{
    (void)raw;
    const char *watched = NULL;
    int stable_ms = 0;
    while (!all_tests_done) {
        const char *now = current_test;
        if (now != NULL && now == watched) {
            stable_ms += WATCHDOG_POLL_MS;
            if (stable_ms >= WATCHDOG_MS) {
                printf("FAIL watchdog: %s made no progress for %d ms -- deadlocked\n",
                       now, WATCHDOG_MS);
                fflush(stdout);
                /* _Exit, not exit: at this point another thread is holding a lock
                 * forever, so anything that tries to run an atexit handler through
                 * this module would hang again and lose the message we just printed. */
                _Exit(1);
            }
        } else {
            watched = now;
            stable_ms = 0;
        }
        sleep_ms(WATCHDOG_POLL_MS);
    }
    return NULL;
}

/* Name the test before running it, so the watchdog can say which one wedged. */
#define RUN(fn)                                                                         \
    do {                                                                                \
        current_test = #fn;                                                             \
        fn();                                                                           \
    } while (0)

static void setup(void)
{
    kernel_hle_init();
    guest_mem_reset();
    kernel_critsec_reset();
    (void)kernel_critsec_register();

    guest_region_request request = {
        .bytes = SCRATCH_BYTES,
        .alignment = 0u,
        .lowest_physical = 0u,
        .highest_physical = 0u,
        .protect = PAGE_READWRITE,
        .state = MEM_COMMIT,
        .contiguous = true,
        .fixed_base = 0u,
    };
    nt_status status = STATUS_SUCCESS;
    scratch = guest_region_alloc(&request, &status);
    if (scratch == 0u) {
        printf("  FATAL: could not allocate scratch memory (status %#x)\n",
               (unsigned)status);
        exit(EXIT_FAILURE);
    }
    fill(scratch, SCRATCH_BYTES, 0u);
}

static void teardown(void)
{
    kernel_hle_set_log(NULL);
    kernel_critsec_reset();
    guest_mem_reset();
    scratch = 0u;
}

/* Each case starts from a clean table but keeps the same scratch page. */
static void fresh(void)
{
    kernel_critsec_reset();
    kernel_hle_set_log(NULL);
    log_lines = 0;
}


/* --- the cases ------------------------------------------------------------ */

static void test_registration(void)
{
    kernel_hle_init();
    CHECK_EQ_U32(kernel_critsec_register(), 3u);

    /* Exactly the three ordinals TSFP imports from this group. The other five --
     * 278, 295, 306, 101, 122 -- have no thunk slot in the XBE, so they are
     * deliberately absent and must stay absent: registering one would shrink the
     * reported backlog without implementing anything the guest can reach. */
    static const unsigned implemented[] = {277u, 291u, 294u};
    CHECK_EQ_U32(kernel_hle_implemented_count(implemented, 3u), 3u);

    static const unsigned skipped[] = {278u, 295u, 306u, 101u, 122u};
    CHECK_EQ_U32(kernel_hle_implemented_count(skipped, 5u), 0u);
}

static void test_initialize_writes_the_measured_image_bytes(void)
{
    fresh();
    const kernel_guest_ptr cs = cs_slot(0u);
    fill(cs, 32u, 0xAAAAAAAAu);

    (void)call_cs(291u, frame_a(), cs);

    /* All seven dwords, against the measurement rather than against the macros the
     * implementation composed them from. */
    CHECK_EQ_U32(read_u32(cs + 0x00u), 0x00040001u);
    CHECK_EQ_U32(read_u32(cs + 0x04u), 0x00000000u);
    CHECK_EQ_U32(read_u32(cs + 0x08u), cs + 0x08u);
    CHECK_EQ_U32(read_u32(cs + 0x0Cu), cs + 0x08u);
    CHECK_EQ_U32(read_u32(cs + 0x10u), 0xFFFFFFFFu);
    CHECK_EQ_U32(read_u32(cs + 0x14u), 0x00000000u);
    CHECK_EQ_U32(read_u32(cs + 0x18u), 0x00000000u);

    /* 28 bytes, not 32: the dword just past the object must be untouched, or the
     * HLE is writing over whatever the guest put next to its lock. */
    CHECK_EQ_U32(read_u32(cs + 0x1Cu), 0xAAAAAAAAu);

    kernel_critsec_info info;
    CHECK(kernel_critsec_state(cs, &info));
    CHECK_EQ_U32(info.owner, 0u);
    CHECK_EQ_U32(info.recursion, 0u);
    /* Created by 291, so it was not adopted. */
    CHECK(!info.adopted);
    CHECK_EQ_U32(kernel_critsec_adopted_count(), 0u);
    /* And the 0xAAAAAAAA garbage 291 overwrote must NOT have been flagged
     * implausible: making the header valid is precisely what Initialize is
     * for, and a false alarm on every fresh heap lock cries wolf. */
    CHECK_EQ_U32(kernel_critsec_implausible_count(), 0u);
}

static void test_a_never_initialised_cs_is_adopted_on_first_enter(void)
{
    fresh();
    const kernel_guest_ptr cs = cs_slot(1u);
    write_image_cs(cs);

    /* Nothing has told us this lock exists. This is the case that covers roughly
     * 105 of the 114 Leave calls in the image. */
    CHECK_EQ_U32(kernel_critsec_tracked_count(), 0u);

    (void)call_cs(277u, frame_a(), cs);

    CHECK_EQ_U32(kernel_critsec_tracked_count(), 1u);
    CHECK_EQ_U32(kernel_critsec_adopted_count(), 1u);

    kernel_critsec_info info;
    CHECK(kernel_critsec_state(cs, &info));
    CHECK(info.adopted);
    CHECK_EQ_U32(info.owner, kernel_critsec_owner_token());
    CHECK_EQ_U32(info.recursion, 1u);
    /* Its header was a perfectly good critical section, so nothing was guessed. */
    CHECK_EQ_U32(kernel_critsec_implausible_count(), 0u);
    CHECK_EQ_U32(kernel_critsec_unreadable_count(), 0u);

    (void)call_cs(294u, frame_a(), cs);
}

static void test_a_never_initialised_cs_is_adopted_on_first_leave(void)
{
    fresh();
    kernel_hle_set_log(counting_log);
    const kernel_guest_ptr cs = cs_slot(2u);
    write_image_cs(cs);

    /* A Leave that is genuinely the first thing we ever see on an address must be
     * diagnosed as "left without entering", not as an unknown pointer: they are
     * different bugs and only one of them is the guest's. */
    (void)call_cs(294u, frame_a(), cs);

    CHECK_EQ_U32(kernel_critsec_tracked_count(), 1u);
    CHECK_EQ_U32(kernel_critsec_adopted_count(), 1u);
    CHECK_EQ_U32(kernel_critsec_bad_leave_count(), 1u);
    CHECK(log_lines > 0);
}

static void test_a_nested_enter_increments_the_recursion_count(void)
{
    fresh();
    const kernel_guest_ptr cs = cs_slot(3u);
    write_image_cs(cs);

    (void)call_cs(277u, frame_a(), cs);
    kernel_critsec_info info;
    CHECK(kernel_critsec_state(cs, &info));
    CHECK_EQ_U32(info.recursion, 1u);
    /* lock_count steps up from the shipped -1. The direction is INFERRED, not
     * measured; what the test pins is that Enter and Leave agree about it. */
    CHECK_EQ_U32(info.guest_lock_count, 0x00000000u);

    (void)call_cs(277u, frame_a(), cs);
    CHECK(kernel_critsec_state(cs, &info));
    CHECK_EQ_U32(info.recursion, 2u);
    CHECK_EQ_U32(info.guest_lock_count, 0x00000001u);
    CHECK_EQ_U32(info.owner, kernel_critsec_owner_token());

    (void)call_cs(277u, frame_a(), cs);
    CHECK(kernel_critsec_state(cs, &info));
    CHECK_EQ_U32(info.recursion, 3u);

    (void)call_cs(294u, frame_a(), cs);
    (void)call_cs(294u, frame_a(), cs);
    (void)call_cs(294u, frame_a(), cs);
    CHECK_EQ_U32(kernel_critsec_bad_leave_count(), 0u);
}

static void test_a_nested_enter_is_not_counted_as_contention(void)
{
    fresh();
    const kernel_guest_ptr cs = cs_slot(4u);
    write_image_cs(cs);

    (void)call_cs(277u, frame_a(), cs);
    (void)call_cs(277u, frame_a(), cs);

    /* Re-entering a lock this thread already holds is not contention and must not
     * wait. Treating it as contention would both mis-report and, with a lock that
     * did not permit it, deadlock. */
    CHECK_EQ_U32(kernel_critsec_contended_count(), 0u);
    CHECK_EQ_U32(kernel_critsec_recursive_enter_count(), 1u);

    (void)call_cs(294u, frame_a(), cs);
    (void)call_cs(294u, frame_a(), cs);
}

static void test_leave_unwinds_recursion_before_releasing(void)
{
    fresh();
    const kernel_guest_ptr cs = cs_slot(5u);
    write_image_cs(cs);

    (void)call_cs(277u, frame_a(), cs);
    (void)call_cs(277u, frame_a(), cs);

    (void)call_cs(294u, frame_a(), cs);
    kernel_critsec_info info;
    CHECK(kernel_critsec_state(cs, &info));
    /* Still ours after one Leave of two Enters. Releasing here would let another
     * thread in while this one is still inside its outer region. */
    CHECK_EQ_U32(info.recursion, 1u);
    CHECK_EQ_U32(info.owner, kernel_critsec_owner_token());
    CHECK_EQ_U32(info.guest_lock_count, 0x00000000u);

    (void)call_cs(294u, frame_a(), cs);
    CHECK(kernel_critsec_state(cs, &info));
    CHECK_EQ_U32(info.recursion, 0u);
    CHECK_EQ_U32(info.owner, 0u);
    /* Back to the shipped sentinel, which is the round trip that proves Enter and
     * Leave step the same field by the same amount in opposite directions. */
    CHECK_EQ_U32(info.guest_lock_count, 0xFFFFFFFFu);
    CHECK_EQ_U32(kernel_critsec_bad_leave_count(), 0u);
}

static void test_leave_by_the_owner_is_accepted(void)
{
    fresh();
    const kernel_guest_ptr cs = cs_slot(6u);
    write_image_cs(cs);

    (void)call_cs(277u, frame_a(), cs);
    (void)call_cs(294u, frame_a(), cs);

    /* The other direction of the ownership test. Without this case an inverted
     * owner check would still satisfy the "foreign Leave is refused" test, because
     * an implementation that refuses EVERY Leave also refuses that one. */
    CHECK_EQ_U32(kernel_critsec_bad_leave_count(), 0u);
    kernel_critsec_info info;
    CHECK(kernel_critsec_state(cs, &info));
    CHECK_EQ_U32(info.owner, 0u);
    CHECK_EQ_U32(info.recursion, 0u);

    /* And the released lock is genuinely available again. */
    (void)call_cs(277u, frame_a(), cs);
    CHECK(kernel_critsec_state(cs, &info));
    CHECK_EQ_U32(info.owner, kernel_critsec_owner_token());
    (void)call_cs(294u, frame_a(), cs);
}

static void test_leave_without_enter_is_counted_and_reported(void)
{
    fresh();
    kernel_hle_set_log(counting_log);
    const kernel_guest_ptr cs = cs_slot(7u);
    write_image_cs(cs);

    /* Adopt it with an Enter/Leave pair first, so the refusal below cannot be
     * confused with the adopt-on-first-Leave path. */
    (void)call_cs(277u, frame_a(), cs);
    (void)call_cs(294u, frame_a(), cs);
    CHECK_EQ_U32(kernel_critsec_bad_leave_count(), 0u);
    const uint32_t before = read_u32(cs + 0x10u);

    (void)call_cs(294u, frame_a(), cs);

    CHECK_EQ_U32(kernel_critsec_bad_leave_count(), 1u);
    CHECK(log_lines > 0);
    /* And it changed nothing. A refused Leave that still decremented would make the
     * next genuine Enter/Leave pair arithmetic wrong. */
    CHECK_EQ_U32(read_u32(cs + 0x10u), before);
    kernel_critsec_info info;
    CHECK(kernel_critsec_state(cs, &info));
    CHECK_EQ_U32(info.recursion, 0u);
    CHECK_EQ_U32(info.owner, 0u);
}

/* --- the two-thread cases ------------------------------------------------- */

typedef struct {
    kernel_guest_ptr cs;
    _Atomic int entered;
    _Atomic int finished;
} worker_args;

static void *worker_enter_then_leave(void *raw)
{
    worker_args *args = (worker_args *)raw;
    (void)call_cs(277u, frame_b(), args->cs);
    args->entered = 1;
    (void)call_cs(294u, frame_b(), args->cs);
    args->finished = 1;
    return NULL;
}

static void *worker_leave_only(void *raw)
{
    worker_args *args = (worker_args *)raw;
    (void)call_cs(294u, frame_b(), args->cs);
    args->finished = 1;
    return NULL;
}

static void test_leave_from_a_thread_that_does_not_hold_it_is_refused(void)
{
    fresh();
    kernel_hle_set_log(counting_log);
    const kernel_guest_ptr cs = cs_slot(8u);
    write_image_cs(cs);

    (void)call_cs(277u, frame_a(), cs);
    const uint32_t owner = kernel_critsec_owner_token();

    worker_args args = {cs, 0, 0};
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, worker_leave_only, &args) == 0);
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(args.finished == 1);

    /* The foreign Leave must be refused and the lock must still be ours. An
     * implementation that released it here would have let that thread, and any
     * other, straight into a region this one is still inside. */
    CHECK_EQ_U32(kernel_critsec_bad_leave_count(), 1u);
    CHECK(log_lines > 0);
    kernel_critsec_info info;
    CHECK(kernel_critsec_state(cs, &info));
    CHECK_EQ_U32(info.owner, owner);
    CHECK_EQ_U32(info.recursion, 1u);

    (void)call_cs(294u, frame_a(), cs);
}

static void test_a_second_thread_blocks_until_the_owner_leaves(void)
{
    fresh();
    const kernel_guest_ptr cs = cs_slot(9u);
    write_image_cs(cs);

    (void)call_cs(277u, frame_a(), cs);

    worker_args args = {cs, 0, 0};
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, worker_enter_then_leave, &args) == 0);

    /* The only direct evidence that mutual exclusion is real rather than recorded:
     * the other thread must still be inside Enter while we hold the lock. */
    sleep_ms(150);
    CHECK(args.entered == 0);

    (void)call_cs(294u, frame_a(), cs);
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(args.entered == 1);
    CHECK(args.finished == 1);

    CHECK_EQ_U32(kernel_critsec_contended_count(), 1u);
    CHECK_EQ_U32(kernel_critsec_bad_leave_count(), 0u);
    kernel_critsec_info info;
    CHECK(kernel_critsec_state(cs, &info));
    CHECK_EQ_U32(info.owner, 0u);
    CHECK_EQ_U32(info.guest_lock_count, 0xFFFFFFFFu);
}

static _Atomic uint32_t worker_token;

static void *worker_report_token(void *raw)
{
    (void)raw;
    worker_token = kernel_critsec_owner_token();
    return NULL;
}

static void test_two_threads_get_different_owner_tokens(void)
{
    fresh();
    /* Every ownership test above is vacuous if two threads look the same, so this
     * checks the premise rather than the behaviour. */
    const uint32_t mine = kernel_critsec_owner_token();
    CHECK(mine != 0u);
    CHECK_EQ_U32(kernel_critsec_owner_token(), mine);

    worker_token = 0u;
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, worker_report_token, NULL) == 0);
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(worker_token != 0u);
    CHECK(worker_token != mine);
}

/* --- the refusal and diagnostic paths ------------------------------------- */

static void test_a_null_critical_section_is_refused_not_locked(void)
{
    fresh();
    kernel_hle_set_log(counting_log);

    (void)call_cs(277u, frame_a(), 0u);

    CHECK_EQ_U32(kernel_critsec_unreadable_count(), 1u);
    CHECK_EQ_U32(kernel_critsec_tracked_count(), 0u);
    CHECK(log_lines > 0);
}

static void test_an_implausible_header_is_tracked_but_reported(void)
{
    fresh();
    kernel_hle_set_log(counting_log);
    const kernel_guest_ptr cs = cs_slot(10u);
    /* Not a critical-section header: the control event at 0x00549660 has Type=0
     * here, which is what made the embedded dispatcher object a measurement rather
     * than a story. */
    fill(cs, 32u, 0u);

    (void)call_cs(277u, frame_a(), cs);

    CHECK_EQ_U32(kernel_critsec_implausible_count(), 1u);
    CHECK(log_lines > 0);
    /* Tracked anyway. Refusing would turn Enter into a no-op and silently delete
     * the guest's mutual exclusion, which is worse than tracking a doubtful object. */
    CHECK_EQ_U32(kernel_critsec_tracked_count(), 1u);
    kernel_critsec_info info;
    CHECK(kernel_critsec_state(cs, &info));
    CHECK_EQ_U32(info.owner, kernel_critsec_owner_token());

    (void)call_cs(294u, frame_a(), cs);
}

static void test_a_control_event_shaped_header_is_implausible(void)
{
    fresh();
    kernel_hle_set_log(counting_log);
    const kernel_guest_ptr cs = cs_slot(13u);
    fill(cs, 32u, 0u);
    /* Type=0, Size=4: the EXACT shape of the control event at 0x00549660, the
     * object whose header made Type a measured discriminator. Size alone says
     * critical section; Type must say no. */
    (void)kernel_guest_write_u32(cs + 0x00u, 0x00040000u);

    (void)call_cs(277u, frame_a(), cs);

    CHECK_EQ_U32(kernel_critsec_implausible_count(), 1u);
    CHECK_EQ_U32(kernel_critsec_tracked_count(), 1u);
    CHECK(log_lines > 0);
    (void)call_cs(294u, frame_a(), cs);
}

static void test_a_wrong_size_header_is_implausible(void)
{
    fresh();
    kernel_hle_set_log(counting_log);
    const kernel_guest_ptr cs = cs_slot(14u);
    fill(cs, 32u, 0u);
    /* Type=1 but Size=1 dword: the type byte alone must not be enough, or any
     * object whose first byte happens to be 1 reads as a critical section. */
    (void)kernel_guest_write_u32(cs + 0x00u, 0x00010001u);

    (void)call_cs(277u, frame_a(), cs);

    CHECK_EQ_U32(kernel_critsec_implausible_count(), 1u);
    CHECK_EQ_U32(kernel_critsec_tracked_count(), 1u);
    (void)call_cs(294u, frame_a(), cs);
}

static void test_two_critical_sections_are_independent(void)
{
    fresh();
    /* The table is keyed by guest address and nothing else. Entering B while
     * holding A must be two entries, not a nested enter of whichever live slot
     * comes first, and leaving B must not release A. */
    const kernel_guest_ptr a = cs_slot(15u);
    const kernel_guest_ptr b = cs_slot(16u);
    write_image_cs(a);
    write_image_cs(b);

    (void)call_cs(277u, frame_a(), a);
    (void)call_cs(277u, frame_a(), b);

    CHECK_EQ_U32(kernel_critsec_tracked_count(), 2u);
    CHECK_EQ_U32(kernel_critsec_recursive_enter_count(), 0u);
    kernel_critsec_info info;
    CHECK(kernel_critsec_state(a, &info));
    CHECK_EQ_U32(info.cs, a);
    CHECK_EQ_U32(info.recursion, 1u);
    CHECK(kernel_critsec_state(b, &info));
    CHECK_EQ_U32(info.cs, b);
    CHECK_EQ_U32(info.recursion, 1u);

    (void)call_cs(294u, frame_a(), b);
    CHECK(kernel_critsec_state(a, &info));
    CHECK_EQ_U32(info.owner, kernel_critsec_owner_token());
    (void)call_cs(294u, frame_a(), a);
    CHECK_EQ_U32(kernel_critsec_bad_leave_count(), 0u);
}

static void test_an_unreadable_address_is_refused_not_locked(void)
{
    fresh();
    kernel_hle_set_log(counting_log);
    /* An address no region backs. The probe proves the premise, so if the
     * allocator ever grows to cover it this fails HERE, not mysteriously. */
    const kernel_guest_ptr cs = 0x7F000000u;
    uint32_t probe = 0u;
    CHECK(!kernel_guest_read_u32(cs, &probe));

    (void)call_cs(277u, frame_a(), cs);

    /* Locking memory we cannot read would invent mutual exclusion over
     * nothing: refused, counted, and never tracked. */
    CHECK_EQ_U32(kernel_critsec_unreadable_count(), 1u);
    CHECK_EQ_U32(kernel_critsec_tracked_count(), 0u);
    CHECK(log_lines > 0);
}

static void test_a_full_table_is_counted_and_refused(void)
{
    fresh();
    kernel_hle_set_log(counting_log);
    /* A page of its own: KERNEL_CRITSEC_MAX + 1 critical sections at a 32-byte
     * stride need more room than the shared scratch page can spare. */
    guest_region_request request = {
        .bytes = (KERNEL_CRITSEC_MAX + 1u) * 32u,
        .alignment = 0u,
        .lowest_physical = 0u,
        .highest_physical = 0u,
        .protect = PAGE_READWRITE,
        .state = MEM_COMMIT,
        .contiguous = true,
        .fixed_base = 0u,
    };
    nt_status status = STATUS_SUCCESS;
    const kernel_guest_ptr base = guest_region_alloc(&request, &status);
    CHECK(base != 0u);
    if (base == 0u) {
        return;
    }

    for (unsigned i = 0u; i < KERNEL_CRITSEC_MAX; i++) {
        const kernel_guest_ptr cs = (kernel_guest_ptr)(base + i * 32u);
        write_image_cs(cs);
        (void)call_cs(277u, frame_a(), cs);
        (void)call_cs(294u, frame_a(), cs);
    }
    CHECK_EQ_U32(kernel_critsec_tracked_count(), KERNEL_CRITSEC_MAX);
    CHECK_EQ_U32(kernel_critsec_table_full_count(), 0u);

    /* The 65th: refused without blocking, counted, and NOT tracked. The
     * counter is the only machine-readable trace that guest locks are now
     * going untracked; the log line alone scrolls away. */
    const kernel_guest_ptr extra = (kernel_guest_ptr)(base + KERNEL_CRITSEC_MAX * 32u);
    write_image_cs(extra);
    CHECK_EQ_U32(call_cs(277u, frame_a(), extra), STATUS_SUCCESS);
    CHECK_EQ_U32(kernel_critsec_table_full_count(), 1u);
    CHECK_EQ_U32(kernel_critsec_tracked_count(), KERNEL_CRITSEC_MAX);
    CHECK(log_lines > 0);
    kernel_critsec_info info;
    CHECK(!kernel_critsec_state(extra, &info));
}

static _Atomic uint32_t reentrant_sink_calls;

static int reentrant_log(const char *format, ...)
{
    (void)format;
    /* Ask the module a question from inside its own logging path. The null
     * refusal below logs with `table_lock` held, so this re-enters it on the
     * same thread; the lock is recursive exactly so that a caller-supplied
     * sink doing this cannot deadlock the kernel. */
    (void)kernel_critsec_tracked_count();
    reentrant_sink_calls++;
    return 0;
}

static void test_a_log_sink_may_reenter_the_module(void)
{
    fresh();
    kernel_hle_set_log(reentrant_log);
    reentrant_sink_calls = 0u;

    (void)call_cs(277u, frame_a(), 0u);

    CHECK_EQ_U32(kernel_critsec_unreadable_count(), 1u);
    CHECK(reentrant_sink_calls > 0u);
    kernel_hle_set_log(NULL);
}

/* Enter, then hold the lock until released, so a reset can be driven while a
 * SECOND thread owns the mutex (the retirement scenario kernel_critsec_reset
 * exists for). */
typedef struct {
    kernel_guest_ptr cs;
    _Atomic int entered;
    _Atomic int release;
    _Atomic int finished;
} holder_args;

static void *worker_hold_until_released(void *raw)
{
    holder_args *args = (holder_args *)raw;
    (void)call_cs(277u, frame_b(), args->cs);
    args->entered = 1;
    while (!args->release) {
        sleep_ms(5);
    }
    (void)call_cs(294u, frame_b(), args->cs);
    args->finished = 1;
    return NULL;
}

static void test_a_retired_slot_is_never_reused(void)
{
    fresh();
    kernel_hle_set_log(counting_log);
    const kernel_guest_ptr cs = cs_slot(17u);
    write_image_cs(cs);

    holder_args args = {cs, 0, 0, 0};
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, worker_hold_until_released, &args) == 0);
    while (!args.entered) {
        sleep_ms(5);
    }

    /* Reset while ANOTHER thread holds the mutex: the slot must be retired. */
    const uint32_t retired_before = kernel_critsec_retired_count();
    kernel_critsec_reset();
    CHECK_EQ_U32(kernel_critsec_retired_count(), retired_before + 1u);

    /* A fresh Enter on the same address must build a NEW entry with a NEW
     * mutex and complete promptly; a reused retired slot would block forever
     * on the abandoned lock the worker still holds. */
    write_image_cs(cs);
    (void)call_cs(277u, frame_a(), cs);
    kernel_critsec_info info;
    CHECK(kernel_critsec_state(cs, &info));
    CHECK_EQ_U32(info.owner, kernel_critsec_owner_token());
    (void)call_cs(294u, frame_a(), cs);

    /* Let the worker go. Its Leave lands on the new entry it does not own and
     * is refused, which is correct: its lock was abandoned by the reset. */
    args.release = 1;
    CHECK(pthread_join(worker, NULL) == 0);
    CHECK(args.finished == 1);
}

static void test_initialize_resets_our_record_of_an_adopted_lock(void)
{
    fresh();
    const kernel_guest_ptr cs = cs_slot(11u);
    write_image_cs(cs);

    (void)call_cs(277u, frame_a(), cs);
    (void)call_cs(294u, frame_a(), cs);
    kernel_critsec_info info;
    CHECK(kernel_critsec_state(cs, &info));
    CHECK(info.adopted);

    (void)call_cs(291u, frame_a(), cs);
    CHECK(kernel_critsec_state(cs, &info));
    /* Same slot, same address, but no longer an adopted lock: 291 has now told us
     * about it explicitly. */
    CHECK(!info.adopted);
    CHECK_EQ_U32(info.owner, 0u);
    CHECK_EQ_U32(info.guest_lock_count, 0xFFFFFFFFu);
    CHECK_EQ_U32(kernel_critsec_reinit_while_held_count(), 0u);
    CHECK_EQ_U32(kernel_critsec_tracked_count(), 1u);
}

/*
 * LAST, because it deliberately abandons a host mutex.
 *
 * Re-initialising a held critical section is a guest bug. We cannot unlock the
 * mutex (undefined for one this thread may not own) so it stays locked, and the
 * following reset retires the slot rather than handing an inherited lock to the
 * next critical section that comes along.
 */
static void test_initialize_while_held_is_reported_and_the_slot_retired(void)
{
    fresh();
    kernel_hle_set_log(counting_log);
    const kernel_guest_ptr cs = cs_slot(12u);
    write_image_cs(cs);

    (void)call_cs(277u, frame_a(), cs);
    (void)call_cs(291u, frame_a(), cs);

    CHECK_EQ_U32(kernel_critsec_reinit_while_held_count(), 1u);
    CHECK(log_lines > 0);
    kernel_critsec_info info;
    CHECK(kernel_critsec_state(cs, &info));
    CHECK_EQ_U32(info.owner, 0u);

    /* Re-acquire it to put a held lock in front of the reset below. */
    (void)call_cs(277u, frame_a(), cs);
    const uint32_t retired_before = kernel_critsec_retired_count();
    kernel_critsec_reset();
    CHECK_EQ_U32(kernel_critsec_retired_count(), retired_before + 1u);

    /* And the retired slot is not reused: a fresh Enter on the same address builds
     * a new entry with a new mutex and completes rather than blocking forever. */
    write_image_cs(cs);
    (void)call_cs(277u, frame_a(), cs);
    CHECK(kernel_critsec_state(cs, &info));
    CHECK_EQ_U32(info.owner, kernel_critsec_owner_token());
    (void)call_cs(294u, frame_a(), cs);
}

int main(void)
{
    setup();

    pthread_t guard;
    const bool guarded = pthread_create(&guard, NULL, watchdog, NULL) == 0;
    CHECK(guarded);

    RUN(test_registration);
    /* kernel_hle_init in test_registration cleared the table, so re-register. */
    (void)kernel_critsec_register();

    RUN(test_initialize_writes_the_measured_image_bytes);
    RUN(test_a_never_initialised_cs_is_adopted_on_first_enter);
    RUN(test_a_never_initialised_cs_is_adopted_on_first_leave);
    RUN(test_a_nested_enter_increments_the_recursion_count);
    RUN(test_a_nested_enter_is_not_counted_as_contention);
    RUN(test_leave_unwinds_recursion_before_releasing);
    RUN(test_leave_by_the_owner_is_accepted);
    RUN(test_leave_without_enter_is_counted_and_reported);
    RUN(test_leave_from_a_thread_that_does_not_hold_it_is_refused);
    RUN(test_a_second_thread_blocks_until_the_owner_leaves);
    RUN(test_two_threads_get_different_owner_tokens);
    RUN(test_a_null_critical_section_is_refused_not_locked);
    RUN(test_an_implausible_header_is_tracked_but_reported);
    RUN(test_a_control_event_shaped_header_is_implausible);
    RUN(test_a_wrong_size_header_is_implausible);
    RUN(test_two_critical_sections_are_independent);
    RUN(test_an_unreadable_address_is_refused_not_locked);
    RUN(test_a_full_table_is_counted_and_refused);
    RUN(test_a_log_sink_may_reenter_the_module);
    RUN(test_initialize_resets_our_record_of_an_adopted_lock);
    /* The last two deliberately abandon host mutexes (see their comments). */
    RUN(test_a_retired_slot_is_never_reused);
    RUN(test_initialize_while_held_is_reported_and_the_slot_retired);

    all_tests_done = 1;
    if (guarded) {
        (void)pthread_join(guard, NULL);
    }
    teardown();

    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("kernel_critsec: all checks passed\n");
    return 0;
}
