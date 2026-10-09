/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Direct3D 8 HLE dispatch, keyed by address.
 *
 * The Xbox has no user-mode graphics driver. D3D8.lib is statically linked into
 * the title and IS the driver, so the boundary we have to replace is not an
 * import list -- there is nothing to import -- but the set of addresses inside
 * the linked XDK code that game code transfers control to.
 *
 * tools/gen_d3d8_surface.py measured that boundary from the real executable: 80
 * distinct D3D functions reached from 358 call sites. Only 11 of the 80 carry a
 * name, recovered from .XTLID, so the table is keyed by address and the name is
 * an optional comment on the row rather than its identity. Those 69 NULLs are
 * the clean-room boundary WORKING, not a gap to be filled: the names exist in
 * FLIRT output from leaked .lib archives, which docs/provenance.md permits only
 * as a local uncommitted aid.
 *
 * THESE NUMBERS WERE WRONG HERE FOR A WHILE, and the way they were wrong is the
 * useful part. This header said 85 / 363 / 19 of 85, taken from a scanner that
 * BYTE-SWEPT for E8/E9 opcodes. Its own docstring warned only that it might
 * OVER-count; it was wrong in BOTH directions, and the under-count was the larger
 * error, because the sweep's `offset += 5` stepped over genuine instructions.
 * Replacing the sweep with a decoder leaves 0 of 236 rows suspect against 4
 * before. Five of the old D3D entries were non-16-aligned false positives.
 *
 * So the counts are no longer upper bounds with a clustering heuristic attached,
 * and the old advice to distrust a single-site row no longer applies in that
 * form. The site count is still the backlog order: implement the function called
 * from 93 places before the one called from one place.
 *
 * Anything reading this header before 2026-10-01 was five functions out. Verify a
 * figure against the generated src/xbox/xdk_surface.c rather than against prose,
 * here or anywhere.
 *
 * NOTHING HERE IS IMPLEMENTED SPECULATIVELY. There are no D3D function bodies in
 * this module and there must not be any. Guessing at what SetRenderState does
 * before the guest has ever asked for it produces code nobody can validate and a
 * failure arbitrarily far from its cause.
 *
 * THE DIAGNOSTICS ARE THE PRODUCT. The deliverable is the scaffold plus honest
 * reporting, so that the first time the guest touches D3D we learn exactly which
 * functions it wants and in what order. Whatever this module logs is precisely
 * what remains to be written, which makes the registry and the backlog the same
 * object. This mirrors src/xbox/kernel_hle.h, keyed by address instead of by
 * ordinal.
 */

#ifndef TSFP_GPU_D3D8_HLE_H
#define TSFP_GPU_D3D8_HLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * One row of the measured surface, injected at init.
 *
 * This is deliberately a separate type from the live `d3d8_entry` below. The
 * measurement is an input we do not own and cannot change at runtime, while the
 * entry carries mutable dispatch state, and collapsing the two would invite code
 * to write back into what is supposed to be ground truth.
 */
typedef struct {
    uint32_t address;
    const char *name; /* NULL when .XTLID supplied none */
    /* Measured call sites. Ranks the backlog before anything has ever run. */
    uint32_t sites;
} d3d8_surface_entry;

/**
 * A D3D function implementation.
 *
 * Arguments are not modelled. The call boundary comes from the decompiled guest
 * code, which has its own view of each function's signature, so inventing a
 * uniform one here would be a guess that later has to be unpicked. `context` is
 * reserved for the per-call state that boundary will eventually supply.
 */
typedef uint32_t (*d3d8_fn)(void *context);

typedef enum {
    /* Measured, but nothing is written yet: reports itself, returns the default. */
    D3D8_ENTRY_STUB = 0,
    /* A real implementation is registered. */
    D3D8_ENTRY_IMPLEMENTED,
} d3d8_entry_state;

typedef struct {
    uint32_t address;
    /* Borrowed from the injected table. See d3d8_hle_init for the lifetime rule. */
    const char *name;
    uint32_t sites;
    d3d8_fn handler;
    d3d8_entry_state state;
    uint32_t default_return;
    /* Times called. Cheap, and it makes the hottest missing function obvious
     * rather than merely present somewhere in a log. */
    uint64_t call_count;
    bool reported;
} d3d8_entry;

/** A printf-style diagnostic sink. */
typedef int (*d3d8_log_fn)(const char *format, ...);

/**
 * Adopt a measured surface.
 *
 * The table is injected rather than compiled in because src/xbox/xdk_surface.c
 * is generated and gitignored, so a fresh clone does not have it. This module
 * must build and be fully testable without it, and tests inject their own rows.
 *
 * `count` rows are copied into heap storage, so the caller may free the array.
 * The `name` pointers are BORROWED, not copied: the generated table holds static
 * string literals. That is a lifetime constraint on the caller -- names must
 * outlive the module, or outlive the next d3d8_hle_shutdown at least.
 *
 * Returns false for a NULL table, a count of 0, or an allocation failure. A
 * failed re-init leaves the previous table intact and usable, because losing a
 * working surface is worse than failing to upgrade it.
 *
 * Calling this again replaces the previous table without leaking. Per-table
 * state, including call counts and the unknown-target counter, resets with it.
 */
bool d3d8_hle_init(const d3d8_surface_entry *table, size_t count);

/** Release the table. Idempotent, and leaves the module re-initialisable. */
void d3d8_hle_shutdown(void);

/**
 * Register a real implementation.
 *
 * Returns false for an address absent from the measured surface, or a NULL
 * handler. Accepting an unknown address would mean either the measurement or the
 * caller is wrong, and silently accepting it would hide which.
 */
bool d3d8_hle_register(uint32_t address, d3d8_fn handler);

/** Set what a stub at this address returns. Defaults to 0. */
bool d3d8_hle_set_default_return(uint32_t address, uint32_t value);

/**
 * Look up an entry, or NULL when the address is not in the measured surface.
 *
 * The returned pointer is invalidated by d3d8_hle_init and d3d8_hle_shutdown.
 */
const d3d8_entry *d3d8_hle_entry(uint32_t address);

/**
 * Dispatch a call to a measured address.
 *
 * An implemented address runs its handler and returns its value. A stub reports
 * itself once, then returns its default. An address absent from the table
 * reports EVERY time and returns 0.
 */
uint32_t d3d8_hle_call(uint32_t address, void *context);

/** Rows in the adopted surface. 0 when uninitialised. */
size_t d3d8_hle_count(void);

/** How many rows have real implementations. */
size_t d3d8_hle_implemented_count(void);

/**
 * Calls that landed on an address absent from the measured surface.
 *
 * Non-zero means the surface and the guest disagree, which is a measurement bug
 * rather than merely unfinished work.
 */
uint64_t d3d8_hle_unknown_count(void);

/**
 * Write the un-implemented entries, busiest first.
 *
 * This is the backlog in priority order. Before the guest has run, every call
 * count is 0 and it degrades into the measured site ranking, which is exactly
 * the priority you want on day one.
 */
void d3d8_hle_report_backlog(void);

/** Redirect diagnostics. Defaults to stderr. Tests use this to capture output. */
void d3d8_hle_set_log(d3d8_log_fn printer);

/**
 * What a handler does when it reaches a state it must not guess at.
 *
 * A handler that returns a made-up value for a path it never modelled is the failure this
 * whole boundary exists to prevent: the title keeps running on it and the trace turns into
 * fiction. `d3d8_hle_fatal` is the alternative. It logs, then calls this hook, which the
 * host points at `host_run_stop` so the run ends AT the address with a report. The default
 * logs and aborts, so a test binary or a host that never installed the hook still cannot
 * continue past it. A hook that returns is treated as a bug and aborts too.
 */
typedef void (*d3d8_fatal_fn)(uint32_t guest_address, const char *message);

/** Install the fatal hook. NULL restores the default (log and abort). */
void d3d8_hle_set_fatal(d3d8_fatal_fn handler);

/** Report that the handler for `guest_address` cannot continue, and never return. */
void d3d8_hle_fatal(uint32_t guest_address, const char *format, ...)
    __attribute__((format(printf, 2, 3), noreturn));

/**
 * Record that a handler skipped a documented part of the original: a cascade into library
 * code that is not ported, taken only when the state says the original would have taken it.
 *
 * This is the middle road between a silent skip and `d3d8_hle_fatal`. It is for omissions
 * that cannot change anything the game reads (the docs say why for each) and that a run
 * should still announce. Logged once per distinct (address, `what`) pair, where `what` must be
 * a string literal because it is compared by pointer, and counted every time.
 */
void d3d8_hle_note_unmodelled(uint32_t guest_address, const char *what);

/** How many times `d3d8_hle_note_unmodelled` was called since the table was adopted. */
uint64_t d3d8_hle_unmodelled_count(void);

/**
 * The current diagnostic sink. Never NULL.
 *
 * Exposed so that the modules eventually implementing these functions report
 * through the same channel as the dispatcher. A second, separate sink would mean
 * a test that captures one still misses the other, and a diagnostic nobody reads
 * is the failure mode this whole subsystem exists to avoid.
 */
d3d8_log_fn d3d8_hle_log(void);

#endif /* TSFP_GPU_D3D8_HLE_H */
