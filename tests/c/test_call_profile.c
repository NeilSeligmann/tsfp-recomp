/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T422: the whole-run call profile, src/host/call_profile.c, and its hook in thunk_trace.c.
 *
 * Synthetic: calls are appended through the same `thunk_trace_append_pending` and
 * `thunk_trace_patch_result` the dispatchers use, so the hook is part of what is tested. No lifted
 * code, XBE or disc. Every group asserts that something was counted before it compares numbers
 * (an empty profile equals an empty expectation), and each says what breaks it.
 */

#include "call_profile.h"

#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "kernel_memory.h"
#include "nt_status.h"
#include "recomp_abi.h"
#include "thunk_trace.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

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

#define SWAP 0x003D8E50u
#define DRAW 0x003D4FB0u

/* One completed XDK call, the way a dispatcher records it: pending, then patched. */
static void xdk_call(uint32_t address, uint32_t caller, uint32_t result)
{
    const size_t slot = thunk_trace_append_pending(THUNK_KIND_XDK, 0u, address, caller, true);
    thunk_trace_patch_result(slot, result);
}

static void ordinal_call(unsigned ordinal, uint32_t caller, uint32_t result)
{
    const size_t slot = thunk_trace_append_pending(THUNK_KIND_ORDINAL, ordinal, 0u, caller, true);
    thunk_trace_patch_result(slot, result);
}

static void fresh(bool enabled)
{
    thunk_trace_reset();
    call_profile_enable(enabled);
}

static const char *fake_ordinal_name(unsigned ordinal)
{
    return ordinal == 21u ? "FakeOrdinalName" : NULL;
}

static const char *fake_xdk_name(uint32_t address)
{
    return address == SWAP ? "FakeSwap" : NULL;
}

static void test_disabled_counts_nothing(void)
{
    fresh(false);
    xdk_call(SWAP, 0x1000u, 1u);
    ordinal_call(21u, 0x2000u, 0u);
    /* The calls really happened, so an empty profile is the default's doing and not an idle test. */
    CHECK_EQ(thunk_trace_total(), 2u);
    CHECK(!call_profile_enabled());
    CHECK_EQ(call_profile_total(), 0u);
    call_profile_row row;
    CHECK_EQ(call_profile_rows(&row, 1u), 0u);
    call_profile_thread thread;
    CHECK_EQ(call_profile_threads(&thread, 1u), 0u);
    CHECK(!call_profile_set_limit(THUNK_KIND_XDK, SWAP, 3u));
}

static void test_counts_per_address_and_caller(void)
{
    fresh(true);
    CHECK(call_profile_enabled());
    for (unsigned index = 0u; index < 3u; index++) {
        xdk_call(SWAP, 0x1000u, index);
    }
    xdk_call(SWAP, 0x1100u, 0u);
    xdk_call(DRAW, 0x1000u, 0u);
    ordinal_call(21u, 0x2000u, 0u);
    ordinal_call(21u, 0x2000u, 0u);
    /* The same number under the other kind is a different address. */
    xdk_call(21u, 0x2000u, 0u);

    CHECK_EQ(call_profile_total(), 8u);
    CHECK_EQ(call_profile_count_of(THUNK_KIND_XDK, SWAP), 4u);
    CHECK_EQ(call_profile_count_of(THUNK_KIND_XDK, DRAW), 1u);
    CHECK_EQ(call_profile_count_of(THUNK_KIND_ORDINAL, 21u), 2u);
    CHECK_EQ(call_profile_count_of(THUNK_KIND_XDK, 21u), 1u);
    CHECK_EQ(call_profile_count_of(THUNK_KIND_XDK, 0x1234u), 0u);

    call_profile_row rows[16];
    const size_t count = call_profile_rows(rows, 16u);
    CHECK_EQ(count, 5u);
    CHECK(count == 5u);
    /* Busiest first, then the stable order. */
    CHECK_EQ(rows[0].count, 3u);
    CHECK_EQ(rows[0].id, SWAP);
    CHECK_EQ(rows[0].caller, 0x1000u);
    CHECK_EQ(rows[1].count, 2u);
    CHECK_EQ(rows[1].kind, THUNK_KIND_ORDINAL);
    CHECK_EQ(rows[1].id, 21u);
    CHECK_EQ(rows[2].count, 1u);
    for (size_t index = 1u; index < count; index++) {
        CHECK(rows[index - 1u].count >= rows[index].count);
    }
}

static void test_thread_ring_and_progress(void)
{
    fresh(true);
    for (uint32_t index = 1u; index <= 100u; index++) {
        xdk_call(SWAP, 0x4000u + index, index);
    }
    call_profile_thread thread;
    const size_t threads = call_profile_threads(&thread, 1u);
    CHECK_EQ(threads, 1u);
    CHECK_EQ(thread.dispatches, 100u);
    CHECK_EQ(thread.returns, 100u);
    CHECK(!thread.in_flight);
    CHECK_EQ(thread.recent_count, CALL_PROFILE_RING);
    /* The last 64 of 100, oldest first. */
    CHECK_EQ(thread.recent[0].sequence, 37u);
    CHECK_EQ(thread.recent[0].caller, 0x4000u + 37u);
    CHECK_EQ(thread.recent[CALL_PROFILE_RING - 1u].sequence, 100u);
    CHECK_EQ(thread.recent[CALL_PROFILE_RING - 1u].caller, 0x4000u + 100u);
    CHECK_EQ(thread.recent[CALL_PROFILE_RING - 1u].result, 100u);
    for (size_t index = 1u; index < thread.recent_count; index++) {
        CHECK_EQ(thread.recent[index].sequence, thread.recent[index - 1u].sequence + 1u);
    }
}

static void test_in_flight_call_is_the_wait(void)
{
    fresh(true);
    xdk_call(DRAW, 0x10u, 0u);
    /* A call that has not returned, as a thread blocked in a wait. */
    const size_t wait =
        thunk_trace_append_pending(THUNK_KIND_ORDINAL, 99u, 0u, 0x5000u, true);
    call_profile_thread thread;
    CHECK_EQ(call_profile_threads(&thread, 1u), 1u);
    CHECK(thread.in_flight);
    CHECK_EQ(thread.dispatches, 2u);
    CHECK_EQ(thread.returns, 1u);
    CHECK_EQ(thread.current.kind, THUNK_KIND_ORDINAL);
    CHECK_EQ(thread.current.id, 99u);
    CHECK_EQ(thread.current.caller, 0x5000u);
    CHECK(!thread.current.returned);
    CHECK(!thread.recent[thread.recent_count - 1u].returned);
    CHECK(thread.recent[0].returned);

    /* A nested dispatch returns first: the outer call is still the one the thread is inside. */
    const size_t nested = thunk_trace_append_pending(THUNK_KIND_XDK, 0u, SWAP, 0x6000u, true);
    thunk_trace_patch_result(nested, 7u);
    CHECK_EQ(call_profile_threads(&thread, 1u), 1u);
    CHECK(thread.in_flight);
    CHECK_EQ(thread.current.id, 99u);
    CHECK_EQ(thread.recent[thread.recent_count - 1u].id, SWAP);
    CHECK(thread.recent[thread.recent_count - 1u].returned);
    CHECK_EQ(thread.recent[thread.recent_count - 1u].result, 7u);
    CHECK(!thread.recent[thread.recent_count - 2u].returned);

    thunk_trace_patch_result(wait, 0x102u);
    CHECK_EQ(call_profile_threads(&thread, 1u), 1u);
    CHECK(!thread.in_flight);
    CHECK_EQ(thread.returns, 3u);
    CHECK(thread.recent[thread.recent_count - 2u].returned);
    CHECK_EQ(thread.recent[thread.recent_count - 2u].result, 0x102u);
}

static void test_table_overflow_is_counted(void)
{
    fresh(true);
    const uint32_t extra = 11u;
    for (uint32_t index = 0u; index < CALL_PROFILE_ROWS + extra; index++) {
        xdk_call(SWAP, 0x10000u + index, 0u);
    }
    CHECK_EQ(thunk_trace_total(), CALL_PROFILE_ROWS + extra);
    CHECK_EQ(call_profile_total(), CALL_PROFILE_ROWS + extra);
    CHECK(call_profile_dropped() != 0u);
    static call_profile_row rows[CALL_PROFILE_ROWS + 64u];
    const size_t kept = call_profile_rows(rows, CALL_PROFILE_ROWS + 64u);
    CHECK(kept != 0u);
    /* One slot stays free so a probe always ends. Every dispatch is a row or a drop. */
    CHECK_EQ(kept + call_profile_dropped(), CALL_PROFILE_ROWS + extra);
    CHECK_EQ(kept, CALL_PROFILE_ROWS - 1u);
    /* A row that fits keeps counting after the table is full. */
    xdk_call(SWAP, 0x10000u, 0u);
    CHECK_EQ(call_profile_count_of(THUNK_KIND_XDK, SWAP), kept + 1u);
}

static void test_limit_stops_at_the_nth_dispatch(void)
{
    fresh(true);
    CHECK(!call_profile_set_limit(THUNK_KIND_XDK, SWAP, 0u));
    CHECK(call_profile_set_limit(THUNK_KIND_XDK, SWAP, 3u));
    /* Other addresses never count against the limit. */
    xdk_call(DRAW, 0x10u, 0u);
    xdk_call(DRAW, 0x10u, 0u);
    xdk_call(SWAP, 0x20u, 0u);
    xdk_call(SWAP, 0x20u, 0u);
    xdk_call(SWAP, 0x30u, 0u);
    CHECK_EQ(call_profile_count_of(THUNK_KIND_XDK, SWAP), 3u);
    CHECK(!call_profile_limit_reached());

    host_stop stop_record;
    memset(&stop_record, 0, sizeof(stop_record));
    bool stopped = false;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        xdk_call(SWAP, 0x40u, 0u); /* the fourth: stops before it is counted */
    } else {
        stopped = true;
        stop_record = *host_run_result();
    }
    host_run_disarm();
    CHECK(stopped);
    CHECK_EQ(stop_record.reason, HOST_STOP_BUDGET);
    CHECK_EQ(stop_record.guest_address, SWAP);
    CHECK(call_profile_limit_reached());
    CHECK_EQ(call_profile_count_of(THUNK_KIND_XDK, SWAP), 3u);
    CHECK_EQ(call_profile_total(), 5u);
    /* thunk_trace still recorded the stopper, as every other stop does. */
    CHECK_EQ(thunk_trace_total(), 6u);

    /* Once reached, any later dispatch stops too, whatever its address. */
    stopped = false;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        xdk_call(DRAW, 0x50u, 0u);
    } else {
        stopped = true;
    }
    host_run_disarm();
    CHECK(stopped);
    CHECK_EQ(call_profile_count_of(THUNK_KIND_XDK, DRAW), 2u);
}

static void test_ordinal_limit(void)
{
    fresh(true);
    CHECK(call_profile_set_limit(THUNK_KIND_ORDINAL, 21u, 2u));
    xdk_call(21u, 0x10u, 0u); /* the XDK address 21 is not ordinal 21 */
    ordinal_call(21u, 0x20u, 0u);
    ordinal_call(21u, 0x20u, 0u);
    CHECK(!call_profile_limit_reached());
    /* The limit is reached for ordinal 21, but XDK address 21 is another address: it neither trips
     * the cut nor counts toward it. */
    xdk_call(21u, 0x10u, 0u);
    CHECK(!call_profile_limit_reached());
    CHECK_EQ(call_profile_count_of(THUNK_KIND_XDK, 21u), 2u);
    CHECK_EQ(call_profile_count_of(THUNK_KIND_ORDINAL, 21u), 2u);
    bool stopped = false;
    host_stop stop_record;
    memset(&stop_record, 0, sizeof(stop_record));
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        ordinal_call(21u, 0x20u, 0u);
    } else {
        stopped = true;
        stop_record = *host_run_result();
    }
    host_run_disarm();
    CHECK(stopped);
    CHECK_EQ(stop_record.reason, HOST_STOP_BUDGET);
    CHECK_EQ(stop_record.ordinal, 21u);
}

static char report_text[16384];

static void render_report(bool include_stack)
{
    (void)include_stack;
    FILE *file = tmpfile();
    CHECK(file != NULL);
    if (file == NULL) {
        report_text[0] = '\0';
        return;
    }
    const call_profile_names names = {.ordinal_name = fake_ordinal_name, .xdk_name = fake_xdk_name};
    call_profile_report(file, 4u, &names);
    fflush(file);
    rewind(file);
    const size_t length = fread(report_text, 1u, sizeof(report_text) - 1u, file);
    report_text[length] = '\0';
    fclose(file);
}

static void test_report_names_the_blocked_thread(void)
{
    fresh(true);
    xdk_call(SWAP, 0x1000u, 0u);
    (void)thunk_trace_append_pending(THUNK_KIND_ORDINAL, 21u, 0u, 0x7000u, true);
    render_report(false);
    CHECK(strlen(report_text) != 0u);
    CHECK(strstr(report_text, "dispatches counted 2 over 2 distinct address(es)") != NULL);
    CHECK(strstr(report_text, "FakeSwap") != NULL);
    CHECK(strstr(report_text, "FakeOrdinalName") != NULL);
    CHECK(strstr(report_text, "INSIDE a call that has not returned") != NULL);
    CHECK(strstr(report_text, "current call  ord 00000015 FakeOrdinalName from 00007000") != NULL);
    CHECK(strstr(report_text, "(WALL CLOCK)") != NULL);
    CHECK(strstr(report_text, "STOPPED by --stop-after-calls") == NULL);
}

static void test_stack_scrape(void)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = 0x2000u;
    request.alignment = 0x1000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    const kernel_guest_ptr base = guest_region_alloc(&request, &status);
    CHECK(base != 0u);
    if (base == 0u) {
        return;
    }
    /* Code at base: the bytes before 0x100 are `call rel32`, before 0x200 `call eax`, and the word
     * 0x300 follows no call. The stack sits at base + 0x1000, outside the code span. */
    const kernel_guest_ptr stack = base + 0x1000u;
    CHECK(kernel_guest_write_u32(stack + 0u, base + 0x300u));        /* not after a call */
    CHECK(kernel_guest_write_u32(stack + 4u, base + 0x100u));        /* after E8 rel32 (+5 bytes) */
    CHECK(kernel_guest_write_u32(stack + 8u, base + 0x200u));        /* after FF D0 */
    CHECK(kernel_guest_write_u32(stack + 12u, 0x12345678u));         /* outside the code span */

    /* E8 xx xx xx xx ends at +0x100, FF D0 ends at +0x200. */
    const uint8_t call_rel32[5] = {0xE8u, 1u, 2u, 3u, 4u};
    CHECK(kernel_guest_write_bytes(base + 0xFBu, call_rel32, sizeof(call_rel32)));
    const uint8_t call_eax[2] = {0xFFu, 0xD0u};
    CHECK(kernel_guest_write_bytes(base + 0x1FEu, call_eax, sizeof(call_eax)));
    /* FF /6 (push [eax]) and FF /0 (inc [eax]) are not calls: the words after them are data. */
    const uint8_t push_eax[2] = {0xFFu, 0x30u};
    CHECK(kernel_guest_write_bytes(base + 0x3FEu, push_eax, sizeof(push_eax)));
    const uint8_t inc_eax[2] = {0xFFu, 0x00u};
    CHECK(kernel_guest_write_bytes(base + 0x4FEu, inc_eax, sizeof(inc_eax)));
    CHECK(kernel_guest_write_u32(stack + 16u, base + 0x400u));
    CHECK(kernel_guest_write_u32(stack + 20u, base + 0x500u));

    fresh(true);
    call_profile_set_code_range(base, base + 0x1000u);
    CHECK(call_profile_set_limit(THUNK_KIND_XDK, SWAP, 1u));
    xdk_call(SWAP, 0x10u, 0u);
    g_esp = stack;
    bool stopped = false;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        xdk_call(SWAP, 0x10u, 0u);
    } else {
        stopped = true;
    }
    host_run_disarm();
    CHECK(stopped);
    uint32_t chain[CALL_PROFILE_STACK];
    const size_t count = call_profile_stack(chain, CALL_PROFILE_STACK);
    CHECK(count != 0u);
    CHECK_EQ(count, 2u);
    if (count == 2u) {
        CHECK_EQ(chain[0], base + 0x100u);
        CHECK_EQ(chain[1], base + 0x200u);
    }
    render_report(true);
    CHECK(strstr(report_text, "guest stack of the stopping thread") != NULL);

    /* Without a code range the scrape is off. */
    fresh(true);
    call_profile_set_code_range(0u, 0u);
    CHECK(call_profile_set_limit(THUNK_KIND_XDK, SWAP, 1u));
    xdk_call(SWAP, 0x10u, 0u);
    stopped = false;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        xdk_call(SWAP, 0x10u, 0u);
    } else {
        stopped = true;
    }
    host_run_disarm();
    CHECK(stopped);
    CHECK_EQ(call_profile_stack(chain, CALL_PROFILE_STACK), 0u);
}

/* T1250 gaps: a call that blocks 30 ms and a 60 ms guest compute stretch between two dispatches are logged with their
 * duration, a quick call and a short gap are not. */
static void sleep_ms(long milliseconds)
{
    const struct timespec nap = {0, milliseconds * 1000000L};
    nanosleep(&nap, NULL);
}

static void test_slow_calls_are_logged(void)
{
    fresh(true);
    xdk_call(SWAP, 0x1000u, 0u);
    xdk_call(SWAP, 0x1000u, 0u); /* quick, no gap: not logged */
    render_report(false);
    CHECK(strstr(report_text, "slow calls (T1250 gaps): 0 entries") != NULL);
    sleep_ms(60); /* compute between dispatches */
    const size_t slot = thunk_trace_append_pending(THUNK_KIND_ORDINAL, 21u, 0u, 0x7000u, false);
    sleep_ms(30); /* the blocked call */
    thunk_trace_patch_result(slot, 0u);
    call_profile_note_return(0u);
    render_report(false);
    CHECK(strstr(report_text, "slow calls (T1250 gaps): 2 entries") != NULL);
    CHECK(strstr(report_text, "COMPUTE-GAP-BEFORE ord 00000015 FakeOrdinalName from 00007000") != NULL);
    CHECK(strstr(report_text, "CALL ord 00000015 FakeOrdinalName from 00007000") != NULL);
    CHECK(strstr(report_text, "slow call  clock ") != NULL);
}

int main(void)
{
    test_disabled_counts_nothing();
    test_counts_per_address_and_caller();
    test_thread_ring_and_progress();
    test_in_flight_call_is_the_wait();
    test_table_overflow_is_counted();
    test_limit_stops_at_the_nth_dispatch();
    test_ordinal_limit();
    test_report_names_the_blocked_thread();
    test_stack_scrape();
    test_slow_calls_are_logged();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
