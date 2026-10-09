/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * XAPI input HLE boundary: the dispatch, the reporting discipline, the no-device
 * default and the un-guessed guest structure layout.
 *
 * WHAT THERE IS TO GET WRONG, in order of how badly it hurts:
 *
 *   1. A LAYOUT ACCEPTED WITHOUT EVIDENCE. The worst bug available in this module, and
 *      the only one that corrupts guest memory rather than merely misreporting. A
 *      lifter patch found upstream declared IO_STATUS_BLOCK at 16 bytes where the
 *      guest's is 8 and overran every one by 8, so the bound checks are asserted
 *      directly: a field past the declared end is REFUSED rather than clamped, a
 *      wrapping offset is refused, overlapping fields are refused, and a size change
 *      drops every mapping that was validated against the old size. The un-derived
 *      case is asserted to write NOTHING against a sentinel-filled buffer, because a
 *      test that only checked the return value would pass against a module that wrote
 *      first and returned 0 afterwards.
 *   2. A STUB THAT REPORTS EVERY TIME. `XInputGetState` is polled once per frame per
 *      port. One log line per call buries every one-shot `XInitDevices` and
 *      `XInputOpen`, and that enumeration order is the only thing this module exists to
 *      learn. Tested by counting OCCURRENCES of a name, not by checking the log
 *      contains it -- "contains" passes against both behaviours.
 *   3. AN UNKNOWN TARGET REPORTED ONCE, OR NOT AT ALL. The opposite error, and worse.
 *      An unknown address means our 15-entry table and the binary disagree, which no
 *      amount of implementing will fix. Rate-limiting it hides it.
 *   4. A LOOKUP THAT RETURNS A NEIGHBOUR. `XInitDevices` at 0x0046DBCD and
 *      `XGetDevices` at 0x0046DBD2 are FIVE BYTES APART, and `XInputClose` at
 *      0x0046E189 and `XInputGetCapabilities` at 0x0046E195 are twelve. An off-by-one
 *      in the lookup silently dispatches one-time device initialisation as a per-frame
 *      enumeration. Tested at every in-between byte of both runs.
 *   5. A PORT THAT DEFAULTS TO CONNECTED. There is no gamepad. Reporting one would
 *      fabricate the first bit the title reads about input, silently, which is the one
 *      shape this codebase's fabrication rule forbids. The default is asserted
 *      directly and the FABRICATED banner is asserted on the synthetic pad.
 *   6. A BACKLOG SORTED THE WRONG WAY. The report is the work queue. Sorted ascending
 *      it points at the least important function first; without the site tiebreak the
 *      eight single-site rows come out in arbitrary order. The expected order is
 *      recomputed INDEPENDENTLY in this file rather than by asking the module, because
 *      asking the module tests nothing.
 *   7. A DATA SYMBOL DIAGNOSED AS AN UNKNOWN FUNCTION. The six device-type descriptor
 *      tables are passed BY ADDRESS to the enumeration calls, so a lifted wrapper that
 *      confuses an argument with a call target lands on one. "Unknown target" sends
 *      someone hunting for a function that never existed; this suite asserts the
 *      specific message instead.
 *
 * DELIBERATELY FREE OF LIFTED CODE, OF THE XBE, AND OF THE GENERATED SURFACE TABLE.
 * The measured surface is compiled into `xinput_hle.c`, the guest writer is a local
 * buffer, and the diagnostic sink is a local capture. This suite runs in a fresh clone
 * with nothing generated and no gamepad attached -- which is the only environment it
 * has ever run in.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED; each test says what breaks it.
 */

#include "xinput_hle.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define CHECK_EQ_U64(actual, expected)                                                 \
    do {                                                                               \
        checks++;                                                                      \
        uint64_t a_ = (uint64_t)(actual);                                               \
        uint64_t e_ = (uint64_t)(expected);                                             \
        if (a_ != e_) {                                                                 \
            printf("FAIL %s:%d  %s == %llu, expected %llu\n", __FILE__, __LINE__,       \
                   #actual, (unsigned long long)a_, (unsigned long long)e_);            \
            failures++;                                                                \
        }                                                                              \
    } while (0)

/* ===================== the measured addresses, spelled out =====================
 *
 * Named constants rather than bare hex at every use, because the neighbour tests and
 * the ordering tests both depend on the exact values and a typo in one of them would
 * silently weaken the test rather than fail it. */
#define VA_XGETDEVICES 0x0046dbd2u
#define VA_XGETDEVICECHANGES 0x0046dbf4u
#define VA_XPEEKDEVICES 0x0046db91u
#define VA_XINPUTCLOSE 0x0046e189u
#define VA_XVOICECREATEEX 0x004754fdu
#define VA_XMOUNTMUA 0x0046d7c4u
#define VA_XUNMOUNTMU 0x0046d8f6u
#define VA_XINITDEVICES 0x0046dbcdu
#define VA_XINPUTOPEN 0x0046e133u
#define VA_XINPUTGETCAPS 0x0046e195u
#define VA_XINPUTGETSTATE 0x0046e36du
#define VA_XINPUTSETSTATE 0x0046e3e0u
#define VA_XGETDEVENUMSTATUS 0x0046f3a8u
#define VA_XREADMUMETADATA 0x0046da04u
#define VA_XVOICECREATE 0x004752c6u

#define VA_DATA_MEMORY_UNIT_TABLE 0x0046c6e0u
#define VA_DATA_GAMEPAD_TABLE 0x0046c75cu
#define VA_DATA_IR_REMOTE_TABLE 0x0046c7c0u

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
            captured_len = sizeof(captured) - 1;
        }
    }
    return written;
}

static void captured_reset(void)
{
    captured_len = 0;
    captured[0] = '\0';
}

static bool captured_contains(const char *needle)
{
    return strstr(captured, needle) != NULL;
}

/*
 * OCCURRENCES, NOT PRESENCE.
 *
 * The once-versus-every-time distinction is invisible to a containment check, and that
 * distinction is the point of the whole module: a stub must report once and an unknown
 * target must report every time. A test asserting only that the log contains a name
 * passes against both behaviours and therefore tests neither.
 */
static unsigned captured_count(const char *needle)
{
    unsigned found = 0;
    const char *at = captured;
    size_t len = strlen(needle);
    if (len == 0) {
        return 0;
    }
    while ((at = strstr(at, needle)) != NULL) {
        found++;
        at += len;
    }
    return found;
}

/* Where a string first appears, for asserting report ORDER. SIZE_MAX when absent, so
 * a missing string sorts after everything and an ordering assertion fails rather than
 * passing vacuously on two absent names. */
static size_t captured_offset(const char *needle)
{
    const char *at = strstr(captured, needle);
    return at ? (size_t)(at - captured) : SIZE_MAX;
}

static void setup(void)
{
    xinput_hle_init();
    captured_reset();
    xinput_hle_set_log(capture_printer);
}

static void teardown(void)
{
    xinput_hle_set_log(NULL);
}

/* ===================== the measured table ===================== */

/*
 * The counts are the measurement, so they are pinned here as literals.
 *
 * MUTATION: change XINPUT_FUNCTION_COUNT or any row's site count in xinput_hle.c, and
 * this fails on the total. The site total is summed from the table rather than read
 * from the macro, so the macro and the rows cannot drift apart without one of these
 * two checks noticing.
 */
static void test_the_measured_table_is_15_functions_and_21_sites(void)
{
    setup();
    size_t count = 0;
    const xinput_entry *table = xinput_hle_table(&count);
    CHECK(table != NULL);
    CHECK_EQ_U64(count, 15u);
    CHECK_EQ_U64(XINPUT_FUNCTION_COUNT, 15u);

    uint32_t total = 0;
    for (size_t i = 0; i < count; i++) {
        total += table[i].sites;
    }
    CHECK_EQ_U64(total, 21u);
    CHECK_EQ_U64(XINPUT_SITE_COUNT, 21u);
    teardown();
}

/*
 * Every row names itself and sits inside the section it was measured from.
 *
 * MUTATION: mistype any address so it falls outside XPP, or NULL out a name, and this
 * fails. The section bound check is not redundant with the table: it is the check that
 * a transcription error in an address produces a test failure rather than a plausible
 * wrong number.
 */
static void test_every_row_is_named_and_inside_the_xpp_section(void)
{
    setup();
    size_t count = 0;
    const xinput_entry *table = xinput_hle_table(&count);
    for (size_t i = 0; i < count; i++) {
        CHECK(table[i].name != NULL);
        CHECK(table[i].address >= XINPUT_SECTION_VA_BEGIN);
        CHECK(table[i].address < XINPUT_SECTION_VA_END);
        CHECK(table[i].state == XINPUT_ENTRY_STUB);
        CHECK_EQ_U64(table[i].call_count, 0u);
        CHECK(!table[i].reported);
    }
    /* The section bounds themselves, from the section header: vaddr 0x0046C680,
     * vsize 0x90EC. A wrong end bound would silently reclassify unknown targets. */
    CHECK_EQ_U64(XINPUT_SECTION_VA_BEGIN, 0x0046c680u);
    CHECK_EQ_U64(XINPUT_SECTION_VA_END, 0x0047576cu);
    CHECK_EQ_U64(XINPUT_SECTION_VA_END - XINPUT_SECTION_VA_BEGIN, 0x90ecu);
    teardown();
}

/*
 * No two rows share an address.
 *
 * MUTATION: duplicate any address in the table and this fails. A duplicate would make
 * the second row permanently unreachable, and the only symptom would be a function
 * that never appears to be called.
 */
static void test_no_two_table_addresses_collide(void)
{
    setup();
    size_t count = 0;
    const xinput_entry *table = xinput_hle_table(&count);
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            CHECK(table[i].address != table[j].address);
            CHECK(strcmp(table[i].name, table[j].name) != 0);
        }
    }
    teardown();
}

/*
 * The kind split is 10 input, 3 memory-unit, 2 voice.
 *
 * MUTATION: relabel any row's kind and this fails on the counts. The split matters
 * because the voice rows outrank most of the input rows on static sites, and a reader
 * who thinks they are input rows will spend a day in the wrong subsystem.
 */
static void test_the_kind_classification_matches_the_measured_split(void)
{
    setup();
    size_t count = 0;
    const xinput_entry *table = xinput_hle_table(&count);
    unsigned input = 0, mu = 0, voice = 0;
    for (size_t i = 0; i < count; i++) {
        switch (table[i].kind) {
        case XINPUT_KIND_INPUT:
            input++;
            break;
        case XINPUT_KIND_MEMORY_UNIT:
            mu++;
            break;
        case XINPUT_KIND_VOICE:
            voice++;
            break;
        }
    }
    CHECK_EQ_U64(input, 10u);
    CHECK_EQ_U64(mu, 3u);
    CHECK_EQ_U64(voice, 2u);

    /* And the specific rows, so a compensating pair of relabels cannot pass. */
    CHECK(xinput_hle_entry(VA_XINPUTGETSTATE)->kind == XINPUT_KIND_INPUT);
    CHECK(xinput_hle_entry(VA_XMOUNTMUA)->kind == XINPUT_KIND_MEMORY_UNIT);
    CHECK(xinput_hle_entry(VA_XVOICECREATEEX)->kind == XINPUT_KIND_VOICE);
    CHECK(strcmp(xinput_hle_kind_name(XINPUT_KIND_INPUT), "input") == 0);
    CHECK(strcmp(xinput_hle_kind_name(XINPUT_KIND_MEMORY_UNIT), "memory-unit") == 0);
    CHECK(strcmp(xinput_hle_kind_name(XINPUT_KIND_VOICE), "voice") == 0);
    teardown();
}

/*
 * The two zero-site rows are zero, and everything else is not.
 *
 * MUTATION: give XReadMUMetaData a site count, or take XGetDevices' four away, and this
 * fails. The zero rows carry the distinction between "no site measured from game
 * `.text`" and "unreachable", which is the whole reason they are in the table.
 */
static void test_only_the_two_known_rows_have_zero_sites(void)
{
    setup();
    CHECK_EQ_U64(xinput_hle_entry(VA_XREADMUMETADATA)->sites, 0u);
    CHECK_EQ_U64(xinput_hle_entry(VA_XVOICECREATE)->sites, 0u);
    CHECK_EQ_U64(xinput_hle_entry(VA_XGETDEVICES)->sites, 4u);
    CHECK_EQ_U64(xinput_hle_entry(VA_XGETDEVICECHANGES)->sites, 3u);
    CHECK_EQ_U64(xinput_hle_entry(VA_XINPUTGETSTATE)->sites, 1u);

    size_t count = 0;
    const xinput_entry *table = xinput_hle_table(&count);
    unsigned zeroes = 0;
    for (size_t i = 0; i < count; i++) {
        if (table[i].sites == 0u) {
            zeroes++;
        }
    }
    CHECK_EQ_U64(zeroes, 2u);
    teardown();
}

/*
 * The six data symbols are distinct, inside the section, and share no address with a
 * function.
 *
 * MUTATION: collide a data symbol with a function address and this fails. A collision
 * would make the dispatcher report "that is a data table" for a real function, which
 * would be a confidently wrong diagnostic rather than a missing one.
 */
static void test_the_data_symbols_are_distinct_and_never_collide_with_a_function(void)
{
    setup();
    size_t data_count = 0;
    const xinput_data_symbol *data = xinput_hle_data_symbols(&data_count);
    CHECK(data != NULL);
    CHECK_EQ_U64(data_count, 6u);
    CHECK_EQ_U64(XINPUT_DATA_SYMBOL_COUNT, 6u);

    for (size_t i = 0; i < data_count; i++) {
        CHECK(data[i].name != NULL);
        CHECK(data[i].address >= XINPUT_SECTION_VA_BEGIN);
        CHECK(data[i].address < XINPUT_SECTION_VA_END);
        /* Not a function. */
        CHECK(xinput_hle_entry(data[i].address) == NULL);
        for (size_t j = i + 1; j < data_count; j++) {
            CHECK(data[i].address != data[j].address);
        }
    }

    CHECK(xinput_hle_data_symbol(VA_DATA_GAMEPAD_TABLE) != NULL);
    CHECK(strcmp(xinput_hle_data_symbol(VA_DATA_GAMEPAD_TABLE)->name,
                 "XDEVICE_TYPE_GAMEPAD_TABLE") == 0);
    /* Exact match only: one byte either side is not the symbol. */
    CHECK(xinput_hle_data_symbol(VA_DATA_GAMEPAD_TABLE + 1u) == NULL);
    CHECK(xinput_hle_data_symbol(VA_DATA_GAMEPAD_TABLE - 1u) == NULL);
    CHECK(xinput_hle_data_symbol(VA_XINPUTGETSTATE) == NULL);
    teardown();
}

/* ===================== dispatch and reporting ===================== */

/*
 * A stub reports ONCE, however many times it is called.
 *
 * MUTATION: delete the `if (!entry->reported)` guard in xinput_hle_call, and this fails
 * on the occurrence count while a containment check would still pass. XInputGetState is
 * the row that makes this matter: it is polled every frame.
 */
static void test_a_stub_reports_exactly_once_however_many_times_it_is_called(void)
{
    setup();
    for (unsigned i = 0; i < 500; i++) {
        CHECK_EQ_U64(xinput_hle_call(VA_XINPUTGETSTATE, NULL), 0u);
    }
    CHECK_EQ_U64(captured_count("XInputGetState"), 1u);
    CHECK_EQ_U64(captured_count("is not implemented"), 1u);
    /* Every call is still counted, which is what makes the backlog ranking work. */
    CHECK_EQ_U64(xinput_hle_entry(VA_XINPUTGETSTATE)->call_count, 500u);
    CHECK_EQ_U64(xinput_hle_touched_count(), 1u);
    CHECK_EQ_U64(xinput_hle_unknown_call_count(), 0u);
    /* The line carries the kind and the measured site count, so the log is readable
     * without the table beside it. */
    CHECK(captured_contains("[input]"));
    CHECK(captured_contains("(1 measured call site)"));
    /* Singular, not "1 call sites". The plural agreement is asserted because the
     * alternative is a log line that reads as though it were generated, which is how a
     * reader learns to stop reading it. */
    CHECK(!captured_contains("1 measured call sites"));
    teardown();
}

/*
 * Each function reports for itself; one stub reporting does not silence another.
 *
 * MUTATION: make `reported` a single module-wide flag instead of per-entry, and this
 * fails -- only the first function would ever log, and the enumeration order this
 * module exists to learn would be a single line.
 */
static void test_each_function_reports_for_itself(void)
{
    setup();
    size_t count = 0;
    const xinput_entry *table = xinput_hle_table(&count);
    for (size_t i = 0; i < count; i++) {
        (void)xinput_hle_call(table[i].address, NULL);
        (void)xinput_hle_call(table[i].address, NULL);
    }
    CHECK_EQ_U64(captured_count("is not implemented"), 15u);
    for (size_t i = 0; i < count; i++) {
        /* THE NEEDLE IS THE NAME PLUS THE FOLLOWING " [", NOT THE BARE NAME.
         *
         * `XVoiceCreateMediaObject` is a strict PREFIX of `XVoiceCreateMediaObjectEx`,
         * so counting the bare name finds two occurrences for one row and this test
         * failed against a correct module when it was first written. Every stub line
         * formats the label immediately followed by " [kind]", so anchoring on that
         * makes the count exact. Worth stating because it is a test-method bug that
         * presents exactly like a module bug. */
        char needle[64];
        (void)snprintf(needle, sizeof(needle), "%s [", table[i].name);
        CHECK_EQ_U64(captured_count(needle), 1u);
    }
    CHECK_EQ_U64(xinput_hle_touched_count(), 15u);
    teardown();
}

/*
 * An unknown target reports EVERY SINGLE TIME.
 *
 * MUTATION: add a `reported`-style latch to the unknown-target path, and this fails. The
 * asymmetry is deliberate and is the most important behavioural pair in the module: a
 * stub is unfinished work, an unknown target is our measurement disagreeing with the
 * binary, and rate-limiting the second hides a bug no amount of implementing will fix.
 */
static void test_an_unknown_target_reports_every_single_time(void)
{
    setup();
    /* Inside the section, between two real rows, and not a data symbol. */
    const uint32_t absent = VA_XINPUTGETSTATE + 1u;
    CHECK(xinput_hle_entry(absent) == NULL);
    for (unsigned i = 0; i < 20; i++) {
        CHECK_EQ_U64(xinput_hle_call(absent, NULL), 0u);
    }
    CHECK_EQ_U64(captured_count("UNKNOWN target"), 20u);
    CHECK_EQ_U64(xinput_hle_unknown_call_count(), 20u);
    /* And it did not quietly become a table row. */
    CHECK_EQ_U64(xinput_hle_touched_count(), 0u);
    teardown();
}

/*
 * Inside the section, outside it, and a data symbol are three DIFFERENT messages.
 *
 * MUTATION: collapse any two of the three branches in xinput_hle_call's unknown path
 * into one message, and this fails. They point at three different first moves:
 * regenerate the table, fix the call site, or fix an argument that reached a call slot.
 */
static void test_the_three_unknown_target_messages_are_distinct(void)
{
    setup();
    (void)xinput_hle_call(XINPUT_SECTION_VA_BEGIN, NULL);
    CHECK(captured_contains("inside the XPP"));
    CHECK(!captured_contains("OUTSIDE the XPP"));
    CHECK(!captured_contains("DATA symbol"));

    captured_reset();
    /* Just past the end. The bound is exclusive, so the last byte of the section is
     * inside and this one is not -- an off-by-one here would misreport forever. */
    (void)xinput_hle_call(XINPUT_SECTION_VA_END, NULL);
    CHECK(captured_contains("OUTSIDE the XPP"));
    CHECK(!captured_contains("inside the XPP"));

    captured_reset();
    (void)xinput_hle_call(XINPUT_SECTION_VA_END - 1u, NULL);
    CHECK(captured_contains("inside the XPP"));

    captured_reset();
    (void)xinput_hle_call(XINPUT_SECTION_VA_BEGIN - 1u, NULL);
    CHECK(captured_contains("OUTSIDE the XPP"));

    /* A game-code address, well clear of the section. */
    captured_reset();
    (void)xinput_hle_call(0x0018ff70u, NULL);
    CHECK(captured_contains("OUTSIDE the XPP"));
    CHECK(captured_contains("not a peripheral address"));

    CHECK_EQ_U64(xinput_hle_unknown_call_count(), 5u);
    teardown();
}

/*
 * A call to a device-type table says it is a DATA symbol, by name.
 *
 * MUTATION: delete the data-symbol lookup from the unknown path and this fails -- the
 * message degrades to "UNKNOWN target", which sends someone hunting for a function
 * that never existed. These tables are passed by address five bytes before an
 * XInputOpen call site, so an argument reaching a call slot is a live risk.
 */
static void test_a_call_to_a_data_symbol_says_it_is_a_data_table(void)
{
    setup();
    (void)xinput_hle_call(VA_DATA_GAMEPAD_TABLE, NULL);
    CHECK(captured_contains("DATA symbol XDEVICE_TYPE_GAMEPAD_TABLE"));
    CHECK(captured_contains("not a function"));
    CHECK(!captured_contains("UNKNOWN target"));

    captured_reset();
    (void)xinput_hle_call(VA_DATA_MEMORY_UNIT_TABLE, NULL);
    CHECK(captured_contains("XDEVICE_TYPE_MEMORY_UNIT_TABLE"));

    captured_reset();
    (void)xinput_hle_call(VA_DATA_IR_REMOTE_TABLE, NULL);
    CHECK(captured_contains("XDEVICE_TYPE_IR_REMOTE_TABLE"));

    /* Still counted as a disagreement, and still reported every time. */
    CHECK_EQ_U64(xinput_hle_unknown_call_count(), 3u);
    captured_reset();
    for (unsigned i = 0; i < 7; i++) {
        (void)xinput_hle_call(VA_DATA_GAMEPAD_TABLE, NULL);
    }
    CHECK_EQ_U64(captured_count("DATA symbol"), 7u);
    teardown();
}

/*
 * The lookup is EXACT and never returns a neighbour.
 *
 * MUTATION: change `==` to `>=` or `<=` in mutable_entry's scan, or introduce a
 * range/nearest match, and this fails. The two tight runs are the reason: XInitDevices
 * at 0x0046DBCD is five bytes below XGetDevices at 0x0046DBD2, so a nearest-match
 * lookup dispatches one-time device initialisation as a per-frame enumeration, and
 * XInputClose is twelve bytes below XInputGetCapabilities.
 */
static void test_lookup_is_exact_and_never_returns_a_neighbour(void)
{
    setup();
    CHECK(xinput_hle_entry(VA_XINITDEVICES) != NULL);
    CHECK(xinput_hle_entry(VA_XGETDEVICES) != NULL);
    CHECK(strcmp(xinput_hle_entry(VA_XINITDEVICES)->name, "XInitDevices") == 0);
    CHECK(strcmp(xinput_hle_entry(VA_XGETDEVICES)->name, "XGetDevices") == 0);
    /* Every byte strictly between the two is nothing at all. */
    for (uint32_t va = VA_XINITDEVICES + 1u; va < VA_XGETDEVICES; va++) {
        CHECK(xinput_hle_entry(va) == NULL);
    }
    /* And the twelve bytes between XInputClose and XInputGetCapabilities. */
    for (uint32_t va = VA_XINPUTCLOSE + 1u; va < VA_XINPUTGETCAPS; va++) {
        CHECK(xinput_hle_entry(va) == NULL);
    }
    /* One byte either side of the busiest row. */
    CHECK(xinput_hle_entry(VA_XGETDEVICES - 1u) == NULL);
    CHECK(xinput_hle_entry(VA_XGETDEVICES + 1u) == NULL);
    teardown();
}

/*
 * Lookup by name is exact, and a NULL name is not a crash.
 *
 * MUTATION: use a prefix compare instead of strcmp and this fails -- "XInput" would
 * match XInputOpen, and "XGetDevice" would match either XGetDevices or
 * XGetDeviceChanges depending on table order.
 */
static void test_lookup_by_name_is_exact(void)
{
    setup();
    CHECK(xinput_hle_entry_by_name("XInputGetState") != NULL);
    CHECK_EQ_U64(xinput_hle_entry_by_name("XInputGetState")->address, VA_XINPUTGETSTATE);
    CHECK(xinput_hle_entry_by_name("XGetDevices") != NULL);
    CHECK_EQ_U64(xinput_hle_entry_by_name("XGetDevices")->address, VA_XGETDEVICES);
    /* A prefix is not a match. */
    CHECK(xinput_hle_entry_by_name("XInput") == NULL);
    CHECK(xinput_hle_entry_by_name("XGetDevice") == NULL);
    CHECK(xinput_hle_entry_by_name("XInputGetStateEx") == NULL);
    CHECK(xinput_hle_entry_by_name("") == NULL);
    CHECK(xinput_hle_entry_by_name(NULL) == NULL);
    teardown();
}

/*
 * An unnamed row labels itself BY ADDRESS, not by a shared placeholder.
 *
 * MUTATION: return a constant like "<unnamed>" from xinput_hle_entry_label, and this
 * fails. "Reports by name where known and by address otherwise" is a requirement; a
 * shared placeholder makes every unnamed row indistinguishable in the log, which is
 * exactly when the log matters most. All 15 rows are named in this image, so this is
 * tested against a synthetic entry -- otherwise the branch is untestable and therefore
 * untested, in the one layer that must never crash.
 */
static void test_an_unnamed_row_labels_itself_by_address(void)
{
    setup();
    char buffer[64];

    xinput_entry named = {VA_XINPUTGETSTATE, "XInputGetState", 1, XINPUT_KIND_INPUT,
                          NULL,              XINPUT_ENTRY_STUB, 0, 0, false};
    CHECK(strcmp(xinput_hle_entry_label(&named, buffer, sizeof(buffer)),
                 "XInputGetState") == 0);

    xinput_entry unnamed = {0x0046e36du, NULL, 1, XINPUT_KIND_INPUT, NULL,
                            XINPUT_ENTRY_STUB, 0, 0, false};
    const char *label = xinput_hle_entry_label(&unnamed, buffer, sizeof(buffer));
    CHECK(strstr(label, "0x0046e36d") != NULL);
    CHECK(strstr(label, "unnamed") != NULL);

    /* A second unnamed row at a different address must not share a label. */
    xinput_entry other = {0x0046e3e0u, NULL, 1, XINPUT_KIND_INPUT, NULL,
                          XINPUT_ENTRY_STUB, 0, 0, false};
    char second[64];
    const char *other_label = xinput_hle_entry_label(&other, second, sizeof(second));
    CHECK(strcmp(label, other_label) != 0);
    CHECK(strstr(other_label, "0x0046e3e0") != NULL);

    /* No diagnostic path may hand NULL to printf("%s"). */
    CHECK(xinput_hle_entry_label(NULL, buffer, sizeof(buffer)) != NULL);
    CHECK(xinput_hle_entry_label(&unnamed, NULL, 0) != NULL);
    CHECK(xinput_hle_entry_label(&unnamed, buffer, 0) != NULL);
    teardown();
}

static unsigned handler_calls;

static uint32_t fake_handler(void *context)
{
    handler_calls++;
    return context ? 0x1234u : 0x5678u;
}

/*
 * An implemented function runs, returns its own value, and does NOT report.
 *
 * MUTATION: report before dispatching to the handler, or ignore the handler and return
 * the default, and this fails. A boundary that logs "not implemented" for an
 * implemented function makes the backlog permanently wrong.
 */
static void test_an_implemented_function_runs_and_does_not_report(void)
{
    setup();
    handler_calls = 0;
    CHECK(xinput_hle_register(VA_XINPUTGETSTATE, fake_handler));
    CHECK(xinput_hle_entry(VA_XINPUTGETSTATE)->state == XINPUT_ENTRY_IMPLEMENTED);
    CHECK_EQ_U64(xinput_hle_implemented_count(), 1u);

    CHECK_EQ_U64(xinput_hle_call(VA_XINPUTGETSTATE, NULL), 0x5678u);
    int marker = 0;
    CHECK_EQ_U64(xinput_hle_call(VA_XINPUTGETSTATE, &marker), 0x1234u);
    CHECK_EQ_U64(handler_calls, 2u);
    CHECK_EQ_U64(captured_count("is not implemented"), 0u);
    /* Still counted, so an implemented function keeps its place in the ranking. */
    CHECK_EQ_U64(xinput_hle_entry(VA_XINPUTGETSTATE)->call_count, 2u);
    teardown();
}

/*
 * Registration refuses an unknown address and a NULL handler.
 *
 * MUTATION: accept either, and this fails. Accepting an unknown address means the table
 * or the caller is wrong, and silently accepting it hides which.
 */
static void test_register_refuses_an_unknown_address_and_a_null_handler(void)
{
    setup();
    CHECK(!xinput_hle_register(VA_XINPUTGETSTATE + 1u, fake_handler));
    CHECK(!xinput_hle_register(VA_DATA_GAMEPAD_TABLE, fake_handler));
    CHECK(!xinput_hle_register(0u, fake_handler));
    CHECK(!xinput_hle_register(VA_XINPUTGETSTATE, NULL));
    CHECK_EQ_U64(xinput_hle_implemented_count(), 0u);
    /* A refused registration must not have changed the state either. */
    CHECK(xinput_hle_entry(VA_XINPUTGETSTATE)->state == XINPUT_ENTRY_STUB);

    CHECK(!xinput_hle_set_default_return(VA_XINPUTGETSTATE + 1u, 7u));
    CHECK(xinput_hle_set_default_return(VA_XINPUTGETSTATE, 0xDEADu));
    CHECK_EQ_U64(xinput_hle_call(VA_XINPUTGETSTATE, NULL), 0xDEADu);
    CHECK(captured_contains("returning 0xdead"));
    teardown();
}

/* ===================== the backlog ordering ===================== */

/*
 * Before anything runs, the backlog is the measured site ranking.
 *
 * The expected order is written out HERE, independently, rather than obtained from the
 * module: sites descending, then address ascending within a tie. Asking the module for
 * the order and then checking the module produced it tests nothing.
 *
 * MUTATION: flip either comparison in ranks_above (`>` to `<` on call_count or on
 * sites), or drop the address tiebreak, and this fails. A backlog sorted the wrong way
 * is not a cosmetic defect: it is a work queue pointing at the least important function
 * first.
 */
static void test_the_backlog_is_ordered_by_measured_sites_before_any_run(void)
{
    setup();
    xinput_hle_report();

    /* EVERY NEEDLE CARRIES A TRAILING SPACE, DELIBERATELY. The backlog pads the label
     * with `%-32s`, so each name is followed by at least one space, and
     * `XVoiceCreateMediaObject` is a strict PREFIX of `XVoiceCreateMediaObjectEx`. The
     * bare name would find the Ex row -- which sorts five places EARLIER -- and the
     * ordering assertion would fail against a module that had ordered correctly. */
    static const char *const expected[15] = {
        "XGetDevices ",                 /* 4 */
        "XGetDeviceChanges ",           /* 3 */
        "XPeekDevices ",                /* 2, 0x0046db91 */
        "XInputClose ",                 /* 2, 0x0046e189 */
        "XVoiceCreateMediaObjectEx ",   /* 2, 0x004754fd */
        "XMountMUA ",                   /* 1, 0x0046d7c4 */
        "XUnmountMU ",                  /* 1, 0x0046d8f6 */
        "XInitDevices ",                /* 1, 0x0046dbcd */
        "XInputOpen ",                  /* 1, 0x0046e133 */
        "XInputGetCapabilities ",       /* 1, 0x0046e195 */
        "XInputGetState ",              /* 1, 0x0046e36d */
        "XInputSetState ",              /* 1, 0x0046e3e0 */
        "XGetDeviceEnumerationStatus ", /* 1, 0x0046f3a8 */
        "XReadMUMetaData ",             /* 0, 0x0046da04 */
        "XVoiceCreateMediaObject ",     /* 0, 0x004752c6 */
    };

    for (size_t i = 0; i < 15; i++) {
        CHECK(captured_offset(expected[i]) != SIZE_MAX);
    }
    for (size_t i = 0; i + 1 < 15; i++) {
        /* Strictly increasing offsets, so a stable-but-wrong order fails too. */
        CHECK(captured_offset(expected[i]) < captured_offset(expected[i + 1]));
    }
    CHECK(captured_contains("15 of 15 measured functions still need implementations"));
    CHECK(captured_contains("21 call sites measured"));
    teardown();
}

/*
 * One observed call outranks a busier measured row.
 *
 * MUTATION: order by sites before call_count, and this fails. This is the single most
 * important ordering property for THIS boundary, because XInputGetState has one
 * measured site and is the busiest function in the table the moment anything runs. The
 * static ranking puts it eleventh; one real call must put it first.
 */
static void test_an_observed_call_outranks_a_busier_measured_row(void)
{
    setup();
    /* XInputGetState: 1 measured site, versus XGetDevices with 4. */
    (void)xinput_hle_call(VA_XINPUTGETSTATE, NULL);
    captured_reset();
    xinput_hle_report();
    CHECK(captured_offset("XInputGetState") < captured_offset("XGetDevices"));
    CHECK(captured_offset("XInputGetState") < captured_offset("XGetDeviceChanges"));

    /* And a busier observation outranks a less busy one regardless of sites. */
    for (unsigned i = 0; i < 10; i++) {
        (void)xinput_hle_call(VA_XVOICECREATE, NULL); /* 0 measured sites */
    }
    captured_reset();
    xinput_hle_report();
    /* Trailing spaces again: the bare `XVoiceCreateMediaObject` also matches the `Ex`
     * row, and relying on which one strstr happens to reach first would make this pass
     * for the wrong reason. */
    CHECK(captured_offset("XVoiceCreateMediaObject ") <
          captured_offset("XInputGetState "));
    CHECK(captured_offset("XVoiceCreateMediaObject ") <
          captured_offset("XVoiceCreateMediaObjectEx "));
    CHECK(captured_offset("XInputGetState ") < captured_offset("XGetDevices "));
    CHECK(captured_contains("11 calls observed this run"));
    teardown();
}

/*
 * The report says input is READ-FROM-NOTHING.
 *
 * MUTATION: delete the line, and this fails. Without it a reader of a run log cannot
 * tell "we never read a device" from "the device failed to open", and those are
 * completely different problems.
 */
static void test_the_report_says_input_is_read_from_nothing(void)
{
    setup();
    xinput_hle_report();
    CHECK(captured_contains("READ-FROM-NOTHING"));
    CHECK(captured_contains("no host input device is opened"));
    CHECK(captured_contains("no USB bus is enumerated"));
    CHECK(captured_contains("all 4 ports report EMPTY"));
    teardown();
}

/*
 * The report surfaces a table-versus-binary disagreement.
 *
 * MUTATION: drop the unknown-call summary from the report, and this fails. The per-call
 * line is on stderr during a run and easy to lose; the summary is the line an operator
 * reads at the end.
 */
static void test_the_report_surfaces_a_table_binary_disagreement(void)
{
    setup();
    xinput_hle_report();
    CHECK(!captured_contains("DISAGREE"));

    captured_reset();
    (void)xinput_hle_call(VA_XINPUTGETSTATE + 1u, NULL);
    xinput_hle_report();
    CHECK(captured_contains("DISAGREE"));
    CHECK(captured_contains("1 call landed"));

    captured_reset();
    (void)xinput_hle_call(VA_XINPUTGETSTATE + 1u, NULL);
    xinput_hle_report();
    CHECK(captured_contains("2 calls landed"));
    teardown();
}

/* ===================== ports ===================== */

/*
 * Every port defaults to EMPTY.
 *
 * MUTATION: initialise any port to SYNTHETIC, or make xinput_hle_port_connected return
 * true by default, and this fails. There is no gamepad; reporting one fabricates the
 * first bit the title reads about input, and a fabrication that does not announce
 * itself is the one shape this codebase forbids.
 */
static void test_every_port_defaults_to_empty(void)
{
    setup();
    for (unsigned port = 0; port < XINPUT_PORT_COUNT; port++) {
        CHECK(xinput_hle_port_state(port) == XINPUT_PORT_EMPTY);
    }
    CHECK_EQ_U64(xinput_hle_connected_count(), 0u);
    CHECK_EQ_U64(XINPUT_PORT_COUNT, 4u);
    /* Nothing is logged merely by asking about the state -- only by querying as the
     * guest would. */
    CHECK_EQ_U64(captured_len, 0u);
    teardown();
}

/*
 * The first EMPTY answer names the symptoms and the switch, exactly once.
 *
 * MUTATION: remove the `empty_reported` latch and this fails on the count (it is polled
 * once per frame per port); remove the message entirely and it fails on the content.
 * The honest default has a cost, and an unexplained dead end is worse than a signposted
 * one.
 */
static void test_an_empty_answer_names_the_symptoms_exactly_once(void)
{
    setup();
    for (unsigned i = 0; i < 100; i++) {
        for (unsigned port = 0; port < XINPUT_PORT_COUNT; port++) {
            CHECK(!xinput_hle_port_connected(port));
        }
    }
    CHECK_EQ_U64(captured_count("NOTHING IS READ FROM HARDWARE"), 1u);
    CHECK(captured_contains("never advances"));
    CHECK(captured_contains("reconnect the controller"));
    CHECK(captured_contains("Attach the fabricated synthetic pad"));
    CHECK_EQ_U64(xinput_hle_empty_query_count(), 400u);
    teardown();
}

/*
 * Attaching the synthetic pad announces the FABRICATION.
 *
 * MUTATION: remove the banner, or drop the word FABRICATED, and this fails. Presence
 * decides which branch the title's entire input init takes, so a run log that does not
 * record it cannot be interpreted at all.
 */
static void test_attaching_the_synthetic_pad_announces_the_fabrication(void)
{
    setup();
    CHECK(xinput_hle_attach_synthetic_pad(0));
    CHECK(captured_contains("FABRICATED"));
    CHECK(captured_contains("there is no gamepad"));
    CHECK(captured_contains("PRESENCE ONLY"));
    CHECK(xinput_hle_port_state(0) == XINPUT_PORT_SYNTHETIC);
    CHECK_EQ_U64(xinput_hle_connected_count(), 1u);
    CHECK(xinput_hle_port_connected(0));
    /* A connected port does not count as an empty query. */
    CHECK_EQ_U64(xinput_hle_empty_query_count(), 0u);
    /* The other three are untouched: attaching one pad must not fabricate four. */
    for (unsigned port = 1; port < XINPUT_PORT_COUNT; port++) {
        CHECK(xinput_hle_port_state(port) == XINPUT_PORT_EMPTY);
    }

    /* Announced once per change, not per attach of an already-attached pad. */
    captured_reset();
    CHECK(xinput_hle_attach_synthetic_pad(0));
    CHECK_EQ_U64(captured_count("FABRICATED"), 0u);

    captured_reset();
    CHECK(xinput_hle_detach_synthetic_pad(0));
    CHECK(captured_contains("EMPTY again"));
    CHECK_EQ_U64(xinput_hle_connected_count(), 0u);

    /* The report changes voice when a pad is present. */
    captured_reset();
    CHECK(xinput_hle_attach_synthetic_pad(2));
    CHECK(xinput_hle_attach_synthetic_pad(3));
    captured_reset();
    xinput_hle_report();
    CHECK(captured_contains("2 of 4 ports report a SYNTHETIC pad"));
    CHECK(captured_contains("FABRICATED"));
    CHECK(!captured_contains("ports report EMPTY"));
    teardown();
}

/*
 * The synthetic pad fabricates PRESENCE ONLY. Every button and axis stays at rest.
 *
 * MUTATION: have attach_synthetic_pad fill the pad state with plausible values, and
 * this fails. Presence is one bit and can be fabricated honestly; a stream of
 * plausible stick deflections could not be, and would be indistinguishable in a log
 * from a working host backend.
 */
static void test_the_synthetic_pad_fabricates_presence_only_not_input(void)
{
    setup();
    CHECK(xinput_hle_attach_synthetic_pad(1));
    xinput_pad_state pad = xinput_hle_pad_state(1);
    CHECK_EQ_U64(pad.packet_number, 0u);
    CHECK_EQ_U64(pad.digital_buttons, 0u);
    /* The triggers are entries 6 and 7 of the analog run, not fields of their own.
     * MEASURED: both get a second analog rescaling path (0x001901C4, 0x0019021D) that
     * the six face bytes do not. */
    CHECK_EQ_U64(pad.analog[XINPUT_ANALOG_LEFT_TRIGGER], 0u);
    CHECK_EQ_U64(pad.analog[XINPUT_ANALOG_RIGHT_TRIGGER], 0u);
    CHECK_EQ_U64(XINPUT_ANALOG_LEFT_TRIGGER, 6u);
    CHECK_EQ_U64(XINPUT_ANALOG_RIGHT_TRIGGER, 7u);
    CHECK_EQ_U64((uint16_t)pad.thumb_left_x, 0u);
    CHECK_EQ_U64((uint16_t)pad.thumb_left_y, 0u);
    CHECK_EQ_U64((uint16_t)pad.thumb_right_x, 0u);
    CHECK_EQ_U64((uint16_t)pad.thumb_right_y, 0u);
    for (unsigned i = 0; i < XINPUT_ANALOG_COUNT; i++) {
        CHECK_EQ_U64(pad.analog[i], 0u);
    }
    /* EIGHT, not six. The run at guest 0x06 is six pressure-sensitive face buttons plus
     * the two triggers, read out of the 8-entry table at .rdata:0x0047DF40 rather than
     * assumed from what a pad "has". A six would be the desktop-shaped guess. */
    CHECK_EQ_U64(XINPUT_ANALOG_COUNT, 8u);
    CHECK_EQ_U64(xinput_hle_synthetic_value_count(), 0u);
    teardown();
}

/*
 * Fabricated button values are refused on an EMPTY port and announced on a synthetic
 * one.
 *
 * MUTATION: honour values on an empty port, and this fails. Values for a port the guest
 * has been told is empty are input from a controller that does not exist even by this
 * module's own account. Drop the separate banner and it fails on the announcement: a
 * log that cannot distinguish "a pad is present but at rest" from "something is
 * pressing buttons" cannot be used to judge a run.
 */
static void test_fabricated_values_need_a_pad_and_announce_themselves(void)
{
    setup();
    xinput_pad_state state;
    memset(&state, 0, sizeof(state));
    state.packet_number = 7u;
    state.analog[XINPUT_ANALOG_LEFT_TRIGGER] = 0xFFu;

    /* No pad on the port yet. */
    CHECK(!xinput_hle_set_synthetic_pad_state(0, state));
    CHECK(captured_contains("REFUSED"));
    CHECK(captured_contains("reports EMPTY"));
    CHECK_EQ_U64(xinput_hle_pad_state(0).packet_number, 0u);
    CHECK_EQ_U64(xinput_hle_synthetic_value_count(), 0u);

    CHECK(xinput_hle_attach_synthetic_pad(0));
    captured_reset();
    CHECK(xinput_hle_set_synthetic_pad_state(0, state));
    CHECK(captured_contains("SYNTHETIC BUTTON AND AXIS VALUES"));
    CHECK(captured_contains("FABRICATED"));
    CHECK(captured_contains("the guest will act on them"));
    CHECK_EQ_U64(xinput_hle_pad_state(0).packet_number, 7u);
    CHECK_EQ_U64(xinput_hle_pad_state(0).analog[XINPUT_ANALOG_LEFT_TRIGGER], 0xFFu);
    CHECK_EQ_U64(xinput_hle_synthetic_value_count(), 1u);
    /* Only the one port. */
    CHECK_EQ_U64(xinput_hle_pad_state(1).packet_number, 0u);

    /* Announced once, counted every time. */
    captured_reset();
    for (unsigned i = 0; i < 10; i++) {
        CHECK(xinput_hle_set_synthetic_pad_state(0, state));
    }
    CHECK_EQ_U64(captured_count("SYNTHETIC BUTTON AND AXIS VALUES"), 0u);
    CHECK_EQ_U64(xinput_hle_synthetic_value_count(), 11u);

    CHECK(!xinput_hle_set_synthetic_pad_state(XINPUT_PORT_COUNT, state));

    captured_reset();
    xinput_hle_report();
    CHECK(captured_contains("11 FABRICATED button/axis value sets installed"));
    CHECK(captured_contains("did not come from a controller"));
    teardown();
}

/*
 * Port APIs reject an out-of-range port, and an out-of-range query counts as EMPTY.
 *
 * MUTATION: drop the range check and this fails -- or crashes, which is also a kill. A
 * caller asking about port 7 is a bug worth seeing in the count rather than silently
 * dropping.
 */
static void test_port_apis_reject_an_out_of_range_port(void)
{
    setup();
    CHECK(!xinput_hle_attach_synthetic_pad(XINPUT_PORT_COUNT));
    CHECK(!xinput_hle_attach_synthetic_pad(99));
    CHECK(!xinput_hle_detach_synthetic_pad(XINPUT_PORT_COUNT));
    CHECK(xinput_hle_port_state(XINPUT_PORT_COUNT) == XINPUT_PORT_EMPTY);
    CHECK(!xinput_hle_port_connected(XINPUT_PORT_COUNT));
    CHECK_EQ_U64(xinput_hle_empty_query_count(), 1u);
    CHECK_EQ_U64(xinput_hle_connected_count(), 0u);

    xinput_pad_state pad = xinput_hle_pad_state(99);
    CHECK_EQ_U64(pad.packet_number, 0u);
    CHECK_EQ_U64(pad.digital_buttons, 0u);
    teardown();
}

/* ===================== the guest layout ===================== */

/* A guest-memory stand-in, prefilled with a sentinel so that "wrote nothing" is
 * provable rather than inferred from a return value. */
#define FAKE_GUEST_SIZE 256u
#define FAKE_SENTINEL 0xA5u

static uint8_t fake_guest[FAKE_GUEST_SIZE];
static unsigned fake_writes;

static void fake_writer(uint32_t guest_address, const void *bytes, uint32_t len,
                        void *user)
{
    (void)user;
    fake_writes++;
    if (guest_address + len > FAKE_GUEST_SIZE) {
        printf("FAIL writer asked for %#x..%#x, outside the %u-byte fake\n",
               guest_address, guest_address + len, FAKE_GUEST_SIZE);
        failures++;
        return;
    }
    memcpy(fake_guest + guest_address, bytes, len);
}

static void fake_guest_reset(void)
{
    memset(fake_guest, FAKE_SENTINEL, sizeof(fake_guest));
    fake_writes = 0;
}

static bool fake_guest_untouched(void)
{
    for (size_t i = 0; i < FAKE_GUEST_SIZE; i++) {
        if (fake_guest[i] != FAKE_SENTINEL) {
            return false;
        }
    }
    return true;
}

/*
 * The layout is UNSET by default, and a write REFUSES having written nothing.
 *
 * MUTATION: give state_size or any placement a nonzero default, or make
 * write_guest_state proceed with an unusable layout, and this fails. This is the most
 * important refusal in the module: the alternative is the IO_STATUS_BLOCK overrun with
 * nine fields to get wrong instead of one.
 *
 * The buffer is checked byte for byte rather than trusting the return value, because a
 * module that wrote first and returned 0 afterwards would pass a return-value-only
 * test.
 */
static void test_the_layout_is_unset_by_default_and_writes_nothing(void)
{
    setup();
    fake_guest_reset();
    xinput_hle_set_writer(fake_writer, NULL);

    CHECK_EQ_U64(xinput_hle_state_size(), XINPUT_STATE_SIZE_UNSET);
    CHECK_EQ_U64(XINPUT_STATE_SIZE_UNSET, 0u);
    CHECK_EQ_U64(xinput_hle_mapped_field_count(), 0u);
    CHECK(!xinput_hle_layout_usable());
    for (int f = 0; f < XINPUT_FIELD_COUNT; f++) {
        xinput_field_placement place = xinput_hle_field((xinput_field)f);
        CHECK(!place.mapped);
        CHECK_EQ_U64(place.offset, XINPUT_OFFSET_UNSET);
        CHECK_EQ_U64(place.width, 0u);
    }

    CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 0u);
    CHECK_EQ_U64(fake_writes, 0u);
    CHECK(fake_guest_untouched());
    CHECK_EQ_U64(xinput_hle_write_refused_count(), 1u);

    /* The refusal names the precedent and what has to be measured. */
    CHECK(captured_contains("REFUSED"));
    CHECK(captured_contains("IO_STATUS_BLOCK"));
    CHECK(captured_contains("16 bytes where the guest's is 8"));
    CHECK(captured_contains("desktop XInput layout is NOT the answer"));
    CHECK(captured_contains("docs/guest-structs.md"));

    /* Reported once, counted every time -- this could be called every frame. */
    for (unsigned i = 0; i < 50; i++) {
        CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 0u);
    }
    CHECK_EQ_U64(captured_count("REFUSED"), 1u);
    CHECK_EQ_U64(xinput_hle_write_refused_count(), 51u);
    CHECK(fake_guest_untouched());

    /* And the report says so. */
    captured_reset();
    xinput_hle_report();
    CHECK(captured_contains("layout NOT DERIVED"));
    CHECK(captured_contains("HOST-SIDE only"));
    CHECK(captured_contains("are NOT guessed here"));
    teardown();
}

/*
 * A field cannot be mapped before a size is derived.
 *
 * MUTATION: allow map_field with no size, and this fails. The size is the bound every
 * field is checked against; without it there is nothing to reject an overrun.
 */
static void test_a_field_cannot_be_mapped_before_a_size_is_derived(void)
{
    setup();
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 4u));
    CHECK(captured_contains("no guest state size has been derived"));
    CHECK_EQ_U64(xinput_hle_mapped_field_count(), 0u);
    CHECK(!xinput_hle_layout_usable());

    xinput_hle_set_state_size(20u);
    CHECK_EQ_U64(xinput_hle_state_size(), 20u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 4u));
    CHECK_EQ_U64(xinput_hle_mapped_field_count(), 1u);
    CHECK(xinput_hle_layout_usable());
    teardown();
}

/*
 * A field past the declared end is REFUSED, not clamped. This is the IO_STATUS_BLOCK
 * check.
 *
 * MUTATION: clamp the width to fit instead of refusing, or use `>=` where the bound
 * needs `>`, and this fails. Clamping writes a truncated field at an offset derived
 * from nothing and reports success, which is strictly worse than refusing.
 */
static void test_a_field_past_the_end_is_refused(void)
{
    setup();
    xinput_hle_set_state_size(8u);

    /* Exactly fits: the last byte of the structure is a legal place for a field. */
    CHECK(xinput_hle_map_field(XINPUT_FIELD_THUMB_LEFT_X, 7u, 1u));
    /* One past. */
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_THUMB_LEFT_Y, 8u, 1u));
    CHECK(captured_contains("exceeds the derived state size"));
    CHECK(captured_contains("IO_STATUS_BLOCK failure exactly"));
    /* Straddling the end. */
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 6u, 4u));
    /* A wrapping sum is the same bug and must not pass by arithmetic accident. */
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, UINT32_MAX - 1u, 4u));
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, XINPUT_OFFSET_UNSET, 4u));
    /* A zero width writes nothing and is not a derivation. */
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 0u));
    CHECK(captured_contains("width of 0"));

    /* Only the one legal mapping survived. */
    CHECK_EQ_U64(xinput_hle_mapped_field_count(), 1u);
    CHECK(xinput_hle_field(XINPUT_FIELD_THUMB_LEFT_X).mapped);
    CHECK(!xinput_hle_field(XINPUT_FIELD_THUMB_LEFT_Y).mapped);
    CHECK(!xinput_hle_field(XINPUT_FIELD_PACKET_NUMBER).mapped);

    /* An out-of-range field id is refused rather than written past the array. */
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_COUNT, 0u, 1u));
    CHECK(!xinput_hle_map_field((xinput_field)99, 0u, 1u));
    CHECK(!xinput_hle_field(XINPUT_FIELD_COUNT).mapped);
    CHECK_EQ_U64(xinput_hle_field((xinput_field)99).offset, XINPUT_OFFSET_UNSET);
    teardown();
}

/*
 * Overlapping fields are refused.
 *
 * MUTATION: drop the overlap loop and this fails. Two fields claiming the same bytes
 * means at least one derivation is wrong, and accepting both writes one over the other
 * in an order nobody chose -- producing a field that is intermittently correct, which
 * is the hardest kind of bug to attribute.
 */
static void test_overlapping_fields_are_refused(void)
{
    setup();
    xinput_hle_set_state_size(32u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 4u));

    /* Every kind of overlap: identical, straddling the front, straddling the back,
     * and fully contained. */
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_DIGITAL_BUTTONS, 0u, 4u));
    CHECK(captured_contains("overlap"));
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_DIGITAL_BUTTONS, 2u, 4u));
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_DIGITAL_BUTTONS, 3u, 2u));
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_DIGITAL_BUTTONS, 1u, 1u));
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_ANALOG_RUN, 0u, 8u));
    CHECK_EQ_U64(xinput_hle_mapped_field_count(), 1u);

    /* Abutting is not overlapping: byte 4 is free. */
    CHECK(xinput_hle_map_field(XINPUT_FIELD_DIGITAL_BUTTONS, 4u, 2u));
    CHECK_EQ_U64(xinput_hle_mapped_field_count(), 2u);

    /* Re-mapping a field onto its own bytes is a correction, not an overlap. */
    CHECK(xinput_hle_map_field(XINPUT_FIELD_DIGITAL_BUTTONS, 4u, 2u));
    CHECK(xinput_hle_map_field(XINPUT_FIELD_DIGITAL_BUTTONS, 6u, 2u));
    CHECK_EQ_U64(xinput_hle_mapped_field_count(), 2u);
    CHECK_EQ_U64(xinput_hle_field(XINPUT_FIELD_DIGITAL_BUTTONS).offset, 6u);
    teardown();
}

/*
 * A scalar field must have a scalar width; only the analog-button run may not.
 *
 * MUTATION: accept any width for every field, and this fails. A 3-byte packet number is
 * not something an access width can produce, so accepting one means the derivation came
 * from somewhere other than an access -- which is the thing this module is built to
 * refuse.
 */
static void test_a_non_scalar_width_is_refused_except_for_the_analog_run(void)
{
    setup();
    xinput_hle_set_state_size(64u);
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 3u));
    CHECK(captured_contains("not a scalar access width"));
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_THUMB_LEFT_X, 0u, 5u));
    CHECK(!xinput_hle_map_field(XINPUT_FIELD_THUMB_LEFT_X, 0u, 8u));
    CHECK(xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 4u));
    CHECK(xinput_hle_map_field(XINPUT_FIELD_DIGITAL_BUTTONS, 4u, 2u));
    CHECK(xinput_hle_map_field(XINPUT_FIELD_THUMB_LEFT_X, 6u, 1u));

    /* The analog region is a RUN, so its length is itself a measurement and any width is
     * legal -- including the measured eight. */
    CHECK(xinput_hle_map_field(XINPUT_FIELD_ANALOG_RUN, 8u, 8u));
    CHECK_EQ_U64(xinput_hle_field(XINPUT_FIELD_ANALOG_RUN).width, 8u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_ANALOG_RUN, 8u, 6u));
    CHECK(xinput_hle_map_field(XINPUT_FIELD_ANALOG_RUN, 8u, 3u));
    CHECK(xinput_hle_map_field(XINPUT_FIELD_ANALOG_RUN, 8u, 12u));
    teardown();
}

/*
 * Changing the size drops EVERY mapping.
 *
 * MUTATION: keep the mappings across a size change, and this fails. A field was
 * accepted because it fitted the size in force at the time; if that size no longer
 * applies the field is unvalidated, and keeping it across a SHRINK reintroduces the
 * overrun directly.
 */
static void test_changing_the_size_drops_every_mapping(void)
{
    setup();
    xinput_hle_set_state_size(32u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 4u));
    CHECK(xinput_hle_map_field(XINPUT_FIELD_DIGITAL_BUTTONS, 28u, 4u));
    CHECK_EQ_U64(xinput_hle_mapped_field_count(), 2u);

    /* Shrinking: the field at 28 would now be past the end. */
    xinput_hle_set_state_size(8u);
    CHECK_EQ_U64(xinput_hle_state_size(), 8u);
    CHECK_EQ_U64(xinput_hle_mapped_field_count(), 0u);
    CHECK(!xinput_hle_layout_usable());
    CHECK(captured_contains("every field mapping dropped"));

    /* Growing drops them too: a field validated against a different size is simply
     * unvalidated, and the direction of the change does not make it less so. */
    CHECK(xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 4u));
    xinput_hle_set_state_size(64u);
    CHECK_EQ_U64(xinput_hle_mapped_field_count(), 0u);

    /* Clearing the size clears everything and is reported differently. */
    CHECK(xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 4u));
    captured_reset();
    xinput_hle_set_state_size(XINPUT_STATE_SIZE_UNSET);
    CHECK(captured_contains("size cleared"));
    CHECK_EQ_U64(xinput_hle_state_size(), XINPUT_STATE_SIZE_UNSET);
    CHECK_EQ_U64(xinput_hle_mapped_field_count(), 0u);
    CHECK(!xinput_hle_layout_usable());
    teardown();
}

/*
 * A derived layout writes ONLY the mapped fields, little-endian, at the derived width.
 *
 * NOTE WHAT THIS TEST IS AND IS NOT. The offsets below are INVENTED BY THIS TEST to
 * exercise the marshalling arithmetic. They are not a claim about the guest layout, and
 * no part of the module ships them -- that is the entire point of the module. A real
 * derivation supplies real offsets with evidence.
 *
 * MUTATION: write host byte order instead of little-endian, write unmapped fields as
 * zero instead of skipping them, or write at `offset` instead of
 * `guest_address + offset`, and this fails.
 */
static void test_a_derived_layout_writes_only_mapped_fields_little_endian(void)
{
    setup();
    fake_guest_reset();
    xinput_hle_set_writer(fake_writer, NULL);

    xinput_hle_set_state_size(16u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 4u));
    CHECK(xinput_hle_map_field(XINPUT_FIELD_DIGITAL_BUTTONS, 4u, 2u));
    CHECK(xinput_hle_map_field(XINPUT_FIELD_ANALOG_RUN, 6u, 6u));
    CHECK(xinput_hle_map_field(XINPUT_FIELD_THUMB_LEFT_X, 12u, 2u));
    CHECK_EQ_U64(xinput_hle_mapped_field_count(), 4u);
    CHECK(xinput_hle_layout_usable());

    /* Written at a nonzero base, so a write that ignored the base would land on the
     * sentinel bytes below it and be caught. */
    const uint32_t base = 64u;
    unsigned written = xinput_hle_write_guest_state(0, base);
    CHECK_EQ_U64(written, 4u);
    CHECK_EQ_U64(fake_writes, 4u);

    /* Every value is zero because nothing is read from hardware, so what is being
     * checked is PLACEMENT: the mapped bytes changed and nothing else did. */
    for (uint32_t i = 0; i < 16u; i++) {
        bool mapped = i < 14u; /* 0..3, 4..5, 6..11, 12..13 */
        CHECK(fake_guest[base + i] == (mapped ? 0x00u : FAKE_SENTINEL));
    }
    /* Bytes 14 and 15 of the structure are unmapped and must be untouched. */
    CHECK(fake_guest[base + 14u] == FAKE_SENTINEL);
    CHECK(fake_guest[base + 15u] == FAKE_SENTINEL);
    /* Nothing below the base, which is what proves the base is honoured. */
    for (uint32_t i = 0; i < base; i++) {
        CHECK(fake_guest[i] == FAKE_SENTINEL);
    }
    CHECK_EQ_U64(xinput_hle_write_refused_count(), 0u);

    /* The report changes voice once a layout exists. */
    captured_reset();
    xinput_hle_report();
    CHECK(captured_contains("layout DERIVED"));
    CHECK(captured_contains("16 byte structure, 4 of 7 fields mapped"));
    CHECK(!captured_contains("NOT DERIVED"));
    teardown();
}

/*
 * The byte order is little-endian explicitly, and a narrow field truncates.
 *
 * MUTATION: memcpy a host integer instead of shifting bytes out, and this passes on
 * x86 and fails on a big-endian host -- so the shift is asserted here against a known
 * multi-byte value rather than left to the build host's luck. A narrowing write must
 * take the LOW bytes, which is what a narrower guest field means.
 */
static void test_the_marshalled_byte_order_is_little_endian(void)
{
    setup();
    fake_guest_reset();
    xinput_hle_set_writer(fake_writer, NULL);

    /* NON-ZERO VALUES ARE MANDATORY HERE, AND THIS IS WHY.
     *
     * Every host value is zero unless something fabricates one, and against an all-zero
     * state a byte-order bug is INVISIBLE: reversing the bytes of 0x00000000 produces
     * 0x00000000. An earlier version of this test asserted little-endianness against
     * zeros and would have passed against a big-endian module, which is the "two empty
     * lists are equal" failure exactly. The synthetic setter exists partly so that this
     * test can be real. */
    CHECK(xinput_hle_attach_synthetic_pad(0));
    xinput_pad_state state;
    memset(&state, 0, sizeof(state));
    state.packet_number = 0x11223344u;
    state.digital_buttons = 0xBEEFu;
    state.thumb_left_x = (int16_t)0xF00Du;
    for (unsigned i = 0; i < XINPUT_ANALOG_COUNT; i++) {
        state.analog[i] = (uint8_t)(0xA0u + i);
    }
    CHECK(xinput_hle_set_synthetic_pad_state(0, state));

    xinput_hle_set_state_size(16u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 4u));
    CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 1u);
    /* Low byte FIRST. Big-endian would put 0x11 at offset 0. */
    CHECK_EQ_U64(fake_guest[0], 0x44u);
    CHECK_EQ_U64(fake_guest[1], 0x33u);
    CHECK_EQ_U64(fake_guest[2], 0x22u);
    CHECK_EQ_U64(fake_guest[3], 0x11u);
    CHECK_EQ_U64(fake_guest[4], FAKE_SENTINEL);

    /* A narrower mapping of the same field takes the LOW bytes, which is what a
     * narrower guest field means. A high-byte truncation would put 0x11 here. */
    fake_guest_reset();
    xinput_hle_set_state_size(16u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 1u));
    CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 1u);
    CHECK_EQ_U64(fake_guest[0], 0x44u);
    CHECK_EQ_U64(fake_guest[1], FAKE_SENTINEL);

    fake_guest_reset();
    xinput_hle_set_state_size(16u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 2u));
    CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 1u);
    CHECK_EQ_U64(fake_guest[0], 0x44u);
    CHECK_EQ_U64(fake_guest[1], 0x33u);
    CHECK_EQ_U64(fake_guest[2], FAKE_SENTINEL);

    /* A signed axis marshals as its two's-complement bit pattern, low byte first, with
     * no sign extension into a neighbouring byte. */
    fake_guest_reset();
    xinput_hle_set_state_size(16u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_THUMB_LEFT_X, 0u, 2u));
    CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 1u);
    CHECK_EQ_U64(fake_guest[0], 0x0Du);
    CHECK_EQ_U64(fake_guest[1], 0xF0u);
    CHECK_EQ_U64(fake_guest[2], FAKE_SENTINEL);

    /* The analog run copies entry for entry, in order, with no rearrangement. */
    fake_guest_reset();
    xinput_hle_set_state_size(16u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_ANALOG_RUN, 0u, XINPUT_ANALOG_COUNT));
    CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 1u);
    for (unsigned i = 0; i < XINPUT_ANALOG_COUNT; i++) {
        CHECK_EQ_U64(fake_guest[i], 0xA0u + i);
    }
    CHECK_EQ_U64(fake_guest[XINPUT_ANALOG_COUNT], FAKE_SENTINEL);
    /* And the triggers really are the last two entries of it. */
    CHECK_EQ_U64(fake_guest[XINPUT_ANALOG_LEFT_TRIGGER], 0xA6u);
    CHECK_EQ_U64(fake_guest[XINPUT_ANALOG_RIGHT_TRIGGER], 0xA7u);

    /* A narrower run writes a PREFIX, not a selection. */
    fake_guest_reset();
    xinput_hle_set_state_size(16u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_ANALOG_RUN, 0u, 2u));
    CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 1u);
    CHECK_EQ_U64(fake_guest[0], 0xA0u);
    CHECK_EQ_U64(fake_guest[1], 0xA1u);
    CHECK_EQ_U64(fake_guest[2], FAKE_SENTINEL);

    /* A run WIDER than the eight host entries zero-fills rather than reading past the
     * host array -- an out-of-bounds read in the module whose point is not overrunning
     * things would be a poor joke. */
    fake_guest_reset();
    xinput_hle_set_state_size(16u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_ANALOG_RUN, 0u, 12u));
    CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 1u);
    for (unsigned i = 0; i < XINPUT_ANALOG_COUNT; i++) {
        CHECK_EQ_U64(fake_guest[i], 0xA0u + i);
    }
    for (unsigned i = XINPUT_ANALOG_COUNT; i < 12u; i++) {
        CHECK_EQ_U64(fake_guest[i], 0u);
    }
    CHECK_EQ_U64(fake_guest[12], FAKE_SENTINEL);
    teardown();
}

/*
 * The compiled-in MEASURED layout is not the default, passes its own validation, and
 * places every field where the image says.
 *
 * MUTATION: change any XINPUT_MEASURED_OFFSET_*, or the analog run's width from 8 to 6,
 * or the declared size from 22, and this fails. Make adoption the default in
 * xinput_hle_init and it fails on the first assertion -- which is the point: the layout
 * is measured and still opt-in, because the cost of being wrong is silent corruption
 * rather than a failed test.
 */
static void test_the_measured_layout_is_available_but_not_the_default(void)
{
    setup();
    /* NOT adopted by default. */
    CHECK(!xinput_hle_measured_layout_adopted());
    CHECK(!xinput_hle_layout_usable());
    CHECK_EQ_U64(xinput_hle_state_size(), XINPUT_STATE_SIZE_UNSET);

    /* The report distinguishes "no measurement" from "measured, not adopted". */
    xinput_hle_report();
    CHECK(captured_contains("a MEASURED layout IS available"));
    CHECK(captured_contains("has NOT been adopted"));

    captured_reset();
    CHECK(xinput_hle_adopt_measured_layout());
    CHECK(xinput_hle_measured_layout_adopted());
    CHECK(xinput_hle_layout_usable());
    CHECK(captured_contains("adopted the MEASURED guest state layout"));
    CHECK(captured_contains("analog run is EIGHT bytes"));

    CHECK_EQ_U64(xinput_hle_state_size(), 22u);
    CHECK_EQ_U64(XINPUT_MEASURED_STATE_SIZE, 22u);
    CHECK_EQ_U64(XINPUT_MEASURED_STATE_FRAME, 24u);
    CHECK_EQ_U64(xinput_hle_mapped_field_count(), 7u);

    /* Every field, at the measured offset and width. */
    CHECK_EQ_U64(xinput_hle_field(XINPUT_FIELD_PACKET_NUMBER).offset, 0x00u);
    CHECK_EQ_U64(xinput_hle_field(XINPUT_FIELD_PACKET_NUMBER).width, 4u);
    CHECK_EQ_U64(xinput_hle_field(XINPUT_FIELD_DIGITAL_BUTTONS).offset, 0x04u);
    CHECK_EQ_U64(xinput_hle_field(XINPUT_FIELD_DIGITAL_BUTTONS).width, 2u);
    CHECK_EQ_U64(xinput_hle_field(XINPUT_FIELD_ANALOG_RUN).offset, 0x06u);
    CHECK_EQ_U64(xinput_hle_field(XINPUT_FIELD_ANALOG_RUN).width, 8u);
    CHECK_EQ_U64(xinput_hle_field(XINPUT_FIELD_THUMB_LEFT_X).offset, 0x0Eu);
    CHECK_EQ_U64(xinput_hle_field(XINPUT_FIELD_THUMB_LEFT_Y).offset, 0x10u);
    CHECK_EQ_U64(xinput_hle_field(XINPUT_FIELD_THUMB_RIGHT_X).offset, 0x12u);
    CHECK_EQ_U64(xinput_hle_field(XINPUT_FIELD_THUMB_RIGHT_Y).offset, 0x14u);
    CHECK_EQ_U64(xinput_hle_field(XINPUT_FIELD_THUMB_RIGHT_Y).width, 2u);

    /* THE LAYOUT IS GAPLESS AND EXACTLY FILLS THE DECLARED SIZE. Checked here rather
     * than trusted, because a transcription error in one offset would otherwise leave a
     * hole or an overlap that no individual assertion above would notice. */
    uint32_t covered = 0;
    uint32_t highest = 0;
    for (int f = 0; f < XINPUT_FIELD_COUNT; f++) {
        xinput_field_placement place = xinput_hle_field((xinput_field)f);
        CHECK(place.mapped);
        covered += place.width;
        if (place.offset + place.width > highest) {
            highest = place.offset + place.width;
        }
    }
    CHECK_EQ_U64(covered, 22u);  /* 4 + 2 + 8 + 2 + 2 + 2 + 2 */
    CHECK_EQ_U64(highest, 22u);  /* so no gaps: sum of widths == span */

    /* Writing the whole structure touches exactly 22 bytes and not the 2 beyond. */
    fake_guest_reset();
    xinput_hle_set_writer(fake_writer, NULL);
    CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 7u);
    for (unsigned i = 0; i < 22u; i++) {
        CHECK_EQ_U64(fake_guest[i], 0u);
    }
    /* The two bytes of the frame whose nature is undecided are NOT written. */
    CHECK_EQ_U64(fake_guest[22], FAKE_SENTINEL);
    CHECK_EQ_U64(fake_guest[23], FAKE_SENTINEL);

    /* A hand-mapped field afterwards means it is no longer the measured layout. */
    CHECK(xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 2u));
    CHECK(!xinput_hle_measured_layout_adopted());

    /* And init puts it back to unadopted. */
    xinput_hle_init();
    CHECK(!xinput_hle_measured_layout_adopted());
    CHECK(!xinput_hle_layout_usable());
    teardown();
}

/*
 * A usable layout with no writer installed writes nothing and says so.
 *
 * MUTATION: call a NULL writer, and this segfaults -- which is a kill, not a survivor.
 * Remove the refusal count and it fails on the counter instead.
 */
static void test_a_usable_layout_with_no_writer_writes_nothing(void)
{
    setup();
    fake_guest_reset();
    xinput_hle_set_state_size(8u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 4u));
    CHECK(xinput_hle_layout_usable());

    /* No writer installed at all. */
    CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 0u);
    CHECK(captured_contains("no writer installed"));
    CHECK_EQ_U64(xinput_hle_write_refused_count(), 1u);
    CHECK(fake_guest_untouched());

    /* Explicitly cleared is the same as never installed. */
    xinput_hle_set_writer(fake_writer, NULL);
    CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 1u);
    xinput_hle_set_writer(NULL, NULL);
    CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 0u);
    CHECK_EQ_U64(xinput_hle_write_refused_count(), 2u);

    /* An out-of-range port is refused even with a usable layout and a writer. */
    fake_guest_reset();
    xinput_hle_set_writer(fake_writer, NULL);
    CHECK_EQ_U64(xinput_hle_write_guest_state(XINPUT_PORT_COUNT, 0u), 0u);
    CHECK(captured_contains("does not exist"));
    CHECK(fake_guest_untouched());
    CHECK_EQ_U64(xinput_hle_write_refused_count(), 3u);
    teardown();
}

/*
 * Every field has a name, and the enum has no gaps.
 *
 * MUTATION: add a field to the enum without a name in the switch, and this fails. A
 * NULL slipping into a diagnostic's printf("%s") is a crash in the layer that exists to
 * explain crashes.
 */
static void test_every_field_has_a_name(void)
{
    setup();
    CHECK_EQ_U64(XINPUT_FIELD_COUNT, 7u);
    for (int f = 0; f < XINPUT_FIELD_COUNT; f++) {
        const char *name = xinput_hle_field_name((xinput_field)f);
        CHECK(name != NULL);
        CHECK(name[0] != '\0');
        for (int g = f + 1; g < XINPUT_FIELD_COUNT; g++) {
            CHECK(strcmp(name, xinput_hle_field_name((xinput_field)g)) != 0);
        }
    }
    CHECK(xinput_hle_field_name(XINPUT_FIELD_COUNT) == NULL);
    CHECK(xinput_hle_field_name((xinput_field)99) == NULL);
    teardown();
}

/* ===================== cross-check ===================== */

/*
 * The cross-check agrees with itself and flags every divergence.
 *
 * MUTATION: delete either direction of the comparison, and this fails. A check that
 * only walked `refs` would pass against a table of ours that had grown an invented row,
 * which is the drift most likely to happen by hand.
 */
static void test_crosscheck_agrees_with_itself_and_flags_every_divergence(void)
{
    setup();
    size_t count = 0;
    const xinput_entry *table = xinput_hle_table(&count);

    xinput_surface_ref refs[XINPUT_FUNCTION_COUNT];
    for (size_t i = 0; i < count; i++) {
        refs[i].address = table[i].address;
        refs[i].name = table[i].name;
        refs[i].sites = table[i].sites;
    }
    CHECK_EQ_U64(xinput_hle_crosscheck(refs, count), 0u);
    CHECK_EQ_U64(captured_len, 0u);

    /* A differing site count. */
    refs[0].sites += 1u;
    CHECK_EQ_U64(xinput_hle_crosscheck(refs, count), 1u);
    CHECK(captured_contains("site count"));
    refs[0].sites -= 1u;

    /* A differing name. */
    captured_reset();
    refs[1].name = "XGetDeviceChangesEx";
    CHECK_EQ_U64(xinput_hle_crosscheck(refs, count), 1u);
    CHECK(captured_contains("named"));
    refs[1].name = table[1].name;

    /* A NULL name on one side only. */
    captured_reset();
    refs[1].name = NULL;
    CHECK_EQ_U64(xinput_hle_crosscheck(refs, count), 1u);
    refs[1].name = table[1].name;

    /* A row they have and we do not. */
    captured_reset();
    refs[2].address = 0x0046dc00u;
    CHECK_EQ_U64(xinput_hle_crosscheck(refs, count), 2u); /* theirs extra + ours missing */
    CHECK(captured_contains("and we do not"));
    CHECK(captured_contains("and the generated table does not"));
    refs[2].address = table[2].address;

    /* A short list: every row of ours they lack is a disagreement. */
    captured_reset();
    CHECK_EQ_U64(xinput_hle_crosscheck(refs, count - 1u), 1u);
    CHECK_EQ_U64(xinput_hle_crosscheck(refs, 0u), 15u);

    /* A NULL pointer with a nonzero count is a disagreement, not a crash. */
    captured_reset();
    CHECK_EQ_U64(xinput_hle_crosscheck(NULL, 5u), 1u);
    CHECK(captured_contains("NULL pointer"));
    /* A NULL pointer with no rows still walks our side. */
    CHECK_EQ_U64(xinput_hle_crosscheck(NULL, 0u), 15u);
    teardown();
}

/*
 * Against the generator's 13-row XPP bucket, the cross-check reports EXACTLY two.
 *
 * This is the expected divergence written down and pinned: the generator lists a
 * function only if it has a call site whose ORIGIN is game `.text`, so the two
 * zero-site rows are absent from it. An expected divergence that is asserted is a
 * cross-check; one that is quietly tolerated is drift.
 *
 * MUTATION: drop either zero-site row from our table, and this fails on the count going
 * to one. Add a sixteenth invented row and it goes to three.
 */
static void test_crosscheck_against_the_generator_subset_reports_the_two_zero_rows(void)
{
    setup();
    /* The generated table's XPP rows, in its own order, transcribed as the generator
     * emits them. No zero-site rows, because it does not emit any. */
    static const xinput_surface_ref generated[13] = {
        {VA_XGETDEVICES, "XGetDevices", 4},
        {VA_XGETDEVICECHANGES, "XGetDeviceChanges", 3},
        {VA_XPEEKDEVICES, "XPeekDevices", 2},
        {VA_XINPUTCLOSE, "XInputClose", 2},
        {VA_XVOICECREATEEX, "XVoiceCreateMediaObjectEx", 2},
        {VA_XMOUNTMUA, "XMountMUA", 1},
        {VA_XUNMOUNTMU, "XUnmountMU", 1},
        {VA_XINITDEVICES, "XInitDevices", 1},
        {VA_XINPUTOPEN, "XInputOpen", 1},
        {VA_XINPUTGETCAPS, "XInputGetCapabilities", 1},
        {VA_XINPUTGETSTATE, "XInputGetState", 1},
        {VA_XINPUTSETSTATE, "XInputSetState", 1},
        {VA_XGETDEVENUMSTATUS, "XGetDeviceEnumerationStatus", 1},
    };

    CHECK_EQ_U64(xinput_hle_crosscheck(generated, 13u), 2u);
    CHECK(captured_contains("XReadMUMetaData"));
    CHECK(captured_contains("XVoiceCreateMediaObject"));
    /* And nothing in the other direction: every generated row is one of ours, with the
     * same name and the same site count. */
    CHECK(!captured_contains("and we do not"));

    /* The 13 generated rows sum to the same 21 sites as our 15, which is the arithmetic
     * that makes the two tables reconcilable at all. */
    uint32_t total = 0;
    for (size_t i = 0; i < 13u; i++) {
        total += generated[i].sites;
    }
    CHECK_EQ_U64(total, 21u);
    teardown();
}

/* ===================== init and the sink ===================== */

/*
 * init clears the counters, the ports, the layout and the once-only latches.
 *
 * MUTATION: leave any counter, port or placement out of xinput_hle_init, and this
 * fails. An init that left a SYNTHETIC pad or a derived layout installed would silently
 * change what every later test is testing -- and in a run, would carry one trace's
 * fabrications into the next.
 */
static void test_init_clears_counters_ports_and_the_layout(void)
{
    setup();
    (void)xinput_hle_call(VA_XINPUTGETSTATE, NULL);
    (void)xinput_hle_call(VA_XINPUTGETSTATE + 1u, NULL);
    (void)xinput_hle_port_connected(0);
    CHECK(xinput_hle_attach_synthetic_pad(1));
    CHECK(xinput_hle_register(VA_XGETDEVICES, fake_handler));
    xinput_hle_set_state_size(16u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 4u));
    xinput_hle_set_writer(fake_writer, NULL);
    CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 1u);

    /* ONE, not two. The second call went to an address absent from the table, which is
     * an unknown-target disagreement and must NOT be counted as a function touched --
     * if it were, `touched_count` would overstate how much of the surface the guest had
     * actually reached, which is the one number this report exists to get right. */
    CHECK_EQ_U64(xinput_hle_touched_count(), 1u);
    CHECK_EQ_U64(xinput_hle_unknown_call_count(), 1u);
    CHECK_EQ_U64(xinput_hle_empty_query_count(), 1u);
    CHECK_EQ_U64(xinput_hle_implemented_count(), 1u);
    CHECK_EQ_U64(xinput_hle_connected_count(), 1u);
    CHECK(xinput_hle_layout_usable());

    xinput_hle_init();
    captured_reset();
    xinput_hle_set_log(capture_printer);

    CHECK_EQ_U64(xinput_hle_touched_count(), 0u);
    CHECK_EQ_U64(xinput_hle_unknown_call_count(), 0u);
    CHECK_EQ_U64(xinput_hle_empty_query_count(), 0u);
    CHECK_EQ_U64(xinput_hle_implemented_count(), 0u);
    CHECK_EQ_U64(xinput_hle_connected_count(), 0u);
    CHECK_EQ_U64(xinput_hle_write_refused_count(), 0u);
    CHECK_EQ_U64(xinput_hle_state_size(), XINPUT_STATE_SIZE_UNSET);
    CHECK_EQ_U64(xinput_hle_mapped_field_count(), 0u);
    CHECK(!xinput_hle_layout_usable());
    for (unsigned port = 0; port < XINPUT_PORT_COUNT; port++) {
        CHECK(xinput_hle_port_state(port) == XINPUT_PORT_EMPTY);
    }
    /* The writer is cleared too, so a stale pointer into a freed address space cannot
     * survive a re-init. */
    xinput_hle_set_state_size(8u);
    CHECK(xinput_hle_map_field(XINPUT_FIELD_PACKET_NUMBER, 0u, 4u));
    CHECK_EQ_U64(xinput_hle_write_guest_state(0, 0u), 0u);
    CHECK(captured_contains("no writer installed"));

    /* And the once-only reports are armed again, so a second run logs its own. */
    xinput_hle_init();
    captured_reset();
    xinput_hle_set_log(capture_printer);
    (void)xinput_hle_call(VA_XINPUTGETSTATE, NULL);
    CHECK_EQ_U64(captured_count("XInputGetState"), 1u);
    (void)xinput_hle_port_connected(0);
    CHECK_EQ_U64(captured_count("NOTHING IS READ FROM HARDWARE"), 1u);
    teardown();
}

/* The sink is never NULL, so no diagnostic path can crash on it. */
static void test_the_log_sink_is_never_null(void)
{
    setup();
    CHECK(xinput_hle_log() != NULL);
    xinput_hle_set_log(NULL);
    CHECK(xinput_hle_log() != NULL);
    teardown();
}

int main(void)
{
    printf("xinput HLE boundary tests\n");

    test_the_measured_table_is_15_functions_and_21_sites();
    test_every_row_is_named_and_inside_the_xpp_section();
    test_no_two_table_addresses_collide();
    test_the_kind_classification_matches_the_measured_split();
    test_only_the_two_known_rows_have_zero_sites();
    test_the_data_symbols_are_distinct_and_never_collide_with_a_function();

    test_a_stub_reports_exactly_once_however_many_times_it_is_called();
    test_each_function_reports_for_itself();
    test_an_unknown_target_reports_every_single_time();
    test_the_three_unknown_target_messages_are_distinct();
    test_a_call_to_a_data_symbol_says_it_is_a_data_table();
    test_lookup_is_exact_and_never_returns_a_neighbour();
    test_lookup_by_name_is_exact();
    test_an_unnamed_row_labels_itself_by_address();
    test_an_implemented_function_runs_and_does_not_report();
    test_register_refuses_an_unknown_address_and_a_null_handler();

    test_the_backlog_is_ordered_by_measured_sites_before_any_run();
    test_an_observed_call_outranks_a_busier_measured_row();
    test_the_report_says_input_is_read_from_nothing();
    test_the_report_surfaces_a_table_binary_disagreement();

    test_every_port_defaults_to_empty();
    test_an_empty_answer_names_the_symptoms_exactly_once();
    test_attaching_the_synthetic_pad_announces_the_fabrication();
    test_the_synthetic_pad_fabricates_presence_only_not_input();
    test_fabricated_values_need_a_pad_and_announce_themselves();
    test_port_apis_reject_an_out_of_range_port();

    test_the_layout_is_unset_by_default_and_writes_nothing();
    test_a_field_cannot_be_mapped_before_a_size_is_derived();
    test_a_field_past_the_end_is_refused();
    test_overlapping_fields_are_refused();
    test_a_non_scalar_width_is_refused_except_for_the_analog_run();
    test_changing_the_size_drops_every_mapping();
    test_a_derived_layout_writes_only_mapped_fields_little_endian();
    test_the_marshalled_byte_order_is_little_endian();
    test_a_usable_layout_with_no_writer_writes_nothing();
    test_the_measured_layout_is_available_but_not_the_default();
    test_every_field_has_a_name();

    test_crosscheck_agrees_with_itself_and_flags_every_divergence();
    test_crosscheck_against_the_generator_subset_reports_the_two_zero_rows();

    test_init_clears_counters_ports_and_the_layout();
    test_the_log_sink_is_never_null();

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
