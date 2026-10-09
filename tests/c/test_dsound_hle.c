/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DirectSound HLE boundary: the dispatch, the reporting discipline, the codec
 * readiness policy and the un-guessed DSP acknowledgement.
 *
 * WHAT THERE IS TO GET WRONG, in order of how badly it hurts:
 *
 *   1. A STUB THAT REPORTS EVERY TIME. `DirectSoundDoWork` is a mixer tick. One
 *      log line per call buries every one-shot init call, and the init order is the
 *      only thing this module exists to learn. Tested by counting OCCURRENCES of a
 *      name, not by checking the log contains it -- "contains" passes against both
 *      behaviours.
 *   2. AN UNKNOWN TARGET REPORTED ONCE, OR NOT AT ALL. The opposite error, and worse.
 *      An unknown address means our 39-entry table and the binary disagree, which no
 *      amount of implementing will fix. Rate-limiting it hides it.
 *   3. A LOOKUP THAT RETURNS A NEIGHBOUR. Nine of the 39 rows are five-byte
 *      `jmp rel32` thunks packed five bytes apart, so `SetVolume`, `SetHeadroom`,
 *      `SetMixBinVolumes` and `Pause` occupy 0x407B14, 0x407B19, 0x407B1E, 0x407B23.
 *      An off-by-one in the lookup silently dispatches a volume change as a pause.
 *      Tested at every in-between byte of that run.
 *   4. A BACKLOG SORTED THE WRONG WAY. The report is the work queue. Sorted
 *      ascending it points at the least important function first; without the site
 *      tiebreak the 17 never-called single-site rows come out in arbitrary order.
 *      The expected order is recomputed INDEPENDENTLY in this file rather than by
 *      asking the module, because asking the module tests nothing.
 *   5. READINESS DEFAULTING TO READY. Upstream measured a fault at guest 0xFFFFFFEC
 *      on the NULL device a not-ready codec produces, so the temptation to default
 *      ready is strong and is exactly the silent fabrication this codebase forbids.
 *      The default is asserted directly and the FABRICATED banner is asserted on the
 *      forced state.
 *   6. A DSP ACKNOWLEDGEMENT GUESSED AT UPSTREAM'S +0x810. Zeroing a guessed guest
 *      address corrupts whatever really lives there, silently. The unconfigured case
 *      is asserted to write NOTHING, against a sentinel-filled fake, because a test
 *      that only checked the return value would pass against a module that wrote
 *      first and returned false afterwards.
 *
 * DELIBERATELY FREE OF LIFTED CODE, OF THE XBE, AND OF THE GENERATED SURFACE TABLE.
 * The measured surface is compiled into `dsound_hle.c`, the DSP writer is a local
 * buffer, and the diagnostic sink is a local capture. This suite runs in a fresh
 * clone with nothing generated.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED; each test says what breaks it.
 */

#include "dsound_hle.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        checks++;                                                                      \
        if (!(cond)) {                                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                      \
            failures++;                                                                 \
        }                                                                              \
    } while (0)

#define CHECK_EQ_U64(actual, expected)                                                 \
    do {                                                                               \
        checks++;                                                                      \
        uint64_t a_ = (uint64_t)(actual);                                                \
        uint64_t e_ = (uint64_t)(expected);                                              \
        if (a_ != e_) {                                                                  \
            printf("FAIL %s:%d  %s == %llu, expected %llu\n", __FILE__, __LINE__,         \
                   #actual, (unsigned long long)a_, (unsigned long long)e_);              \
            failures++;                                                                  \
        }                                                                                \
    } while (0)

/* ===================== diagnostic capture ===================== */

static char captured[65536];
static size_t captured_len;

static int capture_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(captured + captured_len, sizeof(captured) - captured_len,
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

static void captured_reset(void)
{
    captured[0] = '\0';
    captured_len = 0;
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

/* Occurrences, not presence. The once-versus-every-time distinction is invisible to
 * a containment check, and that distinction is the point of the whole module. */
static unsigned captured_count(const char *needle)
{
    unsigned found = 0;
    const char *cursor = captured;
    size_t span = strlen(needle);
    if (span == 0) {
        return 0;
    }
    while ((cursor = strstr(cursor, needle)) != NULL) {
        found++;
        cursor += span;
    }
    return found;
}

/* Byte offset of a needle, or SIZE_MAX. Used to assert report ORDER. */
static size_t captured_offset(const char *needle)
{
    const char *at = strstr(captured, needle);
    return at ? (size_t)(at - captured) : (size_t)-1;
}

static void setup(void)
{
    dsound_hle_init();
    captured_reset();
    dsound_hle_set_log(capture_printer);
}

static void teardown(void)
{
    dsound_hle_set_log(NULL);
    dsound_hle_init();
}

/* ===================== fixtures ===================== */

/* Four of the nine five-byte thunk rows, consecutive in the image. Named here so the
 * neighbour test reads as the hazard it is rather than as four magic numbers. */
#define VA_STREAM_SET_VOLUME 0x00407b14u
#define VA_STREAM_SET_HEADROOM 0x00407b19u
#define VA_STREAM_SET_MIXBIN_VOLUMES 0x00407b1eu
#define VA_STREAM_PAUSE 0x00407b23u

#define VA_BUFFER_PAUSE 0x00407abcu  /* 6 sites: the busiest measured row */
#define VA_BUFFER_PLAY 0x00407a80u   /* 1 site: the row whose count I doubt */
#define VA_DO_WORK 0x00407b40u       /* 5 sites: the mixer tick */
#define VA_CREATE 0x00409635u        /* 3 sites: upstream's gate one */

/* Inside the DSOUND section but not a table row: 0x00407B14's thunk destination,
 * which the scan never saw because a thunk's jump starts inside DSOUND. A real
 * address, really absent from the table, which is the honest shape of this error. */
#define VA_INSIDE_BUT_ABSENT 0x004078e5u
/* `.text`, not DSOUND at all. */
#define VA_OUTSIDE_SECTION 0x00012000u

static uint32_t handler_calls;
static uint32_t stub_handler(void *context)
{
    (void)context;
    handler_calls++;
    return 0x1234u;
}

/* ===================== the table ===================== */

/* The surface is measured data duplicated out of a generated file. If the counts
 * drift, every ranking and every report in this suite silently changes meaning. */
static void test_the_measured_table_is_39_functions_and_80_sites(void)
{
    setup();
    size_t count = 0;
    const dsound_entry *table = dsound_hle_table(&count);
    CHECK(table != NULL);
    CHECK_EQ_U64(count, DSOUND_FUNCTION_COUNT);
    CHECK_EQ_U64(count, 41u);

    uint32_t sites = 0;
    uint32_t named = 0;
    for (size_t i = 0; i < count; i++) {
        sites += table[i].sites;
        named += (table[i].name != NULL) ? 1u : 0u;
        /* Every row sits inside the measured section bounds. A zero-site row would be
         * a generator bug; an out-of-section row would mean the table had been pasted
         * from the wrong section. */
        CHECK(table[i].sites > 0u);
        CHECK(table[i].address >= DSOUND_SECTION_VA_BEGIN);
        CHECK(table[i].address < DSOUND_SECTION_VA_END);
        CHECK(table[i].state == DSOUND_ENTRY_STUB);
        CHECK(table[i].call_count == 0u);
    }
    /* The bounds themselves, pinned to the DSOUND section header's measured values.
     * The range checks above read through the macros, so a loosened bound (say a
     * mutated BEGIN of 0) would otherwise pass while the diagnostics misclassify
     * every unknown target. Mirrors the same pin in test_xinput_hle.c. */
    CHECK_EQ_U64(DSOUND_SECTION_VA_BEGIN, 0x004067C0u);
    CHECK_EQ_U64(DSOUND_SECTION_VA_END, 0x00412D14u);
    CHECK_EQ_U64(sites, DSOUND_SITE_COUNT);
    /* 39 of 41 rows carry an .XTLID name, and that ratio is the section's
     * over-counting filter: a false positive lands wherever a misdecode put it and
     * cannot acquire a symbol-start name. It was 39 of 39 until the surface scanner
     * was corrected to decode rather than byte-sweep, which found two more rows --
     * both thunk targets, both legitimately unnamed. So the filter is weaker than it
     * was, and this records the real figure rather than asserting a property the
     * measurement no longer has. */
    CHECK_EQ_U64(named, 39u);
    CHECK_EQ_U64(sites, 83u);

    /* Nothing is implemented, and that is the design rather than an oversight. */
    CHECK_EQ_U64(dsound_hle_implemented_count(), 0u);
    teardown();
}

/* A duplicated address would make one row permanently unreachable through dispatch
 * while still being counted in every total. */
static void test_no_two_table_addresses_collide(void)
{
    setup();
    size_t count = 0;
    const dsound_entry *table = dsound_hle_table(&count);
    CHECK(count > 0u);
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1u; j < count; j++) {
            CHECK(table[i].address != table[j].address);
        }
        /* And every row is reachable by its own address, which a lookup bug that
         * only ever found the first match would fail. */
        const dsound_entry *found = dsound_hle_entry(table[i].address);
        CHECK(found != NULL);
        if (found) {
            CHECK(found->address == table[i].address);
            CHECK(found->name == table[i].name);
        }
    }
    teardown();
}

/* ===================== dispatch ===================== */

/* MUTATION TARGET 1: a stub that reports every time instead of once.
 * Five calls, one line. A containment check cannot tell the two apart. */
static void test_a_stub_reports_exactly_once_however_many_times_it_is_called(void)
{
    setup();
    for (unsigned i = 0; i < 5u; i++) {
        CHECK_EQ_U64(dsound_hle_call(VA_DO_WORK, NULL), 0u);
    }
    /* Non-empty first: asserting a count of 1 against an empty buffer would pass
     * for "reported nothing" if the count were ever compared the other way. */
    CHECK(captured_len > 0u);
    CHECK(captured_contains("DirectSoundDoWork"));
    CHECK_EQ_U64(captured_count("DirectSoundDoWork"), 1u);
    CHECK_EQ_U64(captured_count("is not implemented"), 1u);
    /* The report carries the measured site count so the log reads without the table. */
    CHECK(captured_contains("5 measured call sites"));

    /* Counted every time even though reported once -- the count is what ranks the
     * backlog, so a module that stopped counting after reporting would produce a
     * work queue that said every function was equally cold. */
    const dsound_entry *entry = dsound_hle_entry(VA_DO_WORK);
    CHECK(entry != NULL);
    if (entry) {
        CHECK_EQ_U64(entry->call_count, 5u);
        CHECK(entry->reported);
    }
    CHECK_EQ_U64(dsound_hle_touched_count(), 1u);
    teardown();
}

/* Each function reports for itself. A single global "reported" flag would silence 38. */
static void test_each_function_reports_for_itself(void)
{
    setup();
    (void)dsound_hle_call(VA_DO_WORK, NULL);
    (void)dsound_hle_call(VA_BUFFER_PAUSE, NULL);
    (void)dsound_hle_call(VA_CREATE, NULL);
    CHECK_EQ_U64(captured_count("is not implemented"), 3u);
    CHECK_EQ_U64(captured_count("DirectSoundDoWork"), 1u);
    CHECK_EQ_U64(captured_count("IDirectSoundBuffer_Pause"), 1u);
    CHECK_EQ_U64(captured_count("DirectSoundCreate"), 1u);
    CHECK_EQ_U64(dsound_hle_touched_count(), 3u);
    teardown();
}

/* MUTATION TARGET 2: an unknown target silently ignored, or reported once.
 * Three calls, three lines, and a nonzero counter. */
static void test_an_unknown_target_reports_every_single_time(void)
{
    setup();
    for (unsigned i = 0; i < 3u; i++) {
        CHECK_EQ_U64(dsound_hle_call(VA_INSIDE_BUT_ABSENT, NULL), 0u);
    }
    CHECK(captured_len > 0u);
    CHECK_EQ_U64(captured_count("UNKNOWN"), 3u);
    CHECK_EQ_U64(dsound_hle_unknown_call_count(), 3u);
    /* An unknown target must never be mistaken for a stub: it is not a missing
     * implementation, so it must not appear in the implemented/stub bookkeeping. */
    CHECK_EQ_U64(captured_count("is not implemented"), 0u);
    CHECK_EQ_U64(dsound_hle_touched_count(), 0u);
    CHECK(dsound_hle_entry(VA_INSIDE_BUT_ABSENT) == NULL);
    teardown();
}

/* The two unknown cases are different bugs with different fixes, so they must not
 * share one message. Inside: regenerate or extend the table. Outside: fix the caller. */
static void test_an_unknown_inside_the_section_is_distinguished_from_one_outside(void)
{
    setup();
    (void)dsound_hle_call(VA_INSIDE_BUT_ABSENT, NULL);
    CHECK(captured_contains("inside the DSOUND section"));
    CHECK(!captured_contains("OUTSIDE"));

    captured_reset();
    (void)dsound_hle_call(VA_OUTSIDE_SECTION, NULL);
    CHECK(captured_contains("OUTSIDE"));
    CHECK(!captured_contains("absent from our"));

    CHECK_EQ_U64(dsound_hle_unknown_call_count(), 2u);
    teardown();
}

/* MUTATION TARGET 3: a lookup that returns a neighbouring entry.
 * The four consecutive five-byte thunks make this a real hazard, not a theoretical
 * one: every byte strictly between two row addresses must resolve to NULL, and each
 * row address must resolve to ITS OWN name. */
static void test_lookup_is_exact_and_never_returns_a_neighbour(void)
{
    setup();

    const struct {
        uint32_t address;
        const char *name;
    } run[] = {
        {VA_STREAM_SET_VOLUME, "IDirectSoundStream_SetVolume"},
        {VA_STREAM_SET_HEADROOM, "IDirectSoundStream_SetHeadroom"},
        {VA_STREAM_SET_MIXBIN_VOLUMES, "IDirectSoundStream_SetMixBinVolumes"},
        {VA_STREAM_PAUSE, "IDirectSoundStream_Pause"},
    };

    for (size_t i = 0; i < sizeof(run) / sizeof(run[0]); i++) {
        const dsound_entry *entry = dsound_hle_entry(run[i].address);
        CHECK(entry != NULL);
        if (entry) {
            CHECK(entry->address == run[i].address);
            CHECK(entry->name != NULL);
            /* strcmp, not pointer identity: a mutation that returned the neighbour's
             * row would still hand back a valid non-NULL name. */
            CHECK(entry->name && strcmp(entry->name, run[i].name) == 0);
        }
        /* The four interior bytes of each five-byte thunk. */
        for (uint32_t delta = 1u; delta < 5u; delta++) {
            CHECK(dsound_hle_entry(run[i].address + delta) == NULL);
        }
    }

    /* Generalised: no byte adjacent to any row resolves, unless it is itself a row. */
    size_t count = 0;
    const dsound_entry *table = dsound_hle_table(&count);
    for (size_t i = 0; i < count; i++) {
        uint32_t before = table[i].address - 1u;
        uint32_t after = table[i].address + 1u;
        const dsound_entry *b = dsound_hle_entry(before);
        const dsound_entry *a = dsound_hle_entry(after);
        CHECK(b == NULL || b->address == before);
        CHECK(a == NULL || a->address == after);
    }
    teardown();
}

/* An implemented function runs its handler, returns its value, and says nothing --
 * a dispatcher that reported it anyway would make the backlog permanently wrong. */
static void test_an_implemented_function_runs_and_does_not_report(void)
{
    setup();
    handler_calls = 0;
    CHECK(dsound_hle_register(VA_BUFFER_PAUSE, stub_handler));
    CHECK_EQ_U64(dsound_hle_implemented_count(), 1u);
    CHECK_EQ_U64(dsound_hle_call(VA_BUFFER_PAUSE, NULL), 0x1234u);
    CHECK_EQ_U64(handler_calls, 1u);
    CHECK_EQ_U64(captured_count("is not implemented"), 0u);
    CHECK_EQ_U64(captured_len, 0u);

    /* And it drops out of the backlog rather than merely being annotated in it. */
    captured_reset();
    dsound_hle_report();
    CHECK(captured_contains("40 of 41"));
    CHECK_EQ_U64(captured_count("IDirectSoundBuffer_Pause"), 0u);
    teardown();
}

/* Registering against an address we do not have means the caller or the table is
 * wrong. Accepting it silently is how a dispatcher ends up with a handler nothing
 * can ever reach. */
static void test_register_refuses_an_unknown_address_and_a_null_handler(void)
{
    setup();
    CHECK(!dsound_hle_register(VA_INSIDE_BUT_ABSENT, stub_handler));
    CHECK(!dsound_hle_register(VA_OUTSIDE_SECTION, stub_handler));
    CHECK(!dsound_hle_register(VA_BUFFER_PAUSE, NULL));
    CHECK_EQ_U64(dsound_hle_implemented_count(), 0u);

    CHECK(!dsound_hle_set_default_return(VA_INSIDE_BUT_ABSENT, 1u));
    CHECK(dsound_hle_set_default_return(VA_BUFFER_PAUSE, 0x80004005u));
    CHECK_EQ_U64(dsound_hle_call(VA_BUFFER_PAUSE, NULL), 0x80004005u);
    CHECK(captured_contains("0x80004005"));
    teardown();
}

/* ===================== the backlog ordering ===================== */

/*
 * The expected order, computed HERE and not by asking the module.
 *
 * Runtime calls descending, then measured sites descending, then address ascending.
 * Written out independently so that a mutation to the module's own comparator has
 * nothing to hide behind -- a test that sorted by calling the module would agree
 * with any comparator at all, including a reversed one.
 */
static bool expect_ranks_above(const dsound_entry *a, const dsound_entry *b)
{
    if (a->call_count != b->call_count) {
        return a->call_count > b->call_count;
    }
    if (a->sites != b->sites) {
        return a->sites > b->sites;
    }
    return a->address < b->address;
}

/* Assert the captured report lists every unimplemented row exactly once, in the
 * order `expect_ranks_above` demands. Addresses are matched as the module prints
 * them, so this reads the real output rather than a parallel accessor. */
static void check_report_order(void)
{
    size_t count = 0;
    const dsound_entry *table = dsound_hle_table(&count);

    /* Snapshot, because the ordering below must not depend on table order. */
    dsound_entry snapshot[DSOUND_FUNCTION_COUNT];
    size_t pending = 0;
    for (size_t i = 0; i < count; i++) {
        if (table[i].state != DSOUND_ENTRY_IMPLEMENTED) {
            snapshot[pending++] = table[i];
        }
    }
    CHECK(pending > 0u);

    size_t previous_offset = 0;
    for (size_t printed = 0; printed < pending; printed++) {
        size_t best = (size_t)-1;
        for (size_t i = 0; i < pending; i++) {
            if (snapshot[i].address == 0u) {
                continue; /* already consumed */
            }
            if (best == (size_t)-1 || expect_ranks_above(&snapshot[i], &snapshot[best])) {
                best = i;
            }
        }
        CHECK(best != (size_t)-1);
        if (best == (size_t)-1) {
            return;
        }

        char needle[16];
        (void)snprintf(needle, sizeof(needle), "0x%08x", snapshot[best].address);
        size_t at = captured_offset(needle);
        /* Present at all: a report that dropped rows would otherwise leave the
         * ordering check trivially satisfied over whatever survived. */
        CHECK(at != (size_t)-1);
        if (at == (size_t)-1) {
            printf("       missing from report: %s %s\n", needle,
                   snapshot[best].name ? snapshot[best].name : "?");
            return;
        }
        if (printed > 0 && at < previous_offset) {
            printf("FAIL %s:%d  report out of order at rank %zu: %s (sites %u, "
                   "calls %llu) appears before the rank above it\n",
                   __FILE__, __LINE__, printed, needle, snapshot[best].sites,
                   (unsigned long long)snapshot[best].call_count);
            failures++;
        }
        checks++;
        previous_offset = at;
        snapshot[best].address = 0u;
    }
}

/* MUTATION TARGET 4a: the backlog not sorted busiest-first.
 * With no run yet, "busiest" is the measured site count, so the head of the report
 * must be the 6-site row and the order must follow sites then address. */
static void test_the_backlog_is_ordered_by_measured_sites_before_any_run(void)
{
    setup();
    captured_reset();
    dsound_hle_report();

    CHECK(captured_len > 0u);
    CHECK(captured_contains("41 of 41"));
    /* Every row listed exactly once. The needle is the row format's DOUBLE space
     * before "calls", which the summary line's single-spaced "N calls observed" does
     * not match -- a looser needle counts the summary as a fortieth row. */
    CHECK_EQ_U64(captured_count("  calls "), DSOUND_FUNCTION_COUNT);

    /* The 6-site row leads, and specifically leads the 4-site rows. Checked as a
     * named pair as well as by the general order walk, because an ordering bug that
     * only affected the head would survive a purely relative check. */
    CHECK(captured_offset("0x00407abc") < captured_offset("0x00407b23"));
    CHECK(captured_offset("0x00407b23") < captured_offset("0x00407aa4"));
    /* The 5-site row leads both 4-site rows. 0x00407b40 DirectSoundDoWork was a
     * 4-site row until the surface scanner was corrected to decode rather than
     * byte-sweep; it is 5 now, which is why this is not a three-way tie any more. */
    CHECK(captured_offset("0x00407abc") < captured_offset("0x00407b40"));
    CHECK(captured_offset("0x00407b40") < captured_offset("0x00407b23"));
    /* Address ascending within an equal site count: the two remaining 4-site rows. */
    CHECK(captured_offset("0x00407b23") < captured_offset("0x00407b28"));

    check_report_order();
    teardown();
}

/* MUTATION TARGET 4b: a comparator that drops the runtime key, or reverses it.
 * `IDirectSoundBuffer_Play` has the fewest measured sites of any row I expect to
 * matter, and it is the row whose count I doubt most. Once it has been called it
 * must outrank the 6-site row that has not been. */
static void test_an_observed_call_outranks_a_busier_measured_row(void)
{
    setup();
    for (unsigned i = 0; i < 2u; i++) {
        (void)dsound_hle_call(VA_BUFFER_PLAY, NULL);
    }
    (void)dsound_hle_call(VA_CREATE, NULL);

    captured_reset();
    dsound_hle_report();

    CHECK(captured_contains("3 calls observed this run"));
    /* Play (1 site, 2 calls) above Create (3 sites, 1 call) above Pause (6 sites, 0). */
    CHECK(captured_offset("0x00407a80") < captured_offset("0x00409635"));
    CHECK(captured_offset("0x00409635") < captured_offset("0x00407abc"));
    check_report_order();
    teardown();
}

/* The report must state plainly that nothing is played. A reader of a run log who
 * mistakes this for a failed device goes looking for an audio driver that was never
 * meant to exist. */
static void test_the_report_says_output_is_render_to_nothing(void)
{
    setup();
    captured_reset();
    dsound_hle_report();
    CHECK(captured_contains("RENDER-TO-NOTHING"));
    CHECK(captured_contains("no host audio device"));
    teardown();
}

/* An unknown call is surfaced in the summary and not only at the moment it happens,
 * because a table/binary disagreement must survive into the end-of-run artefact. */
static void test_the_report_surfaces_a_table_binary_disagreement(void)
{
    setup();
    captured_reset();
    dsound_hle_report();
    CHECK(!captured_contains("DISAGREE"));

    (void)dsound_hle_call(VA_INSIDE_BUT_ABSENT, NULL);
    captured_reset();
    dsound_hle_report();
    CHECK(captured_contains("DISAGREE"));
    teardown();
}

/* ===================== readiness ===================== */

/* MUTATION TARGET 5: the ready state defaulting the wrong way.
 * The honest default is NOT_READY. Defaulting ready fabricates the single most
 * consequential bit in the subsystem without saying so. */
static void test_the_codec_defaults_to_not_ready(void)
{
    setup();
    CHECK(dsound_hle_codec_state() == DSOUND_CODEC_NOT_READY);
    CHECK(!dsound_hle_codec_ready());
    /* And nothing was fabricated on the way to that answer. */
    CHECK(!captured_contains("FABRICATED"));
    CHECK_EQ_U64(dsound_hle_not_ready_query_count(), 1u);
    teardown();
}

/* The honest default costs a crash, so it must signpost the crash. Once, because a
 * readiness poll can sit in a loop. */
static void test_a_not_ready_answer_names_the_expected_fault_exactly_once(void)
{
    setup();
    for (unsigned i = 0; i < 4u; i++) {
        CHECK(!dsound_hle_codec_ready());
    }
    CHECK(captured_len > 0u);
    CHECK_EQ_U64(captured_count("0xffffffec"), 1u);
    CHECK(captured_contains("NOT READY"));
    CHECK(captured_contains("DirectSoundCreate will fail"));
    /* Every query counted even though reported once -- the count is how a run log
     * says how hard the guest leaned on this. */
    CHECK_EQ_U64(dsound_hle_not_ready_query_count(), 4u);

    captured_reset();
    dsound_hle_report();
    CHECK(captured_contains("NOT READY (honest default; 4 readiness queries"));
    CHECK(!captured_contains("FABRICATED"));
    teardown();
}

/* Forcing ready is legitimate and upstream shows it is often necessary. It is not
 * legitimate to do it quietly. */
static void test_forcing_ready_announces_the_fabrication(void)
{
    setup();
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK(captured_contains("FABRICATED"));
    CHECK(captured_contains("FORCED READY"));
    CHECK(captured_contains("0xffffffec"));
    CHECK(dsound_hle_codec_state() == DSOUND_CODEC_READY);

    /* A ready codec answers yes and stops warning. */
    captured_reset();
    for (unsigned i = 0; i < 3u; i++) {
        CHECK(dsound_hle_codec_ready());
    }
    CHECK_EQ_U64(captured_len, 0u);
    CHECK_EQ_U64(dsound_hle_not_ready_query_count(), 0u);

    captured_reset();
    dsound_hle_report();
    CHECK(captured_contains("READY (FABRICATED"));
    teardown();
}

/* T375: readiness is bit 8 of the modelled AC97 global status register, and the poll reads
 * the bit. Pins that the register moves with the model, and that readiness follows the
 * register (not a separately held flag). */
static void test_codec_readiness_is_the_global_status_bit(void)
{
    setup();
    CHECK_EQ_U64(DSOUND_AC97_GLOBAL_STATUS_PRIMARY_CODEC_READY, 0x100u);
    CHECK_EQ_U64(dsound_hle_ac97_global_status(), 0u);
    CHECK(!dsound_hle_codec_ready());
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U64(dsound_hle_ac97_global_status() & DSOUND_AC97_GLOBAL_STATUS_PRIMARY_CODEC_READY,
                 DSOUND_AC97_GLOBAL_STATUS_PRIMARY_CODEC_READY);
    CHECK(dsound_hle_codec_ready());
    dsound_hle_set_codec_state(DSOUND_CODEC_NOT_READY);
    CHECK_EQ_U64(dsound_hle_ac97_global_status(), 0u);
    CHECK(!dsound_hle_codec_ready());
    teardown();
}

/* Announcing once per CHANGE, not once per call -- a host that re-applies its flags
 * must not produce a wall of identical banners. */
static void test_readiness_announces_once_per_change(void)
{
    setup();
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U64(captured_count("FORCED READY"), 1u);

    dsound_hle_set_codec_state(DSOUND_CODEC_NOT_READY);
    CHECK(captured_contains("set NOT READY"));
    CHECK(dsound_hle_codec_state() == DSOUND_CODEC_NOT_READY);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U64(captured_count("FORCED READY"), 2u);
    teardown();
}

/* ===================== DSP acknowledgement ===================== */

#define FAKE_MEM_WORDS 8u
static uint32_t fake_mem[FAKE_MEM_WORDS];
static uint32_t fake_mem_base = 0x01000000u;
static unsigned fake_writes;

static void fake_writer(uint32_t guest_address, uint32_t value, void *user)
{
    (void)user;
    fake_writes++;
    if (guest_address < fake_mem_base) {
        return;
    }
    uint32_t index = (guest_address - fake_mem_base) / 4u;
    if (index < FAKE_MEM_WORDS) {
        fake_mem[index] = value;
    }
}

/* A sentinel, so "untouched" can never be confused with "written a zero" -- which is
 * exactly what an acknowledgement writes. Asserting zero over memory that started at
 * zero would pass against a module that wrote nothing. */
#define SENTINEL 0xA5A5A5A5u

static void fake_mem_reset(void)
{
    for (unsigned i = 0; i < FAKE_MEM_WORDS; i++) {
        fake_mem[i] = SENTINEL;
    }
    fake_writes = 0;
}

/* MUTATION TARGET 6: falling back to upstream's +0x810 against a guessed base.
 * Refused, counted, reported once, and -- the part a return-value-only test would
 * miss -- nothing written anywhere. */
static void test_the_dsp_ack_is_refused_until_an_address_is_derived(void)
{
    setup();
    fake_mem_reset();
    dsound_hle_set_dsp_writer(fake_writer, NULL);

    CHECK(!dsound_hle_dsp_ack_configured());
    CHECK_EQ_U64(dsound_hle_dsp_ack_address(), DSOUND_DSP_ACK_UNSET);

    for (unsigned i = 0; i < 3u; i++) {
        CHECK(!dsound_hle_ack_dsp_command());
    }
    /* Not one write escaped. */
    CHECK_EQ_U64(fake_writes, 0u);
    for (unsigned i = 0; i < FAKE_MEM_WORDS; i++) {
        CHECK_EQ_U64(fake_mem[i], SENTINEL);
    }
    CHECK_EQ_U64(dsound_hle_dsp_ack_count(), 0u);
    CHECK_EQ_U64(dsound_hle_dsp_ack_refused_count(), 3u);
    /* Reported once, and it names what has to be measured rather than merely
     * complaining. */
    CHECK_EQ_U64(captured_count("REFUSED"), 1u);
    CHECK(captured_contains("0x810"));
    CHECK(captured_contains("per-title"));

    captured_reset();
    dsound_hle_report();
    CHECK(captured_contains("NOT CONFIGURED"));
    teardown();
}

/* Once derived, the acknowledgement writes a zero at base + offset and nowhere else. */
static void test_a_derived_dsp_ack_writes_a_zero_at_base_plus_offset(void)
{
    setup();
    fake_mem_reset();
    dsound_hle_set_dsp_writer(fake_writer, NULL);

    /* Offset 4 rather than 0x810, so the suite proves the module uses the offset it
     * was given rather than the one upstream measured. */
    CHECK(dsound_hle_set_dsp_ack(fake_mem_base, 4u));
    CHECK(dsound_hle_dsp_ack_configured());
    CHECK_EQ_U64(dsound_hle_dsp_ack_address(), fake_mem_base + 4u);
    /* An offset that is not upstream's is flagged as differing, because a silent
     * divergence from the one constant that travels between titles is worth a line. */
    CHECK(captured_contains("DIFFERS"));

    CHECK(dsound_hle_ack_dsp_command());
    CHECK_EQ_U64(fake_writes, 1u);
    CHECK_EQ_U64(fake_mem[1], 0u);
    /* The neighbours are untouched: a write at the wrong index is a corruption, not
     * a near miss. */
    CHECK_EQ_U64(fake_mem[0], SENTINEL);
    CHECK_EQ_U64(fake_mem[2], SENTINEL);
    CHECK_EQ_U64(dsound_hle_dsp_ack_count(), 1u);
    CHECK_EQ_U64(dsound_hle_dsp_ack_refused_count(), 0u);

    captured_reset();
    dsound_hle_report();
    CHECK(captured_contains("DERIVED"));
    CHECK(captured_contains("1 acknowledged"));
    teardown();
}

/* The upstream constant is recognised when it is supplied, so the artefact records a
 * confirmation rather than burying it in one summed address. */
static void test_the_upstream_offset_is_recognised_when_supplied(void)
{
    setup();
    CHECK(dsound_hle_set_dsp_ack(0x014f8000u, DSOUND_DSP_ACK_UPSTREAM_OFFSET));
    CHECK_EQ_U64(dsound_hle_dsp_ack_address(), 0x014f8810u);
    CHECK(captured_contains("matches the +0x810"));
    CHECK(!captured_contains("DIFFERS"));
    teardown();
}

/* A wrapping sum is a wrong address, and a wrong address here is a silent write into
 * whatever lives there. Refused, and the previous state is left alone. */
static void test_the_dsp_ack_rejects_a_bad_base_or_a_wrapping_sum(void)
{
    setup();
    CHECK(!dsound_hle_set_dsp_ack(DSOUND_DSP_ACK_UNSET, 0u));
    CHECK(!dsound_hle_dsp_ack_configured());
    CHECK(!dsound_hle_set_dsp_ack(0xFFFFFF00u, 0x200u));
    CHECK(!dsound_hle_dsp_ack_configured());
    CHECK_EQ_U64(dsound_hle_dsp_ack_address(), DSOUND_DSP_ACK_UNSET);
    /* The largest sum that does not wrap is accepted, so the bound is a bound and
     * not an off-by-one that rejects a legitimate address. */
    CHECK(dsound_hle_set_dsp_ack(0xFFFFFF00u, 0xFFu));
    CHECK_EQ_U64(dsound_hle_dsp_ack_address(), 0xFFFFFFFFu);
    teardown();
}

/* A derived address with no writer installed writes nothing and says so, rather than
 * counting an acknowledgement that never reached memory. */
static void test_a_derived_ack_with_no_writer_is_refused(void)
{
    setup();
    CHECK(dsound_hle_set_dsp_ack(fake_mem_base, 4u));
    CHECK(!dsound_hle_ack_dsp_command());
    CHECK_EQ_U64(dsound_hle_dsp_ack_count(), 0u);
    CHECK_EQ_U64(dsound_hle_dsp_ack_refused_count(), 1u);
    CHECK(captured_contains("no writer installed"));
    teardown();
}

/* ===================== cross-check ===================== */

/* The measured surface is duplicated between the generated table and this module.
 * The failure mode of duplicated measured data is silent drift, so the drift detector
 * is itself tested -- including in the direction a one-sided walk would miss. */
static void test_crosscheck_agrees_with_itself_and_flags_every_divergence(void)
{
    setup();
    size_t count = 0;
    const dsound_entry *table = dsound_hle_table(&count);

    dsound_surface_ref refs[DSOUND_FUNCTION_COUNT + 1u];
    for (size_t i = 0; i < count; i++) {
        refs[i].address = table[i].address;
        refs[i].name = table[i].name;
        refs[i].sites = table[i].sites;
    }

    /* Identical: zero disagreements and a silent log. A detector that reported
     * something here would be noise nobody would read. */
    CHECK_EQ_U64(dsound_hle_crosscheck(refs, count), 0u);
    CHECK_EQ_U64(captured_len, 0u);

    /* A changed site count. */
    captured_reset();
    refs[0].sites = table[0].sites + 1u;
    CHECK_EQ_U64(dsound_hle_crosscheck(refs, count), 1u);
    CHECK(captured_contains("site count"));
    refs[0].sites = table[0].sites;

    /* A changed name. */
    captured_reset();
    refs[1].name = "NotTheRightName";
    CHECK_EQ_U64(dsound_hle_crosscheck(refs, count), 1u);
    CHECK(captured_contains("named"));
    refs[1].name = table[1].name;

    /* A row the generator has and we do not. */
    captured_reset();
    refs[count].address = VA_INSIDE_BUT_ABSENT;
    refs[count].name = "SomethingWeMissed";
    refs[count].sites = 2u;
    CHECK_EQ_U64(dsound_hle_crosscheck(refs, count + 1u), 1u);
    CHECK(captured_contains("and we do not"));

    /* A row we have and the generator does not -- the direction a crosscheck that
     * only walked `refs` would pass straight over. */
    captured_reset();
    CHECK_EQ_U64(dsound_hle_crosscheck(refs, count - 1u), 1u);
    CHECK(captured_contains("the generated table does not"));

    /* An empty table disagrees with all 39 rather than agreeing vacuously. */
    captured_reset();
    CHECK_EQ_U64(dsound_hle_crosscheck(refs, 0u), DSOUND_FUNCTION_COUNT);
    teardown();
}

/* ===================== lifecycle ===================== */

/* init() must clear everything, or one test leaks into the next and the suite stops
 * meaning anything. The readiness state and the derived DSP address are global, so
 * they are the two that would do the most damage if they survived. */
static void test_init_clears_counters_reports_readiness_and_the_dsp_address(void)
{
    setup();
    (void)dsound_hle_call(VA_DO_WORK, NULL);
    (void)dsound_hle_call(VA_INSIDE_BUT_ABSENT, NULL);
    (void)dsound_hle_codec_ready();
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK(dsound_hle_register(VA_BUFFER_PAUSE, stub_handler));
    CHECK(dsound_hle_set_dsp_ack(fake_mem_base, 4u));

    CHECK_EQ_U64(dsound_hle_touched_count(), 1u);
    CHECK_EQ_U64(dsound_hle_unknown_call_count(), 1u);
    CHECK_EQ_U64(dsound_hle_not_ready_query_count(), 1u);

    dsound_hle_init();
    captured_reset();
    dsound_hle_set_log(capture_printer);

    CHECK_EQ_U64(dsound_hle_touched_count(), 0u);
    CHECK_EQ_U64(dsound_hle_unknown_call_count(), 0u);
    CHECK_EQ_U64(dsound_hle_not_ready_query_count(), 0u);
    CHECK_EQ_U64(dsound_hle_implemented_count(), 0u);
    CHECK_EQ_U64(dsound_hle_dsp_ack_address(), DSOUND_DSP_ACK_UNSET);
    /* Back to the honest default: an init that left READY installed would silently
     * change what every later test is testing. */
    CHECK(dsound_hle_codec_state() == DSOUND_CODEC_NOT_READY);
    /* And the once-only reports are armed again, so a second run logs its own. */
    (void)dsound_hle_call(VA_DO_WORK, NULL);
    CHECK_EQ_U64(captured_count("DirectSoundDoWork"), 1u);
    teardown();
}

/* The sink is never NULL, so no diagnostic path can crash on it. */
static void test_the_log_sink_is_never_null(void)
{
    setup();
    CHECK(dsound_hle_log() != NULL);
    dsound_hle_set_log(NULL);
    CHECK(dsound_hle_log() != NULL);
    teardown();
}

int main(void)
{
    printf("dsound HLE boundary tests\n");

    test_the_measured_table_is_39_functions_and_80_sites();
    test_no_two_table_addresses_collide();

    test_a_stub_reports_exactly_once_however_many_times_it_is_called();
    test_each_function_reports_for_itself();
    test_an_unknown_target_reports_every_single_time();
    test_an_unknown_inside_the_section_is_distinguished_from_one_outside();
    test_lookup_is_exact_and_never_returns_a_neighbour();
    test_an_implemented_function_runs_and_does_not_report();
    test_register_refuses_an_unknown_address_and_a_null_handler();

    test_the_backlog_is_ordered_by_measured_sites_before_any_run();
    test_an_observed_call_outranks_a_busier_measured_row();
    test_the_report_says_output_is_render_to_nothing();
    test_the_report_surfaces_a_table_binary_disagreement();

    test_the_codec_defaults_to_not_ready();
    test_a_not_ready_answer_names_the_expected_fault_exactly_once();
    test_forcing_ready_announces_the_fabrication();
    test_codec_readiness_is_the_global_status_bit();
    test_readiness_announces_once_per_change();

    test_the_dsp_ack_is_refused_until_an_address_is_derived();
    test_a_derived_dsp_ack_writes_a_zero_at_base_plus_offset();
    test_the_upstream_offset_is_recognised_when_supplied();
    test_the_dsp_ack_rejects_a_bad_base_or_a_wrapping_sum();
    test_a_derived_ack_with_no_writer_is_refused();

    test_crosscheck_agrees_with_itself_and_flags_every_divergence();

    test_init_clears_counters_reports_readiness_and_the_dsp_address();
    test_the_log_sink_is_never_null();

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
