/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests for the D3D8 HLE dispatch. The diagnostics are the feature, so most of
 * these assert on what gets reported, and on the ORDER it gets reported in,
 * rather than on return values.
 *
 * The surface table here is a local fake. The real one lives in a generated,
 * gitignored file, so a test that depended on it would fail on a fresh clone.
 */

#include "d3d8_hle.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                \
    do {                                                                           \
        checks++;                                                                  \
        if (!(cond)) {                                                             \
            failures++;                                                            \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);               \
        }                                                                          \
    } while (0)

/* Capture diagnostics so tests can assert on exact content and line counts. */
static char captured[8192];
static size_t captured_length;
static size_t captured_lines;

static int capture_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(captured + captured_length,
                            sizeof(captured) - captured_length, format, args);
    va_end(args);
    if (written > 0) {
        const char *start = captured + captured_length;
        captured_length += (size_t)written;
        if (captured_length >= sizeof(captured)) {
            captured_length = sizeof(captured) - 1;
        }
        for (const char *scan = start; *scan; scan++) {
            if (*scan == '\n') {
                captured_lines++;
            }
        }
    }
    return written;
}

static void reset_capture(void)
{
    captured[0] = '\0';
    captured_length = 0;
    captured_lines = 0;
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

/* Position of a needle in the captured text, or SIZE_MAX when absent. Used to
 * assert report ORDER without caring about the rest of the line. */
static size_t captured_position(const char *needle)
{
    const char *found = strstr(captured, needle);
    return found ? (size_t)(found - captured) : (size_t)-1;
}

/* Deliberately varied sites, and names on only some rows, mirroring the real
 * surface where only 19 of 85 functions have a recovered name.
 *
 * Note the orderings, which the mutation tests depend on. Address order is the
 * natural table order; descending sites is 0x00100000, 0x00100080, 0x00100040,
 * 0x00100100, 0x001000c0, which is NOT address order. So a report that prints in
 * table order is distinguishable from one that ranks by sites. */
static const d3d8_surface_entry fake_surface[] = {
    {0x00100000u, "D3DDevice_SetRenderState", 93u},
    {0x00100040u, NULL, 17u},
    {0x00100080u, "D3DDevice_DrawVertices", 41u},
    /* One site only: in the real data this is as likely to be E8 scanning noise
     * as a real function, which is why it ranks last. */
    {0x001000c0u, NULL, 1u},
    {0x00100100u, "D3DDevice_Present", 12u},
};
static const size_t fake_surface_count =
    sizeof(fake_surface) / sizeof(fake_surface[0]);

/* A second, disjoint surface, for proving that re-init really replaces. */
static const d3d8_surface_entry other_surface[] = {
    {0x00200000u, "D3DDevice_Clear", 5u},
    {0x00200040u, NULL, 2u},
};

static const uint32_t absent_address = 0x00ABCDEFu;

static uint32_t fake_handler(void *context)
{
    (void)context;
    return 0xFEEDFACEu;
}

static void adopt_fake_surface(void)
{
    CHECK(d3d8_hle_init(fake_surface, fake_surface_count));
    reset_capture();
}

static void test_stub_reports_once_not_every_call(void)
{
    adopt_fake_surface();

    for (int i = 0; i < 5; i++) {
        CHECK(d3d8_hle_call(0x00100000u, NULL) == 0);
    }

    /* A stub inside a per-frame draw loop must not bury every other diagnostic,
     * so the report is once per address and not once per call. */
    CHECK(captured_lines == 1);
    CHECK(captured_contains("not implemented"));

    /* Reporting once must not mean counting once. The count is what ranks the
     * backlog, so it has to keep moving after the report has been emitted. */
    const d3d8_entry *entry = d3d8_hle_entry(0x00100000u);
    CHECK(entry != NULL);
    CHECK(entry->call_count == 5);
}

static void test_unknown_address_reports_every_call(void)
{
    adopt_fake_surface();

    for (int i = 0; i < 4; i++) {
        CHECK(d3d8_hle_call(absent_address, NULL) == 0);
    }

    /* Unlike a stub this is never rate-limited: an unknown target means the
     * measured surface and the guest disagree, which is worse than unfinished
     * work and must never be summarised away. */
    CHECK(captured_lines == 4);
    CHECK(captured_contains("UNKNOWN"));
    CHECK(captured_contains("0x00abcdef"));
    CHECK(d3d8_hle_unknown_count() == 4);
}

static void test_backlog_sorted_busiest_first(void)
{
    adopt_fake_surface();

    /* Call counts are set up as the REVERSE of the measured site order, and of
     * the address order, so a report that sorts by sites or does not sort at all
     * is caught here rather than passing by accident.
     *
     *   0x00100000  sites 93  calls 1
     *   0x00100080  sites 41  calls 3
     *   0x00100100  sites 12  calls 7   <- busiest, fewest sites, highest address
     */
    d3d8_hle_call(0x00100000u, NULL);
    for (int i = 0; i < 3; i++) {
        d3d8_hle_call(0x00100080u, NULL);
    }
    for (int i = 0; i < 7; i++) {
        d3d8_hle_call(0x00100100u, NULL);
    }

    reset_capture();
    d3d8_hle_report_backlog();

    size_t busiest = captured_position("0x00100100");
    size_t middle = captured_position("0x00100080");
    size_t quietest = captured_position("0x00100000");
    CHECK(busiest != (size_t)-1);
    CHECK(middle != (size_t)-1);
    CHECK(quietest != (size_t)-1);
    CHECK(busiest < middle);
    CHECK(middle < quietest);

    CHECK(captured_contains("5 of 5 measured functions"));
}

static void test_lookup_returns_the_requested_entry(void)
{
    adopt_fake_surface();

    for (size_t i = 0; i < fake_surface_count; i++) {
        uint32_t address = fake_surface[i].address;
        const d3d8_entry *entry = d3d8_hle_entry(address);
        CHECK(entry != NULL);
        if (!entry) {
            continue;
        }
        CHECK(entry->address == address);
        CHECK(entry->sites == fake_surface[i].sites);
        CHECK(entry->name == fake_surface[i].name);
        CHECK(entry->state == D3D8_ENTRY_STUB);

        /* One byte either side is a different instruction. Resolving it to the
         * neighbour would turn a measurement error into a plausible wrong answer. */
        CHECK(d3d8_hle_entry(address - 1u) == NULL);
        CHECK(d3d8_hle_entry(address + 1u) == NULL);
    }

    CHECK(d3d8_hle_entry(absent_address) == NULL);
    CHECK(d3d8_hle_entry(0u) == NULL);
    CHECK(d3d8_hle_entry(0xFFFFFFFFu) == NULL);
    /* Just below the lowest and just above the highest row. */
    CHECK(d3d8_hle_entry(0x000FFFFFu) == NULL);
    CHECK(d3d8_hle_entry(0x00100101u) == NULL);
}

static void test_named_and_unnamed_stubs_report_differently(void)
{
    adopt_fake_surface();

    d3d8_hle_call(0x00100000u, NULL);
    CHECK(captured_lines == 1);
    CHECK(captured_contains("D3DDevice_SetRenderState"));
    CHECK(captured_contains("0x00100000"));

    reset_capture();
    d3d8_hle_call(0x00100040u, NULL);
    CHECK(captured_lines == 1);
    CHECK(captured_contains("0x00100040"));
    /* An unnamed row must not claim a name it does not have. */
    CHECK(!captured_contains("D3DDevice"));
    CHECK(captured_contains("17 call sites"));
}

static void test_registered_handler_runs_and_suppresses_the_report(void)
{
    adopt_fake_surface();

    CHECK(d3d8_hle_register(0x00100080u, fake_handler));
    const d3d8_entry *entry = d3d8_hle_entry(0x00100080u);
    CHECK(entry != NULL);
    CHECK(entry->state == D3D8_ENTRY_IMPLEMENTED);

    CHECK(d3d8_hle_call(0x00100080u, NULL) == 0xFEEDFACEu);
    /* An implemented function is silent. */
    CHECK(captured_lines == 0);
    CHECK(entry->call_count == 1);
    CHECK(d3d8_hle_implemented_count() == 1);

    reset_capture();
    d3d8_hle_report_backlog();
    CHECK(captured_contains("4 of 5 measured functions"));
    CHECK(!captured_contains("D3DDevice_DrawVertices"));
    CHECK(!captured_contains("0x00100080"));

    /* Bad registrations must be refused rather than silently accepted. */
    CHECK(!d3d8_hle_register(absent_address, fake_handler));
    CHECK(!d3d8_hle_register(0x00100000u, NULL));
}

static void test_backlog_falls_back_to_site_ranking_before_anything_runs(void)
{
    adopt_fake_surface();

    /* Day-one behaviour: nothing has been called, so every call count ties at 0
     * and the ranking has to come from the measured site counts. */
    d3d8_hle_report_backlog();

    size_t positions[5];
    positions[0] = captured_position("0x00100000"); /* 93 sites */
    positions[1] = captured_position("0x00100080"); /* 41 */
    positions[2] = captured_position("0x00100040"); /* 17 */
    positions[3] = captured_position("0x00100100"); /* 12 */
    positions[4] = captured_position("0x001000c0"); /* 1 */
    for (size_t i = 0; i < 5; i++) {
        CHECK(positions[i] != (size_t)-1);
    }
    for (size_t i = 1; i < 5; i++) {
        CHECK(positions[i - 1] < positions[i]);
    }
    /* Five rows plus the summary line, and no unknown-hits line. */
    CHECK(captured_lines == 6);
}

static void test_default_return_is_honoured_by_stubs(void)
{
    adopt_fake_surface();

    CHECK(d3d8_hle_call(0x00100100u, NULL) == 0);
    CHECK(d3d8_hle_set_default_return(0x00100100u, 0x8876017Cu));
    CHECK(d3d8_hle_call(0x00100100u, NULL) == 0x8876017Cu);
    CHECK(!d3d8_hle_set_default_return(absent_address, 1u));
}

static void test_init_rejects_bad_arguments_and_reinit_replaces_cleanly(void)
{
    d3d8_hle_shutdown();
    CHECK(!d3d8_hle_init(NULL, fake_surface_count));
    CHECK(!d3d8_hle_init(fake_surface, 0));
    /* A rejected init must not have adopted anything. */
    CHECK(d3d8_hle_count() == 0);

    adopt_fake_surface();
    CHECK(d3d8_hle_count() == fake_surface_count);
    d3d8_hle_call(0x00100000u, NULL);
    d3d8_hle_call(absent_address, NULL);
    CHECK(d3d8_hle_unknown_count() == 1);
    CHECK(d3d8_hle_register(0x00100080u, fake_handler));

    /* Re-init replaces the whole table, state and counters included. */
    CHECK(d3d8_hle_init(other_surface, 2));
    reset_capture();
    CHECK(d3d8_hle_count() == 2);
    CHECK(d3d8_hle_implemented_count() == 0);
    CHECK(d3d8_hle_unknown_count() == 0);
    CHECK(d3d8_hle_entry(0x00100000u) == NULL);
    CHECK(d3d8_hle_entry(0x00100080u) == NULL);
    const d3d8_entry *entry = d3d8_hle_entry(0x00200000u);
    CHECK(entry != NULL);
    CHECK(entry != NULL && entry->call_count == 0);
    CHECK(entry != NULL && entry->state == D3D8_ENTRY_STUB);

    /* And the previous table's rows are gone, not merely shadowed. */
    reset_capture();
    d3d8_hle_report_backlog();
    CHECK(captured_contains("2 of 2 measured functions"));
}

static void test_shutdown_twice_is_safe(void)
{
    adopt_fake_surface();
    d3d8_hle_shutdown();
    d3d8_hle_shutdown();
    CHECK(d3d8_hle_count() == 0);
    CHECK(d3d8_hle_implemented_count() == 0);
    CHECK(d3d8_hle_entry(0x00100000u) == NULL);

    /* A call with no table is an unknown target, which is the honest answer. */
    reset_capture();
    CHECK(d3d8_hle_call(0x00100000u, NULL) == 0);
    CHECK(captured_lines == 1);
    CHECK(d3d8_hle_unknown_count() == 1);

    /* Still re-initialisable afterwards. */
    CHECK(d3d8_hle_init(fake_surface, fake_surface_count));
    CHECK(d3d8_hle_count() == fake_surface_count);
    d3d8_hle_shutdown();
}

static void test_log_sink_defaults_and_is_never_null(void)
{
    CHECK(d3d8_hle_log() == capture_printer);
    d3d8_hle_set_log(NULL);
    CHECK(d3d8_hle_log() != NULL);
    CHECK(d3d8_hle_log() != capture_printer);
    d3d8_hle_set_log(capture_printer);
    CHECK(d3d8_hle_log() == capture_printer);
}

typedef struct {
    const char *name;
    void (*run)(void);
} test_case;

/* --- the fatal hook and the unmodelled note ---------------------------------------------- */

#include <setjmp.h>

static jmp_buf fatal_escape;
static uint32_t fatal_seen_address;
static char fatal_seen_message[256];
static int fatal_calls;

static void escaping_fatal(uint32_t address, const char *message)
{
    fatal_calls++;
    fatal_seen_address = address;
    snprintf(fatal_seen_message, sizeof(fatal_seen_message), "%s", message);
    longjmp(fatal_escape, 1);
}

static void test_fatal_reports_then_calls_the_hook(void)
{
    CHECK(d3d8_hle_init(fake_surface, fake_surface_count));
    d3d8_hle_set_fatal(escaping_fatal);
    fatal_calls = 0;

    /* The message is formatted, logged with the guest address, and handed to the hook, which here
     * escapes the way the host's host_run_stop does. MUTATION: a missing log line or a hook that
     * is never called fails one of these. */
    if (setjmp(fatal_escape) == 0) {
        d3d8_hle_fatal(0x003D5670u, "register %u is out of range", 192u);
        CHECK(false);
    }
    CHECK(fatal_calls == 1);
    CHECK(fatal_seen_address == 0x003D5670u);
    CHECK(strcmp(fatal_seen_message, "register 192 is out of range") == 0);
    CHECK(strstr(captured, "FATAL at guest address 0x003d5670: register 192 is out of range") !=
          NULL);

    /* Removing the hook is allowed; the default (log and abort) is not exercised here because it
     * ends the process. */
    d3d8_hle_set_fatal(NULL);
}

static void test_unmodelled_note_logs_once_per_pair_and_counts_every_time(void)
{
    CHECK(d3d8_hle_init(fake_surface, fake_surface_count));
    CHECK(d3d8_hle_unmodelled_count() == 0u);

    static const char first[] = "first omission";
    static const char second[] = "second omission";
    d3d8_hle_note_unmodelled(0x003D81F0u, first);
    d3d8_hle_note_unmodelled(0x003D81F0u, first);
    d3d8_hle_note_unmodelled(0x003D81F0u, first);
    /* One log line for the three calls, three in the count. MUTATION: logging every time makes
     * three lines, deduplicating by address alone merges the next two. */
    CHECK(d3d8_hle_unmodelled_count() == 3u);
    CHECK(captured_lines == 1u);
    CHECK(strstr(captured, "0x003d81f0 does not model: first omission") != NULL);

    d3d8_hle_note_unmodelled(0x003D81F0u, second);
    d3d8_hle_note_unmodelled(0x003D7EE0u, first);
    CHECK(captured_lines == 3u);
    CHECK(d3d8_hle_unmodelled_count() == 5u);

    /* Adopting a new table starts the run's report afresh. */
    CHECK(d3d8_hle_init(fake_surface, fake_surface_count));
    CHECK(d3d8_hle_unmodelled_count() == 0u);
    d3d8_hle_note_unmodelled(0x003D81F0u, first);
    CHECK(captured_lines == 4u);
}

int main(void)
{
    static const test_case cases[] = {
        {"stub_reports_once_not_every_call", test_stub_reports_once_not_every_call},
        {"unknown_address_reports_every_call", test_unknown_address_reports_every_call},
        {"backlog_sorted_busiest_first", test_backlog_sorted_busiest_first},
        {"lookup_returns_the_requested_entry", test_lookup_returns_the_requested_entry},
        {"named_and_unnamed_stubs_report_differently",
         test_named_and_unnamed_stubs_report_differently},
        {"registered_handler_runs_and_suppresses_the_report",
         test_registered_handler_runs_and_suppresses_the_report},
        {"backlog_falls_back_to_site_ranking_before_anything_runs",
         test_backlog_falls_back_to_site_ranking_before_anything_runs},
        {"default_return_is_honoured_by_stubs", test_default_return_is_honoured_by_stubs},
        {"init_rejects_bad_arguments_and_reinit_replaces_cleanly",
         test_init_rejects_bad_arguments_and_reinit_replaces_cleanly},
        {"shutdown_twice_is_safe", test_shutdown_twice_is_safe},
        {"log_sink_defaults_and_is_never_null", test_log_sink_defaults_and_is_never_null},
        {"fatal_reports_then_calls_the_hook", test_fatal_reports_then_calls_the_hook},
        {"unmodelled_note_logs_once_per_pair_and_counts_every_time",
         test_unmodelled_note_logs_once_per_pair_and_counts_every_time},
    };

    printf("d3d8 HLE tests\n");
    d3d8_hle_set_log(capture_printer);
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int before = failures;
        reset_capture();
        cases[i].run();
        printf("  %-56s %s\n", cases[i].name,
               failures == before ? "ok" : "FAILED");
    }
    d3d8_hle_shutdown();
    d3d8_hle_set_log(NULL);

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
