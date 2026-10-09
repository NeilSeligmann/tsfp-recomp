/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * IRQL bookkeeping. These ordinals are a quarter of every kernel call site in the
 * image, so the raise/lower pairing has to be exactly right: a raise returns the
 * PREVIOUS level, and the guest hands that value straight back to KfLowerIrql.
 * Getting it backwards would make every pairing in the game drift.
 */

#include "kernel_sync.h"

#include "kernel_call.h"
#include "kernel_hle.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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
            printf("FAIL %s:%d  %s == %u, expected %u\n", __FILE__, __LINE__,           \
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

static kernel_call_frame fastcall_frame(uint32_t ecx)
{
    kernel_call_frame frame = {0u, 0u, 0u, 0u, false, 0u, false};
    kernel_frame_set_registers(&frame, ecx, 0u);
    return frame;
}

static void test_registration(void)
{
    kernel_hle_init();
    CHECK_EQ_U32(kernel_sync_register(), 5u);

    static const unsigned ordinals[] = {103u, 129u, 130u, 160u, 161u};
    CHECK_EQ_U32(kernel_hle_implemented_count(ordinals, 5u), 5u);
}

static void test_raise_returns_the_previous_level_not_the_new_one(void)
{
    kernel_sync_reset();
    /* The named levels pinned to their NT values ONCE, as literals: every other
     * assertion in these suites reads through the macros, so without this pin a
     * mutated level would move the code and the expectations together. The guest
     * hardcodes these numbers in its own compares (`cmp LO8(eax), 2` at the 6
     * measured read sites), so they are external facts, not our choices. */
    CHECK_EQ_U32(KERNEL_IRQL_PASSIVE, 0u);
    CHECK_EQ_U32(KERNEL_IRQL_APC, 1u);
    CHECK_EQ_U32(KERNEL_IRQL_DISPATCH, 2u);
    CHECK_EQ_U32(KERNEL_IRQL_HIGH, 31u);

    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_PASSIVE);

    /* The whole raise/lower protocol rests on this: the guest stores what the
     * raise returned and hands it back. Returning the NEW level would still look
     * self-consistent in isolation and drift in the game. */
    CHECK_EQ_U32(kernel_hle_call(129u, NULL), KERNEL_IRQL_PASSIVE);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_DISPATCH);

    /* Raising again from DISPATCH returns DISPATCH, not PASSIVE. */
    CHECK_EQ_U32(kernel_hle_call(129u, NULL), KERNEL_IRQL_DISPATCH);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_DISPATCH);
}

static void test_a_raise_never_lowers(void)
{
    kernel_sync_reset();
    kernel_call_frame high = fastcall_frame(KERNEL_IRQL_HIGH);
    CHECK_EQ_U32(kernel_hle_call(160u, &high), KERNEL_IRQL_PASSIVE);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_HIGH);

    /* KfRaiseIrql to a LOWER level must not lower; it is a raise. */
    kernel_call_frame apc = fastcall_frame(KERNEL_IRQL_APC);
    CHECK_EQ_U32(kernel_hle_call(160u, &apc), KERNEL_IRQL_HIGH);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_HIGH);
}

static void test_round_trip_through_a_raise_lower_pair(void)
{
    kernel_sync_reset();
    uint32_t saved = kernel_hle_call(129u, NULL);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_DISPATCH);

    kernel_call_frame restore = fastcall_frame(saved);
    (void)kernel_hle_call(161u, &restore);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_PASSIVE);
    CHECK_EQ_U32(kernel_sync_bad_lower_count(), 0u);
}

static void test_a_mismatched_lower_is_counted_and_reported(void)
{
    kernel_sync_reset();
    kernel_hle_set_log(counting_log);
    log_lines = 0;

    /* Lowering to a HIGHER level is the bug the real kernel bugchecks on. We clamp
     * and count, but it must not pass silently or a real guest bug is invisible. */
    kernel_call_frame bad = fastcall_frame(KERNEL_IRQL_HIGH);
    (void)kernel_hle_call(161u, &bad);

    CHECK_EQ_U32(kernel_sync_bad_lower_count(), 1u);
    CHECK(log_lines > 0);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_PASSIVE);
    kernel_hle_set_log(NULL);
}

static void test_a_fastcall_without_registers_is_refused_not_guessed(void)
{
    kernel_sync_reset();
    kernel_hle_set_log(counting_log);
    log_lines = 0;

    /* A stack-only frame is a dispatcher bug. Reading kernel_frame_arg(0) here
     * would yield the return address; silently treating it as PASSIVE_LEVEL would
     * be invisible because that is also a legitimate value. */
    kernel_call_frame stack_only = {0u, 0u, 0u, 0u, false, 0u, false};
    (void)kernel_hle_call(161u, &stack_only);

    CHECK(log_lines > 0);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_PASSIVE);
    kernel_hle_set_log(NULL);
}

static void test_get_current_irql_reports_the_tracked_level(void)
{
    kernel_sync_reset();
    CHECK_EQ_U32(kernel_hle_call(103u, NULL), KERNEL_IRQL_PASSIVE);
    (void)kernel_hle_call(130u, NULL);
    CHECK_EQ_U32(kernel_hle_call(103u, NULL), KERNEL_IRQL_DISPATCH);
}

/* --- publishing the guest's own copy -------------------------------------
 *
 * The guest reads KPCR.Irql at fs:[0x24] in 6 generated functions and branches on
 * it. We wrote that byte once at thread creation and never again, so against 245
 * raise/lower sites all six comparisons permanently took the PASSIVE arm. A
 * perfectly correct handler is still ignored if the guest reads a stale copy.
 */

static uint32_t published[32];
static unsigned publish_count;

static void record_publish(uint32_t level)
{
    if (publish_count < 32u) {
        published[publish_count] = level;
    }
    publish_count++;
}

static void test_every_change_is_published_to_the_guest(void)
{
    kernel_hle_init();
    (void)kernel_sync_register();
    publish_count = 0u;
    kernel_sync_set_irql_publisher(record_publish);
    kernel_sync_reset();

    /* Installing and resetting both publish, so the guest is never stale. */
    CHECK(publish_count >= 1u);
    const unsigned before = publish_count;

    uint32_t saved = kernel_hle_call(129u, NULL);          /* raise to DISPATCH */
    CHECK(publish_count == before + 1u);
    CHECK_EQ_U32(published[publish_count - 1u], KERNEL_IRQL_DISPATCH);

    kernel_call_frame restore = fastcall_frame(saved);
    (void)kernel_hle_call(161u, &restore);                 /* lower back */
    CHECK(publish_count == before + 2u);
    CHECK_EQ_U32(published[publish_count - 1u], KERNEL_IRQL_PASSIVE);

    kernel_sync_set_irql_publisher(NULL);
}

static void test_a_raise_that_does_not_change_the_level_publishes_nothing(void)
{
    /* Publishing on a no-op would be harmless but would hide a real bug: it would
     * make the count useless as evidence that changes are being propagated. */
    kernel_hle_init();
    (void)kernel_sync_register();
    kernel_sync_set_irql_publisher(record_publish);
    kernel_sync_reset();
    (void)kernel_hle_call(129u, NULL);
    const unsigned after_first = publish_count;

    (void)kernel_hle_call(129u, NULL);  /* already at DISPATCH */
    CHECK(publish_count == after_first);

    kernel_sync_set_irql_publisher(NULL);
}

/* T370: the host-side raise and restore used around the vblank callback. */
static void test_host_raise_and_restore_are_exact_and_published(void)
{
    kernel_hle_init();
    (void)kernel_sync_register();
    kernel_sync_set_irql_publisher(record_publish);
    kernel_sync_reset();
    const unsigned before = publish_count;
    const uint32_t previous = kernel_sync_raise_irql(KERNEL_IRQL_DISPATCH);
    CHECK_EQ_U32(previous, KERNEL_IRQL_PASSIVE);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_DISPATCH);
    CHECK(publish_count == before + 1u);
    CHECK_EQ_U32(published[publish_count - 1u], KERNEL_IRQL_DISPATCH);

    /* Guest code in between may leave the level anywhere, restore is exact all the same. */
    kernel_call_frame lower = fastcall_frame(KERNEL_IRQL_APC);
    (void)kernel_hle_call(161u, &lower);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_APC);
    kernel_sync_restore_irql(previous);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_PASSIVE);
    CHECK_EQ_U32(published[publish_count - 1u], KERNEL_IRQL_PASSIVE);
    CHECK_EQ_U32(kernel_sync_bad_lower_count(), 0u);

    /* A raise never lowers: from HIGH it returns HIGH and changes nothing. */
    (void)kernel_sync_raise_irql(KERNEL_IRQL_HIGH);
    const unsigned at_high = publish_count;
    CHECK_EQ_U32(kernel_sync_raise_irql(KERNEL_IRQL_DISPATCH), KERNEL_IRQL_HIGH);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_HIGH);
    CHECK(publish_count == at_high);
    kernel_sync_restore_irql(KERNEL_IRQL_PASSIVE);
    CHECK_EQ_U32(kernel_sync_bad_lower_count(), 0u);
    kernel_sync_set_irql_publisher(NULL);
}

int main(void)
{
    test_host_raise_and_restore_are_exact_and_published();
    test_registration();
    test_raise_returns_the_previous_level_not_the_new_one();
    test_a_raise_never_lowers();
    test_round_trip_through_a_raise_lower_pair();
    test_a_mismatched_lower_is_counted_and_reported();
    test_a_fastcall_without_registers_is_refused_not_guessed();
    test_get_current_irql_reports_the_tracked_level();
    test_every_change_is_published_to_the_guest();
    test_a_raise_that_does_not_change_the_level_publishes_nothing();

    if (failures != 0) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("kernel_sync: all checks passed\n");
    return 0;
}
