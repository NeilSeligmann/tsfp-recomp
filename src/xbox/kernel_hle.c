/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See kernel_hle.h for why the diagnostics matter more than the dispatch.
 */

#include "kernel_hle.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static kernel_entry entries[XBOX_KERNEL_ORDINAL_MAX + 1];
static bool initialised;

static int default_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vfprintf(stderr, format, args);
    va_end(args);
    return written;
}

static kernel_log_fn log_printer = default_printer;

void kernel_hle_set_log(kernel_log_fn printer)
{
    log_printer = printer ? printer : default_printer;
}

kernel_log_fn kernel_hle_log(void)
{
    return log_printer;
}

#include <stdlib.h>

#if defined(__GNUC__) || defined(__clang__)
#define KERNEL_HLE_TLS __thread
#else
#define KERNEL_HLE_TLS
#endif

/* Thread-local because the pointer is BORROWED by the hook (the host keeps it in
 * its stop record), and two guest threads reaching fatal calls at once must not
 * race over one buffer. Same reasoning as the host's firmware_detail. */
/* 224 holds the widest composed detail (a raise with flags, four Info dwords and
 * the elision marker runs to about 170 characters) without truncation. */
static KERNEL_HLE_TLS char fatal_detail[224];

static void default_fatal(unsigned ordinal, const char *detail)
{
    (void)ordinal;
    (void)detail;
    /* No host attached: there is no stop machinery to unwind into, and returning
     * would continue a guest the real console has already halted. */
    abort();
}

static kernel_fatal_fn fatal_hook = default_fatal;

void kernel_hle_set_fatal(kernel_fatal_fn handler)
{
    fatal_hook = handler ? handler : default_fatal;
}

void kernel_hle_fatal(unsigned ordinal, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    (void)vsnprintf(fatal_detail, sizeof(fatal_detail), format, args);
    va_end(args);
    log_printer("kernel: FATAL ordinal %u (%s): %s\n", ordinal,
                xbox_kernel_ordinal_name(ordinal) ? xbox_kernel_ordinal_name(ordinal)
                                                  : "<unknown ordinal>",
                fatal_detail);
    fatal_hook(ordinal, fatal_detail);
}

void kernel_hle_init(void)
{
    memset(entries, 0, sizeof(entries));
    for (unsigned ordinal = 0; ordinal <= XBOX_KERNEL_ORDINAL_MAX; ordinal++) {
        const char *name = xbox_kernel_ordinal_name(ordinal);
        if (!name) {
            continue;
        }
        entries[ordinal].name = name;
        entries[ordinal].state = KERNEL_ENTRY_STUB;
    }
    initialised = true;
}

static kernel_entry *mutable_entry(unsigned ordinal)
{
    if (!initialised) {
        kernel_hle_init();
    }
    if (ordinal > XBOX_KERNEL_ORDINAL_MAX) {
        return NULL;
    }
    if (entries[ordinal].state == KERNEL_ENTRY_ABSENT && !entries[ordinal].name) {
        return NULL;
    }
    return &entries[ordinal];
}

bool kernel_hle_register(unsigned ordinal, kernel_fn handler)
{
    kernel_entry *entry = mutable_entry(ordinal);
    if (!entry || !handler) {
        return false;
    }
    entry->handler = handler;
    entry->state = KERNEL_ENTRY_IMPLEMENTED;
    return true;
}

bool kernel_hle_set_default_return(unsigned ordinal, uint32_t value)
{
    kernel_entry *entry = mutable_entry(ordinal);
    if (!entry) {
        return false;
    }
    entry->default_return = value;
    return true;
}

const kernel_entry *kernel_hle_entry(unsigned ordinal)
{
    return mutable_entry(ordinal);
}

uint32_t kernel_hle_call(unsigned ordinal, void *context)
{
    kernel_entry *entry = mutable_entry(ordinal);
    if (!entry) {
        /* Not a known ordinal at all. This means the guest's import table and our
         * table disagree, which is a different and worse problem than a missing
         * implementation, so it reports every single time. */
        log_printer("kernel: call to UNKNOWN ordinal %u -- import table mismatch\n",
                    ordinal);
        return 0;
    }

    entry->call_count++;

    if (entry->state == KERNEL_ENTRY_IMPLEMENTED && entry->handler) {
        return entry->handler(context);
    }

    if (!entry->reported) {
        entry->reported = true;
        log_printer("kernel: ordinal %u (%s) is not implemented, returning %#x\n",
                    ordinal, entry->name, entry->default_return);
    }
    return entry->default_return;
}

size_t kernel_hle_implemented_count(const unsigned *ordinals, size_t count)
{
    if (!ordinals) {
        return 0;
    }
    size_t done = 0;
    for (size_t i = 0; i < count; i++) {
        const kernel_entry *entry = kernel_hle_entry(ordinals[i]);
        if (entry && entry->state == KERNEL_ENTRY_IMPLEMENTED) {
            done++;
        }
    }
    return done;
}

void kernel_hle_report_missing(const unsigned *ordinals, size_t count)
{
    if (!ordinals) {
        return;
    }
    if (!initialised) {
        kernel_hle_init();
    }

    /* Selection sort over call_count. The list is at most a few hundred entries
     * and this runs once, so clarity beats cleverness. */
    size_t missing = 0;
    for (size_t i = 0; i < count; i++) {
        const kernel_entry *entry = kernel_hle_entry(ordinals[i]);
        if (entry && entry->state != KERNEL_ENTRY_IMPLEMENTED) {
            missing++;
        }
    }

    log_printer("kernel: %zu of %zu imported ordinals still need implementations\n",
                missing, count);

    bool emitted[XBOX_KERNEL_ORDINAL_MAX + 1] = {false};
    for (size_t printed = 0; printed < missing; printed++) {
        unsigned best = 0;
        uint64_t best_calls = 0;
        bool found = false;
        for (size_t i = 0; i < count; i++) {
            unsigned ordinal = ordinals[i];
            if (ordinal > XBOX_KERNEL_ORDINAL_MAX || emitted[ordinal]) {
                continue;
            }
            const kernel_entry *entry = kernel_hle_entry(ordinal);
            if (!entry || entry->state == KERNEL_ENTRY_IMPLEMENTED) {
                continue;
            }
            if (!found || entry->call_count > best_calls) {
                found = true;
                best = ordinal;
                best_calls = entry->call_count;
            }
        }
        if (!found) {
            break;
        }
        emitted[best] = true;
        const kernel_entry *entry = kernel_hle_entry(best);
        log_printer("  ordinal %3u  %-40s calls %llu\n", best, entry->name,
                    (unsigned long long)entry->call_count);
    }
}
