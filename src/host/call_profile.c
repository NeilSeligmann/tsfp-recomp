/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See call_profile.h.
 */

#include "call_profile.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "guest_mem.h"
#include "host_runtime.h"
#include "kernel_call.h"
#include "recomp_abi.h"

typedef struct {
    bool used;
    call_profile_row row;
} table_slot;

#define NEST_DEPTH 16u

typedef struct {
    call_profile_thread view;
    uint64_t head; /* next write position in the ring, monotonic */
    /* Ring positions of the calls that have not returned, outermost first. A handler can make a
     * nested dispatch, so "the last call" is not always the one that returns next. */
    uint64_t open[NEST_DEPTH];
    unsigned depth;
    struct timespec last;
    struct timespec open_start[NEST_DEPTH]; /* T1250 gaps: wall time each open call started */
    bool touched;
} thread_slot;

/* T1250 gaps: slow call log. A call that took at least SLOW_CALL_MS of wall time (a blocking wait, a disc read, a lock) or a
 * guest compute stretch of at least SLOW_GAP_MS between two dispatches of one thread (no HLE call in between: a long frame,
 * a decode, a thread that was scheduled out). Absolute CLOCK_MONOTONIC ms, the clock the audio sink's stall log prints. */
#define SLOW_CALL_MS 20
#define SLOW_GAP_MS 40
#define SLOW_LOG_MAX 4096u
typedef struct {
    uint64_t clock_ms; /* when it started */
    uint64_t duration_ms;
    unsigned thread;
    thunk_kind kind;
    uint32_t id;
    uint32_t caller;
    bool compute_gap; /* a gap between calls, id/caller name the call that ended it */
} slow_entry;
static slow_entry slow_log[SLOW_LOG_MAX];
static size_t slow_count;
static uint64_t slow_dropped;

static uint64_t ts_ms(const struct timespec *at)
{
    return (uint64_t)at->tv_sec * 1000u + (uint64_t)at->tv_nsec / 1000000u;
}

static int64_t ts_diff_ms(const struct timespec *later, const struct timespec *earlier)
{
    return ((int64_t)later->tv_sec - (int64_t)earlier->tv_sec) * 1000 + ((int64_t)later->tv_nsec - (int64_t)earlier->tv_nsec) / 1000000;
}

static void slow_add(const struct timespec *start, int64_t duration_ms, unsigned thread, thunk_kind kind, uint32_t id,
                     uint32_t caller, bool gap)
{
    if (slow_count >= SLOW_LOG_MAX) {
        slow_dropped++;
        return;
    }
    slow_entry *entry = &slow_log[slow_count++];
    entry->clock_ms = ts_ms(start);
    entry->duration_ms = (uint64_t)duration_ms;
    entry->thread = thread;
    entry->kind = kind;
    entry->id = id;
    entry->caller = caller;
    entry->compute_gap = gap;
}

static pthread_mutex_t profile_lock = PTHREAD_MUTEX_INITIALIZER;
static bool profile_on;
static table_slot table[CALL_PROFILE_ROWS];
static size_t table_used;
static uint64_t total_counted;
static uint64_t dropped;
static thread_slot threads[CALL_PROFILE_THREADS + 1u];

static bool limit_set;
static thunk_kind limit_kind;
static uint32_t limit_id;
static uint64_t limit_count;
static uint64_t limit_seen;
static bool limit_hit;
static uint32_t stack_words[CALL_PROFILE_STACK];
static size_t stack_count;
static uint32_t code_low;
static uint32_t code_high;

void call_profile_set_code_range(uint32_t low, uint32_t high)
{
    pthread_mutex_lock(&profile_lock);
    code_low = low;
    code_high = high;
    pthread_mutex_unlock(&profile_lock);
}

/* The kind is not part of the hash: an ordinal and an address with the same number land on one
 * probe chain and the kind comparison alone keeps their rows apart. */
static uint64_t hash_key(uint32_t id, uint32_t caller)
{
    uint64_t value = ((uint64_t)id << 32) ^ caller;
    value ^= value >> 33;
    value *= UINT64_C(0xFF51AFD7ED558CCD);
    value ^= value >> 33;
    return value;
}

void call_profile_enable(bool enabled)
{
    pthread_mutex_lock(&profile_lock);
    memset(table, 0, sizeof(table));
    memset(threads, 0, sizeof(threads));
    slow_count = 0u;
    slow_dropped = 0u;
    table_used = 0u;
    total_counted = 0u;
    dropped = 0u;
    limit_set = false;
    limit_kind = THUNK_KIND_ORDINAL;
    limit_id = 0u;
    limit_count = 0u;
    limit_seen = 0u;
    limit_hit = false;
    stack_count = 0u;
    profile_on = enabled;
    pthread_mutex_unlock(&profile_lock);
}

bool call_profile_enabled(void)
{
    pthread_mutex_lock(&profile_lock);
    const bool on = profile_on;
    pthread_mutex_unlock(&profile_lock);
    return on;
}

bool call_profile_set_limit(thunk_kind kind, uint32_t id, uint64_t count)
{
    pthread_mutex_lock(&profile_lock);
    const bool ok = profile_on && count != 0u;
    if (ok) {
        limit_set = true;
        limit_kind = kind;
        limit_id = id;
        limit_count = count;
        limit_seen = 0u;
        limit_hit = false;
    }
    pthread_mutex_unlock(&profile_lock);
    return ok;
}

bool call_profile_limit_reached(void)
{
    pthread_mutex_lock(&profile_lock);
    const bool hit = limit_hit;
    pthread_mutex_unlock(&profile_lock);
    return hit;
}

static void touch(thread_slot *slot)
{
    slot->touched = true;
    (void)clock_gettime(CLOCK_MONOTONIC, &slot->last);
}

static void count_row(thunk_kind kind, uint32_t id, uint32_t caller)
{
    size_t index = (size_t)(hash_key(id, caller) % CALL_PROFILE_ROWS);
    for (size_t probe = 0u; probe < CALL_PROFILE_ROWS; probe++) {
        table_slot *slot = &table[index];
        if (!slot->used) {
            if (table_used >= CALL_PROFILE_ROWS - 1u) {
                dropped++;
                return;
            }
            slot->used = true;
            slot->row.kind = kind;
            slot->row.id = id;
            slot->row.caller = caller;
            slot->row.count = 1u;
            table_used++;
            return;
        }
        if (slot->row.kind == kind && slot->row.id == id && slot->row.caller == caller) {
            slot->row.count++;
            return;
        }
        index = (index + 1u) % CALL_PROFILE_ROWS;
    }
    dropped++;
}

/* True when `address` follows a call instruction in guest code: E8 rel32, or FF /2 in any of its
 * forms (2 to 7 bytes). Reads the guest through the kernel accessor, so an unmapped read is false. */
static bool follows_call(uint32_t address)
{
    if (address < code_low + 8u || address >= code_high) {
        return false;
    }
    uint8_t bytes[8];
    if (!kernel_guest_read_bytes(address - 8u, bytes, sizeof(bytes))) {
        return false;
    }
    if (bytes[3] == 0xE8u) {
        return true;
    }
    for (unsigned length = 2u; length <= 7u; length++) {
        if (bytes[8u - length] == 0xFFu && ((bytes[9u - length] >> 3) & 7u) == 2u) {
            return true;
        }
    }
    return false;
}

/* The stopping thread, before it stops. Reads the live guest stack above esp. */
static void scrape_stack(void)
{
    uint32_t words[1024];
    const uint32_t base = g_esp;
    size_t count = 0u;
    size_t available = 0u;
    /* Read in small steps so the end of the mapped stack stops the scan instead of failing it. */
    while (code_high != 0u && available < sizeof(words) / sizeof(words[0]) &&
           kernel_guest_read_bytes(base + (uint32_t)(available * 4u), &words[available], 16u)) {
        available += 4u;
    }
    uint32_t found[CALL_PROFILE_STACK];
    for (size_t index = 0u; index < available && count < CALL_PROFILE_STACK; index++) {
        if (follows_call(words[index])) {
            found[count++] = words[index];
        }
    }
    pthread_mutex_lock(&profile_lock);
    memcpy(stack_words, found, count * sizeof(found[0]));
    stack_count = count;
    pthread_mutex_unlock(&profile_lock);
}

size_t call_profile_stack(uint32_t *out, size_t capacity)
{
    pthread_mutex_lock(&profile_lock);
    const size_t count = stack_count < capacity ? stack_count : capacity;
    memcpy(out, stack_words, count * sizeof(out[0]));
    pthread_mutex_unlock(&profile_lock);
    return count;
}

void call_profile_note_dispatch(thunk_kind kind, unsigned ordinal, uint32_t address,
                                uint32_t return_address, bool known, uint32_t result)
{
    const uint32_t id = kind == THUNK_KIND_ORDINAL ? (uint32_t)ordinal : address;
    bool stop = false;
    pthread_mutex_lock(&profile_lock);
    if (!profile_on) {
        pthread_mutex_unlock(&profile_lock);
        return;
    }
    if (limit_hit) {
        stop = true;
    } else if (limit_set && kind == limit_kind && id == limit_id) {
        if (limit_seen >= limit_count) {
            limit_hit = true;
            stop = true;
        }
    }
    if (!stop) {
        if (limit_set && kind == limit_kind && id == limit_id) {
            limit_seen++;
        }
        total_counted++;
        count_row(kind, id, return_address);
        const unsigned thread = thunk_trace_thread_id();
        if (thread <= CALL_PROFILE_THREADS) {
            thread_slot *slot = &threads[thread];
            slot->view.thread = thread;
            slot->view.dispatches++;
            struct timespec arrival;
            (void)clock_gettime(CLOCK_MONOTONIC, &arrival);
            if (slot->touched && slot->depth == 0u && ts_diff_ms(&arrival, &slot->last) >= SLOW_GAP_MS) {
                slow_add(&slot->last, ts_diff_ms(&arrival, &slot->last), thread, kind, id, return_address, true);
            }
            call_profile_event *event = &slot->view.recent[slot->head % CALL_PROFILE_RING];
            event->kind = kind;
            event->id = id;
            event->caller = return_address;
            event->result = known ? result : 0u;
            event->returned = known;
            event->sequence = slot->view.dispatches;
            if (known) {
                slot->view.returns++;
            } else {
                if (slot->depth < NEST_DEPTH) {
                    slot->open[slot->depth] = slot->head;
                    slot->open_start[slot->depth] = arrival;
                }
                slot->depth++;
            }
            slot->head++;
            slot->view.current = *event;
            slot->view.in_flight = slot->depth != 0u;
            touch(slot);
        }
    }
    pthread_mutex_unlock(&profile_lock);
    if (stop && host_run_armed()) {
        if (stack_count == 0u) {
            scrape_stack();
        }
        host_run_stop(HOST_STOP_BUDGET, id, kind == THUNK_KIND_ORDINAL ? (unsigned)id : 0u,
                      "call profile limit reached (--stop-after-calls): the guest progress cut");
    }
}

void call_profile_note_return(uint32_t result)
{
    pthread_mutex_lock(&profile_lock);
    if (profile_on) {
        const unsigned thread = thunk_trace_thread_id();
        if (thread <= CALL_PROFILE_THREADS) {
            thread_slot *slot = &threads[thread];
            if (slot->depth != 0u) {
                slot->depth--;
                if (slot->depth < NEST_DEPTH) {
                    const uint64_t position = slot->open[slot->depth];
                    struct timespec finished;
                    (void)clock_gettime(CLOCK_MONOTONIC, &finished);
                    if (ts_diff_ms(&finished, &slot->open_start[slot->depth]) >= SLOW_CALL_MS &&
                        slot->head - position <= CALL_PROFILE_RING) {
                        const call_profile_event *slow_event = &slot->view.recent[position % CALL_PROFILE_RING];
                        slow_add(&slot->open_start[slot->depth], ts_diff_ms(&finished, &slot->open_start[slot->depth]), thread,
                                 slow_event->kind, slow_event->id, slow_event->caller, false);
                    }
                    if (slot->head - position <= CALL_PROFILE_RING) {
                        call_profile_event *event = &slot->view.recent[position % CALL_PROFILE_RING];
                        event->result = result;
                        event->returned = true;
                    }
                }
                slot->view.returns++;
                slot->view.in_flight = slot->depth != 0u;
                if (slot->depth != 0u && slot->depth <= NEST_DEPTH) {
                    const uint64_t position = slot->open[slot->depth - 1u];
                    if (slot->head - position <= CALL_PROFILE_RING) {
                        slot->view.current = slot->view.recent[position % CALL_PROFILE_RING];
                    }
                }
                touch(slot);
            }
        }
    }
    pthread_mutex_unlock(&profile_lock);
}

uint64_t call_profile_total(void)
{
    pthread_mutex_lock(&profile_lock);
    const uint64_t value = total_counted;
    pthread_mutex_unlock(&profile_lock);
    return value;
}

uint64_t call_profile_dropped(void)
{
    pthread_mutex_lock(&profile_lock);
    const uint64_t value = dropped;
    pthread_mutex_unlock(&profile_lock);
    return value;
}

static int compare_rows(const void *left, const void *right)
{
    const call_profile_row *a = left;
    const call_profile_row *b = right;
    if (a->count != b->count) {
        return a->count > b->count ? -1 : 1;
    }
    if (a->kind != b->kind) {
        return a->kind < b->kind ? -1 : 1;
    }
    if (a->id != b->id) {
        return a->id < b->id ? -1 : 1;
    }
    return a->caller < b->caller ? -1 : (a->caller > b->caller ? 1 : 0);
}

size_t call_profile_rows(call_profile_row *out, size_t capacity)
{
    size_t copied = 0u;
    pthread_mutex_lock(&profile_lock);
    for (size_t index = 0u; index < CALL_PROFILE_ROWS && copied < capacity; index++) {
        if (table[index].used) {
            out[copied++] = table[index].row;
        }
    }
    pthread_mutex_unlock(&profile_lock);
    qsort(out, copied, sizeof(out[0]), compare_rows);
    return copied;
}

uint64_t call_profile_count_of(thunk_kind kind, uint32_t id)
{
    uint64_t sum = 0u;
    pthread_mutex_lock(&profile_lock);
    for (size_t index = 0u; index < CALL_PROFILE_ROWS; index++) {
        if (table[index].used && table[index].row.kind == kind && table[index].row.id == id) {
            sum += table[index].row.count;
        }
    }
    pthread_mutex_unlock(&profile_lock);
    return sum;
}

size_t call_profile_threads(call_profile_thread *out, size_t capacity)
{
    size_t copied = 0u;
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&profile_lock);
    for (unsigned thread = 1u; thread <= CALL_PROFILE_THREADS && copied < capacity; thread++) {
        const thread_slot *slot = &threads[thread];
        if (slot->view.dispatches == 0u) {
            continue;
        }
        call_profile_thread *view = &out[copied++];
        *view = slot->view;
        const uint64_t held = slot->head < CALL_PROFILE_RING ? slot->head : CALL_PROFILE_RING;
        view->recent_count = (size_t)held;
        for (uint64_t index = 0u; index < held; index++) {
            view->recent[index] = slot->view.recent[(slot->head - held + index) % CALL_PROFILE_RING];
        }
        const int64_t seconds = (int64_t)now.tv_sec - (int64_t)slot->last.tv_sec;
        const int64_t nanos = (int64_t)now.tv_nsec - (int64_t)slot->last.tv_nsec;
        const int64_t millis = seconds * 1000 + nanos / 1000000;
        view->idle_ms = millis > 0 ? (uint64_t)millis : 0u;
    }
    pthread_mutex_unlock(&profile_lock);
    return copied;
}

static const char *label_of(const call_profile_names *names, thunk_kind kind, uint32_t id,
                            char *buffer, size_t length)
{
    const char *name = NULL;
    if (names != NULL) {
        if (kind == THUNK_KIND_ORDINAL && names->ordinal_name != NULL) {
            name = names->ordinal_name(id);
        } else if (kind == THUNK_KIND_XDK && names->xdk_name != NULL) {
            name = names->xdk_name(id);
        }
    }
    if (name != NULL) {
        return name;
    }
    (void)snprintf(buffer, length, kind == THUNK_KIND_ORDINAL ? "ordinal %u" : "-", id);
    return buffer;
}

static const char *kind_tag(thunk_kind kind)
{
    switch (kind) {
    case THUNK_KIND_ORDINAL: return "ord";
    case THUNK_KIND_XDK: return "xdk";
    case THUNK_KIND_MONITOR: return "mon";
    default: return "???";
    }
}

typedef struct {
    thunk_kind kind;
    uint32_t id;
    uint64_t count;
    size_t callers;
} address_total;

static int compare_totals(const void *left, const void *right)
{
    const address_total *a = left;
    const address_total *b = right;
    if (a->count != b->count) {
        return a->count > b->count ? -1 : 1;
    }
    if (a->kind != b->kind) {
        return a->kind < b->kind ? -1 : 1;
    }
    return a->id < b->id ? -1 : (a->id > b->id ? 1 : 0);
}

void call_profile_report(FILE *out, size_t top_addresses, const call_profile_names *names)
{
    call_profile_row *rows = malloc(CALL_PROFILE_ROWS * sizeof(*rows));
    address_total *totals = malloc(CALL_PROFILE_ROWS * sizeof(*totals));
    call_profile_thread *views = calloc(CALL_PROFILE_THREADS, sizeof(*views));
    if (rows == NULL || totals == NULL || views == NULL) {
        fprintf(out, "\ncall profile: out of memory, report skipped\n");
        free(rows);
        free(totals);
        free(views);
        return;
    }
    const size_t row_count = call_profile_rows(rows, CALL_PROFILE_ROWS);
    size_t total_count = 0u;
    for (size_t index = 0u; index < row_count; index++) {
        size_t found = total_count;
        for (size_t scan = 0u; scan < total_count; scan++) {
            if (totals[scan].kind == rows[index].kind && totals[scan].id == rows[index].id) {
                found = scan;
                break;
            }
        }
        if (found == total_count) {
            totals[total_count].kind = rows[index].kind;
            totals[total_count].id = rows[index].id;
            totals[total_count].count = 0u;
            totals[total_count].callers = 0u;
            total_count++;
        }
        totals[found].count += rows[index].count;
        totals[found].callers++;
    }
    qsort(totals, total_count, sizeof(totals[0]), compare_totals);

    fprintf(out, "\n--- call profile (T422, whole run, every dispatch counted) ---\n");
    fprintf(out, "dispatches counted %llu over %zu distinct address(es) and %zu (address, caller) "
                 "row(s), %llu row(s) dropped for a full table%s\n",
            (unsigned long long)call_profile_total(), total_count, row_count,
            (unsigned long long)call_profile_dropped(),
            call_profile_limit_reached() ? ", STOPPED by --stop-after-calls" : "");
    const size_t shown = top_addresses < total_count ? top_addresses : total_count;
    for (size_t index = 0u; index < shown; index++) {
        char buffer[32];
        fprintf(out, "%10llu  %s %08X  %-34s %zu caller(s)\n", (unsigned long long)totals[index].count,
                kind_tag(totals[index].kind), totals[index].id,
                label_of(names, totals[index].kind, totals[index].id, buffer, sizeof(buffer)),
                totals[index].callers);
        size_t listed = 0u;
        for (size_t scan = 0u; scan < row_count && listed < 3u; scan++) {
            if (rows[scan].kind == totals[index].kind && rows[scan].id == totals[index].id) {
                fprintf(out, "              from %08X  %llu\n", rows[scan].caller,
                        (unsigned long long)rows[scan].count);
                listed++;
            }
        }
    }
    if (shown < total_count) {
        fprintf(out, "  ... %zu more address(es)\n", total_count - shown);
    }

    uint32_t chain[CALL_PROFILE_STACK];
    const size_t chain_count = call_profile_stack(chain, CALL_PROFILE_STACK);
    if (chain_count != 0u) {
        fprintf(out, "\nguest stack of the stopping thread (return-like words above esp, innermost first,"
                     " a heuristic scrape):\n ");
        for (size_t index = 0u; index < chain_count; index++) {
            fprintf(out, " %08X", chain[index]);
        }
        fprintf(out, "\n");
    }

    const size_t thread_count = call_profile_threads(views, CALL_PROFILE_THREADS);
    for (size_t index = 0u; index < thread_count; index++) {
        const call_profile_thread *view = &views[index];
        char buffer[32];
        fprintf(out, "\nthread t%u: %llu dispatch(es), %llu returned, %s, idle %llu ms (WALL CLOCK)\n",
                view->thread, (unsigned long long)view->dispatches, (unsigned long long)view->returns,
                view->in_flight ? "INSIDE a call that has not returned" : "between calls",
                (unsigned long long)view->idle_ms);
        if (view->in_flight) {
            fprintf(out, "  current call  %s %08X %s from %08X\n", kind_tag(view->current.kind),
                    view->current.id,
                    label_of(names, view->current.kind, view->current.id, buffer, sizeof(buffer)),
                    view->current.caller);
        }
        fprintf(out, "  last %zu dispatch(es), oldest first:\n", view->recent_count);
        for (size_t scan = 0u; scan < view->recent_count; scan++) {
            const call_profile_event *event = &view->recent[scan];
            fprintf(out, "    #%llu %s %08X from %08X%s\n", (unsigned long long)event->sequence,
                    kind_tag(event->kind), event->id, event->caller,
                    event->returned ? "" : "  (not returned)");
        }
    }
    pthread_mutex_lock(&profile_lock);
    fprintf(out, "\nslow calls (T1250 gaps): %zu entries (%llu dropped), calls of %d ms or more and guest compute stretches of %d ms or more "
                 "between two dispatches of a thread; clock = CLOCK_MONOTONIC ms\n",
            slow_count, (unsigned long long)slow_dropped, SLOW_CALL_MS, SLOW_GAP_MS);
    for (size_t index = 0u; index < slow_count; index++) {
        const slow_entry *entry = &slow_log[index];
        char buffer[32];
        fprintf(out, "slow call  clock %llu ms  t%u  %s %s %08X %s from %08X  %llu ms\n", (unsigned long long)entry->clock_ms,
                entry->thread, entry->compute_gap ? "COMPUTE-GAP-BEFORE" : "CALL", kind_tag(entry->kind), entry->id,
                label_of(names, entry->kind, entry->id, buffer, sizeof(buffer)), entry->caller,
                (unsigned long long)entry->duration_ms);
    }
    pthread_mutex_unlock(&profile_lock);
    free(rows);
    free(totals);
    free(views);
}
