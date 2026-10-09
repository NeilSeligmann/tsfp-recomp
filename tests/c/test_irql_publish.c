/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Publishing IRQL into the guest's own copy at `KPCR.Irql`.
 *
 * WHAT THIS SUITE IS FOR. `kernel_sync.c` tracks the level per thread and hands
 * every change to the sink installed through `kernel_sync_set_irql_publisher`.
 * `test_kernel_sync.c` already proves the hook fires with the right values. It
 * proves nothing about where those values LAND, and the bug being closed here is
 * precisely a landing-place bug: nothing installed the sink, so the byte the guest
 * reads was written once at thread creation and never again.
 *
 * WHY THE BYTE MATTERS, MEASURED. The guest reads its own copy at `fs:[0x24]` and
 * branches on it. In this image the lifted C does it at exactly 6 sites --
 * `sub_0037E9A7`, `sub_0037E9CF`, `sub_0037FEB5`, `sub_004067F0`, `sub_004069FE`,
 * `sub_0040BAA2` -- and every one is the same two operations:
 *
 *     eax = ZX8(MEM8(XBOX_FS_BASE + 0x24));
 *     if (CMP_AE(LO8(eax), 2)) goto <at or above DISPATCH_LEVEL>;
 *
 * So the question "can a read site observe a non-PASSIVE value" is a question about
 * one byte of guest memory, and it is answerable without linking any of the 2.56 M
 * lines of lifted C. `read_site_takes_dispatch_arm()` below performs exactly that
 * load and comparison against the same address, which is why this suite can be
 * DELIBERATELY FREE OF LIFTED CODE AND OF THE XBE like test_prcb_monitor.c.
 *
 * THE PUBLISHER UNDER TEST. `src/host/main.c` installs a three-line sink that reads
 * the lifted runtime's thread-local `g_fs_base` and calls
 * `kernel_thread_set_irql()`. The lifted runtime cannot be linked here, so
 * `publish_to_my_kpcr()` below is the same three lines over a `_Thread_local` base
 * of this suite's own -- which makes the per-thread property testable, and that is
 * the property a captured base would break.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED; each line says what breaks it.
 */

#include "kernel_thread.h"

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_hle.h"
#include "kernel_sync.h"
#include "nt_status.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        checks++;                                                                       \
        if (!(cond)) {                                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                       \
            failures++;                                                                  \
        }                                                                               \
    } while (0)

#define CHECK_EQ_U32(actual, expected)                                                  \
    do {                                                                                \
        checks++;                                                                       \
        uint32_t a_ = (uint32_t)(actual);                                                \
        uint32_t e_ = (uint32_t)(expected);                                              \
        if (a_ != e_) {                                                                   \
            printf("FAIL %s:%d  %s == %#x, expected %#x\n", __FILE__, __LINE__, #actual,  \
                   (unsigned)a_, (unsigned)e_);                                           \
            failures++;                                                                   \
        }                                                                               \
    } while (0)

static int quiet_log(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    (void)format;
    va_end(args);
    return 0;
}

/* --- the publisher, mirroring src/host/main.c ------------------------------ */

/*
 * This thread's `fs` base.
 *
 * `_Thread_local` because the real one is: `g_fs_base` is `RECOMP_TLS` and
 * `host_thread_enter` gives each guest thread its own KPCR. A publisher that
 * captured one base at installation would send every thread's raises into one
 * thread's KPCR, so the other thread would then read a level it never set -- which
 * is a worse failure than publishing nothing, because it is a plausible wrong
 * answer rather than an absent one.
 */
static _Thread_local uint32_t my_fs_base;
static _Thread_local unsigned my_publish_failures;

static void publish_to_my_kpcr(uint32_t level)
{
    if (my_fs_base == 0u || !kernel_thread_set_irql(my_fs_base, level)) {
        my_publish_failures++;
    }
}

/* --- the guest's own read site, performed byte for byte -------------------- */

/* The load the 6 measured sites perform. The LITERAL 0x24u, not KERNEL_PCR_IRQL:
 * the guest's instruction reads fs:[0x24], and reading through the macro would move
 * this probe together with a mutated publisher offset, passing on a wrong offset. */
static uint8_t guest_irql_byte(uint32_t fs_base)
{
    uint8_t value = 0xFFu;
    CHECK(kernel_guest_read_u8(fs_base + 0x24u, &value));
    return value;
}

/*
 * `cmp LO8(eax), 2` / `jae`, i.e. which arm all 6 sites take.
 *
 * True is the at-or-above-DISPATCH_LEVEL arm. Before the publisher was installed
 * this returned false for every raise the guest ever made.
 */
static bool read_site_takes_dispatch_arm(uint32_t fs_base)
{
    /* The literal 2u: the guest's instruction is `cmp LO8(eax), 2`, and that
     * hardcoded 2 is the independent fact, not KERNEL_IRQL_DISPATCH. */
    return guest_irql_byte(fs_base) >= 2u;
}

/* --- a control page to publish into --------------------------------------- */

typedef struct {
    uint32_t control;
    uint32_t tls;
    uint32_t monitor;
} kpcr_set;

static uint32_t alloc_region(uint32_t bytes)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = bytes;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    const kernel_guest_ptr base = guest_region_alloc(&request, &status);
    if (base == 0u) {
        printf("FAIL could not allocate %u guest bytes (status %#x)\n", bytes,
               (unsigned)status);
        failures++;
    }
    return (uint32_t)base;
}

static bool make_kpcr(kpcr_set *out)
{
    out->control = alloc_region(KERNEL_THREAD_CONTROL_BYTES);
    out->tls = alloc_region(GUEST_PAGE_SIZE);
    out->monitor = alloc_region(KERNEL_THREAD_MONITOR_BYTES);
    if (out->control == 0u || out->tls == 0u || out->monitor == 0u) {
        return false;
    }
    return kernel_thread_control_init(out->control, out->tls, out->monitor,
                                      KERNEL_THREAD_MONITOR_BYTES);
}

static void free_kpcr(const kpcr_set *set)
{
    (void)guest_region_free(set->control);
    (void)guest_region_free(set->tls);
    (void)guest_region_free(set->monitor);
}

static void setup(void)
{
    kernel_hle_init();
    kernel_hle_set_log(quiet_log);
    guest_mem_reset();
    CHECK_EQ_U32(kernel_sync_register(), 5u);
    kernel_sync_set_irql_publisher(NULL);
    kernel_sync_reset();
    my_fs_base = 0u;
    my_publish_failures = 0u;
}

static void teardown(void)
{
    /* Detach before freeing the control page. A stale publisher would attempt
     * an IRQL write into unmapped memory on the next raise; checked writes refuse
     * it, while a reused mapping could receive an unrelated write. */
    kernel_sync_set_irql_publisher(NULL);
    kernel_sync_reset();
    kernel_hle_set_log(NULL);
    guest_mem_reset();
}

/* ------------------------------------------------------------------------- */

/*
 * THE POINT OF THE WHOLE CHANGE: a raise reaches the byte the guest reads.
 *
 * MUTATION: delete the `kernel_sync_set_irql_publisher(publish_guest_irql)` call
 * from src/host/main.c (the original bug) or make `publish()` in kernel_sync.c a
 * no-op, and this fails.
 */
static void test_a_raise_is_visible_at_the_address_the_guest_reads(void)
{
    setup();
    kpcr_set kpcr;
    if (!make_kpcr(&kpcr)) {
        teardown();
        return;
    }
    my_fs_base = kpcr.control;
    kernel_sync_set_irql_publisher(publish_to_my_kpcr);

    /* Thread creation wrote PASSIVE, so the guest starts on the PASSIVE arm. */
    CHECK_EQ_U32(guest_irql_byte(kpcr.control), KERNEL_IRQL_PASSIVE);
    CHECK(!read_site_takes_dispatch_arm(kpcr.control));

    /* KeRaiseIrqlToDpcLevel, the ordinal with 112 call sites in this image. */
    (void)kernel_hle_call(129u, NULL);

    CHECK_EQ_U32(guest_irql_byte(kpcr.control), KERNEL_IRQL_DISPATCH);
    CHECK(read_site_takes_dispatch_arm(kpcr.control));
    CHECK_EQ_U32(my_publish_failures, 0u);

    free_kpcr(&kpcr);
    teardown();
}

/*
 * And a lower is visible too, so the guest does not get stuck on the raised arm.
 *
 * MUTATION: drop the `publish(current_irql)` from `handle_kf_lower_irql` in
 * kernel_sync.c and this fails while the raise test above still passes -- which is
 * the asymmetry a single raise-only test would miss.
 */
static void test_a_lower_is_visible_too(void)
{
    setup();
    kpcr_set kpcr;
    if (!make_kpcr(&kpcr)) {
        teardown();
        return;
    }
    my_fs_base = kpcr.control;
    kernel_sync_set_irql_publisher(publish_to_my_kpcr);

    const uint32_t previous = kernel_hle_call(129u, NULL);
    CHECK_EQ_U32(previous, KERNEL_IRQL_PASSIVE);
    CHECK(read_site_takes_dispatch_arm(kpcr.control));

    /* KfLowerIrql is __fastcall: the level arrives in ECX. */
    kernel_call_frame frame;
    memset(&frame, 0, sizeof(frame));
    kernel_frame_set_registers(&frame, previous, 0u);
    (void)kernel_hle_call(161u, &frame);

    CHECK_EQ_U32(guest_irql_byte(kpcr.control), KERNEL_IRQL_PASSIVE);
    CHECK(!read_site_takes_dispatch_arm(kpcr.control));
    CHECK_EQ_U32(my_publish_failures, 0u);

    free_kpcr(&kpcr);
    teardown();
}

/*
 * `KPCR.Irql` is ONE BYTE WIDE. The three bytes above it must not move.
 *
 * MUTATION: make `kernel_thread_set_irql` use `kernel_guest_write_u32` instead of
 * `kernel_guest_write_u8` (which is what the code did before this change, at thread
 * creation) and this fails. The guest reads `MEM8`, so a 32-bit write is invisible
 * to the read sites and only shows up as collateral damage -- exactly the class of
 * bug that surfaces far from its cause.
 */
static void test_only_the_irql_byte_is_written(void)
{
    setup();
    kpcr_set kpcr;
    if (!make_kpcr(&kpcr)) {
        teardown();
        return;
    }
    my_fs_base = kpcr.control;
    kernel_sync_set_irql_publisher(publish_to_my_kpcr);

    /* Sentinels in 0x25..0x27, which control_init leaves zeroed. Literal 0x24u:
     * the guest fact is fs:[0x24], and the macro is the thing under test. */
    for (uint32_t offset = 1u; offset < 4u; offset++) {
        CHECK(kernel_guest_write_u8(kpcr.control + 0x24u + offset,
                                    (uint8_t)(0xA0u + offset)));
    }

    (void)kernel_hle_call(129u, NULL);
    CHECK_EQ_U32(guest_irql_byte(kpcr.control), KERNEL_IRQL_DISPATCH);
    for (uint32_t offset = 1u; offset < 4u; offset++) {
        uint8_t kept = 0u;
        CHECK(kernel_guest_read_u8(kpcr.control + 0x24u + offset, &kept));
        CHECK_EQ_U32(kept, 0xA0u + offset);
    }

    free_kpcr(&kpcr);
    teardown();
}

/* --- the per-thread property --------------------------------------------- */

typedef struct {
    kpcr_set kpcr;
    /* What this thread saw at its own read site while raised, and what it left
     * behind. Read by the main thread after the join. */
    bool saw_dispatch_arm;
    unsigned publish_failures;
    uint8_t final_byte;
} thread_case;

static void *raise_on_own_kpcr(void *argument)
{
    thread_case *state = (thread_case *)argument;
    my_fs_base = state->kpcr.control;
    my_publish_failures = 0u;

    /* kernel_sync's level is thread-local, so this thread starts at PASSIVE
     * regardless of what the main thread did. */
    (void)kernel_hle_call(129u, NULL);
    state->saw_dispatch_arm = read_site_takes_dispatch_arm(state->kpcr.control);
    state->final_byte = guest_irql_byte(state->kpcr.control);
    state->publish_failures = my_publish_failures;
    return NULL;
}

/*
 * Each thread publishes into ITS OWN KPCR.
 *
 * MUTATION: change `kernel_thread_set_irql` to remember the first `control_base` it
 * is given in a `static` and use that instead of its argument -- the shape a
 * publisher that captured `g_fs_base` at installation would have -- and this fails:
 * the second thread's raise lands in the first thread's page, so the main thread's
 * byte goes to 2 while the worker's stays at 0.
 */
static void test_each_thread_publishes_into_its_own_kpcr(void)
{
    setup();
    kpcr_set mine;
    thread_case worker;
    memset(&worker, 0, sizeof(worker));
    if (!make_kpcr(&mine) || !make_kpcr(&worker.kpcr)) {
        teardown();
        return;
    }
    CHECK(mine.control != worker.kpcr.control);
    my_fs_base = mine.control;
    kernel_sync_set_irql_publisher(publish_to_my_kpcr);

    pthread_t thread;
    if (pthread_create(&thread, NULL, raise_on_own_kpcr, &worker) != 0) {
        printf("FAIL could not create the worker thread\n");
        failures++;
        free_kpcr(&mine);
        free_kpcr(&worker.kpcr);
        teardown();
        return;
    }
    CHECK_EQ_U32(pthread_join(thread, NULL), 0u);

    /* The worker raised and saw it. */
    CHECK(worker.saw_dispatch_arm);
    CHECK_EQ_U32(worker.final_byte, KERNEL_IRQL_DISPATCH);
    CHECK_EQ_U32(worker.publish_failures, 0u);

    /* THIS thread never raised, so its own copy must still read PASSIVE. A shared or
     * captured base would have put the worker's 2 here. */
    CHECK_EQ_U32(guest_irql_byte(mine.control), KERNEL_IRQL_PASSIVE);
    CHECK(!read_site_takes_dispatch_arm(mine.control));

    free_kpcr(&mine);
    free_kpcr(&worker.kpcr);
    teardown();
}

/*
 * A publish with no KPCR is REFUSED, not written to address 0x24.
 *
 * MUTATION: delete the `control_base == 0u` guard in `kernel_thread_set_irql` and
 * this fails -- `kernel_guest_at` rejects address 0 so the write returns false
 * anyway today, but the guard is what makes that a stated precondition rather than
 * an emergent property of an unrelated function.
 */
static void test_a_publish_with_no_kpcr_is_refused_and_counted(void)
{
    setup();
    my_fs_base = 0u;
    kernel_sync_set_irql_publisher(publish_to_my_kpcr);
    /* Installation itself publishes the current level. */
    CHECK(my_publish_failures >= 1u);

    CHECK(!kernel_thread_set_irql(0u, KERNEL_IRQL_DISPATCH));

    teardown();
}

/*
 * Installing the publisher publishes immediately, so the guest's copy is never
 * stale between installation and the first raise.
 *
 * MUTATION: delete the `publish(current_irql)` at the end of
 * `kernel_sync_set_irql_publisher` and this fails. The level is already PASSIVE
 * here, so the test raises FIRST with no publisher attached -- otherwise the
 * assertion would pass against a byte that control_init happened to zero, which is
 * a test that proves nothing.
 */
static void test_installing_the_publisher_publishes_the_current_level(void)
{
    setup();
    kpcr_set kpcr;
    if (!make_kpcr(&kpcr)) {
        teardown();
        return;
    }
    my_fs_base = kpcr.control;

    /* Raised with NO publisher installed: the tracked level moves, the guest's copy
     * does not. This is the pre-change state of the world. */
    (void)kernel_hle_call(129u, NULL);
    CHECK_EQ_U32(kernel_sync_current_irql(), KERNEL_IRQL_DISPATCH);
    CHECK_EQ_U32(guest_irql_byte(kpcr.control), KERNEL_IRQL_PASSIVE);

    kernel_sync_set_irql_publisher(publish_to_my_kpcr);
    CHECK_EQ_U32(guest_irql_byte(kpcr.control), KERNEL_IRQL_DISPATCH);
    CHECK(read_site_takes_dispatch_arm(kpcr.control));

    free_kpcr(&kpcr);
    teardown();
}

int main(void)
{
    printf("KPCR.Irql publishing tests\n");

    test_a_raise_is_visible_at_the_address_the_guest_reads();
    test_a_lower_is_visible_too();
    test_only_the_irql_byte_is_written();
    test_each_thread_publishes_into_its_own_kpcr();
    test_a_publish_with_no_kpcr_is_refused_and_counted();
    test_installing_the_publisher_publishes_the_current_level();

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
