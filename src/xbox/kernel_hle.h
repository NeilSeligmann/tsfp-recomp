/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Xbox kernel HLE dispatch.
 *
 * The guest imports kernel functions by ordinal, so this is a table indexed by
 * ordinal with a stub for every one of the 371 known entries. TimeSplitters:
 * Future Perfect imports 151 of them.
 *
 * THE DIAGNOSTICS ARE THE POINT. A missing kernel function must announce itself,
 * by name, the first time it is called. The alternative -- returning a plausible
 * value silently -- produces a failure arbitrarily far from its cause, and on a
 * codebase this size that is the difference between a day and a month. So the
 * registry doubles as the backlog: whatever logs is what remains to be written.
 *
 * Reporting is once-per-ordinal rather than once-per-call, because a stub inside a
 * per-frame loop would otherwise bury everything else.
 */

#ifndef TSFP_XBOX_KERNEL_HLE_H
#define TSFP_XBOX_KERNEL_HLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel_ordinals.h"

/* Guest pointer width. Kept distinct from a host pointer on purpose: a guest
 * pointer is four bytes, and storing a host pointer in one truncates it. */
typedef uint32_t kernel_guest_ptr;

/**
 * A kernel function implementation.
 *
 * Arguments are not modelled yet. The call boundary comes from the decompiled
 * guest code, which has its own view of each function's signature, so inventing a
 * uniform one here would be a guess that later has to be unpicked. `context` is
 * reserved for the per-call state that boundary will eventually supply.
 */
typedef uint32_t (*kernel_fn)(void *context);

typedef enum {
    /* Nothing registered: calling it is a hard error. */
    KERNEL_ENTRY_ABSENT = 0,
    /* A stub that reports itself and returns the default. */
    KERNEL_ENTRY_STUB,
    /* A real implementation. */
    KERNEL_ENTRY_IMPLEMENTED,
} kernel_entry_state;

typedef struct {
    const char *name;
    kernel_fn handler;
    kernel_entry_state state;
    uint32_t default_return;
    /* Times called. Cheap, and it makes the hottest missing function obvious
     * rather than merely present in a log. */
    uint64_t call_count;
    bool reported;
} kernel_entry;

/** Reset the table to stubs for every known ordinal. Safe to call repeatedly. */
void kernel_hle_init(void);

/**
 * Register a real implementation for an ordinal.
 *
 * Returns false for an unknown ordinal, because registering against one would
 * mean either the table or the caller is wrong, and silently accepting it would
 * hide that.
 */
bool kernel_hle_register(unsigned ordinal, kernel_fn handler);

/** Set what a stub returns. Defaults to 0. */
bool kernel_hle_set_default_return(unsigned ordinal, uint32_t value);

/** Look up an entry, or NULL for an unknown ordinal. */
const kernel_entry *kernel_hle_entry(unsigned ordinal);

/**
 * Invoke an ordinal.
 *
 * A stub reports itself once by name, then returns its default. An absent ordinal
 * reports every time, because it should never happen.
 */
uint32_t kernel_hle_call(unsigned ordinal, void *context);

/** How many of `ordinals` have real implementations. */
size_t kernel_hle_implemented_count(const unsigned *ordinals, size_t count);

/**
 * Write a report of what an ordinal list still needs, busiest first.
 *
 * This is the backlog in priority order: pass the guest's own import list and it
 * says what to write next.
 */
void kernel_hle_report_missing(const unsigned *ordinals, size_t count);

/** A printf-style diagnostic sink. */
typedef int (*kernel_log_fn)(const char *format, ...);

/** Redirect diagnostics. Defaults to stderr; tests use this to capture output. */
void kernel_hle_set_log(kernel_log_fn printer);

/**
 * The current diagnostic sink. Never NULL.
 *
 * Exposed so that the modules implementing ordinals report through the same
 * channel as the dispatcher. A second, separate sink would mean a test that
 * captures one still misses the other, and a diagnostic nobody reads is the
 * failure mode this whole subsystem exists to avoid.
 */
kernel_log_fn kernel_hle_log(void);

/**
 * A handler reached a state the guest must never continue from.
 *
 * On hardware KeBugCheck halts the console and an unhandled RtlRaiseException ends
 * in exactly that bugcheck, so continuing past either here would produce a trace no
 * console could ever produce. src/xbox must not depend on the host runtime, so the
 * module calls out through this hook and the host points it at its stop machinery.
 * The default hook aborts the process, because a fatal call with no host attached
 * has no correct continuation.
 *
 * Tests install a capturing hook, which RETURNS. A handler that called this must
 * therefore still return a deterministic value afterwards, and that value is part
 * of its tested contract.
 */
typedef void (*kernel_fatal_fn)(unsigned ordinal, const char *detail);

/** Install the fatal hook. NULL restores the default (log, then abort). */
void kernel_hle_set_fatal(kernel_fatal_fn handler);

/**
 * Report a fatal kernel call: log it through kernel_hle_log(), then invoke the
 * fatal hook with the formatted detail. The detail buffer is thread-local and
 * borrowed, exactly like the host's own stop details.
 */
void kernel_hle_fatal(unsigned ordinal, const char *format, ...);

#endif /* TSFP_XBOX_KERNEL_HLE_H */
