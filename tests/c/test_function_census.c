/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T821: the read-only indirect call census, src/host/function_census.c.
 *
 * Synthetic: targets are fed through `function_census_note` with a stack whose top dword is the pushed return
 * address, a fake present counter stands in for the host's. No lifted code, XBE or disc. Each group asserts that
 * something was counted before it compares numbers (an empty census equals an empty expectation).
 */

#include "function_census.h"

#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_memory.h"
#include "nt_status.h"
#include "recomp_abi.h"

#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

/* the thunk library reads the guest registers, this suite defines them */
TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;

static int failures;
static int checks;
static uint64_t present_now;

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

static uint64_t fake_present(void)
{
    return present_now;
}

static kernel_guest_ptr make_stack(uint32_t pushed)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = 0x1000u;
    request.alignment = 0x1000u;
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;
    nt_status status = STATUS_SUCCESS;
    const kernel_guest_ptr base = guest_region_alloc(&request, &status);
    CHECK(base != 0u);
    if (base != 0u) {
        CHECK(kernel_guest_write_u32(base, pushed));
    }
    return base;
}

static void test_off_counts_nothing(void)
{
    function_census_enable(false);
    function_census_set_present_source(fake_present);
    function_census_note(0x2D6D50u, 0u);
    CHECK(!function_census_enabled());
    CHECK_EQ(function_census_total(), 0u);
    CHECK_EQ(function_census_target_count(), 0u);
    function_census_target row;
    CHECK(!function_census_get(0x2D6D50u, &row));
    /* target 0 must not alias an empty hash slot */
    CHECK(!function_census_get(0u, &row));
}

static void test_counts_first_and_last_presents(void)
{
    const kernel_guest_ptr stack = make_stack(0x00401234u);
    if (stack == 0u) {
        return;
    }
    function_census_set_window(1u, 0u);
    function_census_enable(true);
    CHECK(function_census_enabled());
    present_now = 10u;
    function_census_note(0x2D6D50u, stack);
    function_census_note(0x2D6D50u, stack);
    function_census_note(0x2D2260u, stack);
    present_now = 11u;
    function_census_note(0x2D6D50u, stack);
    present_now = 14u;
    function_census_note(0x2D6D50u, stack);
    function_census_note(0x2D6D50u, stack);
    function_census_note(0x191B10u, stack);
    CHECK_EQ(function_census_total(), 7u);
    CHECK_EQ(function_census_target_count(), 3u);
    function_census_target row;
    CHECK(function_census_get(0x2D6D50u, &row));
    CHECK_EQ(row.calls, 5u);
    CHECK_EQ(row.first_present, 10u);
    CHECK_EQ(row.first_sequence, 1u);
    CHECK_EQ(row.first_caller, 0x00401234u);
    CHECK_EQ(row.last_present, 14u);
    CHECK_EQ(row.presents_called, 3u);
    CHECK_EQ(row.calls_in_last, 2u);
    CHECK_EQ(row.calls_in_previous, 1u);
    CHECK_EQ(row.previous_present_gap, 3u);
    CHECK(function_census_get(0x2D2260u, &row));
    CHECK_EQ(row.calls, 1u);
    CHECK_EQ(row.first_present, 10u);
    CHECK_EQ(row.last_present, 10u);
    CHECK_EQ(row.first_sequence, 3u);
    CHECK(function_census_get(0x191B10u, &row));
    CHECK_EQ(row.first_present, 14u);
    CHECK_EQ(row.first_sequence, 7u);
    /* order of first appearance */
    function_census_target ordered[8];
    const size_t count = function_census_targets(ordered, 8u);
    CHECK_EQ(count, 3u);
    if (count == 3u) {
        CHECK_EQ(ordered[0].target, 0x2D6D50u);
        CHECK_EQ(ordered[1].target, 0x2D2260u);
        CHECK_EQ(ordered[2].target, 0x191B10u);
    }
    /* a wild stack pointer reads as caller 0 and is still counted */
    function_census_note(0x408C0Du, 0u);
    CHECK(function_census_get(0x408C0Du, &row));
    CHECK_EQ(row.first_caller, 0u);
    /* enabling again clears */
    function_census_enable(true);
    CHECK_EQ(function_census_total(), 0u);
    CHECK_EQ(function_census_target_count(), 0u);
}

static void test_window_keeps_the_calls_in_order(void)
{
    const kernel_guest_ptr stack = make_stack(0x00402000u);
    if (stack == 0u) {
        return;
    }
    function_census_set_window(5u, 6u);
    function_census_enable(true);
    present_now = 4u;
    function_census_note(0x100u, stack);
    present_now = 5u;
    function_census_note(0x200u, stack);
    function_census_note(0x300u, stack);
    present_now = 6u;
    function_census_note(0x200u, stack);
    present_now = 7u;
    function_census_note(0x400u, stack);
    function_census_event events[8];
    const size_t count = function_census_events(events, 8u);
    CHECK_EQ(count, 3u);
    if (count == 3u) {
        CHECK_EQ(events[0].target, 0x200u);
        CHECK_EQ(events[0].present, 5u);
        CHECK_EQ(events[0].sequence, 2u);
        CHECK_EQ(events[0].caller, 0x00402000u);
        CHECK_EQ(events[1].target, 0x300u);
        CHECK_EQ(events[2].target, 0x200u);
        CHECK_EQ(events[2].present, 6u);
    }
    CHECK_EQ(function_census_event_overflow(), 0u);
    char *text = NULL;
    size_t length = 0u;
    FILE *out = open_memstream(&text, &length);
    CHECK(out != NULL);
    if (out != NULL) {
        function_census_report(out);
        fclose(out);
        CHECK(text != NULL && strstr(text, "census window presents 5..6: 3 indirect calls") != NULL);
        CHECK(text != NULL && strstr(text, "census target 0x00000200 first present 5") != NULL);
        CHECK(text != NULL && strstr(text, "census live 0x00000400") != NULL);
        free(text);
    }
}

static void test_table_overflow_is_counted(void)
{
    function_census_set_window(1u, 0u);
    function_census_enable(true);
    present_now = 1u;
    for (uint32_t target = 0u; target < FUNCTION_CENSUS_TARGETS + 10u; target++) {
        function_census_note(0x400000u + 4u * target, 0u);
    }
    CHECK_EQ(function_census_target_count(), FUNCTION_CENSUS_TARGETS);
    CHECK_EQ(function_census_overflow(), 10u);
    CHECK_EQ(function_census_total(), FUNCTION_CENSUS_TARGETS + 10u);
    function_census_target row;
    CHECK(function_census_get(0x400000u, &row));
    CHECK(!function_census_get(0x400000u + 4u * (FUNCTION_CENSUS_TARGETS + 5u), &row));
}

static char *slurp(const char *path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return NULL;
    }
    char *text = calloc(1u, 1u << 20);
    if (text != NULL) {
        (void)fread(text, 1u, (1u << 20) - 1u, file);
    }
    fclose(file);
    return text;
}

/* T1502: the dump writes the phase and resets, so the next phase holds only its own calls. */
static void test_phase_dump_resets(void)
{
    const kernel_guest_ptr stack = make_stack(0x00402000u);
    function_census_set_window(1u, 0u);
    function_census_enable(true);
    present_now = 10u;
    function_census_note(0x111u, stack);
    function_census_note(0x111u, stack);
    present_now = 11u;
    function_census_note(0x111u, stack);
    function_census_note(0x222u, stack);
    CHECK(function_census_dump_phase("census_phase_a.txt"));
    CHECK_EQ(function_census_total(), 0u);
    CHECK_EQ(function_census_target_count(), 0u);
    present_now = 12u;
    function_census_note(0x333u, stack);
    CHECK(function_census_dump_phase("census_phase_b.txt"));
    CHECK(function_census_enabled());
    char *first = slurp("census_phase_a.txt");
    char *second = slurp("census_phase_b.txt");
    CHECK(first != NULL && second != NULL);
    if (first != NULL && second != NULL) {
        CHECK(strstr(first, "# calls 4 targets 2 overflow 0 first_present 10 last_present 11") != NULL);
        CHECK(strstr(first, "target 0x00000111 calls 3 presents 2 first_present 10 last_present 11 in_last 1 "
                            "first_caller 0x00402000 thread ") != NULL);
        CHECK(strstr(first, "target 0x00000222 calls 1 presents 1") != NULL);
        CHECK(strstr(first, "0x00000333") == NULL);
        CHECK(strstr(second, "# calls 1 targets 1 overflow 0 first_present 12 last_present 12") != NULL);
        CHECK(strstr(second, "0x00000111") == NULL);
        CHECK(strstr(second, "target 0x00000333 calls 1 presents 1") != NULL);
    }
    free(first);
    free(second);
}

/* The overflow of the 4096 target table is reported per phase and reset with it. */
static void test_phase_reports_overflow_per_phase(void)
{
    function_census_enable(true);
    present_now = 1u;
    for (uint32_t target = 0u; target < FUNCTION_CENSUS_TARGETS + 7u; target++) {
        function_census_note(0x500000u + 4u * target, 0u);
    }
    CHECK(function_census_dump_phase("census_phase_o.txt"));
    char *text = slurp("census_phase_o.txt");
    CHECK(text != NULL && strstr(text, "overflow 7 ") != NULL);
    free(text);
    function_census_note(0x500000u, 0u);
    CHECK_EQ(function_census_overflow(), 0u);
    CHECK_EQ(function_census_target_count(), 1u);
}

static bool wait_for_ack(unsigned want)
{
    for (int attempt = 0; attempt < 250; attempt++) {
        char *text = slurp("census_sig.txt.ack");
        if (text != NULL) {
            const unsigned got = (unsigned)strtoul(text, NULL, 10);
            free(text);
            if (got >= want) {
                return true;
            }
        }
        struct timespec nap = {0, 20000000L};
        nanosleep(&nap, NULL);
    }
    return false;
}

/* The real SIGUSR1 path: pid file, control file phase name, ack, reset between phases. */
static void test_sigusr1_phases(void)
{
    const kernel_guest_ptr stack = make_stack(0x00402000u);
    function_census_enable(true);
    CHECK(function_census_phases_start("census_sig.txt"));
    char *pid_text = slurp("census_sig.txt.pid");
    CHECK(pid_text != NULL && (pid_t)strtol(pid_text, NULL, 10) == getpid());
    free(pid_text);
    present_now = 20u;
    function_census_note(0xA1u, stack);
    FILE *control = fopen("census_sig.txt.phase", "w");
    CHECK(control != NULL);
    if (control != NULL) {
        fputs("menu-idle\n", control);
        fclose(control);
    }
    kill(getpid(), SIGUSR1);
    CHECK(wait_for_ack(1u));
    present_now = 21u;
    function_census_note(0xB2u, stack);
    control = fopen("census_sig.txt.phase", "w");
    if (control != NULL) {
        fputs("fire\n", control);
        fclose(control);
    }
    kill(getpid(), SIGUSR1);
    CHECK(wait_for_ack(2u));
    char *idle = slurp("census_sig.txt.menu-idle");
    char *fire = slurp("census_sig.txt.fire");
    CHECK(idle != NULL && fire != NULL);
    if (idle != NULL && fire != NULL) {
        CHECK(strstr(idle, "target 0x000000A1 calls 1") != NULL);
        CHECK(strstr(idle, "0x000000B2") == NULL);
        CHECK(strstr(fire, "target 0x000000B2 calls 1") != NULL);
        CHECK(strstr(fire, "0x000000A1") == NULL);
    }
    free(idle);
    free(fire);
    function_census_phases_stop();
}

int main(void)
{
    test_off_counts_nothing();
    test_counts_first_and_last_presents();
    test_window_keeps_the_calls_in_order();
    test_table_overflow_is_counted();
    test_phase_dump_resets();
    test_phase_reports_overflow_per_phase();
    test_sigusr1_phases();
    function_census_enable(false);
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
