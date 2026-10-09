/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "host_route.h"
#include "guest_dump.h"
#include "kernel_call.h"
#include "route_probe.h"
#include "xinput_nav_record.h"
#include "xinput_record.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static xinput_route *g_route;
static poke_trigger_set g_pokes;
static host_route_config g_config;

static bool read_guest(uint32_t address, unsigned width, uint32_t *value)
{
    uint8_t raw[4] = {0, 0, 0, 0};
    if (width == 0u || width > 4u || !kernel_guest_read_bytes(address, raw, width)) return false;
    *value = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
    return true;
}

static bool mem_adapter(uint32_t address, unsigned width, uint32_t *value, void *user)
{
    (void)user;
    return g_config.read_mem != NULL ? g_config.read_mem(address, width, value, g_config.test_user)
                                     : read_guest(address, width, value);
}

static bool bytes_adapter(uint32_t address, void *buffer, size_t length, void *user)
{
    (void)user;
    return length != 0u && kernel_guest_read_bytes(address, buffer, length);
}

static void fire(const char *label, const char *why, uint64_t poll, uint64_t cursor)
{
    const bool ran = g_config.poke != NULL ? g_config.poke(label) : guest_dump_poke_now(label);
    fprintf(stderr,
            "route poke (T1616, FORCED-STATE, FABRICATED): label %s at %s, host poll %llu, record position %llu -> %s\n",
            label, why, (unsigned long long)poll, (unsigned long long)cursor,
            ran ? "served (see guestpoke.log for the result)" : "NOT served (dump/poke not started)");
}

/* At most POKE_TRIGGER_MAX triggers exist, so a take that does not retire its entry cannot spin. */
static void fire_due(poke_where where, uint64_t value, const char *why, uint64_t poll, uint64_t cursor)
{
    for (unsigned attempt = 0u; attempt < POKE_TRIGGER_MAX; attempt++) {
        const char *label = poke_trigger_take(&g_pokes, where, value);
        if (label == NULL) return;
        fire(label, why, poll, cursor);
    }
}

static void on_poll(uint64_t poll, uint64_t cursor, void *user)
{
    (void)user;
    fire_due(POKE_AT_POLL, poll, "poll", poll, cursor);
}

static void log_event(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_event(const char *format, ...)
{
    if (!route_probe_log_active()) return;
    char line[900];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    route_probe_log_line("route %s", line);
}

static void on_event(const xinput_route_event *event, void *user)
{
    (void)user;
    switch (event->kind) {
    case XINPUT_ROUTE_MARK:
        fprintf(stderr, "route: mark %u reached at host poll %llu, record position %llu\n", event->index,
                (unsigned long long)event->poll, (unsigned long long)event->cursor);
        log_event("mark %u reached record-position=%llu", event->index, (unsigned long long)event->cursor);
        fire_due(POKE_AT_MARK, event->index, "mark", event->poll, event->cursor);
        break;
    case XINPUT_ROUTE_WAIT_OK:
        fprintf(stderr, "route: wait for mark %u satisfied after %llu stalled poll(s), %.1f s, at host poll %llu\n", event->index,
                (unsigned long long)event->stalled, (double)event->stalled_ms / 1000.0, (unsigned long long)event->poll);
        if (event->detail != NULL && event->detail[0] != '\0') fprintf(stderr, "route:   %s\n", event->detail);
        log_event("wait-ok mark %u stalled-polls=%llu stalled-ms=%llu %s", event->index, (unsigned long long)event->stalled,
                  (unsigned long long)event->stalled_ms, event->detail != NULL ? event->detail : "");
        break;
    case XINPUT_ROUTE_WAIT_PROGRESS:
        fprintf(stderr, "route: still waiting for mark %u after %.1f s (%llu polls): %s\n", event->index,
                (double)event->stalled_ms / 1000.0, (unsigned long long)event->stalled, event->detail != NULL ? event->detail : "");
        log_event("wait-progress mark %u stalled-ms=%llu %s", event->index, (unsigned long long)event->stalled_ms,
                  event->detail != NULL ? event->detail : "");
        break;
    case XINPUT_ROUTE_WAIT_TIMEOUT: {
        char reason[760];
        snprintf(reason, sizeof reason,
                 "route FAILED: the wait for mark %u did not hold within %llu polls / %.1f s (record position %llu, host poll %llu)%s%s",
                 event->index, (unsigned long long)event->stalled, (double)event->stalled_ms / 1000.0,
                 (unsigned long long)event->cursor, (unsigned long long)event->poll,
                 event->detail != NULL && event->detail[0] != '\0' ? "; unmet: " : "", event->detail != NULL ? event->detail : "");
        fprintf(stderr, "%s\nroute: the screen or event the route waits for never appeared (wrong state or disk, or the condition is "
                        "wrong: see --route-event-log and tools.route_events)\n", reason);
        log_event("wait-timeout mark %u %s", event->index, event->detail != NULL ? event->detail : "");
        if (g_config.on_failure != NULL) g_config.on_failure(reason);
        break;
    }
    case XINPUT_ROUTE_NAV_START:
        fprintf(stderr, "route nav: %s (host poll %llu)\n", event->detail != NULL ? event->detail : "", (unsigned long long)event->poll);
        log_event("nav-start %s", event->detail != NULL ? event->detail : "");
        break;
    case XINPUT_ROUTE_NAV_OK:
        fprintf(stderr, "route nav: %s (host poll %llu)\n", event->detail != NULL ? event->detail : "", (unsigned long long)event->poll);
        log_event("nav-ok %s", event->detail != NULL ? event->detail : "");
        break;
    case XINPUT_ROUTE_NAV_FALLBACK:
    case XINPUT_ROUTE_NAV_PROGRESS:
        fprintf(stderr, "%s\n", event->detail != NULL ? event->detail : "");
        log_event("%s %s", event->kind == XINPUT_ROUTE_NAV_FALLBACK ? "nav-fallback" : "nav-progress", event->detail != NULL ? event->detail : "");
        break;
    case XINPUT_ROUTE_NAV_FAIL:
        fprintf(stderr, "%s (record position %llu, host poll %llu)\n", event->detail != NULL ? event->detail : "route FAILED: nav step",
                (unsigned long long)event->cursor, (unsigned long long)event->poll);
        log_event("nav-fail %s", event->detail != NULL ? event->detail : "");
        if (g_config.on_failure != NULL) g_config.on_failure(event->detail != NULL ? event->detail : "route FAILED: nav step");
        break;
    case XINPUT_ROUTE_END:
        fprintf(stderr, "route: the recorded inputs are exhausted at host poll %llu, the %s pad has the controls now\n",
                (unsigned long long)event->poll, g_config.live != NULL ? "live" : "(none: rest)");
        log_event("end record-exhausted");
        fire_due(POKE_AT_END, 0u, "replay-end", event->poll, event->cursor);
        break;
    }
}

static bool read_text_file(const char *path, char **text, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    size_t size = 0u, used = 0u;
    char *buffer = NULL;
    for (;;) {
        if (used + 1u >= size) {
            size = size != 0u ? size * 2u : 4096u;
            if (size > (1u << 20)) { free(buffer); fclose(file); return false; } /* a menu table is a few KB */
            char *grown = realloc(buffer, size);
            if (grown == NULL) { free(buffer); fclose(file); return false; }
            buffer = grown;
        }
        const size_t got = fread(buffer + used, 1u, size - used - 1u, file);
        used += got;
        if (got == 0u) break;
    }
    const bool bad = ferror(file) != 0;
    fclose(file);
    if (bad) { free(buffer); return false; }
    buffer[used] = '\0';
    *text = buffer;
    *length = used;
    return true;
}

/* T1640: the record's nav lines, the menu table, the mode and the timing become the route's nav steps. */
static bool setup_nav(xinput_route *route, const host_route_config *config, const xinput_script *script, const uint64_t *marks,
                      size_t mark_count, char *error, size_t error_size)
{
    if (config->record_nav_count == 0u) return true;
    bool good = false;
    char reason[300];
    route_nav_step *steps = calloc(config->record_nav_count, sizeof *steps);
    route_nav_menu_table *table = calloc(1u, sizeof *table);
    char *file_text = NULL;
    if (steps == NULL || table == NULL) {
        snprintf(error, error_size, "route nav: out of memory");
        goto done;
    }
    for (size_t i = 0u; i < config->record_nav_count; i++) {
        if (!route_nav_line_parse(config->record_navs[i], &steps[i], reason, sizeof reason)) {
            snprintf(error, error_size, "record '# nav:' line %zu: %s", i + 1u, reason);
            goto done;
        }
    }
    if (!route_nav_steps_check(steps, config->record_nav_count, marks, mark_count, xinput_script_total_frames(script), reason, sizeof reason)) {
        snprintf(error, error_size, "record '# nav:' lines: %s", reason);
        goto done;
    }
    const char *menus_text = config->nav_menus_text;
    size_t menus_length = menus_text != NULL ? strlen(menus_text) : 0u;
    if (menus_text == NULL && config->nav_menus_file != NULL) {
        if (!read_text_file(config->nav_menus_file, &file_text, &menus_length)) {
            snprintf(error, error_size, "--route-nav-menus %s: cannot read the menu table", config->nav_menus_file);
            goto done;
        }
        menus_text = file_text;
    }
    bool have_table = false;
    if (menus_text != NULL) {
        if (!route_nav_menus_parse(menus_text, menus_length, table, reason, sizeof reason)) {
            snprintf(error, error_size, "--route-nav-menus %s: %s", config->nav_menus_file != NULL ? config->nav_menus_file : "(text)", reason);
            goto done;
        }
        have_table = true;
    }
    route_nav_mode mode = have_table ? ROUTE_NAV_MODE_ON : ROUTE_NAV_MODE_OFF;
    if (config->nav_mode != NULL && !route_nav_mode_parse(config->nav_mode, &mode)) {
        snprintf(error, error_size, "--route-nav %s: want on, off or strict", config->nav_mode);
        goto done;
    }
    route_nav_timing timing = route_nav_timing_default();
    if (config->nav_timing != NULL && !route_nav_timing_parse(config->nav_timing, &timing, error, error_size)) goto done;
    xinput_route_nav_config nav = {steps, config->record_nav_count, have_table ? table : NULL, mode, &timing,
                                   config->read_bytes != NULL ? config->read_bytes : bytes_adapter, config->test_user};
    if (!xinput_route_set_nav(route, &nav, error, error_size)) goto done;
    if (mode == ROUTE_NAV_MODE_OFF)
        fprintf(stderr, "route nav (T1640): off%s, the record's %zu nav step(s) are ignored and the recorded presses replay open loop\n",
                config->nav_mode == NULL ? " (no --route-nav-menus table)" : "", config->record_nav_count);
    else
        fprintf(stderr, "route nav (T1640, FABRICATED input): %zu step(s), mode %s, %zu menu(s) from %s, hold %u gap %u polls\n",
                config->record_nav_count, mode == ROUTE_NAV_MODE_STRICT ? "strict" : "on", have_table ? table->count : 0u,
                config->nav_menus_file != NULL ? config->nav_menus_file : (have_table ? "(text)" : "(no table)"), timing.hold, timing.gap);
    good = true;
done:
    free(file_text);
    free(table);
    free(steps);
    return good;
}

bool host_route_setup_from(const host_route_config *config, const xinput_script *script, const uint64_t *marks,
                           size_t mark_count, char *error, size_t error_size)
{
    xinput_route_wait waits[XINPUT_ROUTE_MAX_WAITS];
    size_t wait_total = 0u;
    if (config->wait_count + config->event_wait_count > XINPUT_ROUTE_MAX_WAITS || config->poke_count > POKE_TRIGGER_MAX) {
        snprintf(error, error_size, "too many --route-wait/--route-wait-event or --poke-at-poll specs");
        return false;
    }
    for (size_t i = 0u; i < config->wait_count; i++)
        if (!xinput_route_wait_parse(config->waits[i], &waits[wait_total++], error, error_size)) return false;
    for (size_t i = 0u; i < config->event_wait_count; i++)
        if (!xinput_route_event_wait_parse(config->event_waits[i], &waits[wait_total++], error, error_size)) return false;
    const size_t command_line_waits = wait_total;
    for (size_t i = 0u; i < config->record_wait_count; i++) {
        xinput_route_wait wait;
        char parse_error[200];
        if (!xinput_route_event_wait_parse(config->record_waits[i], &wait, parse_error, sizeof parse_error)) {
            snprintf(error, error_size, "record '# wait:' line %zu: %s", i + 1u, parse_error);
            return false;
        }
        bool overridden = false;
        for (size_t j = 0u; j < command_line_waits; j++) overridden = overridden || waits[j].mark == wait.mark;
        if (overridden) {
            fprintf(stderr, "route: the record's wait for mark %u is replaced by the command line's\n", wait.mark);
            continue;
        }
        if (wait_total >= XINPUT_ROUTE_MAX_WAITS) {
            snprintf(error, error_size, "too many waits (command line plus the record's)");
            return false;
        }
        for (size_t j = 0u; j < wait_total; j++) {
            if (waits[j].mark == wait.mark) {
                snprintf(error, error_size, "the record has two '# wait:' lines for mark%u", wait.mark);
                return false;
            }
        }
        waits[wait_total++] = wait;
    }
    poke_trigger_set pokes;
    memset(&pokes, 0, sizeof pokes);
    for (size_t i = 0u; i < config->poke_count; i++) {
        if (!poke_trigger_parse(config->pokes[i], &pokes.entries[i], error, error_size)) return false;
        const poke_trigger_entry *entry = &pokes.entries[i];
        if (entry->where == POKE_AT_MARK && entry->value > mark_count) {
            snprintf(error, error_size, "--poke-at-poll names mark%llu but the record has %zu mark(s)",
                     (unsigned long long)entry->value, mark_count);
            return false;
        }
        pokes.count = (unsigned)i + 1u;
    }
    xinput_route_hooks hooks = {mem_adapter, NULL, config->live, config->live_user, on_event, NULL, on_poll, NULL, {0}};
    if (config->probe != NULL) {
        hooks.probe = *config->probe;
    } else {
        route_probe_enable();
        hooks.probe = *route_probe_view();
    }
    xinput_route *route = xinput_route_create(script, marks, mark_count, waits, wait_total, &hooks, error, error_size);
    if (route == NULL) return false;
    if (!setup_nav(route, config, script, marks, mark_count, error, error_size)) {
        xinput_route_free(route);
        return false;
    }
    host_route_teardown();
    g_route = route;
    g_pokes = pokes;
    g_config = *config;
    xinput_route_install(route);
    return true;
}

bool host_route_setup(const host_route_config *config, char *error, size_t error_size)
{
    const uint64_t *marks = NULL;
    const size_t mark_count = xinput_replay_marks(&marks);
    const xinput_script *script = xinput_replay_script();
    if (script == NULL) {
        snprintf(error, error_size, "no replay is loaded");
        return false;
    }
    host_route_config merged = *config;
    const char *record_waits[XINPUT_ROUTE_MAX_WAITS];
    size_t record_count = xinput_replay_wait_count();
    if (record_count > XINPUT_ROUTE_MAX_WAITS) record_count = XINPUT_ROUTE_MAX_WAITS;
    for (size_t i = 0u; i < record_count; i++) record_waits[i] = xinput_replay_wait(i);
    merged.record_waits = record_waits;
    merged.record_wait_count = record_count;
    const char *record_navs[ROUTE_NAV_MAX_STEPS];
    size_t nav_count = xinput_replay_nav_count();
    if (nav_count > ROUTE_NAV_MAX_STEPS) nav_count = ROUTE_NAV_MAX_STEPS;
    for (size_t i = 0u; i < nav_count; i++) record_navs[i] = xinput_replay_nav(i);
    merged.record_navs = record_navs;
    merged.record_nav_count = nav_count;
    return host_route_setup_from(&merged, script, marks, mark_count, error, error_size);
}

xinput_route *host_route_current(void) { return g_route; }

/* ---- T1640 recorder side ---- */
static nav_recorder *g_nav_recorder;

static bool nav_rec_read(uint32_t address, unsigned width, uint32_t *value, void *user)
{
    (void)user;
    return g_config.read_mem != NULL ? g_config.read_mem(address, width, value, g_config.test_user) : read_guest(address, width, value);
}
static bool nav_rec_bytes(uint32_t address, void *buffer, size_t length, void *user)
{
    (void)user;
    if (g_config.read_bytes != NULL) return g_config.read_bytes(address, buffer, length, g_config.test_user);
    return bytes_adapter(address, buffer, length, NULL);
}
static uint64_t nav_rec_now(void *user)
{
    (void)user;
    return 0u;
}
static void nav_rec_log(const char *line, void *user)
{
    (void)user;
    if (route_probe_log_active()) route_probe_log_line("%s", line);
}
static const char *nav_rec_add(uint64_t at, uint64_t to, uint64_t edge, const char *body, int analog, uint16_t digital, void *user)
{
    (void)user;
    return xinput_record_add_nav(at, to, edge, body, analog, digital);
}
static void nav_rec_observe(uint64_t poll, const xinput_pad_state *recorded, void *user)
{
    nav_recorder_observe(user, poll, recorded);
}
static void nav_rec_close(uint64_t total, void *user)
{
    nav_recorder_close(user, total);
}
static void nav_rec_record_log(const char *line, void *user)
{
    nav_rec_log(line, user);
}

void host_route_nav_record_stop(void)
{
    if (g_nav_recorder == NULL) return;
    xinput_record_set_nav_hooks(NULL, NULL, NULL, NULL);
    nav_recorder_free(g_nav_recorder);
    g_nav_recorder = NULL;
}

bool host_route_nav_record_start(const host_route_config *config, char *error, size_t error_size)
{
    host_route_nav_record_stop();
    g_config = *config; /* the read seams; a replay and a recording never run together */
    char *file_text = NULL;
    const char *text = config->nav_menus_text;
    size_t length = text != NULL ? strlen(text) : 0u;
    if (text == NULL) {
        if (config->nav_menus_file == NULL || !read_text_file(config->nav_menus_file, &file_text, &length)) {
            snprintf(error, error_size, "--route-nav-menus %s: cannot read the menu table", config->nav_menus_file != NULL ? config->nav_menus_file : "(none)");
            return false;
        }
        text = file_text;
    }
    route_nav_menu_table *table = calloc(1u, sizeof *table);
    char reason[300];
    bool good = false;
    if (table == NULL) {
        snprintf(error, error_size, "out of memory");
    } else if (!route_nav_menus_parse(text, length, table, reason, sizeof reason)) {
        snprintf(error, error_size, "--route-nav-menus %s: %s", config->nav_menus_file != NULL ? config->nav_menus_file : "(text)", reason);
    } else if (table->count == 0u) {
        snprintf(error, error_size, "--route-nav-menus %s: the table has no menu", config->nav_menus_file != NULL ? config->nav_menus_file : "(text)");
    } else {
        const nav_record_hooks hooks = {{nav_rec_read, nav_rec_bytes, nav_rec_now, NULL, NULL, NULL, NULL}, nav_rec_log, NULL, nav_rec_add, NULL};
        g_nav_recorder = nav_recorder_create(table, &hooks);
        if (g_nav_recorder == NULL) snprintf(error, error_size, "out of memory");
        else {
            xinput_record_set_nav_hooks(nav_rec_observe, nav_rec_close, nav_rec_record_log, g_nav_recorder);
            good = true;
        }
    }
    free(table);
    free(file_text);
    return good;
}

bool host_route_handed_over(void) { return g_route != NULL && xinput_route_ended(g_route); }

void host_route_teardown(void)
{
    if (g_route != NULL) {
        xinput_source_install(NULL, NULL);
        xinput_route_free(g_route);
        g_route = NULL;
    }
}
