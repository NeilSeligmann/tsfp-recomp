/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The run report: every block tsfp_host prints after the guest stops, plus the
 * stop-record bookkeeping and the verdict those blocks are derived from.
 *
 * WHY THIS IS ITS OWN TRANSLATION UNIT, exactly as src/host/host_options.{c,h} is.
 * These functions lived in `src/host/main.c`, which is compiled into `tsfp_host`
 * ALONE. `tsfp_host` is not a ctest binary, so nothing in it can be covered by a
 * suite or reached by `tools/mutate/c_suites.py`. The report is the DELIVERABLE of
 * a bring-up run -- the ordered trace, the fabrication counts, the created-for-real
 * line -- and a formatter whose "FABRICATED" annotation silently went missing would
 * fail no build and no test while making a fabricated run read as a clean one.
 *
 * THE INTERFACE IS DATA IN, TEXT OUT. Every function takes a `FILE *` and plain
 * values; nothing here reads a global, takes a lock, or calls into the kernel HLE.
 * main.c gathers the numbers (under its own locks where needed) and passes them in;
 * a test passes an `open_memstream` buffer and asserts on the exact bytes. The few
 * lookups that depend on live tables (ordinal names, XDK names, thunk-window
 * membership) arrive through `host_report_names`, so the suite can pin the
 * formatting without linking the thunk layer or the lifted code.
 */

#ifndef TSFP_HOST_REPORT_H
#define TSFP_HOST_REPORT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "host_runtime.h"
#include "thunk_trace.h"

#include "kernel_file.h"

/*
 * Name and state lookups the report lines need. Injected rather than linked so this
 * unit depends on nothing: main.c wires the real functions (xbox_kernel_ordinal_name,
 * xdk_thunk_name_of, kernel_thunk_is_va, kernel_thunk_measured_as_data,
 * host_stop_reason_str), and a suite wires fakes with known answers. Any pointer may
 * be left NULL by a test that never reaches it; main.c sets all five.
 */
typedef struct {
    /** Human-readable stop reason. Never returns NULL. */
    const char *(*stop_reason_str)(host_stop_reason reason);
    /** Kernel ordinal name, or NULL when unknown. */
    const char *(*ordinal_name)(unsigned ordinal);
    /** XDK function name for a guest VA, or NULL when unnamed. */
    const char *(*xdk_name_of)(uint32_t address);
    /** True when the address is a synthetic kernel thunk VA. */
    bool (*thunk_is_va)(uint32_t va);
    /** True when every lifted reference to the ordinal's slot is a read. */
    bool (*measured_as_data)(unsigned ordinal);
    /** T505: `name+0xOFFSET` of the function holding a host code address (writes `out`,
     *  false when unknown). Optional: a fault report prints raw addresses without it. */
    bool (*host_symbol)(uintptr_t address, char *out, size_t size);
    /** T505: `sub_XXXXXXXX+0xOFFSET` of the lifted function containing a guest VA. Optional. */
    bool (*guest_function)(uint32_t guest_va, char *out, size_t size);
} host_report_names;

/**
 * T505: the evidence a fault left in a stop record, as text. Prints nothing for any other
 * stop reason: the faulting RIP and its function, the host `rbp` chain as lifted function
 * names, and the guest `ebp` chain as return addresses and the lifted functions holding them.
 * Without symbols (a stripped binary, NULL name callbacks) the raw addresses still print.
 */
void host_report_fault_frames(FILE *out, const host_stop *stop, const host_report_names *names);

/* --- where each thread stopped -------------------------------------------- */

/**
 * One guest thread's stop, published by the thread itself and read after the join.
 * The table and its lock stay in main.c; the slot-filling policy and the formatting
 * live here so both are testable.
 */
typedef struct {
    uint32_t handle;
    uint32_t entry_va;
    uint32_t esp;
    uint32_t eax;
    host_stop stop;
    /* Stop detail may live in worker TLS, which disappears after the join, so the
     * record keeps its own copy and stop.detail points here. */
    char detail[256];
    /* T1492: this thread's last calls (oldest first) when it stopped, the half of the run a stop
     * report needs. tail_count entries are valid. */
    thunk_trace_entry tail[THUNK_TAIL_MAX];
    unsigned tail_count;
    bool valid;
} host_thread_stop_record;

/**
 * Record one stop in the first free slot of `table`. The CALLER holds whatever lock guards `table`.
 *
 * T1492: a full table no longer drops a stop that matters. The level 2 run (15 guest threads, 8
 * slots, 13 of them orderly PsTerminateSystemThread exits) lost the main game thread's FINAL stop
 * this way and reported only the eight earliest, uninteresting ones. Now an orderly exit
 * (HOST_STOP_THREAD_EXITED) never displaces anything, and any other stop displaces the OLDEST
 * orderly exit when no slot is free. Returns false when the stop was dropped (an orderly exit into
 * a full table, or a full table of non-orderly stops), so the caller can count it.
 * `tail` (may be NULL) is the thread's last calls.
 */
bool host_thread_stop_publish(host_thread_stop_record *table, unsigned capacity,
                              uint32_t handle, uint32_t entry_va, uint32_t esp,
                              uint32_t eax, const host_stop *stop,
                              const thunk_trace_entry *tail, unsigned tail_count);

/**
 * T1492: what the title's last calls and the run's last non-success kernel statuses were, printed
 * once after the thread stops. `failures` oldest first. `orderly_exits` is how many threads ended
 * by PsTerminateSystemThread (itemised or not), `unlisted` how many stops did not fit the table.
 */
void host_report_run_end_evidence(FILE *out, const thunk_trace_entry *failures, unsigned failure_count,
                                  uint64_t failure_total, unsigned orderly_exits, unsigned unlisted,
                                  const host_report_names *names);

/**
 * The run's verdict: the FIRST guest thread's stop reason where one exists, because
 * the main thread's "entry point returned" is the uninteresting half; the main
 * thread's reason otherwise; HOST_STOP_THREAD_TIMEOUT whenever a thread is still
 * running, because an abandoned hang outranks whatever any thread published.
 */
host_stop_reason host_run_verdict(host_stop_reason main_reason,
                                  const host_thread_stop_record *table,
                                  unsigned capacity, unsigned still_running);

/**
 * The process exit status. 0 only when every cleanup succeeded AND the verdict is
 * one of the three EXPECTED ends of a bring-up run: an unimplemented kernel
 * ordinal, an unimplemented XDK address, or a guest thread reaching its own
 * PsTerminateSystemThread. Everything else is 1.
 */
int host_run_exit_status(bool originals_cleaned, bool callback_cleaned,
                         bool buffers_cleaned, bool streams_cleaned,
                         host_stop_reason verdict);

/* --- the report blocks, in the order main.c prints them -------------------- */

/** One guest thread's stop block, exactly as report_thread_stops printed it. */
void host_report_thread_stop(FILE *out, const host_thread_stop_record *record,
                             const host_report_names *names);

/**
 * The calling thread's own stop, with the guest registers and the indirect-call
 * ring as they stood when it jumped out. `icall_trace` has `icall_ring` entries and
 * `icall_idx` is the ring's write cursor, i.e. the OLDEST recorded target.
 */
void host_report_stop(FILE *out, const host_stop *stop, uint32_t esp, uint32_t eax,
                      const uint32_t *icall_trace, uint32_t icall_idx,
                      unsigned icall_ring, const host_report_names *names);

/** Totals for the trace header, one per thunk kind plus the overall count. */
typedef struct {
    unsigned long long total;
    unsigned long long total_ordinal;
    unsigned long long total_xdk;
    unsigned long long total_monitor;
} host_report_trace_totals;

/** The ordered HLE trace, at most `limit` entries of the `count` recorded. */
void host_report_trace(FILE *out, const thunk_trace_entry *entries, size_t count,
                       const host_report_trace_totals *totals, unsigned limit,
                       const host_report_names *names);

/** KPCR.Irql publishing: how many raises reached the guest's copy, and the peak. */
void host_report_irql_publishing(FILE *out, unsigned long published, uint32_t peak,
                                 unsigned long failures);

/* How many distinct EEPROM indices the report shows before eliding. */
#define HOST_REPORT_CONFIG_INDEX_MAX 32u

/**
 * The non-volatile settings block. `indices` holds `total` distinct indices,
 * clamped by the caller to HOST_REPORT_CONFIG_INDEX_MAX entries. Prints nothing
 * when the title asked for none.
 */
void host_report_non_volatile_settings(FILE *out, const uint32_t *indices,
                                       unsigned total, unsigned served,
                                       unsigned refused, unsigned fabricated);

/** Every counter the file-attempts block prints. Gathered by the caller. */
typedef struct {
    unsigned total;
    unsigned refused;
    unsigned relative_refused;
    unsigned fabricated;
    unsigned escape_refused;
    unsigned disc_opened;
    unsigned long long disc_bytes_read;
    unsigned host_opened;
    unsigned long long host_bytes_read;
    unsigned created;
    unsigned long long bytes_written;
    unsigned write_count;
    unsigned write_refused;
} host_report_file_counts;

/**
 * The names-the-title-tried-to-open block. `attempt_at` returns the i-th recorded
 * attempt or NULL, over [0, counts->total). Injected for the same reason the name
 * lookups are. Prints nothing when no attempt was recorded.
 */
void host_report_file_attempts(FILE *out, const host_report_file_counts *counts,
                               const kernel_file_attempt *(*attempt_at)(unsigned i));

/**
 * The symbolic-links block: the drive-letter aliases the TITLE created, resolved
 * through `target_of` (NULL when the name resolves to nothing). `total` is how many
 * links exist in all; prints nothing when it is 0.
 */
void host_report_symbolic_links(FILE *out, unsigned total,
                                const char *(*target_of)(const char *name));

#endif /* TSFP_HOST_REPORT_H */
