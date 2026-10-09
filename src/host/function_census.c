/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See function_census.h.
 */

#include "function_census.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "kernel_call.h"
#include "thunk_trace.h"

#define HASH_SLOTS (FUNCTION_CENSUS_TARGETS * 2u)
#define NO_ROW UINT16_MAX
/* The targets printed in the "still called" block, busiest first. */
#define STILL_CALLED_LISTED 64u

_Static_assert(FUNCTION_CENSUS_TARGETS < UINT16_MAX, "row index type");

static pthread_mutex_t census_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile bool census_on;
static uint64_t (*present_source)(void);
static uint64_t window_first = 1u;
static uint64_t window_last;

static function_census_target rows[FUNCTION_CENSUS_TARGETS];
static uint16_t hash[HASH_SLOTS];
static size_t row_count;
static uint64_t total;
static uint64_t overflow;
static function_census_event events[FUNCTION_CENSUS_TRACE];
static size_t event_count;
static uint64_t event_overflow;
static bool ready; /* the hash is empty (0xFF) only after the first clear */

static size_t slot_of(uint32_t target)
{
    uint32_t mixed = target * 2654435761u;
    return (size_t)(mixed >> 7) & (HASH_SLOTS - 1u);
}

static void clear_locked(void)
{
    memset(rows, 0, sizeof rows);
    memset(hash, 0xFF, sizeof hash);
    row_count = 0u;
    total = 0u;
    overflow = 0u;
    event_count = 0u;
    event_overflow = 0u;
    ready = true;
}

static function_census_target *find_locked(uint32_t target, bool create)
{
    if (!ready) {
        clear_locked();
    }
    size_t slot = slot_of(target);
    for (size_t probe = 0u; probe < HASH_SLOTS; probe++) {
        const uint16_t index = hash[slot];
        if (index == NO_ROW) {
            if (!create || row_count >= FUNCTION_CENSUS_TARGETS) {
                return NULL;
            }
            hash[slot] = (uint16_t)row_count;
            function_census_target *row = &rows[row_count++];
            row->target = target;
            return row;
        }
        if (rows[index].target == target) {
            return &rows[index];
        }
        slot = (slot + 1u) & (HASH_SLOTS - 1u);
    }
    return NULL;
}

void function_census_enable(bool enabled)
{
    pthread_mutex_lock(&census_lock);
    clear_locked();
    census_on = enabled;
    pthread_mutex_unlock(&census_lock);
}

bool function_census_enabled(void)
{
    return census_on;
}

void function_census_set_present_source(uint64_t (*present)(void))
{
    pthread_mutex_lock(&census_lock);
    present_source = present;
    pthread_mutex_unlock(&census_lock);
}

void function_census_set_window(uint64_t first, uint64_t last)
{
    pthread_mutex_lock(&census_lock);
    window_first = first;
    window_last = last;
    pthread_mutex_unlock(&census_lock);
}

void function_census_note(uint32_t target, uint32_t guest_esp)
{
    if (!census_on) {
        return;
    }
    uint32_t caller = 0u;
    (void)kernel_guest_read_u32((kernel_guest_ptr)guest_esp, &caller);
    const unsigned thread = thunk_trace_thread_id();
    pthread_mutex_lock(&census_lock);
    if (!census_on) {
        pthread_mutex_unlock(&census_lock);
        return;
    }
    const uint64_t present = present_source != NULL ? present_source() : 0u;
    total++;
    function_census_target *row = find_locked(target, true);
    if (row == NULL) {
        overflow++;
    } else {
        if (row->calls == 0u) {
            row->thread = thread;
            row->first_caller = caller;
            row->first_present = present;
            row->first_sequence = total;
            row->presents_called = 1u;
            row->last_present = present;
            row->calls_in_last = 0u;
        } else if (row->last_present != present) {
            row->calls_in_previous = row->calls_in_last;
            row->previous_present_gap = (uint32_t)(present - row->last_present);
            row->last_present = present;
            row->calls_in_last = 0u;
            row->presents_called++;
        }
        row->calls++;
        row->calls_in_last++;
    }
    if (window_last >= window_first && present >= window_first && present <= window_last) {
        if (event_count < FUNCTION_CENSUS_TRACE) {
            function_census_event *event = &events[event_count++];
            event->present = present;
            event->sequence = total;
            event->target = target;
            event->caller = caller;
            event->thread = thread;
        } else {
            event_overflow++;
        }
    }
    pthread_mutex_unlock(&census_lock);
}

uint64_t function_census_total(void)
{
    pthread_mutex_lock(&census_lock);
    const uint64_t value = total;
    pthread_mutex_unlock(&census_lock);
    return value;
}

size_t function_census_target_count(void)
{
    pthread_mutex_lock(&census_lock);
    const size_t value = row_count;
    pthread_mutex_unlock(&census_lock);
    return value;
}

uint64_t function_census_overflow(void)
{
    pthread_mutex_lock(&census_lock);
    const uint64_t value = overflow;
    pthread_mutex_unlock(&census_lock);
    return value;
}

bool function_census_get(uint32_t target, function_census_target *out)
{
    pthread_mutex_lock(&census_lock);
    const function_census_target *row = find_locked(target, false);
    if (row != NULL && out != NULL) {
        *out = *row;
    }
    pthread_mutex_unlock(&census_lock);
    return row != NULL;
}

size_t function_census_targets(function_census_target *out, size_t capacity)
{
    pthread_mutex_lock(&census_lock);
    const size_t count = row_count < capacity ? row_count : capacity;
    if (out != NULL && count > 0u) {
        memcpy(out, rows, count * sizeof rows[0]);
    }
    pthread_mutex_unlock(&census_lock);
    return count;
}

size_t function_census_events(function_census_event *out, size_t capacity)
{
    pthread_mutex_lock(&census_lock);
    const size_t count = event_count < capacity ? event_count : capacity;
    if (out != NULL && count > 0u) {
        memcpy(out, events, count * sizeof events[0]);
    }
    pthread_mutex_unlock(&census_lock);
    return count;
}

uint64_t function_census_event_overflow(void)
{
    pthread_mutex_lock(&census_lock);
    const uint64_t value = event_overflow;
    pthread_mutex_unlock(&census_lock);
    return value;
}

static int busiest_first(const void *left, const void *right)
{
    const function_census_target *a = *(const function_census_target *const *)left;
    const function_census_target *b = *(const function_census_target *const *)right;
    if (a->calls_in_last != b->calls_in_last) {
        return a->calls_in_last < b->calls_in_last ? 1 : -1;
    }
    return a->target < b->target ? -1 : a->target > b->target;
}

void function_census_report(FILE *out)
{
    pthread_mutex_lock(&census_lock);
    uint64_t final_present = 0u;
    for (size_t index = 0u; index < row_count; index++) {
        if (rows[index].last_present > final_present) {
            final_present = rows[index].last_present;
        }
    }
    fprintf(out,
            "function census (opt-in, T821, read-only): %llu indirect calls, %zu distinct targets (%llu calls not counted "
            "per target: table full), last present seen %llu\n",
            (unsigned long long)total, row_count, (unsigned long long)overflow,
            (unsigned long long)final_present);
    fprintf(out, "  census first present sequence thread caller target calls last gap\n");
    for (size_t index = 0u; index < row_count; index++) {
        const function_census_target *row = &rows[index];
        fprintf(out,
                "  census target 0x%08X first present %llu seq %llu thread %u caller 0x%08X calls %llu last present %llu "
                "presents %llu in last %llu before %llu gap %u\n",
                row->target, (unsigned long long)row->first_present, (unsigned long long)row->first_sequence,
                row->thread, row->first_caller, (unsigned long long)row->calls, (unsigned long long)row->last_present,
                (unsigned long long)row->presents_called, (unsigned long long)row->calls_in_last,
                (unsigned long long)row->calls_in_previous, row->previous_present_gap);
    }
    const function_census_target *live[FUNCTION_CENSUS_TARGETS];
    size_t live_count = 0u;
    for (size_t index = 0u; index < row_count; index++) {
        if (rows[index].last_present + 1u >= final_present) {
            live[live_count++] = &rows[index];
        }
    }
    qsort(live, live_count, sizeof live[0], busiest_first);
    fprintf(out, "  census still called in the last two presents (%zu targets, busiest per present first):\n", live_count);
    for (size_t index = 0u; index < live_count && index < STILL_CALLED_LISTED; index++) {
        fprintf(out, "  census live 0x%08X in last present %llu total %llu\n", live[index]->target,
                (unsigned long long)live[index]->calls_in_last, (unsigned long long)live[index]->calls);
    }
    if (event_count > 0u || window_last >= window_first) {
        fprintf(out, "  census window presents %llu..%llu: %zu indirect calls in order (%llu more not kept)\n",
                (unsigned long long)window_first, (unsigned long long)window_last, event_count,
                (unsigned long long)event_overflow);
        for (size_t index = 0u; index < event_count; index++) {
            fprintf(out, "  census call present %llu seq %llu thread %u target 0x%08X caller 0x%08X\n",
                    (unsigned long long)events[index].present, (unsigned long long)events[index].sequence,
                    events[index].thread, events[index].target, events[index].caller);
        }
    }
    pthread_mutex_unlock(&census_lock);
}

/* T1502 phase dump. Format (one fact per line, `#` lines are the header):
 *   # census-phase 1
 *   # calls N targets M overflow K first_present A last_present B
 *   target 0xVA calls C presents P first_present F last_present L in_last I first_caller 0xR thread T
 * `overflow` is the calls that found the 4096-target table full in this phase (not attributed to any target). */
bool function_census_dump_phase(const char *path)
{
    function_census_target *copy = malloc(sizeof rows);
    if (copy == NULL) {
        return false;
    }
    pthread_mutex_lock(&census_lock);
    if (!ready) {
        clear_locked();
    }
    const size_t count = row_count;
    const uint64_t calls = total;
    const uint64_t dropped = overflow;
    memcpy(copy, rows, count * sizeof rows[0]);
    clear_locked();
    pthread_mutex_unlock(&census_lock);
    uint64_t first = 0u;
    uint64_t last = 0u;
    for (size_t index = 0u; index < count; index++) {
        if (index == 0u || copy[index].first_present < first) {
            first = copy[index].first_present;
        }
        if (copy[index].last_present > last) {
            last = copy[index].last_present;
        }
    }
    FILE *file = fopen(path, "w");
    if (file != NULL) {
        fprintf(file, "# census-phase 1\n# calls %llu targets %zu overflow %llu first_present %llu last_present %llu\n",
                (unsigned long long)calls, count, (unsigned long long)dropped, (unsigned long long)first,
                (unsigned long long)last);
        for (size_t index = 0u; index < count; index++) {
            const function_census_target *row = &copy[index];
            fprintf(file,
                    "target 0x%08X calls %llu presents %llu first_present %llu last_present %llu in_last %llu "
                    "first_caller 0x%08X thread %u\n",
                    row->target, (unsigned long long)row->calls, (unsigned long long)row->presents_called,
                    (unsigned long long)row->first_present, (unsigned long long)row->last_present,
                    (unsigned long long)row->calls_in_last, row->first_caller, row->thread);
        }
        fclose(file);
    }
    free(copy);
    return file != NULL;
}

static char phase_path[512];
static pthread_t phase_thread;
static atomic_bool phase_run;
static atomic_uint phase_requests;
static unsigned phase_served;
static unsigned phase_number;
static struct sigaction phase_previous;
static bool phase_started;

static void on_phase_signal(int signal_number)
{
    atomic_fetch_add(&phase_requests, 1u);
    if (phase_previous.sa_handler != SIG_DFL && phase_previous.sa_handler != SIG_IGN && phase_previous.sa_handler != NULL &&
        (phase_previous.sa_flags & SA_SIGINFO) == 0) {
        phase_previous.sa_handler(signal_number);
    }
}

static void *phase_main(void *argument)
{
    (void)argument;
    while (atomic_load(&phase_run)) {
        if (atomic_load(&phase_requests) == phase_served) {
            struct timespec nap = {0, 20000000L};
            nanosleep(&nap, NULL);
            continue;
        }
        phase_served++;
        char phase[64];
        snprintf(phase, sizeof phase, "%u", ++phase_number);
        char control_path[sizeof phase_path + 8u];
        snprintf(control_path, sizeof control_path, "%s.phase", phase_path);
        FILE *control = fopen(control_path, "r");
        if (control != NULL) {
            char word[64] = "";
            if (fscanf(control, "%63s", word) == 1) {
                size_t kept = 0u;
                char clean[64];
                for (size_t index = 0u; word[index] != '\0'; index++) {
                    const char c = word[index];
                    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
                        clean[kept++] = c;
                    }
                }
                if (kept > 0u) {
                    clean[kept] = '\0';
                    strcpy(phase, clean);
                }
            }
            fclose(control);
        }
        char out_path[sizeof phase_path + 80u];
        snprintf(out_path, sizeof out_path, "%s.%s", phase_path, phase);
        (void)function_census_dump_phase(out_path);
        char ack_path[sizeof phase_path + 8u];
        snprintf(ack_path, sizeof ack_path, "%s.ack", phase_path);
        FILE *ack = fopen(ack_path, "w");
        if (ack != NULL) {
            fprintf(ack, "%u %s\n", phase_served, phase);
            fclose(ack);
        }
    }
    return NULL;
}

bool function_census_phases_start(const char *path)
{
    if (phase_started || path == NULL || strlen(path) >= sizeof phase_path) {
        return false;
    }
    strcpy(phase_path, path);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_phase_signal;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    memset(&phase_previous, 0, sizeof phase_previous);
    if (sigaction(SIGUSR1, &action, &phase_previous) != 0) {
        return false;
    }
    atomic_store(&phase_run, true);
    if (pthread_create(&phase_thread, NULL, phase_main, NULL) != 0) {
        sigaction(SIGUSR1, &phase_previous, NULL);
        return false;
    }
    phase_started = true;
    char pid_path[sizeof phase_path + 8u];
    snprintf(pid_path, sizeof pid_path, "%s.pid", phase_path);
    FILE *pid_file = fopen(pid_path, "w");
    if (pid_file != NULL) {
        fprintf(pid_file, "%d\n", (int)getpid());
        fclose(pid_file);
    }
    return true;
}

void function_census_phases_stop(void)
{
    if (!phase_started) {
        return;
    }
    atomic_store(&phase_run, false);
    pthread_join(phase_thread, NULL);
    sigaction(SIGUSR1, &phase_previous, NULL);
    phase_started = false;
}
