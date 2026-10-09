/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Suite for src/xbox/kernel_arity_oracle.c -- the ordinal-indexed arity table derived
 * from nxdk's CC0-1.0 `xboxkrnl.exe.def`.
 *
 * DELIBERATELY FREE OF LIFTED CODE, OF THE XBE, AND OF THE GENERATED ARITY TABLE. The
 * oracle is committed precisely so it builds and is checked in a fresh clone with
 * nothing generated, and a suite that needed `kernel_arity.inc` would throw that away.
 * The cross-check AGAINST the generated measured table, and against the hand-written
 * `ABI_TABLE` that lives in a `static` nobody outside its translation unit can see,
 * is in tests/test_arity_oracle.py instead.
 *
 * EVERY CHECK HERE IS MUTATION-TESTED; each test says what breaks it. The set is
 * tools/mutate/sets/arity_oracle.py.
 *
 * WHAT THERE IS TO GET WRONG, in order of how badly it hurts.
 *
 *   1. A DWORD COUNT THAT IS REALLY A BYTE COUNT. The `.def` decoration carries BYTES;
 *      everything downstream wants dwords. Forgetting the divide is a silent four-times
 *      error, and for small arities the wrong answer is still a plausible one -- 1 arg
 *      becomes 4, which is an entirely ordinary arity for a kernel export. This is the
 *      worst defect available because `__stdcall` is callee-cleanup: popping four times
 *      too many eats the caller's locals and `esp` never recovers, so the run does not
 *      crash, it keeps going and lies.
 *   2. A DATA EXPORT GIVEN AN ARITY. 34 rows here are variables, not functions. Handing
 *      one a pop count makes the thunk pop bytes nobody pushed. Note the table stores
 *      zero in `stack_args` for those rows because the field must hold something, so
 *      reading that field without checking the convention LOOKS right and is not --
 *      which is why `kernel_arity_oracle_callee_pop()` refuses them rather than
 *      returning zero.
 *   3. FASTCALL MISREAD AS STDCALL. 24 rows pass one or two arguments in ecx/edx. Read
 *      as stdcall, `KfLowerIrql@4` would both read a bogus argument off the stack and
 *      pop four bytes nobody pushed. The register/stack SPLIT is the subtle half:
 *      `@ExInterlockedCompareExchange64@12` is three dwords, so two in registers and
 *      exactly ONE on the stack.
 *   4. __cdecl CONFLATED WITH "UNKNOWN". Five rows are undecorated variadic
 *      printf-family exports. The CALLER cleans up, so the callee pops zero -- a
 *      POSITIVE answer, not an absence of one. Refusing them would stop a run that
 *      could have continued correctly; giving them a non-zero pop would desync esp.
 *   5. A LOOKUP THAT RETURNS A NEIGHBOUR. Ordinals are sparse: 371 rows span 1..378, so
 *      a binary search with an off-by-one bound can answer with the wrong export's
 *      arity instead of admitting it has no row. The table must be strictly ascending
 *      for the search to be correct at all, so that is checked directly rather than
 *      assumed.
 */

#include "kernel_arity_oracle.h"

#include <stdio.h>

static int failures;
static int checks;

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        checks++;                                                                          \
        if (!(cond)) {                                                                      \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                          \
            failures++;                                                                     \
        }                                                                                   \
    } while (0)

#define CHECK_EQ(actual, expected)                                                         \
    do {                                                                                   \
        checks++;                                                                          \
        unsigned long long a_ = (unsigned long long)(actual);                               \
        unsigned long long e_ = (unsigned long long)(expected);                             \
        if (a_ != e_) {                                                                     \
            printf("FAIL %s:%d  %s: got %llu, want %llu\n", __FILE__, __LINE__, #actual,    \
                   a_, e_);                                                                 \
            failures++;                                                                     \
        }                                                                                   \
    } while (0)

/* Convenience: pop count, or a sentinel when the oracle refuses. 0xFFFF is outside
 * every legal arity, so a refusal can never be confused with a zero. */
#define REFUSED 0xFFFFu

static unsigned pop_or_refused(unsigned ordinal)
{
    unsigned dwords = 0;
    if (!kernel_arity_oracle_callee_pop(ordinal, &dwords)) {
        return REFUSED;
    }
    return dwords;
}

/* The table's shape. 371 rows is the nxdk export list in full; a row count that drifts
 * means the vendored `.def` changed or the generator dropped rows, and a dropped row
 * reads as "the oracle does not cover that ordinal" rather than as a bug. */
static void test_the_table_is_371_rows_spanning_ordinals_1_to_378(void)
{
    CHECK_EQ(kernel_arity_oracle_count(), 371u);

    const kernel_arity_oracle_entry *first = kernel_arity_oracle_at(0);
    const kernel_arity_oracle_entry *last = kernel_arity_oracle_at(370);
    CHECK(first != NULL);
    CHECK(last != NULL);
    if (first != NULL) {
        CHECK_EQ(first->ordinal, 1u);
    }
    if (last != NULL) {
        CHECK_EQ(last->ordinal, 378u);
    }
    /* Past the end must be NULL, not the last row again. */
    CHECK(kernel_arity_oracle_at(371u) == NULL);
    CHECK(kernel_arity_oracle_at(0xFFFFFFFFu) == NULL);
}

/* STRICTLY ascending, which is the binary search's precondition. Equal neighbours would
 * make the search's answer depend on which half it happened to land in. */
static void test_ordinals_are_strictly_ascending(void)
{
    unsigned previous = 0;
    unsigned count = kernel_arity_oracle_count();
    for (unsigned i = 0; i < count; i++) {
        const kernel_arity_oracle_entry *entry = kernel_arity_oracle_at(i);
        CHECK(entry != NULL);
        if (entry == NULL) {
            return;
        }
        CHECK(entry->ordinal > previous);
        previous = entry->ordinal;
    }
}

/* The convention histogram. A single row reclassified in either direction moves two of
 * these four numbers, so the set pins the classifier far harder than any one of them. */
static void test_the_convention_histogram_is_308_24_5_34(void)
{
    unsigned stdcall = 0;
    unsigned fastcall = 0;
    unsigned cdecl_ = 0;
    unsigned data = 0;
    unsigned count = kernel_arity_oracle_count();
    for (unsigned i = 0; i < count; i++) {
        const kernel_arity_oracle_entry *entry = kernel_arity_oracle_at(i);
        if (entry == NULL) {
            continue;
        }
        switch (entry->convention) {
        case KERNEL_ARITY_ORACLE_STDCALL:
            stdcall++;
            break;
        case KERNEL_ARITY_ORACLE_FASTCALL:
            fastcall++;
            break;
        case KERNEL_ARITY_ORACLE_CDECL:
            cdecl_++;
            break;
        case KERNEL_ARITY_ORACLE_DATA:
            data++;
            break;
        }
    }
    CHECK_EQ(stdcall, 308u);
    CHECK_EQ(fastcall, 24u);
    CHECK_EQ(cdecl_, 5u);
    CHECK_EQ(data, 34u);
    CHECK_EQ(stdcall + fastcall + cdecl_ + data, kernel_arity_oracle_count());
}

/* Lookup must be exact. These ordinals are real holes in the export table, and a
 * neighbour's arity returned for one of them is the quiet wrong answer that a stop
 * would have turned into a bug report. */
static void test_lookup_is_exact_and_never_returns_a_neighbour(void)
{
    /* 367..373 is the only real hole inside the table's range -- seven consecutive
     * unassigned ordinals, which makes them the probes most likely to catch a search
     * that clamps to a neighbour instead of failing. */
    static const unsigned absent[] = {0u, 367u, 370u, 373u, 379u, 1000u};
    for (unsigned i = 0; i < sizeof(absent) / sizeof(absent[0]); i++) {
        CHECK(kernel_arity_oracle_lookup(absent[i]) == NULL);
        CHECK_EQ(pop_or_refused(absent[i]), REFUSED);
    }
    /* Both ends and an interior probe, since an off-by-one bound shows at the ends. */
    const kernel_arity_oracle_entry *one = kernel_arity_oracle_lookup(1u);
    CHECK(one != NULL && one->ordinal == 1u);
    const kernel_arity_oracle_entry *end = kernel_arity_oracle_lookup(378u);
    CHECK(end != NULL && end->ordinal == 378u);
    /* Every row must be findable by its own ordinal, which is the only check that
     * covers the whole search space rather than a handful of probes. */
    unsigned count = kernel_arity_oracle_count();
    for (unsigned i = 0; i < count; i++) {
        const kernel_arity_oracle_entry *entry = kernel_arity_oracle_at(i);
        if (entry == NULL) {
            continue;
        }
        CHECK(kernel_arity_oracle_lookup(entry->ordinal) == entry);
    }
}

/*
 * DWORDS, NOT BYTES. These are the largest arities in the table, so they are where a
 * missing divide-by-four is most obviously wrong -- and three of them are ordinals the
 * call-site scanner got WRONG, which is why the oracle exists at all.
 *
 *   190 NtCreateFile    9 dwords (36 bytes). The scanner's measured row said 11.
 *   219 NtReadFile      8 dwords (32 bytes). The scanner said 6; hand count 8.
 *   236 NtWriteFile     8 dwords (32 bytes). The scanner said 6; hand count 8.
 *   340 XcHMAC          7 dwords (28 bytes). The scanner's minimum rule said 6.
 *   289 RtlInitAnsiString  2 dwords (8 bytes). The scanner said 1.
 *
 * Every one of those agrees with the hand count in src/host/kernel_thunk.c's ABI_TABLE
 * and disagrees with the measurement. That is the single strongest argument for the
 * oracle: it is a name decoration, so it cannot be fooled by the late `_icall_esp`
 * bracket that made 219 and 340 under-count.
 */
static void test_arities_are_dwords_and_match_the_hand_counts_the_scanner_missed(void)
{
    CHECK_EQ(pop_or_refused(190u), 9u);
    CHECK_EQ(pop_or_refused(219u), 8u);
    CHECK_EQ(pop_or_refused(236u), 8u);
    CHECK_EQ(pop_or_refused(340u), 7u);
    CHECK_EQ(pop_or_refused(289u), 2u);
    CHECK_EQ(pop_or_refused(279u), 3u);
    CHECK_EQ(pop_or_refused(255u), 10u);
    CHECK_EQ(pop_or_refused(24u), 5u);
    CHECK_EQ(pop_or_refused(202u), 6u);

    /* The structural form of the same check, which does not depend on knowing any
     * particular export. No x86 kernel export in this table takes more than ten stack
     * dwords; a byte count read as a dword count would reach forty. */
    unsigned count = kernel_arity_oracle_count();
    for (unsigned i = 0; i < count; i++) {
        const kernel_arity_oracle_entry *entry = kernel_arity_oracle_at(i);
        if (entry == NULL) {
            continue;
        }
        CHECK(entry->stack_args <= 10u);
    }
}

/* DATA exports have NO arity and must be refused, not answered with zero. The sweep is
 * the point: a classifier that lost one row would still pass a spot check. */
static void test_every_data_export_is_refused_rather_than_given_an_arity(void)
{
    unsigned data_rows = 0;
    unsigned count = kernel_arity_oracle_count();
    for (unsigned i = 0; i < count; i++) {
        const kernel_arity_oracle_entry *entry = kernel_arity_oracle_at(i);
        if (entry == NULL || entry->convention != KERNEL_ARITY_ORACLE_DATA) {
            continue;
        }
        data_rows++;
        CHECK_EQ(pop_or_refused(entry->ordinal), REFUSED);
        /* And the stored field must be zero, so a caller that ignores the convention
         * at least gets the least harmful wrong answer rather than a random one. */
        CHECK_EQ(entry->stack_args, 0u);
        CHECK_EQ(entry->register_args, 0u);
    }
    CHECK_EQ(data_rows, 34u);

    /* Named rows, so a wholesale reclassification cannot pass by emptying the loop.
     * All five are object-type pointers or key blobs the guest dereferences. */
    static const unsigned known_data[] = {16u, 102u, 156u, 324u, 357u};
    for (unsigned i = 0; i < sizeof(known_data) / sizeof(known_data[0]); i++) {
        const kernel_arity_oracle_entry *entry = kernel_arity_oracle_lookup(known_data[i]);
        CHECK(entry != NULL);
        if (entry != NULL) {
            CHECK_EQ(entry->convention, KERNEL_ARITY_ORACLE_DATA);
        }
    }
}

/*
 * FASTCALL, and specifically the register/stack split.
 *
 * `register_args` is min(2, total dwords) and `stack_args` is the remainder. Two rows in
 * the whole table have a remainder at all, which makes them the only rows that can tell
 * a correct split from a lucky one.
 */
static void test_fastcall_splits_arguments_between_registers_and_the_stack(void)
{
    /* One register argument, nothing on the stack. Measured independently at this
     * image's own call sites: KfLowerIrql is reached by `mov cl, al` with nothing
     * pushed, which is the evidence ABI_TABLE's fastcall rows rest on. */
    const kernel_arity_oracle_entry *lower = kernel_arity_oracle_lookup(161u);
    CHECK(lower != NULL);
    if (lower != NULL) {
        CHECK_EQ(lower->convention, KERNEL_ARITY_ORACLE_FASTCALL);
        CHECK_EQ(lower->register_args, 1u);
        CHECK_EQ(lower->stack_args, 0u);
    }

    /* Two register arguments, nothing on the stack. IofCompleteRequest(PIRP, CCHAR). */
    const kernel_arity_oracle_entry *complete = kernel_arity_oracle_lookup(87u);
    CHECK(complete != NULL);
    if (complete != NULL) {
        CHECK_EQ(complete->convention, KERNEL_ARITY_ORACLE_FASTCALL);
        CHECK_EQ(complete->register_args, 2u);
        CHECK_EQ(complete->stack_args, 0u);
    }

    /* THE ROW THAT PROVES THE SPLIT. Three dwords: two in registers, ONE on the stack.
     * A fastcall handler that popped all three, or none, would be wrong here and right
     * everywhere else in the table except ordinal 51. */
    const kernel_arity_oracle_entry *cmpxchg = kernel_arity_oracle_lookup(21u);
    CHECK(cmpxchg != NULL);
    if (cmpxchg != NULL) {
        CHECK_EQ(cmpxchg->convention, KERNEL_ARITY_ORACLE_FASTCALL);
        CHECK_EQ(cmpxchg->register_args, 2u);
        CHECK_EQ(cmpxchg->stack_args, 1u);
    }
    const kernel_arity_oracle_entry *other = kernel_arity_oracle_lookup(51u);
    CHECK(other != NULL);
    if (other != NULL) {
        CHECK_EQ(other->register_args, 2u);
        CHECK_EQ(other->stack_args, 1u);
    }

    /* Structural: registers are a fastcall-only thing, and a fastcall row always has at
     * least one. A fastcall row misread as stdcall would show up here as a stdcall row
     * carrying register arguments. */
    unsigned count = kernel_arity_oracle_count();
    unsigned with_stack_remainder = 0;
    for (unsigned i = 0; i < count; i++) {
        const kernel_arity_oracle_entry *entry = kernel_arity_oracle_at(i);
        if (entry == NULL) {
            continue;
        }
        if (entry->convention == KERNEL_ARITY_ORACLE_FASTCALL) {
            CHECK(entry->register_args >= 1u);
            CHECK(entry->register_args <= 2u);
            if (entry->stack_args > 0u) {
                with_stack_remainder++;
            }
        } else {
            CHECK_EQ(entry->register_args, 0u);
        }
    }
    CHECK_EQ(with_stack_remainder, 2u);
}

/*
 * __cdecl pops ZERO, and that is an ANSWER.
 *
 * All five undecorated rows are variadic printf-family exports whose true argument
 * count varies per call site and is unknowable from any decoration. That is exactly why
 * callee cleanup cannot apply to them: the callee has no way to know what to pop, so the
 * caller does it. A thunk for one of these must pop nothing and must not refuse.
 */
static void test_cdecl_rows_pop_zero_and_are_not_refused(void)
{
    static const unsigned cdecl_ordinals[] = {8u, 361u, 362u, 363u, 364u};
    for (unsigned i = 0; i < sizeof(cdecl_ordinals) / sizeof(cdecl_ordinals[0]); i++) {
        const kernel_arity_oracle_entry *entry =
            kernel_arity_oracle_lookup(cdecl_ordinals[i]);
        CHECK(entry != NULL);
        if (entry == NULL) {
            continue;
        }
        CHECK_EQ(entry->convention, KERNEL_ARITY_ORACLE_CDECL);
        CHECK_EQ(entry->register_args, 0u);
        /* Not REFUSED. This is the distinction from a DATA row, which stores the same
         * zero and must be refused -- so a `callee_pop` that keyed off the stored
         * field alone would pass the DATA sweep and fail here, or vice versa. */
        CHECK_EQ(pop_or_refused(entry->ordinal), 0u);
    }
    /* DbgPrint is the one of the five this title actually imports, and it currently has
     * no row in either the hand or the measured table -- so the oracle is the only thing
     * that can say anything about it. */
    CHECK_EQ(pop_or_refused(8u), 0u);
}

/*
 * ORDINAL 49, the one ordinal this project has settled from the target image itself.
 *
 * tools/kernel_ordinals.py flagged 49 as suspected ABI drift: HalReturnToFirmware on
 * builds 3944/4039, possibly HalRequestSoftwareInterrupt on XDK 5849. It was resolved
 * AS HalReturnToFirmware on three independent lines from this image, one of which was
 * that all four call sites push exactly ONE stack argument -- the alternative is
 * __fastcall and pushes nothing.
 *
 * The oracle agrees, and it agrees in a way that settles the alternative too: 49 takes
 * one stack argument as __stdcall, and HalRequestSoftwareInterrupt sits at 48 as
 * __fastcall with one register argument and nothing on the stack. That is a THIRD line
 * of evidence from a source that shares no failure mode with the other two.
 *
 * It is not proof of no drift anywhere. The oracle is nxdk's export list, not XDK
 * 5849's, and agreement here could in principle come from a shared ancestor rather than
 * from both being right. What it does license is narrower and still worth having: the
 * numbering that produced the hand resolution and the numbering in the oracle are the
 * same numbering.
 */
static void test_ordinal_49_takes_one_stack_argument_and_48_is_the_fastcall_one(void)
{
    const kernel_arity_oracle_entry *firmware = kernel_arity_oracle_lookup(49u);
    CHECK(firmware != NULL);
    if (firmware != NULL) {
        CHECK_EQ(firmware->convention, KERNEL_ARITY_ORACLE_STDCALL);
        CHECK_EQ(firmware->stack_args, 1u);
        CHECK_EQ(firmware->register_args, 0u);
    }
    const kernel_arity_oracle_entry *softirq = kernel_arity_oracle_lookup(48u);
    CHECK(softirq != NULL);
    if (softirq != NULL) {
        CHECK_EQ(softirq->convention, KERNEL_ARITY_ORACLE_FASTCALL);
        CHECK_EQ(softirq->stack_args, 0u);
        CHECK_EQ(softirq->register_args, 1u);
    }
}

/*
 * ORDINAL 196, where the oracle confirms a PREDICTION rather than a measurement.
 *
 * kernel_thunk.c's quorum refusal says of NtDeviceIoControlFile: "popping TWELVE dwords
 * inferred from a single call site -- and 12 is more than that export actually takes."
 * That was an argument made without a number to put in its place. The oracle supplies
 * one: TEN. The hand reasoning was right that 12 was an over-count, and the quorum gate
 * that refused it was doing its job.
 *
 * Also 127 and 358, where a single-site measurement invented arguments for exports that
 * take none at all.
 */
static void test_the_oracle_confirms_the_quorum_gates_over_count_suspicions(void)
{
    CHECK_EQ(pop_or_refused(196u), 10u);
    CHECK_EQ(pop_or_refused(127u), 0u); /* KeQueryPerformanceFrequency; measured said 2 */
    CHECK_EQ(pop_or_refused(358u), 0u); /* HalIsResetOrShutdownPending; measured said 1 */
}

/*
 * TWO ORDINALS THE CALL-SITE SCANNER CLASSIFIES AS DATA AND THE ORACLE CALLS FUNCTIONS.
 *
 * `callsites.py` decides "DATA export" from `calls == 0 && reads > 0`, i.e. the guest
 * reads the thunk slot and never calls through it. For ordinals 3 and 74 that reading is
 * TRUE AND THE CONCLUSION IS WRONG: both are functions whose address this title takes
 * without ever calling. 74 is IoInvalidDeviceRequest, whose entire purpose is to be
 * stored into a driver dispatch table as a placeholder, so a binary that only ever reads
 * its slot is behaving exactly as designed.
 *
 * The distinction matters because the two statements are different: "is a variable" and
 * "is never called in this image". The scanner can only establish the second. Nothing
 * depends on it today -- `kernel_thunk_measured_as_data()` only enriches a diagnostic --
 * but a future decision keyed off it would be wrong for these two.
 */
static void test_ordinals_3_and_74_are_functions_the_scanner_calls_data(void)
{
    const kernel_arity_oracle_entry *display = kernel_arity_oracle_lookup(3u);
    CHECK(display != NULL);
    if (display != NULL) {
        CHECK_EQ(display->convention, KERNEL_ARITY_ORACLE_STDCALL);
        CHECK_EQ(display->stack_args, 6u);
    }
    const kernel_arity_oracle_entry *invalid = kernel_arity_oracle_lookup(74u);
    CHECK(invalid != NULL);
    if (invalid != NULL) {
        CHECK_EQ(invalid->convention, KERNEL_ARITY_ORACLE_STDCALL);
        CHECK_EQ(invalid->stack_args, 2u);
    }
}

int main(void)
{
    test_the_table_is_371_rows_spanning_ordinals_1_to_378();
    test_ordinals_are_strictly_ascending();
    test_the_convention_histogram_is_308_24_5_34();
    test_lookup_is_exact_and_never_returns_a_neighbour();

    test_arities_are_dwords_and_match_the_hand_counts_the_scanner_missed();
    test_every_data_export_is_refused_rather_than_given_an_arity();
    test_fastcall_splits_arguments_between_registers_and_the_stack();
    test_cdecl_rows_pop_zero_and_are_not_refused();

    test_ordinal_49_takes_one_stack_argument_and_48_is_the_fastcall_one();
    test_the_oracle_confirms_the_quorum_gates_over_count_suspicions();
    test_ordinals_3_and_74_are_functions_the_scanner_calls_data();

    printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
