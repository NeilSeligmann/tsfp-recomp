/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_hle.h for why the diagnostics matter more than the dispatch, and why
 * there are deliberately no D3D function bodies in this file.
 */

#include "d3d8_hle.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The live table. Heap-allocated because the row count is a runtime value: the
 * measured surface arrives from a generated file that a fresh clone does not
 * have, so nothing about its size can be known at compile time. */
static d3d8_entry *entries;
static size_t entry_count;

/* Calls to addresses outside the measured surface. Not per-entry by definition,
 * since there is no entry to put it on. */
static uint64_t unknown_hits;

static int default_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vfprintf(stderr, format, args);
    va_end(args);
    return written;
}

static d3d8_log_fn log_printer = default_printer;

void d3d8_hle_set_log(d3d8_log_fn printer)
{
    log_printer = printer ? printer : default_printer;
}

d3d8_log_fn d3d8_hle_log(void)
{
    return log_printer;
}

static d3d8_fatal_fn fatal_handler;

void d3d8_hle_set_fatal(d3d8_fatal_fn handler)
{
    fatal_handler = handler;
}

void d3d8_hle_fatal(uint32_t guest_address, const char *format, ...)
{
    char message[256];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    log_printer("d3d8: FATAL at guest address 0x%08x: %s\n", (unsigned)guest_address,
                message);
    if (fatal_handler) {
        fatal_handler(guest_address, message);
    }
    /* A hook that returns has not stopped anything. Continuing from here would hand the
     * guest a value nobody computed, which is exactly what this function is for. */
    abort();
}

#define UNMODELLED_SEEN_MAX 64u

static struct {
    uint32_t address;
    const char *what;
} unmodelled_seen[UNMODELLED_SEEN_MAX];
static size_t unmodelled_seen_count;
static uint64_t unmodelled_total;

void d3d8_hle_note_unmodelled(uint32_t guest_address, const char *what)
{
    unmodelled_total++;
    for (size_t index = 0; index < unmodelled_seen_count; index++) {
        if (unmodelled_seen[index].address == guest_address &&
            unmodelled_seen[index].what == what) {
            return;
        }
    }
    if (unmodelled_seen_count < UNMODELLED_SEEN_MAX) {
        unmodelled_seen[unmodelled_seen_count].address = guest_address;
        unmodelled_seen[unmodelled_seen_count].what = what;
        unmodelled_seen_count++;
    }
    log_printer("d3d8: 0x%08x does not model: %s\n", (unsigned)guest_address, what);
}

uint64_t d3d8_hle_unmodelled_count(void)
{
    return unmodelled_total;
}

static int compare_by_address(const void *left, const void *right)
{
    const d3d8_entry *left_entry = (const d3d8_entry *)left;
    const d3d8_entry *right_entry = (const d3d8_entry *)right;
    /* Subtraction would overflow for addresses this far apart, so compare. */
    if (left_entry->address < right_entry->address) {
        return -1;
    }
    if (left_entry->address > right_entry->address) {
        return 1;
    }
    return 0;
}

bool d3d8_hle_init(const d3d8_surface_entry *table, size_t count)
{
    if (!table || count == 0) {
        return false;
    }

    /* Allocate before freeing the old table, so a failed re-init leaves the
     * previous surface intact rather than destroying a working one. */
    d3d8_entry *adopted = calloc(count, sizeof(*adopted));
    if (!adopted) {
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        adopted[i].address = table[i].address;
        /* Borrowed, not copied. The generated table holds string literals, and
         * the lifetime rule is documented on d3d8_hle_init. */
        adopted[i].name = table[i].name;
        adopted[i].sites = table[i].sites;
        adopted[i].state = D3D8_ENTRY_STUB;
    }

    /* Sorted once here so every later lookup can be a binary search. The rows
     * arrive in whatever order the generator emitted, which is not guaranteed. */
    qsort(adopted, count, sizeof(*adopted), compare_by_address);

    free(entries);
    entries = adopted;
    entry_count = count;
    unknown_hits = 0;
    unmodelled_seen_count = 0;
    unmodelled_total = 0;
    return true;
}

void d3d8_hle_shutdown(void)
{
    /* free(NULL) is a no-op, and clearing the globals is what makes a second
     * shutdown, or a later re-init, safe. */
    free(entries);
    entries = NULL;
    entry_count = 0;
    unknown_hits = 0;
}

/**
 * Binary search for an exact address match.
 *
 * Exact is the whole point. An address one byte either side of a real entry is a
 * different instruction and must not resolve to its neighbour, because silently
 * dispatching a near-miss would turn a measurement error into a plausible-looking
 * wrong answer. A miss returns NULL, never the insertion point.
 */
static d3d8_entry *find_entry(uint32_t address)
{
    if (!entries || entry_count == 0) {
        return NULL;
    }

    size_t low = 0;
    size_t high = entry_count; /* exclusive */
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        uint32_t candidate = entries[middle].address;
        if (candidate == address) {
            return &entries[middle];
        }
        if (candidate < address) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    return NULL;
}

bool d3d8_hle_register(uint32_t address, d3d8_fn handler)
{
    d3d8_entry *entry = find_entry(address);
    if (!entry || !handler) {
        return false;
    }
    entry->handler = handler;
    entry->state = D3D8_ENTRY_IMPLEMENTED;
    return true;
}

bool d3d8_hle_set_default_return(uint32_t address, uint32_t value)
{
    d3d8_entry *entry = find_entry(address);
    if (!entry) {
        return false;
    }
    entry->default_return = value;
    return true;
}

const d3d8_entry *d3d8_hle_entry(uint32_t address)
{
    return find_entry(address);
}

uint32_t d3d8_hle_call(uint32_t address, void *context)
{
    d3d8_entry *entry = find_entry(address);
    if (!entry) {
        /* Not in the measured surface at all. A stub is merely unfinished work,
         * but an unknown target means the measurement and the guest's actual
         * behaviour disagree, which is both worse and more urgent. So this is
         * never summarised away and never rate-limited: every single occurrence
         * reports, including the hundredth from the same address. */
        unknown_hits++;
        log_printer("d3d8: call to UNKNOWN address 0x%08x -- not in the measured "
                    "surface\n",
                    (unsigned)address);
        return 0;
    }

    /* Counted before dispatch so the backlog ranking sees implemented work too,
     * and so a handler that longjmps or aborts has still been recorded. */
    entry->call_count++;

    if (entry->state == D3D8_ENTRY_IMPLEMENTED && entry->handler) {
        return entry->handler(context);
    }

    if (!entry->reported) {
        /* Once per address, not once per call: a stub inside a per-frame draw
         * loop would otherwise bury every other diagnostic within a second. */
        entry->reported = true;
        if (entry->name) {
            log_printer("d3d8: %s (0x%08x) is not implemented, returning %#x\n",
                        entry->name, (unsigned)entry->address,
                        (unsigned)entry->default_return);
        } else {
            /* 66 of the 85 measured functions have no recovered name, so the
             * nameless shape is the common one and says what we do know. */
            log_printer("d3d8: unnamed function at 0x%08x (%u call sites) is not "
                        "implemented, returning %#x\n",
                        (unsigned)entry->address, (unsigned)entry->sites,
                        (unsigned)entry->default_return);
        }
    }
    return entry->default_return;
}

size_t d3d8_hle_count(void)
{
    return entry_count;
}

size_t d3d8_hle_implemented_count(void)
{
    size_t done = 0;
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].state == D3D8_ENTRY_IMPLEMENTED) {
            done++;
        }
    }
    return done;
}

uint64_t d3d8_hle_unknown_count(void)
{
    return unknown_hits;
}

/**
 * Backlog ranking key.
 *
 * call_count first, then sites, then address. The middle term is the point: before
 * the guest has ever run every call count is 0, the first term ties for every row,
 * and the report degrades gracefully into the measured site ranking -- which is
 * exactly the backlog priority we want on day one. Address last so the output is
 * deterministic and diffable rather than dependent on table order.
 */
typedef struct {
    uint64_t call_count;
    uint32_t sites;
    uint32_t address;
} backlog_key;

static backlog_key key_of(const d3d8_entry *entry)
{
    backlog_key key = {entry->call_count, entry->sites, entry->address};
    return key;
}

/** Positive when `left` should be printed before `right`. */
static int outranks(backlog_key left, backlog_key right)
{
    if (left.call_count != right.call_count) {
        return left.call_count > right.call_count ? 1 : -1;
    }
    if (left.sites != right.sites) {
        return left.sites > right.sites ? 1 : -1;
    }
    if (left.address != right.address) {
        /* Lower address first, purely for a stable order. */
        return left.address < right.address ? 1 : -1;
    }
    return 0;
}

void d3d8_hle_report_backlog(void)
{
    size_t missing = 0;
    for (size_t i = 0; i < entry_count; i++) {
        if (entries[i].state != D3D8_ENTRY_IMPLEMENTED) {
            missing++;
        }
    }

    log_printer("d3d8: %zu of %zu measured functions still need implementations\n",
                missing, entry_count);
    if (unknown_hits > 0) {
        /* Surfaced in the summary as well as per call, because a surface that
         * disagrees with the guest invalidates the ranking below it. */
        log_printer("d3d8: %llu call(s) hit addresses absent from the measured "
                    "surface\n",
                    (unsigned long long)unknown_hits);
    }

    /* Selection sort by descending key. The key is a total order over distinct
     * addresses, so "already printed" is just "outranks the previous winner",
     * which keeps this allocation-free -- a report that fails to allocate is a
     * report we do not get, in the one situation where we need it most. 85 rows
     * printed once makes the quadratic cost irrelevant. */
    bool have_previous = false;
    backlog_key previous = {0, 0, 0};
    for (size_t printed = 0; printed < missing; printed++) {
        const d3d8_entry *best = NULL;
        backlog_key best_key = {0, 0, 0};
        for (size_t i = 0; i < entry_count; i++) {
            const d3d8_entry *candidate = &entries[i];
            if (candidate->state == D3D8_ENTRY_IMPLEMENTED) {
                continue;
            }
            backlog_key candidate_key = key_of(candidate);
            if (have_previous && outranks(candidate_key, previous) >= 0) {
                continue; /* already printed */
            }
            if (!best || outranks(candidate_key, best_key) > 0) {
                best = candidate;
                best_key = candidate_key;
            }
        }
        if (!best) {
            break;
        }
        log_printer("  0x%08x  %-40s  sites %4u  calls %llu\n",
                    (unsigned)best->address, best->name ? best->name : "<unnamed>",
                    (unsigned)best->sites, (unsigned long long)best->call_count);
        previous = best_key;
        have_previous = true;
    }
}
