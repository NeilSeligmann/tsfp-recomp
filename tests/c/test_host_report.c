/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The run report: every block tsfp_host prints after the guest stops, plus the
 * stop-record table and the exit verdict.
 *
 * WHY THIS SUITE EXISTS. These functions lived in `src/host/main.c`, compiled into
 * `tsfp_host` alone -- not a ctest binary, so no suite and no mutation could reach
 * them (the same gap T9 closed for option parsing). The report is the DELIVERABLE
 * of a bring-up run and is read as evidence by operators and later tasks, so the
 * annotations that distinguish honesty from fabrication are pinned here BYTE FOR
 * BYTE: "FABRICATED" on made-up zeros and empties, "MISSING" on an unimplemented
 * boundary, "(a NULL function pointer)" on the one icall target that used to be
 * silently suppressed, and the unconditional CREATED/WRITTEN lines whose absence
 * would make a refused run look like an idle one.
 *
 * DELIBERATELY FREE OF THE THUNK LAYER, THE LIFTED CODE AND ANY THREAD. Every
 * formatter takes plain data and a FILE*; the suite writes into an open_memstream
 * buffer and compares bytes. The name lookups arrive through host_report_names, so
 * the fakes below answer exactly what each case needs.
 */

#include "host_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define CHECK_TEXT(actual, expected)                                                    \
    do {                                                                                \
        checks++;                                                                       \
        const char *a_ = (actual);                                                      \
        const char *e_ = (expected);                                                    \
        if (a_ == NULL || strcmp(a_, e_) != 0) {                                        \
            printf("FAIL %s:%d\n--- expected ---\n%s\n--- actual ---\n%s\n---\n",       \
                   __FILE__, __LINE__, e_, a_ ? a_ : "(null)");                         \
            failures++;                                                                 \
        }                                                                               \
    } while (0)

/* --- capture ---------------------------------------------------------------- */

static char *capture_buf;
static size_t capture_len;

static FILE *capture(void)
{
    free(capture_buf);
    capture_buf = NULL;
    capture_len = 0u;
    FILE *out = open_memstream(&capture_buf, &capture_len);
    if (!out) {
        printf("FAIL %s:%d  open_memstream\n", __FILE__, __LINE__);
        exit(1);
    }
    return out;
}

static const char *captured(FILE *out)
{
    fclose(out);
    return capture_buf ? capture_buf : "";
}

/* --- fake lookups ------------------------------------------------------------ */

static const char *fake_reason_str(host_stop_reason reason)
{
    switch (reason) {
    case HOST_STOP_KERNEL_UNIMPLEMENTED:
        return "unimplemented kernel ordinal";
    case HOST_STOP_FAULT:
        return "host fault";
    case HOST_STOP_ICALL_UNRESOLVED:
        return "unresolved indirect call";
    default:
        return "some stop";
    }
}

static const char *fake_ordinal_name(unsigned ordinal)
{
    return ordinal == 255u ? "PsTerminateSystemThread" : NULL;
}

static const char *fake_xdk_name(uint32_t address)
{
    return address == 0x003D4D70u ? "D3DResource_Register" : NULL;
}

static bool fake_is_thunk_va_yes(uint32_t va)
{
    (void)va;
    return true;
}

static bool fake_is_thunk_va_no(uint32_t va)
{
    (void)va;
    return false;
}

static bool fake_measured_as_data_yes(unsigned ordinal)
{
    (void)ordinal;
    return true;
}

static bool fake_measured_as_data_no(unsigned ordinal)
{
    (void)ordinal;
    return false;
}

static const host_report_names names_plain = {
    .stop_reason_str = fake_reason_str,
    .ordinal_name = fake_ordinal_name,
    .xdk_name_of = fake_xdk_name,
    .thunk_is_va = fake_is_thunk_va_no,
    .measured_as_data = fake_measured_as_data_no,
};

/* --- KPCR.Irql publishing ----------------------------------------------------- */

static void test_irql_block_quiet_run(void)
{
    FILE *out = capture();
    host_report_irql_publishing(out, 3ul, 0u, 0ul);
    CHECK_TEXT(captured(out),
               "\n--- KPCR.Irql publishing ---\n"
               "published      3 time(s), peak level 0 (read back and confirmed)\n");
}

static void test_irql_block_reports_dispatch_peak_and_failures(void)
{
    FILE *out = capture();
    host_report_irql_publishing(out, 41ul, 2u, 5ul);
    CHECK_TEXT(captured(out),
               "\n--- KPCR.Irql publishing ---\n"
               "published      41 time(s), peak level 2 (read back and confirmed)\n"
               "               peak >= DISPATCH_LEVEL (2), so the guest's 6 "
               "`fs:[0x24]` sites\n               had a non-PASSIVE value to read\n"
               "FAILED         5 publish(es) had no KPCR to write, so the guest's "
               "copy went stale\n");
}

/* --- non-volatile settings ----------------------------------------------------- */

static void test_settings_block_prints_nothing_when_nothing_was_asked(void)
{
    FILE *out = capture();
    host_report_non_volatile_settings(out, NULL, 0u, 0u, 0u, 0u);
    CHECK_TEXT(captured(out), "");
}

static void test_settings_block_announces_the_fabrication(void)
{
    const uint32_t indices[] = {0x8u, 0x104u};
    FILE *out = capture();
    host_report_non_volatile_settings(out, indices, 2u, 1u, 0u, 7u);
    /* The FABRICATED line is unconditional, zero included: it is the one line that
     * says how far our zeros reach into the title's configuration. */
    CHECK_TEXT(captured(out),
               "\n--- non-volatile settings the title asked for ---\n"
               "distinct index(es) 2: 0x8 0x104\n"
               "served from store   1\n"
               "refused             0\n"
               "FABRICATED zeros    7 -- the title's configuration from the first of "
               "these on is\n                    ours, not a console's\n");
}

static void test_settings_block_elides_beyond_the_report_bound(void)
{
    uint32_t indices[HOST_REPORT_CONFIG_INDEX_MAX];
    for (unsigned i = 0u; i < HOST_REPORT_CONFIG_INDEX_MAX; i++) {
        indices[i] = i;
    }
    FILE *out = capture();
    host_report_non_volatile_settings(out, indices, 40u, 0u, 0u, 40u);
    const char *text = captured(out);
    CHECK(strstr(text, " ... (8 more)\n") != NULL);
    CHECK(strstr(text, "distinct index(es) 40:") != NULL);
}

/* --- file attempts -------------------------------------------------------------- */

static kernel_file_attempt fake_attempts[2];

static const kernel_file_attempt *fake_attempt_at(unsigned i)
{
    return i < 2u ? &fake_attempts[i] : NULL;
}

static void test_file_block_prints_nothing_when_no_open_was_tried(void)
{
    const host_report_file_counts counts = {0};
    FILE *out = capture();
    host_report_file_attempts(out, &counts, fake_attempt_at);
    CHECK_TEXT(captured(out), "");
}

static void test_file_block_separates_honest_refusals_from_fabrications(void)
{
    memset(fake_attempts, 0, sizeof(fake_attempts));
    snprintf(fake_attempts[0].path, sizeof(fake_attempts[0].path),
             "\\Device\\Harddisk0\\partition1\\");
    fake_attempts[0].desired_access = 0x100001u;
    fake_attempts[0].share_access = 0x3u;
    fake_attempts[0].open_options = 0x21u;
    fake_attempts[0].attempts = 1u;
    fake_attempts[0].opened = true;
    snprintf(fake_attempts[1].path, sizeof(fake_attempts[1].path),
             "\\Device\\CdRom0\\default.xbe");
    fake_attempts[1].desired_access = 0x80000000u;
    fake_attempts[1].share_access = 0x1u;
    fake_attempts[1].open_options = 0x60u;
    fake_attempts[1].attempts = 3u;
    fake_attempts[1].opened = false;

    const host_report_file_counts counts = {
        .total = 2u,
        .refused = 3u,
        .relative_refused = 1u,
        .fabricated = 2u,
        .escape_refused = 1u,
        .disc_opened = 4u,
        .disc_bytes_read = 2048ull,
        .host_opened = 5u,
        .host_bytes_read = 512ull,
        .created = 2u,
        .bytes_written = 76ull,
        .write_count = 6u,
        .write_refused = 1u,
    };
    FILE *out = capture();
    host_report_file_attempts(out, &counts, fake_attempt_at);
    CHECK_TEXT(
        captured(out),
        "\n--- names the title tried to open ---\n"
        "  ok  1x  access 0x00100001 share 0x3 options 0x21  "
        "\"\\Device\\Harddisk0\\partition1\\\"\n"
        "  ERR 3x  access 0x80000000 share 0x1 options 0x60  "
        "\"\\Device\\CdRom0\\default.xbe\"\n"
        "refused             3 (no volume mounted behind the name)\n"
        "refused, relative   1 (a root-directory handle whose meaning is not "
        "derived)\n"
        "FABRICATED empties  2 -- everything the title reads through those "
        "handles is\n                    ours, not a disc's\n"
        "refused, escaping   1 (a \".\" or \"..\" component, or a host symbolic "
        "link --\n                    the guest tried to name something outside "
        "its own volume)\n"
        "from a real disc    4 open(s), 2048 byte(s) served\n"
        "from a real hdd     5 open(s), 512 byte(s) served\n"
        "CREATED for real    2 object(s) on a writable volume -- these outlive the "
        "run,\n                    unlike every fabricated empty above\n"
        "WRITTEN to hdd      76 byte(s) in 6 NtWriteFile call(s), 1 refused\n");
}

static void test_file_block_created_and_written_appear_even_at_zero(void)
{
    memset(fake_attempts, 0, sizeof(fake_attempts));
    snprintf(fake_attempts[0].path, sizeof(fake_attempts[0].path), "\\x");
    fake_attempts[0].attempts = 1u;
    const host_report_file_counts counts = {.total = 1u};
    FILE *out = capture();
    host_report_file_attempts(out, &counts, fake_attempt_at);
    const char *text = captured(out);
    /* A run where every write was REFUSED must not look like one that wrote
     * nothing, and a --hdd run that created nothing has not exercised --hdd. */
    CHECK(strstr(text, "CREATED for real    0 object(s)") != NULL);
    CHECK(strstr(text, "WRITTEN to hdd      0 byte(s) in 0 NtWriteFile call(s), 0 refused")
          != NULL);
    /* The two fabrication lines are CONDITIONAL: at zero there is nothing to
     * announce, and announcing it anyway would train readers to skip the word. */
    CHECK(strstr(text, "FABRICATED") == NULL);
    CHECK(strstr(text, "escaping") == NULL);
}

/* --- symbolic links -------------------------------------------------------------- */

static const char *fake_symlink_target(const char *name)
{
    return strcmp(name, "\\??\\D:") == 0 ? "\\Device\\CdRom0" : NULL;
}

static void test_symlink_block_prints_nothing_when_the_title_created_none(void)
{
    FILE *out = capture();
    host_report_symbolic_links(out, 0u, fake_symlink_target);
    CHECK_TEXT(captured(out), "");
}

static void test_symlink_block_resolves_the_probed_aliases(void)
{
    FILE *out = capture();
    host_report_symbolic_links(out, 1u, fake_symlink_target);
    CHECK_TEXT(captured(out),
               "\n--- symbolic links the title created ---\n"
               "  \\??\\D:     -> \\Device\\CdRom0\n");
}

static void test_symlink_block_counts_links_outside_the_probe_set(void)
{
    FILE *out = capture();
    host_report_symbolic_links(out, 3u, fake_symlink_target);
    CHECK_TEXT(captured(out),
               "\n--- symbolic links the title created ---\n"
               "  \\??\\D:     -> \\Device\\CdRom0\n"
               "  ... and 2 more not in the probed set\n");
}

/* --- thread stops ------------------------------------------------------------------ */

static void test_thread_stop_block_names_the_ordinal(void)
{
    host_thread_stop_record record = {
        .handle = 0xE10000u,
        .entry_va = 0x0037FE1Du,
        .esp = 0x40BFF010u,
        .eax = 0x0u,
        .stop = {.reason = HOST_STOP_KERNEL_UNIMPLEMENTED,
                 .ordinal = 255u,
                 .detail = "stopping at the first missing ordinal"},
        .valid = true,
    };
    FILE *out = capture();
    host_report_thread_stop(out, &record, &names_plain);
    CHECK_TEXT(captured(out),
               "\nguest thread 0xe10000 (entered 0x0037FE1D) stopped: "
               "unimplemented kernel ordinal\n"
               "  ordinal        255 (PsTerminateSystemThread)\n"
               "  detail         stopping at the first missing ordinal\n"
               "  guest esp      0x40BFF010\n"
               "  guest eax      0x00000000\n");
}

static void test_thread_stop_fault_notes_a_dereferenced_thunk_slot(void)
{
    host_thread_stop_record record = {
        .handle = 0x4u,
        .entry_va = 0x00380000u,
        .esp = 0x1000u,
        .eax = 0x2u,
        .stop = {.reason = HOST_STOP_FAULT,
                 .fault_address = 0xFE000270u,
                 .signal_number = 11,
                 .detail = ""},
        .valid = true,
    };
    host_report_names names = names_plain;
    names.thunk_is_va = fake_is_thunk_va_yes;
    FILE *out = capture();
    host_report_thread_stop(out, &record, &names);
    CHECK_TEXT(captured(out),
               "\nguest thread 0x4 (entered 0x00380000) stopped: host fault\n"
               "  signal         11\n"
               "  fault address  0x00000000FE000270\n"
               "  NOTE: that is a synthetic kernel thunk VA, so the guest\n"
               "        DEREFERENCED a thunk slot instead of calling it.\n"
               "  guest esp      0x00001000\n"
               "  guest eax      0x00000002\n");
}

/* --- the calling thread's stop ------------------------------------------------------- */

static void test_stop_reports_a_null_icall_target_instead_of_suppressing_it(void)
{
    const host_stop stop = {.reason = HOST_STOP_ICALL_UNRESOLVED,
                            .guest_address = 0u,
                            .detail = ""};
    const uint32_t ring[4] = {0x11u, 0x22u, 0x33u, 0x44u};
    FILE *out = capture();
    host_report_stop(out, &stop, 0x100u, 0x0u, ring, 2u, 4u, &names_plain);
    /* `guest_address == 0` is the single most common garbage target, and the old
     * `!= 0` guard hid exactly that case. The ring starts at the write cursor, so
     * the OLDEST entry prints first and the most recent last. */
    CHECK_TEXT(captured(out),
               "\nstopped: unresolved indirect call\n"
               "  icall target   0x00000000  (a NULL function pointer)\n"
               "  guest esp      0x00000100\n"
               "  guest eax      0x00000000\n"
               "  recent indirect targets (most recent last):\n"
               "    00000033 00000044 00000011 00000022 \n");
}

static void test_stop_notes_an_ordinal_measured_as_data(void)
{
    const host_stop stop = {.reason = HOST_STOP_KERNEL_UNIMPLEMENTED,
                            .ordinal = 1u,
                            .guest_address = 0u,
                            .detail = "d"};
    const uint32_t ring[2] = {0u, 0u};
    host_report_names names = names_plain;
    names.measured_as_data = fake_measured_as_data_yes;
    FILE *out = capture();
    host_report_stop(out, &stop, 0x0u, 0x0u, ring, 0u, 2u, &names);
    const char *text = captured(out);
    CHECK(strstr(text, "  ordinal        1 (<unknown>)\n") != NULL);
    CHECK(strstr(text, "is a read, not a call, yet the guest has just called it") != NULL);
    /* guest_address 0 on a non-icall stop stays suppressed: 0 means "none" there. */
    CHECK(strstr(text, "guest address") == NULL);
}

/* --- the faulting function and its call chain (T505) ----------------------------------- */

static bool fake_host_symbol(uintptr_t address, char *out, size_t size)
{
    const char *name = address == 0x5010u   ? "sub_00382000+0x10"
                       : address == 0x6020u ? "sub_00383DF3+0x20"
                       : address == 0x7000u ? "libc.so.6!memcpy+0x4"
                                            : NULL;
    if (name != NULL) {
        (void)snprintf(out, size, "%s", name);
    }
    return name != NULL;
}

static bool fake_guest_function(uint32_t va, char *out, size_t size)
{
    if (va == 0x3820BAu) {
        (void)snprintf(out, size, "sub_00382000+0xBA");
        return true;
    }
    return false;
}

static host_stop fault_stop(void)
{
    host_stop stop = {.reason = HOST_STOP_FAULT,
                      .signal_number = 11,
                      .fault_address = 0u,
                      .detail = "faulted inside lifted code",
                      .fault_rip = 0x5010u,
                      .fault_stack_top = 0x7000u,
                      .fault_guest_ebp = 0x12340u,
                      .fault_host_frame_count = 2u,
                      .fault_guest_frame_count = 2u};
    stop.fault_host_frames[0] = 0x6020u;
    stop.fault_host_frames[1] = 0x9999u;
    stop.fault_guest_frames[0] = 0x3820BAu;
    stop.fault_guest_frames[1] = 0x1u;
    return stop;
}

static void test_fault_frames_name_the_lifted_functions(void)
{
    host_report_names names = names_plain;
    names.host_symbol = fake_host_symbol;
    names.guest_function = fake_guest_function;
    const host_stop stop = fault_stop();
    FILE *out = capture();
    host_report_fault_frames(out, &stop, &names);
    /* RIP is inside a lifted function, so the stack top word is NOT printed as a caller. */
    CHECK_TEXT(captured(out),
               "  fault rip      0x0000000000005010  sub_00382000+0x10\n"
               "  host frames    (return addresses up the rbp chain, innermost first)\n"
               "    #0  0x0000000000006020  sub_00383DF3+0x20\n"
               "    #1  0x0000000000009999\n"
               "  guest ebp      0x00012340\n"
               "  guest frames   (return addresses up the ebp chain, innermost first)\n"
               "    #0  0x003820BA  sub_00382000+0xBA\n"
               "    #1  0x00000001\n");
}

static void test_fault_frames_offer_the_stack_top_when_rip_is_outside_lifted_code(void)
{
    host_report_names names = names_plain;
    names.host_symbol = fake_host_symbol;
    host_stop stop = fault_stop();
    stop.fault_rip = 0x7000u;
    stop.fault_stack_top = 0x6020u;
    stop.fault_host_frame_count = 0u;
    stop.fault_guest_ebp = 0u;
    stop.fault_guest_frame_count = 0u;
    FILE *out = capture();
    host_report_fault_frames(out, &stop, &names);
    CHECK_TEXT(captured(out),
               "  fault rip      0x0000000000007000  libc.so.6!memcpy+0x4\n"
               "  stack top      0x0000000000006020  sub_00383DF3+0x20  (caller of a frameless "
               "leaf when rip is\n"
               "                 outside lifted code)\n");
}

static void test_fault_frames_print_raw_addresses_without_symbols(void)
{
    const host_stop stop = fault_stop();
    FILE *out = capture();
    host_report_fault_frames(out, &stop, &names_plain);
    CHECK_TEXT(captured(out),
               "  fault rip      0x0000000000005010\n"
               "  host frames    (return addresses up the rbp chain, innermost first)\n"
               "    #0  0x0000000000006020\n"
               "    #1  0x0000000000009999\n"
               "  guest ebp      0x00012340\n"
               "  guest frames   (return addresses up the ebp chain, innermost first)\n"
               "    #0  0x003820BA\n"
               "    #1  0x00000001\n");
}

static void test_fault_frames_are_only_for_faults(void)
{
    host_stop stop = fault_stop();
    stop.reason = HOST_STOP_KERNEL_UNIMPLEMENTED;
    FILE *out = capture();
    host_report_fault_frames(out, &stop, &names_plain);
    CHECK_TEXT(captured(out), "");
}

static void test_stop_and_thread_stop_place_the_frames_between_address_and_detail(void)
{
    host_report_names names = names_plain;
    names.host_symbol = fake_host_symbol;
    names.guest_function = fake_guest_function;
    const host_stop stop = fault_stop();
    const uint32_t ring[1] = {0u};
    FILE *out = capture();
    host_report_stop(out, &stop, 0x100u, 0x0u, ring, 0u, 1u, &names);
    const char *text = captured(out);
    const char *address = strstr(text, "  fault address  0x0000000000000000\n");
    const char *rip = strstr(text, "  fault rip      0x0000000000005010  sub_00382000+0x10\n");
    const char *detail = strstr(text, "  detail         faulted inside lifted code\n");
    CHECK(address != NULL && rip != NULL && detail != NULL);
    CHECK(address != NULL && rip != NULL && address < rip);
    CHECK(rip != NULL && detail != NULL && rip < detail);

    const host_thread_stop_record record = {
        .handle = 4u, .entry_va = 0x380000u, .esp = 0x1000u, .eax = 2u, .stop = stop};
    out = capture();
    host_report_thread_stop(out, &record, &names);
    text = captured(out);
    CHECK(strstr(text, "    #0  0x0000000000006020  sub_00383DF3+0x20\n") != NULL);
    CHECK(strstr(text, "    #0  0x003820BA  sub_00382000+0xBA\n") != NULL);
}

/* --- the trace ------------------------------------------------------------------------- */

static void test_trace_reports_an_empty_run_as_exactly_that(void)
{
    const host_report_trace_totals totals = {0};
    FILE *out = capture();
    host_report_trace(out, NULL, 0u, &totals, 64u, &names_plain);
    CHECK_TEXT(captured(out),
               "\nHLE calls reached, in call order (0 recorded, 0 total: 0 kernel "
               "ordinal, 0 XDK address, 0 monitor notify):\n"
               "  (none -- the guest stopped before its first HLE call)\n");
}

static void test_trace_labels_each_boundary_and_says_missing_out_loud(void)
{
    const thunk_trace_entry entries[3] = {
        {.thread = 1u,
         .kind = THUNK_KIND_ORDINAL,
         .ordinal = 255u,
         .return_address = 0x0037CA52u,
         .result = 0u,
         .result_known = true,
         .implemented = true},
        {.thread = 2u,
         .kind = THUNK_KIND_XDK,
         .address = 0x003D4D70u,
         .return_address = 0x00380010u,
         .result_known = false,
         .implemented = false},
        {.thread = 2u,
         .kind = THUNK_KIND_MONITOR,
         .return_address = 0x00380020u,
         .result = 0x1u,
         .result_known = true,
         .implemented = true},
    };
    const host_report_trace_totals totals = {
        .total = 3ull, .total_ordinal = 1ull, .total_xdk = 1ull, .total_monitor = 1ull};
    FILE *out = capture();
    host_report_trace(out, entries, 3u, &totals, 64u, &names_plain);
    CHECK_TEXT(
        captured(out),
        "\nHLE calls reached, in call order (3 recorded, 3 total: 1 kernel "
        "ordinal, 1 XDK address, 1 monitor notify):\n"
        "    1  t1  ordinal 255  PsTerminateSystemThread            implemented   "
        "eax=0x00000000  from 0x0037CA52\n"
        "    2  t2  xdk 0x003D4D70  D3DResource_Register               MISSING       "
        "eax=<no return>  from 0x00380010\n"
        "    3  t2  monitor      <Prcb debug-monitor notify>        no-op         "
        "eax=0x00000001  from 0x00380020\n");
}

static void test_trace_elides_past_the_limit_and_says_how_to_see_the_rest(void)
{
    thunk_trace_entry entries[3];
    memset(entries, 0, sizeof(entries));
    for (unsigned i = 0u; i < 3u; i++) {
        entries[i].thread = 1u;
        entries[i].kind = THUNK_KIND_ORDINAL;
        entries[i].ordinal = 1u;
        entries[i].implemented = true;
        entries[i].result_known = true;
    }
    const host_report_trace_totals totals = {.total = 3ull, .total_ordinal = 3ull};
    FILE *out = capture();
    host_report_trace(out, entries, 3u, &totals, 1u, &names_plain);
    const char *text = captured(out);
    CHECK(strstr(text, "  ... 2 more; pass --trace N to see them\n") != NULL);
    CHECK(strstr(text, "ordinal   1  <unknown>") != NULL);
}

/* --- stop records and the verdict --------------------------------------------------------- */

static void test_publish_fills_slots_in_order_and_drops_past_capacity(void)
{
    host_thread_stop_record table[2];
    memset(table, 0, sizeof(table));
    const host_stop first = {.reason = HOST_STOP_THREAD_EXITED, .detail = "a"};
    const host_stop second = {.reason = HOST_STOP_FAULT, .detail = "b"};
    const host_stop orderly = {.reason = HOST_STOP_THREAD_EXITED, .detail = "c"};

    CHECK(host_thread_stop_publish(table, 2u, 0x10u, 0x100u, 0x1u, 0x2u, &first, NULL, 0u));
    CHECK(host_thread_stop_publish(table, 2u, 0x20u, 0x200u, 0x3u, 0x4u, &second, NULL, 0u));
    /* An ORDERLY exit into a full table is dropped and reported as dropped (T1492). */
    CHECK(!host_thread_stop_publish(table, 2u, 0x30u, 0x300u, 0x5u, 0x6u, &orderly, NULL, 0u));

    CHECK(table[0].valid && table[0].handle == 0x10u &&
          table[0].stop.reason == HOST_STOP_THREAD_EXITED);
    CHECK(table[0].entry_va == 0x100u && table[0].esp == 0x1u && table[0].eax == 0x2u);
    CHECK(table[1].valid && table[1].handle == 0x20u &&
          table[1].stop.reason == HOST_STOP_FAULT);
}

static void test_publish_a_real_stop_displaces_the_oldest_orderly_exit(void)
{
    /* T1492 regression: the level 2 run had 15 threads and 8 slots, 13 orderly exits filled the
     * table and the main game thread's FINAL stop was silently dropped. */
    host_thread_stop_record table[3];
    memset(table, 0, sizeof(table));
    const host_stop orderly = {.reason = HOST_STOP_THREAD_EXITED, .detail = "exit"};
    const host_stop shutdown = {.reason = HOST_STOP_HOST_SHUTDOWN, .detail = "window closed"};
    const host_stop fault = {.reason = HOST_STOP_FAULT, .detail = "fault"};
    for (uint32_t i = 1u; i <= 3u; i++) {
        CHECK(host_thread_stop_publish(table, 3u, i, 0u, 0u, 0u, &orderly, NULL, 0u));
    }
    thunk_trace_entry tail[2];
    memset(tail, 0, sizeof(tail));
    tail[0].ordinal = 7u;
    tail[1].ordinal = 8u;
    CHECK(host_thread_stop_publish(table, 3u, 0x99u, 0x1u, 0x2u, 0x3u, &shutdown, tail, 2u));
    CHECK(table[0].handle == 0x99u && table[0].stop.reason == HOST_STOP_HOST_SHUTDOWN);
    CHECK(strcmp(table[0].stop.detail, "window closed") == 0);
    CHECK(table[0].tail_count == 2u && table[0].tail[1].ordinal == 8u);
    CHECK(table[1].handle == 2u && table[2].handle == 3u);
    /* Orderly exits are never evicted by each other, and a second real stop takes the next one. */
    CHECK(host_thread_stop_publish(table, 3u, 0x98u, 0u, 0u, 0u, &fault, NULL, 0u));
    CHECK(table[1].handle == 0x98u && table[2].handle == 3u);
    CHECK(host_thread_stop_publish(table, 3u, 0x97u, 0u, 0u, 0u, &fault, NULL, 0u));
    /* A table of only real stops is full: the sixth is dropped, never overwriting a real stop. */
    CHECK(!host_thread_stop_publish(table, 3u, 0x96u, 0u, 0u, 0u, &fault, NULL, 0u));
    CHECK(table[0].handle == 0x99u && table[1].handle == 0x98u && table[2].handle == 0x97u);
}

static void test_verdict_a_real_stop_outranks_earlier_orderly_exits(void)
{
    host_thread_stop_record table[3];
    memset(table, 0, sizeof(table));
    table[0].valid = true;
    table[0].stop.reason = HOST_STOP_THREAD_EXITED;
    table[1].valid = true;
    table[1].stop.reason = HOST_STOP_FAULT;
    CHECK(host_run_verdict(HOST_STOP_RETURNED, table, 3u, 0u) == HOST_STOP_FAULT);
}

static void test_thread_stop_report_prints_the_tail_and_the_run_end_evidence(void)
{
    host_thread_stop_record record;
    memset(&record, 0, sizeof(record));
    record.valid = true;
    record.handle = 0xe10000u;
    record.entry_va = 0x37FE1Du;
    record.stop.reason = HOST_STOP_HOST_SHUTDOWN;
    record.stop.detail = "interactive window closed by user";
    record.tail_count = 1u;
    record.tail[0].thread = 2u;
    record.tail[0].kind = THUNK_KIND_ORDINAL;
    record.tail[0].ordinal = 190u;
    record.tail[0].result = 0xC0000034u;
    record.tail[0].result_known = true;
    record.tail[0].implemented = true;
    FILE *out = capture();
    host_report_thread_stop(out, &record, &names_plain);
    const char *text = captured(out);
    CHECK(strstr(text, "interactive window closed by user") != NULL);
    CHECK(strstr(text, "last 1 HLE call(s), oldest first (T1492)") != NULL);
    CHECK(strstr(text, "ordinal 190") != NULL && strstr(text, "eax=0xC0000034") != NULL);

    out = capture();
    host_report_run_end_evidence(out, record.tail, 1u, 5u, 13u, 2u, &names_plain);
    text = captured(out);
    CHECK(strstr(text, "orderly): 13\n") != NULL);
    CHECK(strstr(text, "did NOT fit the per-thread table and are not itemised above: 2\n") != NULL);
    CHECK(strstr(text, "whole run: 5\n") != NULL);
    CHECK(strstr(text, "the last 1 of them") != NULL && strstr(text, "eax=0xC0000034") != NULL);

    out = capture();
    host_report_run_end_evidence(out, NULL, 0u, 0u, 0u, 0u, &names_plain);
    text = captured(out);
    CHECK(strstr(text, "did NOT fit") == NULL);
    CHECK(strstr(text, "whole run: 0\n") != NULL);
}

static void test_main_entry_return_is_annotated_as_expected(void)
{
    host_stop stop = {.reason = HOST_STOP_RETURNED, .detail = "entry point returned"};
    FILE *out = capture();
    host_report_stop(out, &stop, 0u, 1u, (const uint32_t[16]){0}, 0u, 16u, &names_plain);
    CHECK(strstr(captured(out), "EXPECTED, not the end of the title") != NULL);
    stop.detail = "guest thread entry routine returned";
    out = capture();
    host_report_stop(out, &stop, 0u, 1u, (const uint32_t[16]){0}, 0u, 16u, &names_plain);
    CHECK(strstr(captured(out), "EXPECTED, not the end") == NULL);
}

static void test_verdict_prefers_the_first_guest_thread_over_the_main_thread(void)
{
    host_thread_stop_record table[3];
    memset(table, 0, sizeof(table));
    CHECK(host_run_verdict(HOST_STOP_RETURNED, table, 3u, 0u) == HOST_STOP_RETURNED);

    table[1].valid = true;
    table[1].stop.reason = HOST_STOP_KERNEL_UNIMPLEMENTED;
    table[2].valid = true;
    table[2].stop.reason = HOST_STOP_FAULT;
    /* The FIRST valid record decides, not the last. */
    CHECK(host_run_verdict(HOST_STOP_RETURNED, table, 3u, 0u) ==
          HOST_STOP_KERNEL_UNIMPLEMENTED);
}

static void test_verdict_a_hung_thread_outranks_everything(void)
{
    host_thread_stop_record table[1];
    memset(table, 0, sizeof(table));
    table[0].valid = true;
    table[0].stop.reason = HOST_STOP_THREAD_EXITED;
    CHECK(host_run_verdict(HOST_STOP_RETURNED, table, 1u, 1u) ==
          HOST_STOP_THREAD_TIMEOUT);
}

static void test_exit_status_accepts_exactly_the_three_expected_ends(void)
{
    /* The three EXPECTED ends of a bring-up run exit 0... */
    CHECK(host_run_exit_status(true, true, true, true, HOST_STOP_KERNEL_UNIMPLEMENTED) == 0);
    CHECK(host_run_exit_status(true, true, true, true, HOST_STOP_THREAD_EXITED) == 0);
    CHECK(host_run_exit_status(true, true, true, true, HOST_STOP_HOST_SHUTDOWN) == 0);
    CHECK(host_run_exit_status(true, true, true, true, HOST_STOP_XDK_UNIMPLEMENTED) == 0);
    /* ...and nothing else does, however plausible. */
    CHECK(host_run_exit_status(true, true, true, true, HOST_STOP_RETURNED) == 1);
    CHECK(host_run_exit_status(true, true, true, true, HOST_STOP_FAULT) == 1);
    CHECK(host_run_exit_status(true, true, true, true, HOST_STOP_THREAD_TIMEOUT) == 1);
    CHECK(host_run_exit_status(true, true, true, true, HOST_STOP_FIRMWARE_RETURN) == 1);
    /* A refused cleanup poisons even an expected verdict: preserved allocations are
     * a real problem the exit status must not paper over. */
    CHECK(host_run_exit_status(true, false, true, true, HOST_STOP_THREAD_EXITED) == 1);
    CHECK(host_run_exit_status(true, true, false, true, HOST_STOP_THREAD_EXITED) == 1);
    CHECK(host_run_exit_status(true, true, true, false, HOST_STOP_THREAD_EXITED) == 1);
    CHECK(host_run_exit_status(false, true, true, true, HOST_STOP_THREAD_EXITED) == 1);
}

static void test_published_stop_detail_survives_the_source_buffer(void)
{
    /* Stop detail may live in worker TLS, which disappears after the join. The
     * record must keep its own copy, so overwriting the source after publish must
     * not change what the record reads back. */
    host_thread_stop_record table[2] = {0};
    char source[64];
    (void)snprintf(source, sizeof(source), "the original detail");
    host_stop stop = {0};
    stop.reason = HOST_STOP_FAULT;
    stop.detail = source;
    host_thread_stop_publish(table, 2, 0x100u, 0x2000u, 0x30u, 0x40u, &stop, NULL, 0u);
    (void)snprintf(source, sizeof(source), "CLOBBERED after the join");
    CHECK(table[0].valid);
    CHECK(table[0].stop.detail != NULL);
    CHECK(strcmp(table[0].stop.detail, "the original detail") == 0);
    CHECK(table[0].stop.detail == table[0].detail);
}

int main(void)
{
    test_irql_block_quiet_run();
    test_irql_block_reports_dispatch_peak_and_failures();
    test_settings_block_prints_nothing_when_nothing_was_asked();
    test_settings_block_announces_the_fabrication();
    test_settings_block_elides_beyond_the_report_bound();
    test_file_block_prints_nothing_when_no_open_was_tried();
    test_file_block_separates_honest_refusals_from_fabrications();
    test_file_block_created_and_written_appear_even_at_zero();
    test_symlink_block_prints_nothing_when_the_title_created_none();
    test_symlink_block_resolves_the_probed_aliases();
    test_symlink_block_counts_links_outside_the_probe_set();
    test_thread_stop_block_names_the_ordinal();
    test_thread_stop_fault_notes_a_dereferenced_thunk_slot();
    test_stop_reports_a_null_icall_target_instead_of_suppressing_it();
    test_stop_notes_an_ordinal_measured_as_data();
    test_fault_frames_name_the_lifted_functions();
    test_fault_frames_offer_the_stack_top_when_rip_is_outside_lifted_code();
    test_fault_frames_print_raw_addresses_without_symbols();
    test_fault_frames_are_only_for_faults();
    test_stop_and_thread_stop_place_the_frames_between_address_and_detail();
    test_trace_reports_an_empty_run_as_exactly_that();
    test_trace_labels_each_boundary_and_says_missing_out_loud();
    test_trace_elides_past_the_limit_and_says_how_to_see_the_rest();
    test_publish_fills_slots_in_order_and_drops_past_capacity();
    test_publish_a_real_stop_displaces_the_oldest_orderly_exit();
    test_verdict_a_real_stop_outranks_earlier_orderly_exits();
    test_thread_stop_report_prints_the_tail_and_the_run_end_evidence();
    test_main_entry_return_is_annotated_as_expected();
    test_published_stop_detail_survives_the_source_buffer();
    test_verdict_prefers_the_first_guest_thread_over_the_main_thread();
    test_verdict_a_hung_thread_outranks_everything();
    test_exit_status_accepts_exactly_the_three_expected_ends();

    printf("host_report: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
