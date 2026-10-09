/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "route_probe.h"
#include "xinput_route_spec.h"
#include "xinput_source.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    route_cond_kind kind;
    char text[ROUTE_COND_TEXT];
    uint32_t va;
    _Atomic uint64_t count, bytes, last_ms;
} watch;

#define PATH_TABLE 64u
#define PATH_LEN 160u
typedef struct {
    char path[PATH_LEN];
    uint64_t opens, reads, read_bytes, writes, write_bytes, log_next_ms;
} path_entry;

typedef struct {
    uint64_t start_ms, events, opens, read_bytes, write_bytes, first_ms, last_ms;
    uint64_t hdd_events, hdd_last_ms; /* file I/O that is not the disc (saves, caches, partitions), see is_disc_path */
    char last_open[3][48];
    unsigned last_open_count;
    uint64_t frame_changes_at_start;
} segment;

typedef struct {
    char spec[48];
    route_cond cond;
    bool valid;
    uint32_t last;
} mem_entry;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool enabled;
static watch watches[ROUTE_PROBE_MAX_WATCHES];
static atomic_uint watch_count;
static atomic_uint call_watch_count;
static path_entry paths[PATH_TABLE];
static unsigned path_count;
static segment seg;
static uint64_t total_events, total_opens, total_reads, total_read_bytes, total_writes, total_write_bytes;
static _Atomic uint64_t frame_changes, frame_last_change_ms, frame_last_fp, frames_seen;
static uint64_t (*clock_fn)(void *user);
static void *clock_user;
static FILE *log_file;
static uint64_t log_start_ms;
static mem_entry mems[ROUTE_PROBE_MAX_MEM];
static unsigned mem_count;
static pthread_t mem_thread;
static atomic_bool mem_running;
static bool mem_thread_started;
static route_probe_read_fn mem_read;
static void *mem_read_user;
static unsigned mem_period_ms = 20u;

static uint64_t now_ms(void *user)
{
    (void)user;
    if (clock_fn != NULL) return clock_fn(clock_user);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u + 1u; /* never 0: 0 means "no event yet" */
}

void route_probe_set_clock(uint64_t (*fn)(void *user), void *user)
{
    clock_fn = fn;
    clock_user = user;
}

static void on_file_event(kernel_file_event_kind kind, const char *path, uint64_t bytes) { route_probe_note_file(kind, path, bytes); }

void route_probe_enable(void)
{
    if (atomic_exchange(&enabled, true)) return;
    pthread_mutex_lock(&lock);
    seg.start_ms = now_ms(NULL);
    pthread_mutex_unlock(&lock);
    kernel_file_set_observer(on_file_event);
}

bool route_probe_enabled(void) { return atomic_load(&enabled); }

void route_probe_reset(void)
{
    route_probe_mem_stop();
    route_probe_log_close();
    atomic_store(&enabled, false);
    kernel_file_set_observer(NULL);
    pthread_mutex_lock(&lock);
    atomic_store(&watch_count, 0u);
    atomic_store(&call_watch_count, 0u);
    for (unsigned i = 0u; i < ROUTE_PROBE_MAX_WATCHES; i++) {
        watches[i].kind = 0;
        watches[i].text[0] = '\0';
        watches[i].va = 0u;
        atomic_store(&watches[i].count, 0u);
        atomic_store(&watches[i].bytes, 0u);
        atomic_store(&watches[i].last_ms, 0u);
    }
    memset(paths, 0, sizeof paths);
    path_count = 0u;
    memset(&seg, 0, sizeof seg);
    total_events = total_opens = total_reads = total_read_bytes = total_writes = total_write_bytes = 0u;
    atomic_store(&frame_changes, 0u);
    atomic_store(&frame_last_change_ms, 0u);
    atomic_store(&frame_last_fp, 0u);
    atomic_store(&frames_seen, 0u);
    memset(mems, 0, sizeof mems);
    mem_count = 0u;
    clock_fn = NULL;
    clock_user = NULL;
    pthread_mutex_unlock(&lock);
}

/* ---- watches ---- */

static int watch_register(route_cond_kind kind, const char *text, uint32_t va, void *user)
{
    (void)user;
    if (kind != RCOND_FILE_OPEN && kind != RCOND_FILE_READ && kind != RCOND_FILE_IDLE && kind != RCOND_CALL) return -1;
    pthread_mutex_lock(&lock);
    const unsigned count = atomic_load(&watch_count);
    for (unsigned i = 0u; i < count; i++) {
        if (watches[i].kind == kind && watches[i].va == va && strcmp(watches[i].text, text != NULL ? text : "") == 0) {
            pthread_mutex_unlock(&lock);
            return (int)i;
        }
    }
    if (count >= ROUTE_PROBE_MAX_WATCHES) {
        pthread_mutex_unlock(&lock);
        return -1;
    }
    watches[count].kind = kind;
    snprintf(watches[count].text, sizeof watches[count].text, "%s", text != NULL ? text : "");
    watches[count].va = va;
    if (kind == RCOND_CALL) atomic_fetch_add(&call_watch_count, 1u);
    atomic_store(&watch_count, count + 1u);
    pthread_mutex_unlock(&lock);
    return (int)count;
}

static bool watch_state(int id, xinput_route_watch_state *out, void *user)
{
    (void)user;
    if (id < 0 || (unsigned)id >= atomic_load(&watch_count)) return false;
    out->count = atomic_load(&watches[id].count);
    out->bytes = atomic_load(&watches[id].bytes);
    out->last_ms = atomic_load(&watches[id].last_ms);
    return true;
}

static bool frame_state(xinput_route_frame_state *out, void *user)
{
    (void)user;
    out->changes = atomic_load(&frame_changes);
    out->last_change_ms = atomic_load(&frame_last_change_ms);
    return true;
}

const xinput_route_probe *route_probe_view(void)
{
    static const xinput_route_probe view = {now_ms, watch_register, watch_state, frame_state, NULL};
    return &view;
}

bool route_probe_calls_watched(void) { return atomic_load(&call_watch_count) != 0u; }

/* ---- the event log ---- */

static void log_vline(const char *format, va_list args)
{
    if (log_file == NULL) return;
    const uint64_t now = now_ms(NULL);
    fprintf(log_file, "t=%llu poll=%llu ", (unsigned long long)(now - log_start_ms),
            (unsigned long long)xinput_source_port_poll_count(0u));
    vfprintf(log_file, format, args);
    fputc('\n', log_file);
    fflush(log_file);
}

void route_probe_log_line(const char *format, ...)
{
    pthread_mutex_lock(&lock);
    va_list args;
    va_start(args, format);
    log_vline(format, args);
    va_end(args);
    pthread_mutex_unlock(&lock);
}

static void log_locked(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    log_vline(format, args);
    va_end(args);
}

bool route_probe_log_open(const char *path, char *error, size_t error_size)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        snprintf(error, error_size, "cannot write %s", path);
        return false;
    }
    pthread_mutex_lock(&lock);
    if (log_file != NULL) fclose(log_file);
    log_file = file;
    log_start_ms = now_ms(NULL);
    fprintf(log_file, "# tsfp route event log v1 (T1633): t = wall ms since the log opened, poll = port 0 pad poll index\n");
    fflush(log_file);
    pthread_mutex_unlock(&lock);
    return true;
}

void route_probe_log_close(void)
{
    pthread_mutex_lock(&lock);
    if (log_file != NULL) fclose(log_file);
    log_file = NULL;
    pthread_mutex_unlock(&lock);
}

bool route_probe_log_active(void) { return log_file != NULL; }

/* ---- file events ---- */

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '\\');
    const char *other = strrchr(path, '/');
    if (other != NULL && (slash == NULL || other > slash)) slash = other;
    return slash != NULL ? slash + 1 : path;
}

/* The disc (D: and the CD-ROM device) streams paks and music through every menu, the saves and caches are the rest. */
static bool is_disc_path(const char *lowered)
{
    return strncmp(lowered, "\\??\\d:", 6u) == 0 || strncmp(lowered, "d:", 2u) == 0 || strstr(lowered, "\\device\\cdrom") != NULL;
}

static path_entry *path_slot(const char *path)
{
    for (unsigned i = 0u; i < path_count; i++)
        if (strncmp(paths[i].path, path, PATH_LEN - 1u) == 0) return &paths[i];
    if (path_count >= PATH_TABLE) return NULL;
    path_entry *slot = &paths[path_count++];
    snprintf(slot->path, sizeof slot->path, "%s", path);
    return slot;
}

void route_probe_note_file(kernel_file_event_kind kind, const char *path, uint64_t bytes)
{
    if (!atomic_load(&enabled) || path == NULL) return;
    char lowered[PATH_LEN * 2u];
    size_t length = 0u;
    for (; path[length] != '\0' && length + 1u < sizeof lowered; length++) {
        const char c = path[length];
        lowered[length] = (char)((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
    }
    lowered[length] = '\0';
    const uint64_t now = now_ms(NULL);
    pthread_mutex_lock(&lock);
    const unsigned count = atomic_load(&watch_count);
    for (unsigned i = 0u; i < count; i++) {
        watch *w = &watches[i];
        if (w->kind == RCOND_CALL) continue;
        if (w->text[0] != '\0') { /* "!SUBSTR" matches the files that do NOT contain SUBSTR */
            const bool negated = w->text[0] == '!';
            const bool found = strstr(lowered, w->text + (negated ? 1 : 0)) != NULL;
            if (negated ? found : !found) continue;
        }
        if (w->kind == RCOND_FILE_OPEN && kind != KERNEL_FILE_EVENT_OPEN) continue;
        if (w->kind == RCOND_FILE_READ && kind != KERNEL_FILE_EVENT_READ) continue;
        atomic_fetch_add(&w->count, 1u);
        if (kind == KERNEL_FILE_EVENT_READ) atomic_fetch_add(&w->bytes, bytes);
        atomic_store(&w->last_ms, now);
    }
    total_events++;
    seg.events++;
    if (seg.first_ms == 0u) seg.first_ms = now;
    seg.last_ms = now;
    if (!is_disc_path(lowered)) {
        seg.hdd_events++;
        seg.hdd_last_ms = now;
    }
    path_entry *slot = path_slot(path);
    if (kind == KERNEL_FILE_EVENT_OPEN) {
        total_opens++;
        seg.opens++;
        const char *name = base_name(path);
        if (name[0] != '\0') {
            if (seg.last_open_count < 3u) {
                snprintf(seg.last_open[seg.last_open_count++], sizeof seg.last_open[0], "%s", name);
            } else {
                memmove(seg.last_open[0], seg.last_open[1], sizeof seg.last_open[0] * 2u);
                snprintf(seg.last_open[2], sizeof seg.last_open[2], "%s", name);
            }
        }
        if (slot != NULL) slot->opens++;
        log_locked("open %s", path);
    } else if (kind == KERNEL_FILE_EVENT_READ) {
        total_reads++;
        total_read_bytes += bytes;
        seg.read_bytes += bytes;
        if (slot != NULL) {
            slot->reads++;
            slot->read_bytes += bytes;
            if (now >= slot->log_next_ms) {
                log_locked("read %s reads=%llu bytes=%llu", path, (unsigned long long)slot->reads, (unsigned long long)slot->read_bytes);
                slot->log_next_ms = now + 250u;
            }
        }
    } else {
        total_writes++;
        total_write_bytes += bytes;
        seg.write_bytes += bytes;
        if (slot != NULL) {
            slot->writes++;
            slot->write_bytes += bytes;
            if (now >= slot->log_next_ms) {
                log_locked("write %s writes=%llu bytes=%llu", path, (unsigned long long)slot->writes, (unsigned long long)slot->write_bytes);
                slot->log_next_ms = now + 250u;
            }
        }
    }
    pthread_mutex_unlock(&lock);
}

void route_probe_note_call(uint32_t va)
{
    if (atomic_load(&call_watch_count) == 0u) return;
    const unsigned count = atomic_load(&watch_count);
    uint64_t now = 0u;
    for (unsigned i = 0u; i < count; i++) {
        watch *w = &watches[i];
        if (w->kind != RCOND_CALL || w->va != va) continue;
        if (now == 0u) now = now_ms(NULL);
        const uint64_t seen = atomic_fetch_add(&w->count, 1u) + 1u;
        atomic_store(&w->last_ms, now);
        if (seen <= 4u && log_file != NULL) route_probe_log_line("call 0x%X n=%llu", (unsigned)va, (unsigned long long)seen);
    }
}

void route_probe_note_frame(uint64_t fingerprint)
{
    if (!atomic_load(&enabled)) return;
    atomic_fetch_add(&frames_seen, 1u);
    if (atomic_exchange(&frame_last_fp, fingerprint) == fingerprint && atomic_load(&frame_last_change_ms) != 0u) return;
    atomic_fetch_add(&frame_changes, 1u);
    atomic_store(&frame_last_change_ms, now_ms(NULL));
}

/* ---- recorder facts ---- */

bool route_probe_mark_facts(unsigned mark, char *out, size_t out_size, void *user)
{
    (void)user;
    if (!atomic_load(&enabled)) return false;
    const uint64_t now = now_ms(NULL);
    pthread_mutex_lock(&lock);
    const uint64_t changes = atomic_load(&frame_changes);
    int n = snprintf(out, out_size, "seg-ms=%llu io=%llu opens=%llu read-bytes=%llu write-bytes=%llu first-io-ms=%llu last-io-ago-ms=%llu hdd-io=%llu hdd-last-io-ago-ms=%llu frame-changes=%llu",
                     (unsigned long long)(now - seg.start_ms), (unsigned long long)seg.events, (unsigned long long)seg.opens,
                     (unsigned long long)seg.read_bytes, (unsigned long long)seg.write_bytes,
                     (unsigned long long)(seg.first_ms != 0u ? seg.first_ms - seg.start_ms : 0u),
                     (unsigned long long)(seg.last_ms != 0u ? now - seg.last_ms : 0u), (unsigned long long)seg.hdd_events,
                     (unsigned long long)(seg.hdd_last_ms != 0u ? now - seg.hdd_last_ms : 0u),
                     (unsigned long long)(changes - seg.frame_changes_at_start));
    if (n > 0 && (size_t)n < out_size && seg.last_open_count != 0u) {
        n += snprintf(out + n, out_size - (size_t)n, " last-open=");
        for (unsigned i = 0u; i < seg.last_open_count && (size_t)n < out_size; i++)
            n += snprintf(out + n, out_size - (size_t)n, "%s%s", i != 0u ? "," : "", seg.last_open[i]);
    }
    log_locked("mark %u seg-ms=%llu io=%llu opens=%llu read-bytes=%llu", mark, (unsigned long long)(now - seg.start_ms),
               (unsigned long long)seg.events, (unsigned long long)seg.opens, (unsigned long long)seg.read_bytes);
    memset(&seg, 0, sizeof seg);
    seg.start_ms = now;
    seg.frame_changes_at_start = changes;
    pthread_mutex_unlock(&lock);
    return true;
}

/* ---- sampled memory ---- */

bool route_probe_mem_add(const char *spec, char *error, size_t error_size)
{
    char text[96];
    if (spec == NULL || mem_count >= ROUTE_PROBE_MAX_MEM || strlen(spec) >= sizeof mems[0].spec ||
        snprintf(text, sizeof text, "mark1:mem=%s==0", spec) >= (int)sizeof text) {
        snprintf(error, error_size, "--route-log-mem '%s': too long, or more than %u", spec != NULL ? spec : "", ROUTE_PROBE_MAX_MEM);
        return false;
    }
    xinput_route_wait wait;
    if (!xinput_route_event_wait_parse(text, &wait, error, error_size) || wait.cond_count != 1u || wait.conds[0].kind != RCOND_MEM) {
        snprintf(error, error_size, "--route-log-mem '%s': want [*]ADDR[+OFF][:W] (W = 1, 2 or 4)", spec);
        return false;
    }
    mem_entry *entry = &mems[mem_count++];
    snprintf(entry->spec, sizeof entry->spec, "%s", spec);
    entry->cond = wait.conds[0];
    return true;
}

void route_probe_mem_sample(route_probe_read_fn read, void *user)
{
    for (unsigned i = 0u; i < mem_count; i++) {
        mem_entry *entry = &mems[i];
        uint32_t address = entry->cond.address, value = 0u;
        bool ok = true;
        if (entry->cond.indirect) {
            uint32_t pointer = 0u;
            ok = read(address, 4u, &pointer, user) && pointer != 0u;
            address = pointer + entry->cond.offset;
        }
        ok = ok && read(address, entry->cond.width, &value, user);
        if (!ok) {
            if (entry->valid) {
                entry->valid = false;
                route_probe_log_line("mem %s %s -> unreadable", entry->spec, "value");
            }
            continue;
        }
        if (!entry->valid || value != entry->last) {
            if (entry->valid)
                route_probe_log_line("mem %s 0x%X -> 0x%X", entry->spec, (unsigned)entry->last, (unsigned)value);
            else
                route_probe_log_line("mem %s = 0x%X", entry->spec, (unsigned)value);
            entry->valid = true;
            entry->last = value;
        }
    }
}

static void *mem_main(void *unused)
{
    (void)unused;
    while (atomic_load(&mem_running)) {
        route_probe_mem_sample(mem_read, mem_read_user);
        struct timespec nap = {0, (long)mem_period_ms * 1000000L};
        nanosleep(&nap, NULL);
    }
    return NULL;
}

bool route_probe_mem_start(route_probe_read_fn read, void *user, unsigned period_ms)
{
    if (mem_thread_started || mem_count == 0u || read == NULL) return false;
    mem_read = read;
    mem_read_user = user;
    mem_period_ms = period_ms != 0u ? period_ms : 20u;
    atomic_store(&mem_running, true);
    if (pthread_create(&mem_thread, NULL, mem_main, NULL) != 0) {
        atomic_store(&mem_running, false);
        return false;
    }
    mem_thread_started = true;
    return true;
}

void route_probe_mem_stop(void)
{
    if (!mem_thread_started) return;
    atomic_store(&mem_running, false);
    pthread_join(mem_thread, NULL);
    mem_thread_started = false;
}

void route_probe_report(void)
{
    if (!atomic_load(&enabled)) return;
    pthread_mutex_lock(&lock);
    printf("route probe    T1633 (observer only): file events %llu (opens %llu, reads %llu / %llu bytes, writes %llu / %llu bytes), "
           "%u distinct path(s), frame changes %llu of %llu presents, %u watch(es)\n",
           (unsigned long long)total_events, (unsigned long long)total_opens, (unsigned long long)total_reads,
           (unsigned long long)total_read_bytes, (unsigned long long)total_writes, (unsigned long long)total_write_bytes, path_count,
           (unsigned long long)atomic_load(&frame_changes), (unsigned long long)atomic_load(&frames_seen), atomic_load(&watch_count));
    for (unsigned i = 0u; i < watch_count && i < ROUTE_PROBE_MAX_WATCHES; i++)
        printf("route probe    watch %u kind %d '%s' va 0x%X: count %llu bytes %llu\n", i, (int)watches[i].kind, watches[i].text,
               (unsigned)watches[i].va, (unsigned long long)atomic_load(&watches[i].count),
               (unsigned long long)atomic_load(&watches[i].bytes));
    pthread_mutex_unlock(&lock);
}
