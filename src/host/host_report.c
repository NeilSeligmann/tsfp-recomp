/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The run report tsfp_host prints, moved out of main.c so a suite and the mutation
 * harness can reach it. See host_report.h for why, and tests/c/test_host_report.c
 * for what each block pins.
 *
 * EVERY FORMAT STRING HERE IS BYTE-IDENTICAL to what main.c printed before the
 * move, verified by diffing full driver captures across the refactor. The report is
 * read by operators and by later tasks as evidence, so a formatting change is a
 * behaviour change and gets made deliberately or not at all.
 */

#include "host_report.h"

#include <stddef.h>
#include <string.h>

#include "kernel_thunk.h"
#include "monitor_thunk.h"

#include "kernel_sync.h"

static void report_trace_line(FILE *out, size_t number, const thunk_trace_entry *entry,
                              const host_report_names *names);

/* --- stop records and the verdict ------------------------------------------ */

bool host_thread_stop_publish(host_thread_stop_record *table, unsigned capacity,
                              uint32_t handle, uint32_t entry_va, uint32_t esp,
                              uint32_t eax, const host_stop *stop,
                              const thunk_trace_entry *tail, unsigned tail_count)
{
    unsigned slot = capacity;
    for (unsigned i = 0; i < capacity; i++) {
        if (!table[i].valid) {
            slot = i;
            break;
        }
    }
    if (slot == capacity && stop->reason != HOST_STOP_THREAD_EXITED) {
        for (unsigned i = 0; i < capacity; i++) {
            if (table[i].stop.reason == HOST_STOP_THREAD_EXITED) {
                slot = i;
                break;
            }
        }
    }
    if (slot == capacity) {
        return false;
    }
    memset(&table[slot], 0, sizeof(table[slot]));
    table[slot].handle = handle;
    table[slot].entry_va = entry_va;
    table[slot].esp = esp;
    table[slot].eax = eax;
    table[slot].stop = *stop;
    /* Stop detail may live in worker TLS, which disappears after the join. */
    if (table[slot].stop.detail != NULL) {
        (void)snprintf(table[slot].detail, sizeof(table[slot].detail), "%s",
                       table[slot].stop.detail);
        table[slot].stop.detail = table[slot].detail;
    }
    if (tail != NULL) {
        table[slot].tail_count = tail_count < THUNK_TAIL_MAX ? tail_count : THUNK_TAIL_MAX;
        memcpy(table[slot].tail, tail, table[slot].tail_count * sizeof(table[slot].tail[0]));
    }
    table[slot].valid = true;
    return true;
}

host_stop_reason host_run_verdict(host_stop_reason main_reason,
                                  const host_thread_stop_record *table,
                                  unsigned capacity, unsigned still_running)
{
    host_stop_reason verdict = main_reason;
    bool decided = false;
    /* T1492: the first stop that is NOT an orderly exit decides (a fault after eight clean thread
     * exits is the run's verdict, not the first clean exit); with only orderly exits the first one. */
    for (unsigned i = 0; i < capacity && !decided; i++) {
        if (table[i].valid && table[i].stop.reason != HOST_STOP_THREAD_EXITED) {
            verdict = table[i].stop.reason;
            decided = true;
        }
    }
    for (unsigned i = 0; i < capacity && !decided; i++) {
        if (table[i].valid) {
            verdict = table[i].stop.reason;
            decided = true;
        }
    }
    if (still_running > 0u) {
        verdict = HOST_STOP_THREAD_TIMEOUT;
    }
    return verdict;
}

int host_run_exit_status(bool originals_cleaned, bool callback_cleaned,
                         bool buffers_cleaned, bool streams_cleaned,
                         host_stop_reason verdict)
{
    /* Stopping at a missing ordinal is the expected bring-up outcome, and so is a
     * guest thread reaching its own PsTerminateSystemThread. Anything else is not. */
    /* An unimplemented XDK function is the address-keyed twin of an unimplemented
     * ordinal: the EXPECTED end of a bring-up run, not a failure (host_runtime.h says
     * so for both). */
    return (originals_cleaned && callback_cleaned && buffers_cleaned && streams_cleaned &&
            (verdict == HOST_STOP_KERNEL_UNIMPLEMENTED ||
             verdict == HOST_STOP_THREAD_EXITED ||
             verdict == HOST_STOP_HOST_SHUTDOWN ||
             verdict == HOST_STOP_XDK_UNIMPLEMENTED))
               ? 0
               : 1;
}

/* --- the report blocks ------------------------------------------------------ */

/* For the two indirect-call stops, `guest_address` is the TARGET the guest tried
 * to call -- and the single most common garbage target is 0, which the plain
 * `!= 0` guard below silently suppressed. A stop that printed no address at all
 * read as "we do not know where", when in fact 0 WAS the answer: a NULL function
 * pointer. Report it unconditionally for these two, and say which it is. */
static bool stop_address_is_icall_target(host_stop_reason reason)
{
    return reason == HOST_STOP_ICALL_NOT_CODE || reason == HOST_STOP_ICALL_UNRESOLVED;
}

static void report_stop_guest_address(FILE *out, const host_stop *stop)
{
    if (stop_address_is_icall_target(stop->reason)) {
        fprintf(out, "  icall target   0x%08X%s\n", stop->guest_address,
                stop->guest_address == 0u ? "  (a NULL function pointer)" : "");
    } else if (stop->guest_address != 0u) {
        fprintf(out, "  guest address  0x%08X\n", stop->guest_address);
    }
}

/* T505. A frame line: index, raw address, and the function when one is known. */
static void report_frame(FILE *out, const char *label, unsigned index, unsigned long long address,
                         int width, const char *name)
{
    fprintf(out, "    %s%-2u 0x%0*llX", label, index, width, address);
    if (name != NULL) {
        fprintf(out, "  %s", name);
    }
    fprintf(out, "\n");
}

void host_report_fault_frames(FILE *out, const host_stop *stop, const host_report_names *names)
{
    if (stop->reason != HOST_STOP_FAULT) {
        return;
    }
    char text[160];
    bool have_rip_name = false;
    if (stop->fault_rip != 0u) {
        have_rip_name = names->host_symbol != NULL &&
                        names->host_symbol(stop->fault_rip, text, sizeof(text));
        fprintf(out, "  fault rip      0x%016llX%s%s\n", (unsigned long long)stop->fault_rip,
                have_rip_name ? "  " : "", have_rip_name ? text : "");
    }
    /* A frameless leaf (a libc routine, say) pushes no `rbp` frame, so the chain starts at
     * ITS caller's caller. The word at the stack pointer is then the missing return address. */
    if (stop->fault_stack_top != 0u && names->host_symbol != NULL &&
        (!have_rip_name || strncmp(text, "sub_", 4u) != 0) &&
        names->host_symbol(stop->fault_stack_top, text, sizeof(text))) {
        fprintf(out, "  stack top      0x%016llX  %s  (caller of a frameless leaf when rip is\n"
                     "                 outside lifted code)\n",
                (unsigned long long)stop->fault_stack_top, text);
    }
    if (stop->fault_host_frame_count != 0u) {
        fprintf(out, "  host frames    (return addresses up the rbp chain, innermost first)\n");
        for (unsigned i = 0; i < stop->fault_host_frame_count; i++) {
            const bool named = names->host_symbol != NULL &&
                               names->host_symbol(stop->fault_host_frames[i], text, sizeof(text));
            report_frame(out, "#", i, (unsigned long long)stop->fault_host_frames[i], 16,
                         named ? text : NULL);
        }
    }
    if (stop->fault_guest_ebp != 0u) {
        fprintf(out, "  guest ebp      0x%08X\n", stop->fault_guest_ebp);
    }
    if (stop->fault_guest_frame_count != 0u) {
        fprintf(out, "  guest frames   (return addresses up the ebp chain, innermost first)\n");
        for (unsigned i = 0; i < stop->fault_guest_frame_count; i++) {
            const bool named = names->guest_function != NULL &&
                               names->guest_function(stop->fault_guest_frames[i], text, sizeof(text));
            report_frame(out, "#", i, stop->fault_guest_frames[i], 8, named ? text : NULL);
        }
    }
}

void host_report_thread_stop(FILE *out, const host_thread_stop_record *record,
                             const host_report_names *names)
{
    fprintf(out, "\nguest thread %#x (entered 0x%08X) stopped: %s\n",
            (unsigned)record->handle, record->entry_va,
            names->stop_reason_str(record->stop.reason));
    if (record->stop.ordinal != 0u) {
        const char *name = names->ordinal_name(record->stop.ordinal);
        fprintf(out, "  ordinal        %u (%s)\n", record->stop.ordinal,
                name ? name : "<unknown>");
    }
    report_stop_guest_address(out, &record->stop);
    if (record->stop.reason == HOST_STOP_FAULT) {
        fprintf(out, "  signal         %d\n", record->stop.signal_number);
        fprintf(out, "  fault address  0x%016llX\n",
                (unsigned long long)record->stop.fault_address);
        if (names->thunk_is_va((uint32_t)record->stop.fault_address)) {
            fprintf(out, "  NOTE: that is a synthetic kernel thunk VA, so the guest\n"
                         "        DEREFERENCED a thunk slot instead of calling it.\n");
        }
    }
    host_report_fault_frames(out, &record->stop, names);
    if (record->stop.detail && record->stop.detail[0] != '\0') {
        fprintf(out, "  detail         %s\n", record->stop.detail);
    }
    fprintf(out, "  guest esp      0x%08X\n", record->esp);
    fprintf(out, "  guest eax      0x%08X\n", record->eax);
    if (record->tail_count > 0u) {
        fprintf(out, "  this thread's last %u HLE call(s), oldest first (T1492):\n", record->tail_count);
        for (unsigned i = 0; i < record->tail_count; i++) {
            report_trace_line(out, (size_t)i + 1u, &record->tail[i], names);
        }
    }
}

void host_report_run_end_evidence(FILE *out, const thunk_trace_entry *failures, unsigned failure_count,
                                  uint64_t failure_total, unsigned orderly_exits, unsigned unlisted,
                                  const host_report_names *names)
{
    fprintf(out, "\n--- how the run ended (T1492) ---\n");
    fprintf(out, "guest threads that ended by PsTerminateSystemThread (orderly): %u\n", orderly_exits);
    if (unlisted > 0u) {
        fprintf(out, "stops that did NOT fit the per-thread table and are not itemised above: %u\n", unlisted);
    }
    fprintf(out, "kernel calls that returned a non-success NTSTATUS (error or warning), whole run: %llu\n",
            (unsigned long long)failure_total);
    if (failure_count > 0u) {
        fprintf(out, "the last %u of them, oldest first (t = host thread id, 'from' = guest caller):\n",
                failure_count);
        for (unsigned i = 0; i < failure_count; i++) {
            report_trace_line(out, (size_t)i + 1u, &failures[i], names);
        }
    }
}

void host_report_stop(FILE *out, const host_stop *stop, uint32_t esp, uint32_t eax,
                      const uint32_t *icall_trace, uint32_t icall_idx,
                      unsigned icall_ring, const host_report_names *names)
{
    fprintf(out, "\nstopped: %s\n", names->stop_reason_str(stop->reason));
    if (stop->reason == HOST_STOP_RETURNED && stop->detail != NULL &&
        strcmp(stop->detail, "entry point returned") == 0) {
        fprintf(out, "  NOTE: EXPECTED, not the end of the title. The XBE entry stub only spawns the guest\n"
                     "        main thread (0x0037FEB5) and returns. Why the RUN ended is under the\n"
                     "        'guest thread ... stopped' blocks and 'how the run ended' below.\n");
    }
    if (stop->ordinal != 0u) {
        const char *name = names->ordinal_name(stop->ordinal);
        fprintf(out, "  ordinal        %u (%s)\n", stop->ordinal,
                name ? name : "<unknown>");
        if (names->measured_as_data(stop->ordinal)) {
            fprintf(out,
                    "  NOTE: every reference to this ordinal's thunk slot in the lifted\n"
                    "        code is a read, not a call, yet the guest has just called it.\n"
                    "        Either it is a function reached through a register, or the\n"
                    "        call arrived from somewhere unexpected.\n");
        }
    }
    report_stop_guest_address(out, stop);
    if (stop->reason == HOST_STOP_FAULT) {
        fprintf(out, "  signal         %d\n", stop->signal_number);
        fprintf(out, "  fault address  0x%016llX\n",
                (unsigned long long)stop->fault_address);
        if (names->thunk_is_va((uint32_t)stop->fault_address)) {
            fprintf(out,
                    "  NOTE: that address is a synthetic kernel thunk VA, so the guest\n"
                    "        DEREFERENCED a thunk slot instead of calling it. Ordinal %u\n"
                    "        is a DATA export and needs a guest-visible object, not a\n"
                    "        dispatch stub.\n",
                    KERNEL_THUNK_ORDINAL((uint32_t)stop->fault_address));
        }
    }
    host_report_fault_frames(out, stop, names);
    if (stop->detail && stop->detail[0] != '\0') {
        fprintf(out, "  detail         %s\n", stop->detail);
    }
    fprintf(out, "  guest esp      0x%08X\n", esp);
    fprintf(out, "  guest eax      0x%08X\n", eax);

    fprintf(out, "  recent indirect targets (most recent last):\n    ");
    for (unsigned i = 0; i < icall_ring; i++) {
        uint32_t index = (icall_idx + i) % icall_ring;
        fprintf(out, "%08X ", icall_trace[index]);
    }
    fprintf(out, "\n");
}

static void report_trace_line(FILE *out, size_t number, const thunk_trace_entry *entry,
                              const host_report_names *names)
{
    char result_text[32];
    if (entry->result_known) {
        (void)snprintf(result_text, sizeof(result_text), "eax=0x%08X",
                       entry->result);
    } else {
        (void)snprintf(result_text, sizeof(result_text), "eax=<no return>");
    }
    /* The thread column is what keeps this readable now that there is more than
     * one guest thread: without it two interleaved call orders are one
     * uninterpretable sequence. */
    if (entry->kind == THUNK_KIND_MONITOR) {
        /* Its own line shape, labelled "monitor", because it is neither an
         * ordinal nor an XDK address and printing it as either would make the
         * trace claim the guest reached console surface it did not reach. */
        fprintf(out, "  %3zu  t%u  monitor      %-34s %-13s %s  from 0x%08X\n",
                number, entry->thread, MONITOR_THUNK_NOTIFY_NAME, "no-op",
                result_text, entry->return_address);
        return;
    }
    if (entry->kind == THUNK_KIND_XDK) {
        const char *name = names->xdk_name_of(entry->address);
        fprintf(out, "  %3zu  t%u  xdk 0x%08X  %-34s %-13s %s  from 0x%08X\n",
                number, entry->thread, entry->address,
                name ? name : "<unnamed>",
                entry->implemented ? "implemented" : "MISSING", result_text,
                entry->return_address);
        return;
    }
    const char *name = names->ordinal_name(entry->ordinal);
    fprintf(out, "  %3zu  t%u  ordinal %3u  %-34s %-13s %s  from 0x%08X\n", number,
            entry->thread, entry->ordinal, name ? name : "<unknown>",
            entry->implemented ? "implemented" : "MISSING", result_text,
            entry->return_address);
}

/*
 * The ordered trace, covering BOTH boundaries in ONE sequence.
 *
 * A kernel ordinal and an XDK address are printed in the same list on purpose: the
 * interleaving is the deliverable, and two separate lists cannot be merged after the
 * fact. Each line says which kind it was, because an ordinal and a guest VA are both
 * just numbers and a reader has to be able to tell which column means anything.
 */
void host_report_trace(FILE *out, const thunk_trace_entry *entries, size_t count,
                       const host_report_trace_totals *totals, unsigned limit,
                       const host_report_names *names)
{
    fprintf(out,
            "\nHLE calls reached, in call order (%zu recorded, %llu total: %llu kernel "
            "ordinal, %llu XDK address, %llu monitor notify):\n",
            count, totals->total, totals->total_ordinal, totals->total_xdk,
            totals->total_monitor);
    if (count == 0) {
        fprintf(out, "  (none -- the guest stopped before its first HLE call)\n");
        return;
    }
    size_t shown = count < (size_t)limit ? count : (size_t)limit;
    for (size_t i = 0; i < shown; i++) {
        report_trace_line(out, i + 1, &entries[i], names);
    }
    if (shown < count) {
        fprintf(out, "  ... %zu more; pass --trace N to see them\n", count - shown);
    }
}

void host_report_irql_publishing(FILE *out, unsigned long published, uint32_t peak,
                                 unsigned long failures)
{
    fprintf(out, "\n--- KPCR.Irql publishing ---\n");
    fprintf(out,
            "published      %lu time(s), peak level %u (read back and confirmed)\n",
            published, (unsigned)peak);
    if (peak >= KERNEL_IRQL_DISPATCH) {
        fprintf(out,
                "               peak >= DISPATCH_LEVEL (%u), so the guest's 6 "
                "`fs:[0x24]` sites\n               had a non-PASSIVE value to read\n",
                KERNEL_IRQL_DISPATCH);
    }
    if (failures > 0u) {
        fprintf(out,
                "FAILED         %lu publish(es) had no KPCR to write, so the guest's "
                "copy went stale\n",
                failures);
    }
}

/*
 * What the title asked the console's EEPROM for, and how much of the answer we made
 * up.
 *
 * REPORTED RATHER THAN COUNTED QUIETLY. Every fabricated setting is a place where the
 * run from that point on reflects our zeros instead of a console's configuration, so
 * a reader has to be able to see how far that reaches without grepping the log. The
 * indices are the actionable part: they say exactly which settings a later task has
 * to derive for real.
 */
void host_report_non_volatile_settings(FILE *out, const uint32_t *indices,
                                       unsigned total, unsigned served,
                                       unsigned refused, unsigned fabricated)
{
    if (total == 0u) {
        return;
    }

    fprintf(out, "\n--- non-volatile settings the title asked for ---\n");
    fprintf(out, "distinct index(es) %u:", total);
    const unsigned shown =
        total < HOST_REPORT_CONFIG_INDEX_MAX ? total : HOST_REPORT_CONFIG_INDEX_MAX;
    for (unsigned i = 0u; i < shown; i++) {
        fprintf(out, " %#x", (unsigned)indices[i]);
    }
    if (total > shown) {
        fprintf(out, " ... (%u more)", total - shown);
    }
    fprintf(out, "\n");
    fprintf(out, "served from store   %u\n", served);
    fprintf(out, "refused             %u\n", refused);
    fprintf(out,
            "FABRICATED zeros    %u -- the title's configuration from the first of "
            "these on is\n                    ours, not a console's\n",
            fabricated);
}

/*
 * Which names the title tried to open, and what happened to each.
 *
 * THESE PATHS ARE THE DELIVERABLE of a bring-up run that reaches file I/O: they say
 * exactly which volumes a later task has to provide, with the access the title asks
 * for. Reported in one block rather than left scattered through the trace.
 */
void host_report_file_attempts(FILE *out, const host_report_file_counts *counts,
                               const kernel_file_attempt *(*attempt_at)(unsigned i))
{
    if (counts->total == 0u) {
        return;
    }

    fprintf(out, "\n--- names the title tried to open ---\n");
    for (unsigned i = 0u; i < counts->total; i++) {
        const kernel_file_attempt *attempt = attempt_at(i);
        if (!attempt) {
            continue;
        }
        fprintf(out, "  %-3s %ux  access %#010x share %#x options %#x  \"%s\"\n",
                attempt->opened ? "ok" : "ERR", (unsigned)attempt->attempts,
                (unsigned)attempt->desired_access, (unsigned)attempt->share_access,
                (unsigned)attempt->open_options, attempt->path);
    }
    fprintf(out, "refused             %u (no volume mounted behind the name)\n",
            counts->refused);
    fprintf(out,
            "refused, relative   %u (a root-directory handle whose meaning is not "
            "derived)\n",
            counts->relative_refused);
    if (counts->fabricated > 0u) {
        fprintf(out,
                "FABRICATED empties  %u -- everything the title reads through those "
                "handles is\n                    ours, not a disc's\n",
                counts->fabricated);
    }
    if (counts->escape_refused > 0u) {
        fprintf(out,
                "refused, escaping   %u (a \".\" or \"..\" component, or a host symbolic "
                "link --\n                    the guest tried to name something outside "
                "its own volume)\n",
                counts->escape_refused);
    }
    fprintf(out, "from a real disc    %u open(s), %llu byte(s) served\n",
            counts->disc_opened, counts->disc_bytes_read);
    fprintf(out, "from a real hdd     %u open(s), %llu byte(s) served\n",
            counts->host_opened, counts->host_bytes_read);
    /*
     * CREATED is reported unconditionally, including as a zero, and that is the point. A
     * run with --hdd that creates nothing has not exercised the thing --hdd is for, and a
     * line that only appears on success would make the two cases look the same.
     */
    fprintf(out,
            "CREATED for real    %u object(s) on a writable volume -- these outlive the "
            "run,\n                    unlike every fabricated empty above\n",
            counts->created);
    /*
     * WRITTEN is reported unconditionally for the same reason CREATED is. "from a real hdd
     * ... 0 byte(s) served" says nothing about writes, and a run where every write was
     * REFUSED would otherwise look identical to one where the title wrote nothing at all.
     * The refused count is what tells them apart.
     */
    fprintf(out, "WRITTEN to hdd      %llu byte(s) in %u NtWriteFile call(s), %u refused\n",
            counts->bytes_written, counts->write_count, counts->write_refused);
}

/*
 * The symbolic links the TITLE created, not ones we invented.
 *
 * Worth its own block because it is the evidence for a design decision: the disc mounts
 * behind `\Device\CdRom0` rather than `D:` precisely because the guest is seen here
 * creating that alias itself. If this block is ever empty on a run that reached the
 * disc, the mount is resolving for some other reason and that is worth knowing.
 */
void host_report_symbolic_links(FILE *out, unsigned total,
                                const char *(*target_of)(const char *name))
{
    if (total == 0u) {
        return;
    }
    fprintf(out, "\n--- symbolic links the title created ---\n");
    static const char *const probes[] = {
        "\\??\\D:", "\\??\\T:", "\\??\\U:", "\\??\\Z:",
        "\\??\\W:", "\\??\\X:", "\\??\\Y:", "\\??\\N:",
    };
    unsigned shown = 0u;
    for (size_t i = 0u; i < sizeof(probes) / sizeof(probes[0]); i++) {
        const char *target = target_of(probes[i]);
        if (target) {
            fprintf(out, "  %-10s -> %s\n", probes[i], target);
            shown++;
        }
    }
    if (shown < total) {
        fprintf(out, "  ... and %u more not in the probed set\n", total - shown);
    }
}
