/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The runtime's untranslated-instruction policy: `src/host/recomp_runtime.c`.
 *
 * WHAT THIS SUITE EXISTS TO PROVE. The lifter emits RECOMP_UNIMPL("wbinvd", va) at 10
 * sites, among them the sixth instruction of the frame loop. The policy is: that one
 * mnemonic is a COUNTED, ANNOUNCED NO-OP, and every other untranslated instruction
 * still stops. Four independent things can be wrong and none of them crashes the suite
 * that does not look:
 *
 *   1. THE MACRO CAN BE THE NORETURN ONE. recomp_types.h makes RECOMP_UNIMPL a call to a
 *      RECOMP_NORETURN function unless RECOMP_UNIMPL_CONTINUE is defined, and a noreturn
 *      call cannot be returned from. This file is compiled with the SAME definition
 *      CMake gives tsfp_lifted (TSFP_LIFTED_UNIMPL_POLICY), and uses the macro itself
 *      rather than calling recomp_unimpl, so a build that dropped the definition from the
 *      lifted target but kept the runtime correct would still be caught.
 *   2. THE NO-OP CAN BE SILENT. The count is asserted per site and in total, so a
 *      "return early" that forgets to record is a failure.
 *   3. THE EXCEPTION CAN BE TOO WIDE. A prefix or substring match would let `wbinvd` run
 *      past, and would equally let a hypothetical `wbinvdx` or `invd` (which DISCARDS
 *      dirty lines without writing them back, and is not a no-op) run past too. Near
 *      misses are asserted to stop.
 *   4. THE TRAP CAN LOSE ITS PROMISE. recomp_unimpl_trap is declared noreturn, so it must
 *      stop for wbinvd as well. Returning from it is undefined behaviour.
 *
 * DELIBERATELY FREE OF THE XBE, THE DISC AND THE 2.56 M LINES OF LIFTED C. It links the
 * runtime object (which defines the register file) and host_runtime.o, and includes the
 * one generated header the lifted code itself includes.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED BY HAND, and each test says what breaks it.
 */

#include "host_runtime.h"
#include "recomp_abi.h"

#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

/* The generated view, exactly as recomp_runtime.c includes it, so RECOMP_UNIMPL below
 * is the macro the lifted code gets. */
#include "recomp_types.h"

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

static bool stopped;
static host_stop stop_record;

/* Run `body` the way a guest thread runs: armed, so a stop is a siglongjmp back here
 * rather than an abort. */
static void run_armed(void (*body)(void))
{
    stopped = false;
    memset(&stop_record, 0, sizeof(stop_record));
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        body();
    } else {
        stopped = true;
        stop_record = *host_run_result();
    }
    host_run_disarm();
}

static volatile int reached_after_macro;

/* THE MACRO, NOT THE FUNCTION. Reaching the statement after it is the whole proof that
 * the configuration CMake gave this target is the one that lets `wbinvd` continue. */
static void three_hits_at_two_sites(void)
{
    reached_after_macro = 0;
    RECOMP_UNIMPL("wbinvd", 0x0003D379u);
    reached_after_macro = 1;
    RECOMP_UNIMPL("wbinvd", 0x0003D379u);
    RECOMP_UNIMPL("wbinvd", 0x000236E4u);
    reached_after_macro = 2;
}

/* BREAKS THIS: dropping RECOMP_UNIMPL_CONTINUE from TSFP_LIFTED_UNIMPL_POLICY (the call
 * becomes recomp_unimpl_trap, which stops: `stopped` is true and the count is 0); making
 * recomp_unimpl stop for wbinvd; returning before note_wbinvd (counts stay 0). */
static void test_wbinvd_continues_and_is_counted_per_site(void)
{
    recomp_wbinvd_reset();
    run_armed(three_hits_at_two_sites);

    CHECK(!stopped);
    CHECK_EQ_U64(reached_after_macro, 2);
    CHECK_EQ_U64(recomp_wbinvd_total(), 3u);
    CHECK_EQ_U64(recomp_wbinvd_site_count(), 2u);
    CHECK_EQ_U64(recomp_wbinvd_site_hits(0x0003D379u), 2u);
    CHECK_EQ_U64(recomp_wbinvd_site_hits(0x000236E4u), 1u);
    CHECK_EQ_U64(recomp_wbinvd_site_hits(0x00028DB5u), 0u);
}

static void daa_at_site(void)
{
    RECOMP_UNIMPL("daa", 0x00012340u);
    reached_after_macro = 99;
}

/* Every other mnemonic still stops, with the address and the text, and a stop does not
 * touch the wbinvd count.
 * BREAKS THIS: letting every mnemonic continue (reached_after_macro becomes 99);
 * recording the wrong address; counting a non-wbinvd hit as a flush. */
static void test_any_other_mnemonic_still_stops(void)
{
    recomp_wbinvd_reset();
    reached_after_macro = 0;
    run_armed(daa_at_site);

    CHECK(stopped);
    CHECK_EQ_U64(reached_after_macro, 0);
    CHECK_EQ_U64(stop_record.reason, HOST_STOP_UNIMPLEMENTED);
    CHECK_EQ_U64(stop_record.guest_address, 0x00012340u);
    CHECK(strcmp(stop_record.detail, "daa") == 0);
    CHECK_EQ_U64(recomp_wbinvd_total(), 0u);
}

static const char *near_miss_text;

static void near_miss_body(void)
{
    recomp_unimpl(near_miss_text, 0x00011000u);
    reached_after_macro = 99;
}

/* The exception is an exact match.
 * BREAKS THIS: strncmp / strstr / strcasecmp for strcmp; matching on the first letter. */
static void test_the_exception_is_exact(void)
{
    static const char *const near_misses[] = {"invd", "wbinvdx", "wbinvd eax", "WBINVD",
                                              "wbinv", ""};
    for (size_t index = 0; index < sizeof(near_misses) / sizeof(near_misses[0]); index++) {
        recomp_wbinvd_reset();
        reached_after_macro = 0;
        near_miss_text = near_misses[index];
        run_armed(near_miss_body);
        CHECK(stopped);
        CHECK_EQ_U64(reached_after_macro, 0);
        CHECK_EQ_U64(recomp_wbinvd_total(), 0u);
    }

    /* A NULL text is a stop that says so, not a crash. */
    reached_after_macro = 0;
    near_miss_text = NULL;
    run_armed(near_miss_body);
    CHECK(stopped);
    CHECK(strcmp(stop_record.detail, "?") == 0);
}

static void trap_with_wbinvd(void)
{
    recomp_unimpl_trap("wbinvd", 0x0003D379u);
    reached_after_macro = 99;
}

/* The noreturn entry point stops for wbinvd too, and counts nothing.
 * BREAKS THIS: routing recomp_unimpl_trap through the wbinvd branch, which would return
 * into __builtin_unreachable(). */
static void test_the_noreturn_trap_never_returns(void)
{
    recomp_wbinvd_reset();
    reached_after_macro = 0;
    run_armed(trap_with_wbinvd);

    CHECK(stopped);
    CHECK_EQ_U64(reached_after_macro, 0);
    CHECK_EQ_U64(stop_record.reason, HOST_STOP_UNIMPLEMENTED);
    CHECK_EQ_U64(stop_record.guest_address, 0x0003D379u);
    CHECK_EQ_U64(recomp_wbinvd_total(), 0u);
}

static void many_sites(void)
{
    for (uint32_t index = 0; index < 100u; index++) {
        RECOMP_UNIMPL("wbinvd", 0x00100000u + index * 2u);
    }
}

/* The site table is bounded, and past the bound hits are still COUNTED.
 * BREAKS THIS: an unbounded write (the suite crashes or the count is wrong); dropping a
 * hit when the table is full (total becomes 64). */
static void test_the_site_table_is_bounded_but_total_is_not(void)
{
    recomp_wbinvd_reset();
    run_armed(many_sites);

    CHECK(!stopped);
    CHECK_EQ_U64(recomp_wbinvd_total(), 100u);
    CHECK_EQ_U64(recomp_wbinvd_site_count(), 64u);
    CHECK_EQ_U64(recomp_wbinvd_site_hits(0x00100000u), 1u);
    CHECK_EQ_U64(recomp_wbinvd_site_hits(0x00100000u + 99u * 2u), 0u);
}

#define THREADS 4
#define HITS_PER_THREAD 20000

static void *hammer(void *unused)
{
    (void)unused;
    for (int index = 0; index < HITS_PER_THREAD; index++) {
        RECOMP_UNIMPL("wbinvd", 0x0003D379u);
    }
    return NULL;
}

/* Guest threads are host threads, so the count is shared state.
 * BREAKS THIS: removing the mutex (lost updates make the total short on a multi-core
 * host; run it a few times). */
static void test_the_count_survives_concurrent_guest_threads(void)
{
    pthread_t threads[THREADS];
    recomp_wbinvd_reset();
    for (int index = 0; index < THREADS; index++) {
        CHECK(pthread_create(&threads[index], NULL, hammer, NULL) == 0);
    }
    for (int index = 0; index < THREADS; index++) {
        pthread_join(threads[index], NULL);
    }
    CHECK_EQ_U64(recomp_wbinvd_total(), (unsigned long long)THREADS * HITS_PER_THREAD);
    CHECK_EQ_U64(recomp_wbinvd_site_count(), 1u);
}

/* Independent xemu probe measured CW=027F and status=0 in both main and
 * freshly created guest threads. Each thread must start fresh while mutations
 * on another thread remain intact; copying the parent's current CW is wrong. */
static void *check_initial_x87_thread(void *result)
{
    uint16_t *word = result;
    *word = g_fp_control_word;
    g_fp_control_word = 0x0F7Fu;
    return NULL;
}

static void test_kernel_initial_x87_state(void)
{
    CHECK_EQ_U64(g_fp_control_word, 0x027Fu);
    CHECK_EQ_U64(g_fp_top, 0u);
    CHECK_EQ_U64(g_fp_cc, 0u);
    g_fp_control_word = 0x077Fu;
    for (unsigned index = 0; index < 2u; ++index) {
        pthread_t thread;
        uint16_t fresh_word = 0u;
        CHECK(pthread_create(&thread, NULL, check_initial_x87_thread, &fresh_word) == 0);
        CHECK(pthread_join(thread, NULL) == 0);
        CHECK_EQ_U64(fresh_word, 0x027Fu);
        CHECK_EQ_U64(g_fp_control_word, 0x077Fu);
    }
    g_fp_control_word = 0x027Fu;
}

int main(void)
{
    test_kernel_initial_x87_state();
    /* Line-buffered, so a mutation that kills the process still leaves its FAIL lines. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    test_wbinvd_continues_and_is_counted_per_site();
    test_any_other_mnemonic_still_stops();
    test_the_exception_is_exact();
    test_the_noreturn_trap_never_returns();
    test_the_site_table_is_bounded_but_total_is_not();
    test_the_count_survives_concurrent_guest_threads();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
