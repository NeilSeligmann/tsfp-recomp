/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Suite for the LOGIC in src/host/kernel_thunk.c: how an ordinal's stack-argument count is
 * chosen (stack_args_for), and what the dispatcher does with the answer. The table ROWS are
 * out of scope here (tests/test_arity_oracle.py cross-checks them).
 *
 * THE .c IS INCLUDED, NOT LINKED, so the static resolver and ABI_TABLE are visible, and a
 * SYNTHETIC measured table (fixtures/thunk_arity/kernel_arity.inc) replaces the generated one.
 * That makes the quorum boundary deterministic and lets the suite run in a fresh clone with no
 * lifted tree. A wrong count permanently desyncs esp (T26), so every tier decision is pinned
 * by the SOURCE that answered as well as the number.
 *
 * Mutation set: tools/mutate/sets/kernel_thunk_logic.py.
 */
#include "kernel_thunk.c"

#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>

#include "guest_mem.h"
#include "kernel_clock.h"
#include "kernel_sync.h"

TSFP_RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp, g_fs_base,
    g_seh_ebp;

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

static bool is_hand_row(unsigned ordinal)
{
    for (size_t i = 0; i < ABI_TABLE_COUNT; i++) {
        if (ABI_TABLE[i].ordinal == ordinal) {
            return true;
        }
    }
    return false;
}

static unsigned oracle_of(unsigned ordinal)
{
    unsigned dwords = 0xFFFFu;
    if (!kernel_arity_oracle_callee_pop(ordinal, &dwords)) {
        return 0xFFFFu;
    }
    return dwords;
}

static void resolves(unsigned ordinal, arity_source want_source, unsigned want_args)
{
    unsigned args = 0xEEEEu;
    arity_source source = ARITY_SOURCE_NONE;
    const bool ok = stack_args_for(ordinal, &args, &source);
    if (!ok || source != want_source || args != want_args) {
        printf("ordinal %u: ok=%d source=%d args=%u, want source=%d args=%u\n", ordinal, ok,
               (int)source, args, (int)want_source, want_args);
    }
    CHECK(ok);
    CHECK(source == want_source);
    CHECK(args == want_args);
}

/* The fixture rows are only meaningful while no hand row shadows them. */
static void test_fixture_ordinals_are_not_hand_rows(void)
{
    static const unsigned used[] = {12u, 14u, 300u, 350u, 100u, 60u, 16u, 30u, 367u};
    for (size_t i = 0; i < sizeof(used) / sizeof(used[0]); i++) {
        if (is_hand_row(used[i])) {
            printf("fixture ordinal %u became an ABI_TABLE hand row: pick another\n", used[i]);
        }
        CHECK(!is_hand_row(used[i]));
    }
    CHECK(is_hand_row(219u));
}

/* Every hand row answers from the hand tier with its own stack_args, whatever else knows. */
static void test_every_hand_row_answers_from_the_hand_tier(void)
{
    CHECK(ABI_TABLE_COUNT > 10u);
    for (size_t i = 0; i < ABI_TABLE_COUNT; i++) {
        resolves(ABI_TABLE[i].ordinal, ARITY_SOURCE_HAND, ABI_TABLE[i].stack_args);
    }
    /* The synthetic measured row says 6 with eleven unanimous voters and the oracle says 8,
     * so only the hand tier yields exactly its own row. */
    resolves(219u, ARITY_SOURCE_HAND, 8u);
}

static void test_quorum_and_unanimity_gate(void)
{
    CHECK(MEASURED_ARITY_MIN_SITES == 3u);
    resolves(12u, ARITY_SOURCE_MEASURED, 5u);  /* exactly the quorum is believed */
    resolves(100u, ARITY_SOURCE_MEASURED, 9u); /* above it too */
    resolves(60u, ARITY_SOURCE_MEASURED, 6u);  /* attributed to the measurement, not the oracle */
    CHECK(oracle_of(12u) == 1u && oracle_of(100u) == 1u);
    resolves(14u, ARITY_SOURCE_ORACLE, oracle_of(14u));  /* two voters: refused, oracle wins */
    CHECK(oracle_of(14u) == 1u);
    resolves(300u, ARITY_SOURCE_ORACLE, 3u); /* nine voters, not unanimous: refused */
    resolves(350u, ARITY_SOURCE_ORACLE, 2u); /* quorum, not unanimous: refused */
}

static void test_oracle_tier_and_unanswered(void)
{
    /* No measured row, no hand row: the oracle answers and says so. */
    resolves(10u, ARITY_SOURCE_ORACLE, 3u);
    resolves(378u, ARITY_SOURCE_ORACLE, 2u); /* the last ordinal is still inside the bound */
    unsigned args = 0xEEEEu;
    arity_source source = ARITY_SOURCE_HAND;
    /* DATA export with a one-voter measured row: refused by both tiers. */
    CHECK(!stack_args_for(16u, &args, &source));
    /* DATA export with no row anywhere. */
    CHECK(!stack_args_for(40u, &args, &source));
    /* The hole in the ordinal space. */
    CHECK(!stack_args_for(367u, &args, &source));
}

static void test_measured_as_data(void)
{
    CHECK(kernel_thunk_measured_as_data(16u));
    CHECK(kernel_thunk_measured_as_data(30u));
    CHECK(!kernel_thunk_measured_as_data(17u));
    CHECK(!kernel_thunk_measured_as_data(0u));
}

/* ------------------------------------------------------------------ dispatch */

static uint32_t stack_base;

typedef struct {
    bool stopped;
    host_stop_reason reason;
    unsigned stop_ordinal;
    uint32_t stop_address;
} outcome;

static outcome call_ordinal(unsigned ordinal, uint32_t return_address)
{
    outcome result = {0};
    const uint32_t zeroes[16] = {0};
    (void)kernel_guest_write_bytes(stack_base, zeroes, sizeof(zeroes));
    (void)kernel_guest_write_u32(stack_base, return_address);
    g_esp = stack_base;
    thunk_trace_reset();
    recomp_func_t fn = recomp_lookup_kernel(KERNEL_THUNK_VA(ordinal));
    if (!fn) {
        result.stopped = true;
        return result;
    }
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        fn();
    } else {
        result.stopped = true;
        result.reason = host_run_result()->reason;
        result.stop_ordinal = host_run_result()->ordinal;
        result.stop_address = host_run_result()->guest_address;
    }
    host_run_disarm();
    return result;
}

static const thunk_trace_entry *only_trace_entry(void)
{
    size_t count = 0;
    const thunk_trace_entry *trace = thunk_trace_entries(&count);
    CHECK(count == 1u);
    return count == 1u ? &trace[0] : NULL;
}

static char *stderr_capture_begin(int *saved)
{
    fflush(stderr);
    *saved = dup(fileno(stderr));
    FILE *tmp = tmpfile();
    (void)dup2(fileno(tmp), fileno(stderr));
    return (char *)tmp;
}

static void stderr_capture_end(char *handle, int saved, char *out, size_t cap)
{
    FILE *tmp = (FILE *)handle;
    fflush(stderr);
    (void)dup2(saved, fileno(stderr));
    close(saved);
    rewind(tmp);
    size_t got = fread(out, 1, cap - 1u, tmp);
    out[got] = '\0';
    fclose(tmp);
}

static unsigned count_substr(const char *text, const char *needle)
{
    unsigned n = 0;
    for (const char *p = strstr(text, needle); p; p = strstr(p + 1, needle)) {
        n++;
    }
    return n;
}

static void test_dispatch_pops_return_address_plus_stack_args(void)
{
    kernel_thunk_set_stop_on_missing(false);
    char quiet[4096];
    int saved = 0;
    char *capture = stderr_capture_begin(&saved);
    /* Hand stdcall, zero args: KeGetCurrentIrql. Only the return address is popped. */
    outcome o = call_ordinal(103u, 0x38004Cu);
    CHECK(!o.stopped && g_esp == stack_base + 4u);
    /* Hand stdcall, nonzero args: KeSetEvent (3) pops 4 + 12. */
    o = call_ordinal(145u, 0x38004Cu);
    CHECK(!o.stopped && g_esp == stack_base + 16u);
    /* cdecl, caller cleans up: DbgPrint pops the return address only (T34). */
    o = call_ordinal(8u, 0x38004Cu);
    CHECK(!o.stopped && g_esp == stack_base + 4u);
    /* fastcall: register args are not on the stack. KfLowerIrql pops only the return. */
    o = call_ordinal(161u, 0x38004Cu);
    CHECK(!o.stopped && g_esp == stack_base + 4u);
    /* Measured tier: 5 dwords. */
    o = call_ordinal(12u, 0x38004Cu);
    CHECK(!o.stopped && g_esp == stack_base + 4u + 20u);
    stderr_capture_end(capture, saved, quiet, sizeof(quiet));
    /* Hand and measured answers are never announced as oracle-derived. */
    CHECK(strstr(quiet, "DERIVED") == NULL);
    /* Refused measurement falls to the oracle: ordinal 14 pops 1. */
    o = call_ordinal(14u, 0x38004Cu);
    CHECK(!o.stopped && g_esp == stack_base + 8u);
    kernel_thunk_set_stop_on_missing(true);
}

static void test_oracle_derived_pop_is_announced_once_per_ordinal(void)
{
    char text[4096];
    int saved = 0;
    kernel_thunk_set_stop_on_missing(false);
    char *capture = stderr_capture_begin(&saved);
    (void)call_ordinal(300u, 0x38004Cu);
    (void)call_ordinal(300u, 0x38004Cu);
    (void)call_ordinal(350u, 0x38004Cu);
    (void)call_ordinal(12u, 0x38004Cu); /* measured: silent */
    (void)call_ordinal(103u, 0x38004Cu); /* hand: silent */
    (void)call_ordinal(378u, 0x38004Cu); /* the last ordinal is announced too */
    stderr_capture_end(capture, saved, text, sizeof(text));
    kernel_thunk_set_stop_on_missing(true);
    CHECK(count_substr(text, "kernel: ordinal 300 (RtlMultiByteToUnicodeSize) unwound") == 1u);
    CHECK(count_substr(text, "kernel: ordinal 350 (XcCryptService) unwound") == 1u);
    CHECK(count_substr(text, "kernel: ordinal 378 (MmDbgWriteCheck) unwound") == 1u);
    CHECK(count_substr(text, "kernel: ordinal 12 (ExAcquireReadWriteLockExclusive) unwound") == 0u);
    CHECK(count_substr(text, "kernel: ordinal 103 (KeGetCurrentIrql) unwound") == 0u);
    CHECK(count_substr(text, "unwound with 3 stack dwords DERIVED FROM THE nxdk .def ORACLE") == 1u);
    CHECK(count_substr(text, "unwound with 2 stack dwords") == 2u);
    CHECK(strstr(text, "RtlMultiByteToUnicodeSize") != NULL);
}

static void test_an_unanswerable_ordinal_stops_naming_it(void)
{
    kernel_thunk_set_stop_on_missing(false);
    /* Even with stop-on-missing OFF, an unknown ABI must stop: guessing zero desyncs esp. */
    outcome o = call_ordinal(16u, 0x391234u);
    CHECK(o.stopped);
    CHECK(o.reason == HOST_STOP_KERNEL_ABI_UNKNOWN);
    CHECK(o.stop_ordinal == 16u);
    CHECK(o.stop_address == 0x391234u);
    CHECK(g_esp == stack_base);
    const thunk_trace_entry *entry = only_trace_entry();
    CHECK(entry && entry->ordinal == 16u && entry->return_address == 0x391234u);
    CHECK(entry && !entry->result_known);
    o = call_ordinal(367u, 0x391234u);
    CHECK(o.stopped && o.reason == HOST_STOP_KERNEL_ABI_UNKNOWN && o.stop_ordinal == 367u);
    kernel_thunk_set_stop_on_missing(true);
}

static void test_unimplemented_policy_and_default_return(void)
{
    /* Stub ordinal 12 (oracle/measured arity known): stop-on-missing stops BEFORE unwinding. */
    kernel_thunk_set_stop_on_missing(true);
    outcome o = call_ordinal(12u, 0x38004Cu);
    CHECK(o.stopped && o.reason == HOST_STOP_KERNEL_UNIMPLEMENTED);
    CHECK(o.stop_ordinal == 12u && o.stop_address == 0x38004Cu);
    CHECK(g_esp == stack_base);
    const thunk_trace_entry *entry = only_trace_entry();
    CHECK(entry && !entry->implemented && !entry->result_known);

    /* Continuing: the default return is delivered and the stack is still unwound. */
    kernel_thunk_set_stop_on_missing(false);
    CHECK(kernel_hle_set_default_return(12u, 0xC0000002u));
    g_eax = 0x1111u;
    o = call_ordinal(12u, 0x38004Cu);
    CHECK(!o.stopped);
    CHECK(g_eax == 0xC0000002u);
    CHECK(g_esp == stack_base + 4u + 20u);
    entry = only_trace_entry();
    CHECK(entry && entry->result_known && entry->result == 0xC0000002u && !entry->implemented);
    CHECK(kernel_hle_set_default_return(12u, 0u));
    kernel_thunk_set_stop_on_missing(true);
}

static void test_implemented_handler_result_registers_and_trace(void)
{
    kernel_thunk_set_stop_on_missing(true);
    /* KfRaiseIrql(ecx = new level): the thunk must hand the handler ecx, not edx. */
    g_ecx = 2u;
    g_edx = 7u;
    outcome o = call_ordinal(160u, 0x38004Cu);
    CHECK(!o.stopped && g_esp == stack_base + 4u);
    const thunk_trace_entry *entry = only_trace_entry();
    CHECK(entry && entry->implemented && entry->result_known);
    o = call_ordinal(103u, 0x38004Cu);
    CHECK(!o.stopped && g_eax == 2u);
    entry = only_trace_entry();
    CHECK(entry && entry->result == 2u && entry->return_address == 0x38004Cu);
    CHECK(entry && entry->ordinal == 103u && entry->implemented);
    g_ecx = 0u;
    g_edx = 0u;
    (void)call_ordinal(161u, 0x38004Cu); /* KfLowerIrql(ecx = 0) restores PASSIVE */
    o = call_ordinal(103u, 0x38004Cu);
    CHECK(g_eax == 0u);

    /* 64-bit result: edx carries the high half only when the handler sets it, and eax
     * is overwritten by the result in every case. */
    kernel_clock_reset();
    g_eax = 0xAAAAu;
    g_edx = 0xBBBBu;
    o = call_ordinal(126u, 0x38004Cu);
    CHECK(!o.stopped && g_esp == stack_base + 4u);
    CHECK(g_edx == 0u); /* the handler publishes the (zero) high half of a just-reset clock */
    /* 103 sets no high half: edx must be untouched. */
    g_edx = 0xBBBBu;
    (void)call_ordinal(103u, 0x38004Cu);
    CHECK(g_edx == 0xBBBBu);
}

static void test_lookup_accepts_only_thunk_vas(void)
{
    CHECK(recomp_lookup_kernel(KERNEL_THUNK_VA_BASE - 4u) == NULL);
    CHECK(recomp_lookup_kernel(0x00401000u) == NULL);
    CHECK(recomp_lookup_kernel(KERNEL_THUNK_VA(XBOX_KERNEL_ORDINAL_MAX + 1u)) == NULL);
    CHECK(recomp_lookup_kernel(KERNEL_THUNK_VA(XBOX_KERNEL_ORDINAL_MAX)) != NULL);
    CHECK(recomp_lookup_kernel(KERNEL_THUNK_VA(0u)) != NULL);
    CHECK(kernel_thunk_is_va(KERNEL_THUNK_VA_BASE));
    CHECK(kernel_thunk_is_va(KERNEL_THUNK_VA(XBOX_KERNEL_ORDINAL_MAX)));
    CHECK(!kernel_thunk_is_va(KERNEL_THUNK_VA(XBOX_KERNEL_ORDINAL_MAX + 1u)));
    CHECK(!kernel_thunk_is_va(KERNEL_THUNK_VA_BASE - 1u));
}


/* ------------------------------------------------------------ patch table and window */

static void test_patch_table_rules(void)
{
    guest_region_request request = {
        .bytes = 4096u, .alignment = 4096u, .protect = PAGE_READWRITE, .state = MEM_COMMIT};
    nt_status status;
    const uint32_t table = guest_region_alloc(&request, &status);
    CHECK(table != 0u);
    const uint32_t entries[] = {
        0x80000000u | 156u,                    /* ordinary import: its own slot */
        0x12345678u,                           /* no import flag: already an address, left alone */
        156u,                                  /* no flag but a LEGAL ordinal value: still left alone */
        0x80000000u | 0x10000u | 156u,         /* bits above the ordinal field: out of range, left alone */
        0x80000000u | (XBOX_KERNEL_ORDINAL_MAX + 1u), /* past the last ordinal: left alone */
        0x80000000u | 322u,                    /* XboxHardwareInfo: redirected into the annex */
        0x80000000u | 324u,                    /* XboxKrnlVersion: redirected into the annex */
        0x80000000u | 323u,                    /* XboxHDKey: absent genuine source is NULL */
        0x80000000u | 325u,                    /* XboxSignatureKey, the neighbour: NOT redirected */
        0x80000000u | XBOX_KERNEL_ORDINAL_MAX, /* the last ordinal is patched */
        0u,                                    /* terminator */
        0x80000000u | 50u,                     /* beyond the terminator: untouched */
    };
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        CHECK(kernel_guest_write_u32(table + (uint32_t)(i * 4u), entries[i]));
    }
    size_t skipped = 99u;
    const size_t patched = kernel_thunk_patch_table(table, 12u, &skipped);
    CHECK(patched == 6u);
    CHECK(skipped == 4u);
    const uint32_t want[] = {
        KERNEL_THUNK_VA(156u), 0x12345678u, 156u, entries[3], entries[4],
        KERNEL_THUNK_VA_XBOX_HARDWARE_INFO, KERNEL_THUNK_VA_XBOX_KRNL_VERSION,
        0u, KERNEL_THUNK_VA(325u), KERNEL_THUNK_VA(XBOX_KERNEL_ORDINAL_MAX),
        0u, entries[11],
    };
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        uint32_t got = 0xDEADu;
        CHECK(kernel_guest_read_u32(table + (uint32_t)(i * 4u), &got));
        if (got != want[i]) {
            printf("slot %zu: got %08X want %08X\n", i, got, want[i]);
        }
        CHECK(got == want[i]);
    }
    /* count bounds the walk, and a NULL skipped pointer is allowed. */
    CHECK(kernel_guest_write_u32(table, 0x80000000u | 156u));
    CHECK(kernel_guest_write_u32(table + 4u, 0x80000000u | 157u));
    CHECK(kernel_thunk_patch_table(table, 1u, NULL) == 1u);
    uint32_t untouched = 0u;
    CHECK(kernel_guest_read_u32(table + 4u, &untouched));
    CHECK(untouched == (0x80000000u | 157u));
    /* A table that runs off mapped memory stops instead of faulting. */
    CHECK(kernel_thunk_patch_table(0x00000010u, 4u, &skipped) == 0u);
    CHECK(guest_region_free(table));
}

static void test_window_map_is_idempotent_and_unmap_resets(void)
{
    kernel_thunk_unmap_window();
    CHECK(kernel_thunk_map_window());
    volatile uint32_t *slot = (volatile uint32_t *)(uintptr_t)KERNEL_THUNK_VA(5u);
    CHECK(*slot == 0u);
    *slot = 0xCAFEF00Du;
    CHECK(kernel_thunk_map_window()); /* already mapped: true, and the mapping is kept */
    CHECK(*slot == 0xCAFEF00Du);
    kernel_thunk_unmap_window();
    kernel_thunk_unmap_window(); /* second unmap is harmless */
    CHECK(kernel_thunk_map_window()); /* a genuinely fresh mapping */
    CHECK(*slot == 0u);
    kernel_thunk_unmap_window();
}

int main(void)
{
    guest_region_request request = {
        .bytes = 8192u, .alignment = 4096u, .protect = PAGE_READWRITE, .state = MEM_COMMIT};
    nt_status status;
    const uint32_t allocation = guest_region_alloc(&request, &status);
    CHECK(allocation != 0u);
    stack_base = allocation + 1024u;
    kernel_hle_init();
    CHECK(kernel_sync_register() == 5u);
    CHECK(kernel_clock_register() > 0u);
    kernel_clock_reset();

    test_fixture_ordinals_are_not_hand_rows();
    test_every_hand_row_answers_from_the_hand_tier();
    test_quorum_and_unanimity_gate();
    test_oracle_tier_and_unanswered();
    test_measured_as_data();
    test_dispatch_pops_return_address_plus_stack_args();
    test_oracle_derived_pop_is_announced_once_per_ordinal();
    test_an_unanswerable_ordinal_stops_naming_it();
    test_unimplemented_policy_and_default_return();
    test_implemented_handler_result_registers_and_trace();
    test_lookup_accepts_only_thunk_vas();
    test_patch_table_rules();
    test_window_map_is_idempotent_and_unmap_resets();

    if (failures != 0) {
        printf("test_kernel_thunk_arity: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("test_kernel_thunk_arity: %d checks passed\n", checks);
    return 0;
}
