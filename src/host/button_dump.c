/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1629: the pure core of --dump-on-button, see button_dump.h. No guest dependency.
 *
 * EVENT LIFE. An edge (or an idle control) opens an EVENT at poll E: the BEFORE snapshot is captured at once, in the
 * pre install hook of poll E, so it shows the guest memory with every earlier input applied and the new input not yet
 * reported. The AFTER snapshots follow at the first poll >= E + offset. Edges within `coalesce` polls of the first edge of
 * the newest event join it (listed in its edges[], no new dumps). The files of an event are NAMED (the label carries the
 * final button list) when that coalesce window has closed, so the snapshots captured meanwhile are held by the event and
 * queued then; the memory they take is reserved at admission, so a later queue push cannot overflow the queue bound.
 */
#define _POSIX_C_SOURCE 200809L
#include "button_dump.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

const char *const button_dump_names[BUTTON_DUMP_BUTTONS] = {
    "UP", "DOWN", "LEFT", "RIGHT", "START", "BACK", "LTHUMB", "RTHUMB", "A", "B", "X", "Y", "BLACK", "WHITE", "LT", "RT",
};
const uint16_t button_dump_game_bits[BUTTON_DUMP_BUTTONS] = {
    0x0004u, 0x0008u, 0x0001u, 0x0002u, 0x0200u, 0x0100u, 0x1000u, 0x2000u,
    0x0080u, 0x0020u, 0x0010u, 0x0040u, 0x4000u, 0x8000u, 0x0400u, 0x0800u,
};

uint16_t button_dump_mask(const xinput_pad_state *state, uint8_t threshold)
{
    uint16_t mask = (uint16_t)(state->digital_buttons & 0x00FFu);
    for (unsigned index = 0u; index < XINPUT_ANALOG_COUNT; index++) {
        if (state->analog[index] > threshold) {
            mask = (uint16_t)(mask | (1u << (8u + index)));
        }
    }
    return mask;
}

/* ---- records ---- */

typedef struct {
    uint16_t digital;
    uint8_t analog[8];
} bd_raw;

typedef struct {
    uint64_t poll;
    bool press;
    uint8_t button;
} bd_edge;

typedef struct {
    unsigned index;
    bool idle;
    bool complete;
    uint64_t poll;
    uint64_t present;
    uint64_t mono_ms;
    char wall[40];
    uint16_t mask_before;
    uint16_t mask_after;
    bd_raw raw_before;
    bd_raw raw_after;
    bd_edge edges[BUTTON_DUMP_MAX_EDGES];
    unsigned edge_count; /* listed */
    unsigned coalesced;  /* edges merged after the first poll's */
    uint8_t order[BUTTON_DUMP_BUTTONS];
    unsigned order_count;
    uint16_t seen;
    bool presses;
    bool releases;
} bd_event_info;

typedef enum { KIND_BEFORE, KIND_AFTER, KIND_IDLE_BEFORE, KIND_IDLE_AFTER } bd_kind;
static const char *const kind_names[] = {"before", "after", "idle-before", "idle-after"};

typedef struct {
    void *snapshot;
    unsigned event;
    bd_kind kind;
    char tag[16];
    unsigned offset;
    uint64_t poll;
    uint64_t present;
    char label[BUTTON_DUMP_LABEL_MAX];
} bd_dump_job;

typedef struct {
    uint64_t poll;
    uint64_t mono_ms;
    char reason[16];
    unsigned count;
    bd_edge edges[BUTTON_DUMP_BUTTONS];
} bd_drop;

typedef enum { ITEM_DUMP, ITEM_EVENT, ITEM_DROP } bd_item_type;

typedef struct bd_item {
    struct bd_item *next;
    bd_item_type type;
    union {
        bd_dump_job dump;
        bd_event_info event;
        bd_drop drop;
    } u;
} bd_item;

typedef struct {
    bd_event_info info;
    bool named;
    unsigned captured;
    unsigned held_count;
    bd_dump_job held[1u + BUTTON_DUMP_MAX_AFTER];
} bd_slot;

typedef struct {
    bd_kind kind;
    char tag[16];
    unsigned offset;
    uint64_t poll;
    uint64_t present;
    char label[BUTTON_DUMP_LABEL_MAX];
    char file[BUTTON_DUMP_LABEL_MAX + 32u];
    uint64_t bytes;
    bool exists;
    bool ok;
} bd_result;

typedef struct bd_writer_event {
    struct bd_writer_event *next;
    unsigned index;
    unsigned count;
    bd_result results[1u + BUTTON_DUMP_MAX_AFTER];
} bd_writer_event;

struct button_dump {
    button_dump_config cfg;
    button_dump_ops ops;
    char dir[400];
    char buttons_dir[420];
    uint64_t base_mono;

    /* guest thread */
    uint16_t prev_mask;
    bd_raw prev_raw;
    bool was_enabled;
    uint16_t cur_mask; /* the state of the poll in progress */
    bd_raw cur_raw;
    uint64_t idle_due;
    bool have_edge;
    uint64_t last_edge_poll;
    unsigned counter;
    unsigned pending;
    unsigned drop_lines; /* "dropped" manifest lines written */
    bool drop_marker_sent;
    bd_slot *active[BUTTON_DUMP_MAX_PENDING_LIMIT];
    unsigned active_count;
    bd_slot *newest;
    button_dump_stats stats;
    uint64_t bytes_committed;
    uint64_t queue_limit;
    bool finished;
    atomic_uint_fast64_t reserved;

    /* queue */
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_cond_t drained;
    bd_item *head;
    bd_item *tail;
    unsigned inflight;
    bool thread_started;
    bool stopping; /* under `lock`: the writer leaves once the queue is empty (no allocation on the stop path) */
    pthread_t thread;

    /* writer */
    FILE *manifest;
    bd_writer_event *writer_events;
    atomic_uint dumps_written;
    atomic_uint_fast64_t bytes_written;
    atomic_uint writes;
    atomic_uint_fast64_t write_ns_total;
    atomic_uint_fast64_t write_ns_max;
};

/* ---- clock and text helpers ---- */

static void real_clock(uint64_t *mono_ms, uint64_t *wall_ms)
{
    struct timespec mono, wall;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &wall);
    *mono_ms = (uint64_t)mono.tv_sec * 1000u + (uint64_t)mono.tv_nsec / 1000000u;
    *wall_ms = (uint64_t)wall.tv_sec * 1000u + (uint64_t)wall.tv_nsec / 1000000u;
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static void clock_now(const button_dump *dump, uint64_t *mono_ms, uint64_t *wall_ms)
{
    if (dump->ops.clock != NULL) {
        dump->ops.clock(dump->ops.user, mono_ms, wall_ms);
    } else {
        real_clock(mono_ms, wall_ms);
    }
}

static void iso_time(char *out, size_t size, uint64_t wall_ms, bool with_ms)
{
    const time_t seconds = (time_t)(wall_ms / 1000u);
    struct tm parts;
    gmtime_r(&seconds, &parts);
    char base[32];
    strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &parts);
    if (with_ms) {
        snprintf(out, size, "%s.%03uZ", base, (unsigned)(wall_ms % 1000u));
    } else {
        snprintf(out, size, "%sZ", base);
    }
}

typedef struct {
    char *buffer;
    size_t size;
    size_t length;
} text_builder;

static void add(text_builder *builder, const char *format, ...)
{
    if (builder->length + 1u >= builder->size) {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(builder->buffer + builder->length, builder->size - builder->length, format, arguments);
    va_end(arguments);
    if (written > 0) {
        const size_t grown = builder->length + (size_t)written;
        builder->length = grown < builder->size ? grown : builder->size - 1u;
    }
}

static void add_json_string(text_builder *builder, const char *text)
{
    add(builder, "\"");
    for (const char *cursor = text; *cursor != '\0'; cursor++) {
        const unsigned char c = (unsigned char)*cursor;
        if (c == '"' || c == '\\') {
            add(builder, "\\%c", c);
        } else if (c < 0x20u) {
            add(builder, "\\u%04x", (unsigned)c);
        } else {
            add(builder, "%c", c);
        }
    }
    add(builder, "\"");
}

static void add_mask_names(text_builder *builder, uint16_t mask)
{
    add(builder, "[");
    bool first = true;
    for (unsigned bit = 0u; bit < BUTTON_DUMP_BUTTONS; bit++) {
        if ((mask & (1u << bit)) != 0u) {
            add(builder, "%s\"%s\"", first ? "" : ",", button_dump_names[bit]);
            first = false;
        }
    }
    add(builder, "]");
}

static void add_raw(text_builder *builder, const bd_raw *raw)
{
    add(builder, "{\"digital\":\"0x%04X\",\"analog\":[%u,%u,%u,%u,%u,%u,%u,%u]}", (unsigned)raw->digital,
        (unsigned)raw->analog[0], (unsigned)raw->analog[1], (unsigned)raw->analog[2], (unsigned)raw->analog[3],
        (unsigned)raw->analog[4], (unsigned)raw->analog[5], (unsigned)raw->analog[6], (unsigned)raw->analog[7]);
}

static void add_edges(text_builder *builder, const bd_edge *edges, unsigned count)
{
    add(builder, "[");
    for (unsigned index = 0u; index < count; index++) {
        add(builder, "%s{\"poll\":%llu,\"edge\":\"%s\",\"button\":\"%s\"}", index == 0u ? "" : ",",
            (unsigned long long)edges[index].poll, edges[index].press ? "press" : "release",
            button_dump_names[edges[index].button]);
    }
    add(builder, "]");
}

static const char *edge_word(const bd_event_info *info)
{
    if (info->idle) {
        return "idle";
    }
    if (info->presses && info->releases) {
        return "mixed";
    }
    return info->releases ? "release" : "press";
}

static void buttons_joined(const bd_event_info *info, char *out, size_t size)
{
    out[0] = '\0';
    if (info->order_count == 0u) {
        snprintf(out, size, "none");
        return;
    }
    size_t used = 0u;
    for (unsigned index = 0u; index < info->order_count && used + 1u < size; index++) {
        const int written = snprintf(out + used, size - used, "%s%s", index == 0u ? "" : "-",
                                     button_dump_names[info->order[index]]);
        if (written < 0) {
            break;
        }
        used += (size_t)written;
    }
}

/* ---- queue ---- */

static bool push(button_dump *dump, bd_item *item)
{
    item->next = NULL;
    pthread_mutex_lock(&dump->lock);
    if (dump->tail != NULL) {
        dump->tail->next = item;
    } else {
        dump->head = item;
    }
    dump->tail = item;
    dump->inflight++;
    pthread_cond_signal(&dump->wake);
    pthread_mutex_unlock(&dump->lock);
    return true;
}

static void release_reserved(button_dump *dump)
{
    atomic_fetch_sub(&dump->reserved, (uint_fast64_t)dump->cfg.snapshot_bytes);
}

/* ---- writer thread ---- */

static void emit_log(button_dump *dump, const char *line)
{
    if (dump->ops.log != NULL) {
        dump->ops.log(dump->ops.user, line);
    } else {
        fprintf(stderr, "%s\n", line);
    }
}

static void manifest_line(button_dump *dump, const char *text)
{
    fputs(text, dump->manifest);
    fputc('\n', dump->manifest);
    fflush(dump->manifest);
}

static bd_writer_event *writer_event_of(button_dump *dump, unsigned index)
{
    for (bd_writer_event *cursor = dump->writer_events; cursor != NULL; cursor = cursor->next) {
        if (cursor->index == index) {
            return cursor;
        }
    }
    bd_writer_event *created = calloc(1u, sizeof *created);
    if (created != NULL) {
        created->index = index;
        created->next = dump->writer_events;
        dump->writer_events = created;
    }
    return created;
}

static void process_dump(button_dump *dump, bd_dump_job *job)
{
    bd_writer_event *event = writer_event_of(dump, job->event);
    char path[BUTTON_DUMP_LABEL_MAX + 480u];
    snprintf(path, sizeof path, "%s/guestdump.%s", dump->buttons_dir, job->label);
    bd_result result;
    memset(&result, 0, sizeof result);
    result.kind = job->kind;
    snprintf(result.tag, sizeof result.tag, "%s", job->tag);
    result.offset = job->offset;
    result.poll = job->poll;
    result.present = job->present;
    snprintf(result.label, sizeof result.label, "%s", job->label);
    snprintf(result.file, sizeof result.file, "buttons/guestdump.%s", job->label);
    if (job->snapshot != NULL) {
        char header[BUTTON_DUMP_LABEL_MAX + 160u];
        snprintf(header, sizeof header, "# button-dump event=%u kind=%s label=%s poll=%llu present=%llu offset=%u\n",
                 job->event, kind_names[job->kind], job->label, (unsigned long long)job->poll,
                 (unsigned long long)job->present, job->offset);
        const uint64_t started = monotonic_ns();
        result.ok = dump->ops.write(dump->ops.user, job->snapshot, path, header);
        const uint64_t spent = monotonic_ns() - started;
        atomic_fetch_add(&dump->writes, 1u);
        atomic_fetch_add(&dump->write_ns_total, (uint_fast64_t)spent);
        if (spent > (uint64_t)atomic_load(&dump->write_ns_max)) {
            atomic_store(&dump->write_ns_max, (uint_fast64_t)spent);
        }
        struct stat info;
        if (stat(path, &info) == 0) {
            result.exists = true;
            result.bytes = (uint64_t)info.st_size;
            atomic_fetch_add(&dump->dumps_written, 1u);
            atomic_fetch_add(&dump->bytes_written, (uint_fast64_t)info.st_size);
        }
        dump->ops.discard(dump->ops.user, job->snapshot);
    }
    release_reserved(dump);
    if (event != NULL && event->count < sizeof event->results / sizeof event->results[0]) {
        event->results[event->count++] = result;
    }
}

static void retire_writer_event(button_dump *dump, bd_writer_event *event)
{
    if (event == NULL) {
        return;
    }
    for (bd_writer_event **cursor = &dump->writer_events; *cursor != NULL; cursor = &(*cursor)->next) {
        if (*cursor == event) {
            *cursor = event->next;
            break;
        }
    }
    free(event);
}

static void process_event(button_dump *dump, const bd_event_info *info)
{
    bd_writer_event *event = writer_event_of(dump, info->index);
    char *buffer = malloc(16384u);
    if (buffer == NULL) {
        retire_writer_event(dump, event); /* no line can be formatted, but the accumulated dumps must not leak */
        return;
    }
    text_builder builder = {buffer, 16384u, 0u};
    char first_button[16] = "none";
    if (info->order_count != 0u) {
        snprintf(first_button, sizeof first_button, "%s", button_dump_names[info->edges[0].button]);
    }
    add(&builder, "{\"type\":\"event\",\"index\":%u,\"edge\":\"%s\",\"button\":\"%s\",\"buttons\":[", info->index,
        edge_word(info), first_button);
    for (unsigned index = 0u; index < info->order_count; index++) {
        add(&builder, "%s\"%s\"", index == 0u ? "" : ",", button_dump_names[info->order[index]]);
    }
    add(&builder, "],\"poll\":%llu,\"present\":%llu,\"mono_ms\":%llu,\"wall_time\":\"%s\",", (unsigned long long)info->poll,
        (unsigned long long)info->present, (unsigned long long)info->mono_ms, info->wall);
    add(&builder, "\"mask_before\":\"0x%04X\",\"mask_after\":\"0x%04X\",\"mask_names_before\":", (unsigned)info->mask_before,
        (unsigned)info->mask_after);
    add_mask_names(&builder, info->mask_before);
    add(&builder, ",\"mask_names_after\":");
    add_mask_names(&builder, info->mask_after);
    add(&builder, ",\"raw_before\":");
    add_raw(&builder, &info->raw_before);
    add(&builder, ",\"raw_after\":");
    add_raw(&builder, &info->raw_after);
    add(&builder, ",\"edges\":");
    add_edges(&builder, info->edges, info->edge_count);
    add(&builder, ",\"coalesced\":%u,\"dumps\":[", info->coalesced);
    const unsigned count = event != NULL ? event->count : 0u;
    for (unsigned index = 0u; index < count; index++) {
        const bd_result *result = &event->results[index];
        add(&builder,
            "%s{\"kind\":\"%s\",\"tag\":\"%s\",\"offset\":%u,\"poll\":%llu,\"present\":%llu,\"label\":\"%s\","
            "\"file\":\"%s\",\"bytes\":%llu,\"ok\":%s}",
            index == 0u ? "" : ",", kind_names[result->kind], result->tag, result->offset,
            (unsigned long long)result->poll, (unsigned long long)result->present, result->label, result->file,
            (unsigned long long)result->bytes, result->ok ? "true" : "false");
    }
    add(&builder, "],\"complete\":%s}", info->complete ? "true" : "false");
    manifest_line(dump, buffer);
    free(buffer);

    char buttons[160];
    buttons_joined(info, buttons, sizeof buttons);
    char line[320];
    snprintf(line, sizeof line, "button dump: event %u %s %s poll %llu -> %u dumps%s", info->index, edge_word(info), buttons,
             (unsigned long long)info->poll, count, info->complete ? "" : " (INCOMPLETE, closed at exit)");
    emit_log(dump, line);
    retire_writer_event(dump, event);
}

static void process_drop(button_dump *dump, const bd_drop *drop)
{
    char buffer[2048];
    text_builder builder = {buffer, sizeof buffer, 0u};
    add(&builder, "{\"type\":\"dropped\",\"poll\":%llu,\"reason\":\"%s\",\"edges\":", (unsigned long long)drop->poll,
        drop->reason);
    add_edges(&builder, drop->edges, drop->count);
    add(&builder, ",\"mono_ms\":%llu}", (unsigned long long)drop->mono_ms);
    manifest_line(dump, buffer);
    char line[160];
    snprintf(line, sizeof line, "button dump: DROPPED %u edge(s) at poll %llu (%s)", drop->count,
             (unsigned long long)drop->poll, drop->reason);
    emit_log(dump, line);
}

static void *writer_main(void *argument)
{
    button_dump *dump = argument;
    for (;;) {
        pthread_mutex_lock(&dump->lock);
        while (dump->head == NULL && !dump->stopping) {
            pthread_cond_wait(&dump->wake, &dump->lock);
        }
        if (dump->head == NULL) {
            pthread_mutex_unlock(&dump->lock);
            return NULL; /* stopping and drained */
        }
        bd_item *item = dump->head;
        dump->head = item->next;
        if (dump->head == NULL) {
            dump->tail = NULL;
        }
        pthread_mutex_unlock(&dump->lock);
        const bd_item_type type = item->type;
        switch (type) {
        case ITEM_DUMP:
            process_dump(dump, &item->u.dump);
            break;
        case ITEM_EVENT:
            process_event(dump, &item->u.event);
            break;
        case ITEM_DROP:
            process_drop(dump, &item->u.drop);
            break;
        }
        free(item);
        pthread_mutex_lock(&dump->lock);
        dump->inflight--;
        if (dump->inflight == 0u) {
            pthread_cond_broadcast(&dump->drained);
        }
        pthread_mutex_unlock(&dump->lock);
    }
}

void button_dump_flush(button_dump *dump)
{
    pthread_mutex_lock(&dump->lock);
    while (dump->inflight != 0u) {
        pthread_cond_wait(&dump->drained, &dump->lock);
    }
    pthread_mutex_unlock(&dump->lock);
}

/* ---- events (guest thread) ---- */

static void enqueue_dump(button_dump *dump, const bd_dump_job *job)
{
    bd_item *item = malloc(sizeof *item);
    if (item == NULL) {
        if (job->snapshot != NULL) {
            dump->ops.discard(dump->ops.user, job->snapshot);
        }
        release_reserved(dump);
        return;
    }
    item->type = ITEM_DUMP;
    item->u.dump = *job;
    push(dump, item);
}

static void name_label(const bd_slot *slot, bd_dump_job *job)
{
    char buttons[160];
    buttons_joined(&slot->info, buttons, sizeof buttons);
    snprintf(job->label, sizeof job->label, "%04u_%s_%s_%s", slot->info.index, edge_word(&slot->info), buttons, job->tag);
}

static void flush_held(button_dump *dump, bd_slot *slot)
{
    for (unsigned index = 0u; index < slot->held_count; index++) {
        name_label(slot, &slot->held[index]);
        enqueue_dump(dump, &slot->held[index]);
    }
    slot->held_count = 0u;
}

static void remove_active(button_dump *dump, bd_slot *slot)
{
    for (unsigned index = 0u; index < dump->active_count; index++) {
        if (dump->active[index] == slot) {
            memmove(&dump->active[index], &dump->active[index + 1u], (dump->active_count - index - 1u) * sizeof dump->active[0]);
            dump->active_count--;
            break;
        }
    }
    if (dump->newest == slot) {
        dump->newest = NULL;
    }
    dump->pending--;
    free(slot);
}

static void enqueue_event(button_dump *dump, const bd_slot *slot, bool complete)
{
    bd_item *item = malloc(sizeof *item);
    if (item == NULL) {
        return;
    }
    item->type = ITEM_EVENT;
    item->u.event = slot->info;
    item->u.event.complete = complete;
    push(dump, item);
}

static void try_complete(button_dump *dump, bd_slot *slot)
{
    if (slot->named && slot->captured == dump->cfg.after_count) {
        enqueue_event(dump, slot, true);
        remove_active(dump, slot);
    }
}

static void name_event(button_dump *dump, bd_slot *slot)
{
    slot->named = true;
    flush_held(dump, slot);
    if (dump->newest == slot) {
        dump->newest = NULL;
    }
    try_complete(dump, slot);
}

static void hold_capture(button_dump *dump, bd_slot *slot, bd_kind kind, const char *tag, unsigned offset, uint64_t poll)
{
    bd_dump_job *job = &slot->held[slot->held_count++];
    memset(job, 0, sizeof *job);
    job->event = slot->info.index;
    job->kind = kind;
    snprintf(job->tag, sizeof job->tag, "%s", tag);
    job->offset = offset;
    job->poll = poll;
    job->present = dump->ops.present != NULL ? dump->ops.present(dump->ops.user) : 0u;
    const uint64_t started = monotonic_ns();
    job->snapshot = dump->ops.capture(dump->ops.user);
    const uint64_t spent = monotonic_ns() - started;
    dump->stats.captures++;
    dump->stats.capture_ns_total += spent;
    if (spent > dump->stats.capture_ns_max) {
        dump->stats.capture_ns_max = spent;
    }
}

static void add_slot_edge(bd_slot *slot, uint64_t poll, bool press, unsigned button)
{
    bd_event_info *info = &slot->info;
    if (info->edge_count < BUTTON_DUMP_MAX_EDGES) {
        info->edges[info->edge_count].poll = poll;
        info->edges[info->edge_count].press = press;
        info->edges[info->edge_count].button = (uint8_t)button;
        info->edge_count++;
    }
    if ((info->seen & (1u << button)) == 0u) {
        info->seen = (uint16_t)(info->seen | (1u << button));
        info->order[info->order_count++] = (uint8_t)button;
    }
    if (press) {
        info->presses = true;
    } else {
        info->releases = true;
    }
}

static void capture_afters(button_dump *dump, uint64_t poll)
{
    for (unsigned at = 0u; at < dump->active_count;) {
        bd_slot *slot = dump->active[at];
        bool removed = false;
        while (slot->captured < dump->cfg.after_count && poll >= slot->info.poll + dump->cfg.after[slot->captured]) {
            const unsigned offset = dump->cfg.after[slot->captured];
            char tag[16];
            snprintf(tag, sizeof tag, "after%u", offset);
            hold_capture(dump, slot, slot->info.idle ? KIND_IDLE_AFTER : KIND_AFTER, tag, offset, poll);
            slot->captured++;
            slot->info.mask_after = dump->prev_mask;
            slot->info.raw_after = dump->prev_raw;
            if (slot->named) {
                flush_held(dump, slot);
            }
        }
        const unsigned before = dump->active_count;
        try_complete(dump, slot); /* frees a named, fully captured slot, the next one moves into `at` */
        removed = dump->active_count != before;
        if (!removed) {
            at++;
        }
    }
}

static uint64_t queue_limit_for(const button_dump *dump)
{
    const uint64_t one_event = (1u + (uint64_t)dump->cfg.after_count) * dump->cfg.snapshot_bytes;
    uint64_t limit = dump->cfg.queue_limit != 0u ? dump->cfg.queue_limit : BUTTON_DUMP_QUEUE_BYTES;
    if (limit < one_event) {
        limit = one_event;
    }
    return limit;
}

/* NULL when an event with all its dumps fits, else the reason. */
static const char *admit_reason(button_dump *dump, bool check_pending)
{
    const uint64_t dumps = 1u + (uint64_t)dump->cfg.after_count;
    if (check_pending && dump->pending >= dump->cfg.max_pending) {
        return "max_pending";
    }
    if ((uint64_t)dump->stats.dumps_committed + dumps > dump->cfg.max_dumps) {
        return "max_dumps";
    }
    if (dump->bytes_committed + dumps * dump->cfg.bytes_per_dump > dump->cfg.max_bytes) {
        return "max_bytes";
    }
    if ((uint64_t)atomic_load(&dump->reserved) + dumps * dump->cfg.snapshot_bytes > dump->queue_limit) {
        return "queue_full";
    }
    return NULL;
}

static bd_slot *open_event(button_dump *dump, uint64_t poll, bool idle)
{
    bd_slot *slot = calloc(1u, sizeof *slot);
    if (slot == NULL) {
        return NULL;
    }
    const uint64_t dumps = 1u + (uint64_t)dump->cfg.after_count;
    bd_event_info *info = &slot->info;
    info->index = ++dump->counter;
    info->idle = idle;
    info->poll = poll;
    info->mask_before = dump->prev_mask;
    info->mask_after = dump->cur_mask; /* an event closed before any after dump shows the state it opened with */
    info->raw_before = dump->prev_raw;
    info->raw_after = dump->cur_raw;
    uint64_t mono = 0u;
    uint64_t wall = 0u;
    clock_now(dump, &mono, &wall);
    info->mono_ms = mono - dump->base_mono;
    iso_time(info->wall, sizeof info->wall, wall, true);
    info->present = dump->ops.present != NULL ? dump->ops.present(dump->ops.user) : 0u;
    dump->stats.dumps_committed += (unsigned)dumps;
    dump->bytes_committed += dumps * dump->cfg.bytes_per_dump;
    atomic_fetch_add(&dump->reserved, (uint_fast64_t)(dumps * dump->cfg.snapshot_bytes));
    dump->active[dump->active_count++] = slot;
    dump->pending++;
    if (idle) {
        dump->stats.idle_events++;
    } else {
        dump->stats.events++;
    }
    return slot;
}

static void drop_edges(button_dump *dump, uint64_t poll, const char *reason, const bd_edge *edges, unsigned count)
{
    dump->stats.dropped++; /* the total, whether or not it gets a manifest line */
    if (dump->drop_lines >= BUTTON_DUMP_MAX_DROP_LINES) {
        if (dump->drop_marker_sent) {
            return;
        }
        dump->drop_marker_sent = true; /* one marker line, then only the counters move */
        reason = "suppressed";
        count = 0u;
    } else {
        dump->drop_lines++;
    }
    bd_item *item = malloc(sizeof *item);
    if (item == NULL) {
        return;
    }
    item->type = ITEM_DROP;
    memset(&item->u.drop, 0, sizeof item->u.drop);
    item->u.drop.poll = poll;
    snprintf(item->u.drop.reason, sizeof item->u.drop.reason, "%s", reason);
    item->u.drop.count = count;
    memcpy(item->u.drop.edges, edges, count * sizeof edges[0]);
    uint64_t mono = 0u;
    uint64_t wall = 0u;
    clock_now(dump, &mono, &wall);
    item->u.drop.mono_ms = mono - dump->base_mono;
    push(dump, item);
}

static void handle_edges(button_dump *dump, uint64_t poll, uint16_t mask)
{
    const uint16_t changed = (uint16_t)(mask ^ dump->prev_mask);
    bd_edge edges[BUTTON_DUMP_BUTTONS];
    unsigned count = 0u;
    for (unsigned bit = 0u; bit < BUTTON_DUMP_BUTTONS; bit++) {
        if ((changed & (1u << bit)) != 0u) {
            edges[count].poll = poll;
            edges[count].press = (mask & (1u << bit)) != 0u;
            edges[count].button = (uint8_t)bit;
            count++;
        }
    }
    dump->have_edge = true;
    dump->last_edge_poll = poll;
    bd_slot *newest = dump->newest;
    if (newest != NULL && !newest->named && poll - newest->info.poll <= dump->cfg.coalesce) {
        for (unsigned index = 0u; index < count; index++) {
            add_slot_edge(newest, poll, edges[index].press, edges[index].button);
        }
        newest->info.coalesced += count;
        dump->stats.coalesced_edges += count;
        return;
    }
    const char *reason = admit_reason(dump, true);
    if (reason != NULL) {
        drop_edges(dump, poll, reason, edges, count);
        return;
    }
    bd_slot *slot = open_event(dump, poll, false);
    if (slot == NULL) {
        drop_edges(dump, poll, "queue_full", edges, count);
        return;
    }
    for (unsigned index = 0u; index < count; index++) {
        add_slot_edge(slot, poll, edges[index].press, edges[index].button);
    }
    hold_capture(dump, slot, KIND_BEFORE, "before", 0u, poll);
    if (dump->cfg.coalesce == 0u) {
        name_event(dump, slot);
    } else {
        dump->newest = slot;
    }
}

static void idle_control(button_dump *dump, uint64_t poll)
{
    if (dump->cfg.idle_every == 0u || dump->stats.idle_events >= dump->cfg.max_idle || poll < dump->idle_due) {
        return;
    }
    dump->idle_due = poll + dump->cfg.idle_every;
    const uint64_t quiet = (dump->cfg.after_count != 0u ? dump->cfg.after[dump->cfg.after_count - 1u] : 0u) + 2u;
    if (dump->pending != 0u || (dump->have_edge && poll - dump->last_edge_poll <= quiet)) {
        return;
    }
    if (admit_reason(dump, false) != NULL) {
        return;
    }
    bd_slot *slot = open_event(dump, poll, true);
    if (slot == NULL) {
        return;
    }
    hold_capture(dump, slot, KIND_IDLE_BEFORE, "before", 0u, poll);
    name_event(dump, slot);
}

void button_dump_poll(button_dump *dump, uint64_t poll, const xinput_pad_state *state)
{
    if (dump->finished) {
        return;
    }
    const uint16_t mask = button_dump_mask(state, dump->cfg.threshold);
    dump->cur_mask = mask;
    dump->cur_raw.digital = state->digital_buttons;
    memcpy(dump->cur_raw.analog, state->analog, sizeof dump->cur_raw.analog);
    const bool enabled = poll >= dump->cfg.start_poll &&
                         (!dump->cfg.after_replay || (dump->ops.armed != NULL && dump->ops.armed(dump->ops.user)));
    if (enabled && !dump->was_enabled) {
        dump->was_enabled = true;
        dump->idle_due = poll + dump->cfg.idle_every;
    }
    /* 0. a coalesce window that closed names its event and queues its held snapshots */
    for (unsigned at = 0u; at < dump->active_count;) {
        bd_slot *slot = dump->active[at];
        const unsigned before = dump->active_count;
        if (!slot->named && poll - slot->info.poll > dump->cfg.coalesce) {
            name_event(dump, slot);
        }
        if (dump->active_count == before) {
            at++;
        }
    }
    /* 1. after snapshots that are due */
    capture_afters(dump, poll);
    /* 2. edges of the new state, 3. idle control */
    if (enabled) {
        if (mask != dump->prev_mask) {
            handle_edges(dump, poll, mask);
        }
        idle_control(dump, poll);
    }
    dump->prev_mask = mask;
    dump->prev_raw.digital = state->digital_buttons;
    memcpy(dump->prev_raw.analog, state->analog, sizeof dump->prev_raw.analog);
}

void button_dump_stats_get(button_dump *dump, button_dump_stats *out)
{
    *out = dump->stats;
    out->dumps_written = atomic_load(&dump->dumps_written);
    out->bytes_written = (uint64_t)atomic_load(&dump->bytes_written);
    out->writes = atomic_load(&dump->writes);
    out->write_ns_total = (uint64_t)atomic_load(&dump->write_ns_total);
    out->write_ns_max = (uint64_t)atomic_load(&dump->write_ns_max);
}

/* ---- create / finish ---- */

static bool make_directory(const char *path)
{
    char copy[420];
    if (strlen(path) >= sizeof copy) {
        return false;
    }
    strcpy(copy, path);
    for (char *cursor = copy + 1; *cursor != '\0'; cursor++) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(copy, 0777) != 0 && errno != EEXIST) {
                return false;
            }
            *cursor = '/';
        }
    }
    return mkdir(copy, 0777) == 0 || errno == EEXIST;
}

static void fail(char *error, size_t size, const char *message)
{
    if (error != NULL && size != 0u) {
        snprintf(error, size, "%s", message);
    }
}

button_dump *button_dump_create(const button_dump_config *config, const button_dump_ops *ops, char *error, size_t error_size)
{
    if (config == NULL || ops == NULL || ops->capture == NULL || ops->write == NULL || ops->discard == NULL ||
        config->dump_dir == NULL || config->after_count == 0u || config->after_count > BUTTON_DUMP_MAX_AFTER ||
        config->max_pending == 0u || config->max_pending > BUTTON_DUMP_MAX_PENDING_LIMIT || config->max_dumps == 0u ||
        config->coalesce > 60u) {
        fail(error, error_size, "bad button dump configuration");
        return NULL;
    }
    for (unsigned index = 0u; index < config->after_count; index++) {
        if (config->after[index] == 0u || (index != 0u && config->after[index] <= config->after[index - 1u])) {
            fail(error, error_size, "after offsets must be strictly increasing and at least 1");
            return NULL;
        }
    }
    button_dump *dump = calloc(1u, sizeof *dump);
    if (dump == NULL) {
        fail(error, error_size, "out of memory");
        return NULL;
    }
    dump->cfg = *config;
    dump->ops = *ops;
    if (strlen(config->dump_dir) >= sizeof dump->dir) {
        fail(error, error_size, "dump directory path too long");
        free(dump);
        return NULL;
    }
    strcpy(dump->dir, config->dump_dir);
    dump->cfg.dump_dir = dump->dir;
    snprintf(dump->buttons_dir, sizeof dump->buttons_dir, "%s/buttons", dump->dir);
    dump->queue_limit = queue_limit_for(dump);
    uint64_t mono = 0u;
    uint64_t wall = 0u;
    clock_now(dump, &mono, &wall);
    dump->base_mono = mono;
    pthread_mutex_init(&dump->lock, NULL);
    pthread_cond_init(&dump->wake, NULL);
    pthread_cond_init(&dump->drained, NULL);
    if (!make_directory(dump->buttons_dir)) {
        fail(error, error_size, "cannot create the buttons directory");
        dump->finished = true;
        button_dump_destroy(dump);
        return NULL;
    }
    char manifest_path[sizeof dump->dir + 32u];
    snprintf(manifest_path, sizeof manifest_path, "%s/buttons.jsonl", dump->dir);
    dump->manifest = fopen(manifest_path, "w");
    if (dump->manifest == NULL) {
        fail(error, error_size, "cannot create buttons.jsonl");
        dump->finished = true;
        button_dump_destroy(dump);
        return NULL;
    }
    char buffer[4096];
    text_builder builder = {buffer, sizeof buffer, 0u};
    char when[40];
    iso_time(when, sizeof when, wall, false);
    add(&builder, "{\"type\":\"start\",\"version\":1,\"wall_start\":\"%s\",\"after_frames\":[", when);
    for (unsigned index = 0u; index < config->after_count; index++) {
        add(&builder, "%s%u", index == 0u ? "" : ",", (unsigned)config->after[index]);
    }
    add(&builder,
        "],\"threshold\":%u,\"coalesce_frames\":%u,\"start_poll\":%llu,\"max_dumps\":%u,\"max_bytes\":%llu,"
        "\"max_pending\":%u,\"idle_every\":%u,\"ranges\":%u,\"bytes_per_dump\":%llu,\"button_names\":[",
        (unsigned)config->threshold, config->coalesce, (unsigned long long)config->start_poll, config->max_dumps,
        (unsigned long long)config->max_bytes, config->max_pending, config->idle_every, config->ranges,
        (unsigned long long)config->bytes_per_dump);
    for (unsigned index = 0u; index < BUTTON_DUMP_BUTTONS; index++) {
        add(&builder, "%s\"%s\"", index == 0u ? "" : ",", button_dump_names[index]);
    }
    add(&builder, "],\"game_bits\":{");
    for (unsigned index = 0u; index < BUTTON_DUMP_BUTTONS; index++) {
        add(&builder, "%s\"%s\":%u", index == 0u ? "" : ",", button_dump_names[index], (unsigned)button_dump_game_bits[index]);
    }
    add(&builder, "},\"dump_dir\":");
    add_json_string(&builder, config->dump_dir);
    add(&builder, ",\"max_idle\":%u,\"after_replay\":%s,\"forced_state\":%s}", config->max_idle,
        config->after_replay ? "true" : "false", config->forced_state ? "true" : "false");
    manifest_line(dump, buffer);
    if (pthread_create(&dump->thread, NULL, writer_main, dump) != 0) {
        fail(error, error_size, "cannot start the writer thread");
        dump->finished = true;
        button_dump_destroy(dump);
        return NULL;
    }
    dump->thread_started = true;
    return dump;
}

void button_dump_finish(button_dump *dump, const char *reason)
{
    if (dump == NULL || dump->finished) {
        return;
    }
    dump->finished = true;
    while (dump->active_count != 0u) {
        bd_slot *slot = dump->active[0];
        if (!slot->named) {
            slot->named = true;
            flush_held(dump, slot);
        }
        enqueue_event(dump, slot, slot->captured == dump->cfg.after_count);
        remove_active(dump, slot);
    }
    if (dump->thread_started) {
        pthread_mutex_lock(&dump->lock);
        dump->stopping = true;
        pthread_cond_signal(&dump->wake);
        pthread_mutex_unlock(&dump->lock);
        pthread_join(dump->thread, NULL);
        dump->thread_started = false;
    }
    button_dump_stats stats;
    button_dump_stats_get(dump, &stats);
    uint64_t mono = 0u;
    uint64_t wall = 0u;
    clock_now(dump, &mono, &wall);
    char when[40];
    iso_time(when, sizeof when, wall, true);
    char buffer[512];
    snprintf(buffer, sizeof buffer,
             "{\"type\":\"end\",\"events\":%u,\"idle_events\":%u,\"dumps\":%u,\"bytes\":%llu,\"dropped\":%u,"
             "\"coalesced_edges\":%u,\"reason\":\"%s\",\"wall_time\":\"%s\"}",
             stats.events, stats.idle_events, stats.dumps_written, (unsigned long long)stats.bytes_written, stats.dropped,
             stats.coalesced_edges, reason != NULL ? reason : "exit", when);
    if (dump->manifest != NULL) {
        manifest_line(dump, buffer);
    }
    char line[480];
    snprintf(line, sizeof line,
             "button dump: %u event(s) + %u idle, %u dump file(s), %llu byte(s), %u dropped, %u coalesced edge(s), reason %s",
             stats.events, stats.idle_events, stats.dumps_written, (unsigned long long)stats.bytes_written, stats.dropped,
             stats.coalesced_edges, reason != NULL ? reason : "exit");
    emit_log(dump, line);
    snprintf(line, sizeof line,
             "button dump cost: %u capture(s) on the guest thread avg %llu us max %llu us, %u write(s) on the writer avg %llu us max %llu us",
             stats.captures, (unsigned long long)(stats.captures != 0u ? stats.capture_ns_total / stats.captures / 1000u : 0u),
             (unsigned long long)(stats.capture_ns_max / 1000u), stats.writes,
             (unsigned long long)(stats.writes != 0u ? stats.write_ns_total / stats.writes / 1000u : 0u),
             (unsigned long long)(stats.write_ns_max / 1000u));
    emit_log(dump, line);
}

void button_dump_destroy(button_dump *dump)
{
    if (dump == NULL) {
        return;
    }
    button_dump_finish(dump, "exit");
    if (dump->manifest != NULL) {
        fclose(dump->manifest);
    }
    while (dump->writer_events != NULL) {
        bd_writer_event *next = dump->writer_events->next;
        free(dump->writer_events);
        dump->writer_events = next;
    }
    pthread_mutex_destroy(&dump->lock);
    pthread_cond_destroy(&dump->wake);
    pthread_cond_destroy(&dump->drained);
    free(dump);
}
