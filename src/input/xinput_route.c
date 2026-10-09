/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xinput_route.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* T1633: per wait runtime state: probe watch ids and the counters taken when the segment (the previous mark) began. */
typedef struct {
    int watch[ROUTE_COND_MAX];
    xinput_route_watch_state base[ROUTE_COND_MAX];
    xinput_route_frame_state base_frame;
    bool armed;
} wait_runtime;

struct xinput_route {
    const xinput_script *script;
    uint64_t total;
    uint64_t marks[XINPUT_ROUTE_MAX_MARKS];
    bool mark_fired[XINPUT_ROUTE_MAX_MARKS];
    size_t mark_count;
    xinput_route_wait waits[XINPUT_ROUTE_MAX_WAITS];
    wait_runtime runtime[XINPUT_ROUTE_MAX_WAITS];
    size_t wait_count;
    xinput_route_hooks hooks;
    uint64_t cursor;
    uint64_t stalled;      /* polls spent in the current wait */
    uint64_t stalled_total;
    uint64_t stall_start_ms, last_progress_ms; /* T1633, wall clock of the current wait */
    uint64_t stalled_ms_total;
    bool segment_started;  /* the first pad poll armed mark 1's wait */
    bool waiting, ended, failed;
    /* T1640 closed loop menu navigation */
    route_nav_step nav_steps[ROUTE_NAV_MAX_STEPS];
    wait_runtime nav_runtime[ROUTE_NAV_MAX_STEPS];
    size_t nav_count, nav_next;
    route_nav_menu_table nav_menus;
    bool nav_have_menus;
    route_nav_mode nav_mode;
    route_nav_timing nav_timing;
    route_nav nav;
    bool nav_active;
    uint64_t nav_started_poll, nav_polls_total;
    bool (*nav_read_bytes)(uint32_t address, void *buffer, size_t length, void *user);
    void *nav_bytes_user;
};

/* Register the observers a wait's conditions need (file/call watches) and check the others can be observed. `label` is the mark
 * number (or 0 for a T1640 nav expect, worded accordingly). */
static bool register_conditions(xinput_route *route, const xinput_route_wait *wait, wait_runtime *rt, unsigned label, char *error,
                                size_t error_size)
{
    const xinput_route_hooks *hooks = &route->hooks;
    char where[40];
    if (label != 0u) snprintf(where, sizeof where, "route wait mark%u", label);
    else snprintf(where, sizeof where, "route nav expect=");
    for (unsigned c = 0u; c < wait->cond_count; c++) {
        const route_cond *cond = &wait->conds[c];
        rt->watch[c] = -1;
        char text[80];
        xinput_route_cond_format(cond, text, sizeof text);
        if (cond->kind == RCOND_FRAME_CHANGE || cond->kind == RCOND_FRAME_STABLE) {
            if (hooks->probe.frame_state == NULL) {
                snprintf(error, error_size, "%s: %s cannot be observed (this host has no presented frame observer)", where, text);
                return false;
            }
        } else if (cond->kind != RCOND_MEM) {
            const int id = hooks->probe.watch != NULL ? hooks->probe.watch(cond->kind, cond->text, cond->va, hooks->probe.user) : -1;
            if (id < 0) {
                snprintf(error, error_size, "%s: %s cannot be observed (this host has no %s observer)", where, text,
                         cond->kind == RCOND_CALL ? "guest call" : "guest file I/O");
                return false;
            }
            rt->watch[c] = id;
        } else if (hooks->read_mem == NULL) {
            snprintf(error, error_size, "%s: %s cannot be observed (no guest memory reader)", where, text);
            return false;
        }
    }
    return true;
}

xinput_route *xinput_route_create(const xinput_script *script, const uint64_t *marks, size_t mark_count,
                                  const xinput_route_wait *waits, size_t wait_count, const xinput_route_hooks *hooks,
                                  char *error, size_t error_size)
{
    if (script == NULL || hooks == NULL || mark_count > XINPUT_ROUTE_MAX_MARKS || wait_count > XINPUT_ROUTE_MAX_WAITS ||
        (mark_count != 0u && marks == NULL) || (wait_count != 0u && waits == NULL)) {
        snprintf(error, error_size, "route: bad arguments");
        return NULL;
    }
    const uint64_t total = xinput_script_total_frames(script);
    for (size_t i = 0u; i < mark_count; i++) {
        if (marks[i] > total || (i != 0u && marks[i] < marks[i - 1u])) {
            snprintf(error, error_size, "route: mark %zu at %llu is out of order or past the record (%llu polls)", i + 1u,
                     (unsigned long long)marks[i], (unsigned long long)total);
            return NULL;
        }
    }
    for (size_t i = 0u; i < wait_count; i++) {
        if (waits[i].mark == 0u || waits[i].mark > mark_count) {
            snprintf(error, error_size, "route wait names mark%u but the record has %zu mark(s)", waits[i].mark, mark_count);
            return NULL;
        }
        for (size_t j = 0u; j < i; j++) {
            if (waits[j].mark == waits[i].mark) {
                snprintf(error, error_size, "route: two waits for mark%u", waits[i].mark);
                return NULL;
            }
        }
    }
    xinput_route *route = calloc(1u, sizeof *route);
    if (route == NULL) {
        snprintf(error, error_size, "route: out of memory");
        return NULL;
    }
    route->script = script;
    route->total = total;
    route->mark_count = mark_count;
    if (mark_count != 0u) memcpy(route->marks, marks, mark_count * sizeof *marks);
    route->wait_count = wait_count;
    if (wait_count != 0u) memcpy(route->waits, waits, wait_count * sizeof *waits);
    route->hooks = *hooks;
    for (size_t i = 0u; i < wait_count; i++) {
        if (!register_conditions(route, &route->waits[i], &route->runtime[i], route->waits[i].mark, error, error_size)) {
            free(route);
            return NULL;
        }
    }
    return route;
}

void xinput_route_free(xinput_route *route) { free(route); }

static uint64_t now_ms(const xinput_route *route)
{
    if (route->hooks.probe.now_ms != NULL) return route->hooks.probe.now_ms(route->hooks.probe.user);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void emit_detail(xinput_route *route, xinput_route_event_kind kind, unsigned index, uint64_t poll, const char *detail)
{
    if (route->hooks.on_event == NULL) return;
    const uint64_t spent = route->waiting || kind == XINPUT_ROUTE_WAIT_OK ? now_ms(route) - route->stall_start_ms : 0u;
    const xinput_route_event event = {kind, index, route->cursor, poll, route->stalled, spent, detail};
    route->hooks.on_event(&event, route->hooks.event_user);
}

static void emit(xinput_route *route, xinput_route_event_kind kind, unsigned index, uint64_t poll)
{
    emit_detail(route, kind, index, poll, NULL);
}

static bool read_value(const xinput_route *route, const route_cond *cond, uint32_t *value)
{
    return xinput_route_mem_read(cond, route->hooks.read_mem, route->hooks.mem_user, value);
}

static bool compare(route_cmp cmp, uint32_t left, uint32_t right) { return xinput_route_compare(cmp, left, right); }

/* One condition against the observations. `note` (optional) receives what was seen, for the progress and failure lines. */
static bool cond_holds(const xinput_route *route, const xinput_route_wait *wait, const wait_runtime *rt, unsigned c, char *note,
                       size_t note_size)
{
    const route_cond *cond = &wait->conds[c];
    char name[80];
    xinput_route_cond_format(cond, name, sizeof name);
    bool holds = false;
    char seen[96];
    seen[0] = '\0';
    if (cond->kind == RCOND_MEM) {
        uint32_t value = 0u;
        if (route->hooks.read_mem != NULL && read_value(route, cond, &value)) {
            holds = compare(cond->cmp, value & cond->mask, cond->value);
            snprintf(seen, sizeof seen, "read 0x%X", (unsigned)(value & cond->mask));
        } else {
            snprintf(seen, sizeof seen, "unreadable");
        }
    } else if (cond->kind == RCOND_FRAME_CHANGE || cond->kind == RCOND_FRAME_STABLE) {
        xinput_route_frame_state frame;
        memset(&frame, 0, sizeof frame);
        if (route->hooks.probe.frame_state != NULL && route->hooks.probe.frame_state(&frame, route->hooks.probe.user)) {
            const uint64_t changes = frame.changes - rt->base_frame.changes;
            const uint64_t quiet = changes != 0u ? now_ms(route) - frame.last_change_ms : 0u;
            holds = changes != 0u && (cond->kind == RCOND_FRAME_CHANGE || quiet >= cond->amount);
            snprintf(seen, sizeof seen, "%llu change(s), quiet %llu ms", (unsigned long long)changes, (unsigned long long)quiet);
        } else {
            snprintf(seen, sizeof seen, "no frame observer");
        }
    } else {
        xinput_route_watch_state state;
        memset(&state, 0, sizeof state);
        if (rt->watch[c] >= 0 && route->hooks.probe.watch_state != NULL &&
            route->hooks.probe.watch_state(rt->watch[c], &state, route->hooks.probe.user)) {
            const uint64_t count = state.count - rt->base[c].count;
            const uint64_t bytes = state.bytes - rt->base[c].bytes;
            const uint64_t quiet = state.last_ms != 0u ? now_ms(route) - state.last_ms : 0u;
            switch (cond->kind) {
            case RCOND_FILE_OPEN: holds = count >= 1u; snprintf(seen, sizeof seen, "%llu open(s)", (unsigned long long)count); break;
            case RCOND_FILE_READ:
                holds = bytes >= cond->amount;
                snprintf(seen, sizeof seen, "%llu byte(s) in %llu read(s)", (unsigned long long)bytes, (unsigned long long)count);
                break;
            case RCOND_FILE_IDLE:
                holds = count != 0u && quiet >= cond->amount;
                if (count == 0u) snprintf(seen, sizeof seen, "no file I/O yet");
                else snprintf(seen, sizeof seen, "%llu event(s), last %llu ms ago", (unsigned long long)count, (unsigned long long)quiet);
                break;
            case RCOND_CALL: holds = count >= cond->amount; snprintf(seen, sizeof seen, "%llu call(s)", (unsigned long long)count); break;
            default: break;
            }
        } else {
            snprintf(seen, sizeof seen, "no observer");
        }
    }
    if (note != NULL) snprintf(note, note_size, "%s %s (%s)", holds ? "OK  " : "WAIT", name, seen);
    return holds;
}

/* `stalled` polls and `start_ms` since the wait's position was reached (a mark wait: the route's stall, a T1640 expect: the activation). */
static bool wait_holds(const xinput_route *route, const xinput_route_wait *wait, const wait_runtime *rt, uint64_t stalled, uint64_t start_ms)
{
    if (stalled < wait->min_polls) return false;
    if (wait->min_ms != 0u && now_ms(route) - start_ms < wait->min_ms) return false;
    if (wait->has_mem) {
        uint32_t value = 0u;
        if (route->hooks.read_mem == NULL || !route->hooks.read_mem(wait->address, wait->width, &value, route->hooks.mem_user))
            return false;
        if ((value & wait->mask) != wait->value) return false;
    }
    if (wait->cond_count == 0u) return true;
    for (unsigned c = 0u; c < wait->cond_count; c++) {
        const bool holds = cond_holds(route, wait, rt, c, NULL, 0u);
        if (wait->any && holds) return true;
        if (!wait->any && !holds) return false;
    }
    return !wait->any;
}

/* One line per condition, `; ` separated, plus the poll and ms minimums. Empty for a legacy wait. */
static void describe_wait(const xinput_route *route, const xinput_route_wait *wait, const wait_runtime *rt, uint64_t stalled, char *out,
                          size_t out_size)
{
    size_t used = 0u;
    out[0] = '\0';
    if (!wait->event) return;
    for (unsigned c = 0u; c < wait->cond_count && used + 2u < out_size; c++) {
        char note[200];
        cond_holds(route, wait, rt, c, note, sizeof note);
        const int n = snprintf(out + used, out_size - used, "%s%s", c != 0u ? "; " : "", note);
        if (n < 0) break;
        used += (size_t)n;
        if (used >= out_size) { used = out_size - 1u; break; }
    }
    if (wait->any && used + 12u < out_size) used += (size_t)snprintf(out + used, out_size - used, " [any of]");
    if ((wait->min_polls != 0u || wait->min_ms != 0u) && used + 48u < out_size)
        snprintf(out + used, out_size - used, "%smin %llu polls / %llu ms (stalled %llu polls)", used != 0u ? "; " : "",
                 (unsigned long long)wait->min_polls, (unsigned long long)wait->min_ms, (unsigned long long)stalled);
}

/* The segment of wait `mark` starts now: take the counters its conditions are measured from. */
static void arm_runtime(xinput_route *route, const xinput_route_wait *wait, wait_runtime *rt)
{
    for (unsigned c = 0u; c < wait->cond_count; c++) {
        memset(&rt->base[c], 0, sizeof rt->base[c]);
        if (rt->watch[c] >= 0 && route->hooks.probe.watch_state != NULL)
            route->hooks.probe.watch_state(rt->watch[c], &rt->base[c], route->hooks.probe.user);
    }
    memset(&rt->base_frame, 0, sizeof rt->base_frame);
    if (route->hooks.probe.frame_state != NULL) route->hooks.probe.frame_state(&rt->base_frame, route->hooks.probe.user);
    rt->armed = true;
}

static void arm_for_mark(xinput_route *route, unsigned mark)
{
    for (size_t i = 0u; i < route->wait_count; i++)
        if (route->waits[i].mark == mark) arm_runtime(route, &route->waits[i], &route->runtime[i]);
}

static size_t wait_index_of(const xinput_route *route, unsigned mark)
{
    for (size_t i = 0u; i < route->wait_count; i++)
        if (route->waits[i].mark == mark) return i;
    return (size_t)-1;
}

/* Fire the marks at the cursor (after their waits). False when the route stalls at one or has failed: the pad stays at rest. */
static bool fire_marks(xinput_route *route, uint64_t poll_index)
{
    for (size_t i = 0u; i < route->mark_count; i++) {
        if (route->mark_fired[i] || route->marks[i] != route->cursor) continue;
        const unsigned number = (unsigned)i + 1u;
        const size_t wait_index = wait_index_of(route, number);
        if (wait_index != (size_t)-1) {
            const xinput_route_wait *wait = &route->waits[wait_index];
            if (!route->waiting) {
                route->waiting = true;
                route->stall_start_ms = route->last_progress_ms = now_ms(route);
            }
            if (!wait_holds(route, wait, &route->runtime[wait_index], route->stalled, route->stall_start_ms)) {
                const uint64_t spent = now_ms(route) - route->stall_start_ms;
                if ((wait->max_polls != 0u && route->stalled >= wait->max_polls) || (wait->timeout_ms != 0u && spent >= wait->timeout_ms)) {
                    char detail[640];
                    describe_wait(route, wait, &route->runtime[wait_index], route->stalled, detail, sizeof detail);
                    route->failed = true;
                    emit_detail(route, XINPUT_ROUTE_WAIT_TIMEOUT, number, poll_index, detail);
                    return false;
                }
                if (wait->event && now_ms(route) - route->last_progress_ms >= XINPUT_ROUTE_PROGRESS_MS) {
                    char detail[640];
                    route->last_progress_ms = now_ms(route);
                    describe_wait(route, wait, &route->runtime[wait_index], route->stalled, detail, sizeof detail);
                    emit_detail(route, XINPUT_ROUTE_WAIT_PROGRESS, number, poll_index, detail);
                }
                route->stalled++;
                route->stalled_total++;
                return false; /* the pad stays at rest, the record does not advance */
            }
            char detail[640];
            describe_wait(route, wait, &route->runtime[wait_index], route->stalled, detail, sizeof detail);
            route->stalled_ms_total += now_ms(route) - route->stall_start_ms;
            emit_detail(route, XINPUT_ROUTE_WAIT_OK, number, poll_index, wait->event ? detail : NULL);
            route->waiting = false;
            route->stalled = 0u;
        }
        route->mark_fired[i] = true;
        arm_for_mark(route, number + 1u); /* the next segment begins at this mark */
        emit(route, XINPUT_ROUTE_MARK, number, poll_index);
    }
    return true;
}

/* ---- T1640 closed loop menu navigation ---- */

static uint64_t nav_now(void *user) { return now_ms(user); }
static bool nav_read(uint32_t address, unsigned width, uint32_t *value, void *user)
{
    const xinput_route *route = user;
    return route->hooks.read_mem != NULL && route->hooks.read_mem(address, width, value, route->hooks.mem_user);
}
static bool nav_read_bytes(uint32_t address, void *buffer, size_t length, void *user)
{
    const xinput_route *route = user;
    return route->nav_read_bytes != NULL && route->nav_read_bytes(address, buffer, length, route->nav_bytes_user);
}
static void nav_expect_arm(unsigned number, void *user)
{
    xinput_route *route = user;
    arm_runtime(route, &route->nav_steps[number - 1u].expect_wait, &route->nav_runtime[number - 1u]);
}
static bool nav_expect_holds(unsigned number, uint64_t polls, uint64_t started_ms, char *note, size_t note_size, void *user)
{
    xinput_route *route = user;
    const xinput_route_wait *wait = &route->nav_steps[number - 1u].expect_wait;
    const wait_runtime *rt = &route->nav_runtime[number - 1u];
    describe_wait(route, wait, rt, polls, note, note_size);
    return wait_holds(route, wait, rt, polls, started_ms);
}
static void nav_progress(const char *line, void *user)
{
    xinput_route *route = user;
    emit_detail(route, XINPUT_ROUTE_NAV_PROGRESS, (unsigned)route->nav_next + 1u, route->nav_started_poll, line);
}

typedef enum { NAV_IDLE, NAV_PAD, NAV_JUMPED } nav_result;

/* At the position `at` of the next step the nav machine takes the pad. NAV_PAD: `out` is this poll's pad (or the route failed).
 * NAV_JUMPED: the cursor moved to `to`, marks there fire next. NAV_IDLE: replay the record as usual (also after a fallback). */
static nav_result nav_poll_step(xinput_route *route, uint64_t poll_index, xinput_pad_state *out)
{
    if (!route->nav_active) {
        if (route->nav_next >= route->nav_count || route->nav_steps[route->nav_next].at != route->cursor) return NAV_IDLE;
        const route_nav_step *step = &route->nav_steps[route->nav_next];
        const route_nav_probe probe = {nav_read, nav_read_bytes, nav_now, nav_expect_arm, nav_expect_holds, nav_progress, route};
        route_nav_begin(&route->nav, step, (unsigned)route->nav_next + 1u, route_nav_menu_find(route->nav_have_menus ? &route->nav_menus : NULL, step->menu),
                        route->nav_mode, &route->nav_timing, &probe);
        route->nav_active = true;
        route->nav_started_poll = poll_index;
        char detail[220], label[ROUTE_NAV_SELECT_TEXT + ROUTE_NAV_ID_MAX + 16u];
        route_nav_step_label(step, label, sizeof label);
        snprintf(detail, sizeof detail, "step %zu (%s) at record position %llu replaces the recorded presses up to %llu", route->nav_next + 1u, label,
                 (unsigned long long)step->at, (unsigned long long)step->to);
        emit_detail(route, XINPUT_ROUTE_NAV_START, (unsigned)route->nav_next + 1u, poll_index, detail);
    }
    const route_nav_status status = route_nav_poll(&route->nav, poll_index, out);
    route->nav_polls_total++;
    if (status == ROUTE_NAV_RUNNING) return NAV_PAD;
    const route_nav_step *step = &route->nav_steps[route->nav_next];
    const unsigned number = (unsigned)route->nav_next + 1u;
    route->nav_active = false;
    route->nav_next++;
    if (status == ROUTE_NAV_FAILED) {
        route->failed = true;
        emit_detail(route, XINPUT_ROUTE_NAV_FAIL, number, poll_index, route_nav_message(&route->nav));
        return NAV_PAD;
    }
    if (status == ROUTE_NAV_FALLBACK) {
        emit_detail(route, XINPUT_ROUTE_NAV_FALLBACK, number, poll_index, route_nav_message(&route->nav));
        return NAV_IDLE;
    }
    char detail[240];
    snprintf(detail, sizeof detail, "step %u done after %u press(es), %u lost, %llu polls: the record continues at position %llu (menu first ready after %llu polls)",
             number, route->nav.presses, route->nav.lost, (unsigned long long)route->nav.polls, (unsigned long long)step->to,
             (unsigned long long)route->nav.ready_polls);
    route->cursor = step->to;
    emit_detail(route, XINPUT_ROUTE_NAV_OK, number, poll_index, detail);
    return NAV_JUMPED;
}

bool xinput_route_source(uint64_t poll_index, xinput_pad_state *out, void *user)
{
    xinput_route *route = user;
    memset(out, 0, sizeof *out);
    if (route->hooks.on_poll != NULL) route->hooks.on_poll(poll_index, route->cursor, route->hooks.poll_user);
    xinput_pad_state live_state;
    memset(&live_state, 0, sizeof live_state);
    bool live_supplied = false;
    if (route->hooks.live != NULL) live_supplied = route->hooks.live(poll_index, &live_state, route->hooks.live_user);
    if (route->failed) return true; /* the host is stopping, nothing is replayed any more */
    if (!route->segment_started) {
        route->segment_started = true;
        arm_for_mark(route, 1u);
    }
    for (;;) {
        if (!fire_marks(route, poll_index)) return true; /* stalled at a mark wait, or failed */
        const nav_result nav = nav_poll_step(route, poll_index, out);
        if (nav == NAV_PAD) return true;
        if (nav == NAV_IDLE) break;
    }
    if (route->cursor >= route->total) {
        if (!route->ended) {
            route->ended = true;
            emit(route, XINPUT_ROUTE_END, 0u, poll_index);
        }
        if (live_supplied) *out = live_state;
        return true;
    }
    *out = xinput_script_state_at(route->script, route->cursor++);
    return true;
}

void xinput_route_install(xinput_route *route) { xinput_source_install(xinput_route_source, route); }
uint64_t xinput_route_cursor(const xinput_route *route) { return route->cursor; }
bool xinput_route_ended(const xinput_route *route) { return route->ended; }
bool xinput_route_failed(const xinput_route *route) { return route->failed; }
uint64_t xinput_route_stalled_total(const xinput_route *route) { return route->stalled_total; }
uint64_t xinput_route_stalled_ms_total(const xinput_route *route) { return route->stalled_ms_total; }
size_t xinput_route_nav_done(const xinput_route *route) { return route->nav_next; }
uint64_t xinput_route_nav_polls_total(const xinput_route *route) { return route->nav_polls_total; }

bool xinput_route_set_nav(xinput_route *route, const xinput_route_nav_config *config, char *error, size_t error_size)
{
    if (route == NULL || config == NULL || (config->step_count != 0u && config->steps == NULL) || config->step_count > ROUTE_NAV_MAX_STEPS ||
        route->nav_count != 0u) {
        snprintf(error, error_size, "route nav: bad arguments");
        return false;
    }
    route->nav_mode = config->mode;
    route->nav_timing = config->timing != NULL ? *config->timing : route_nav_timing_default();
    route->nav_read_bytes = config->read_bytes;
    route->nav_bytes_user = config->bytes_user;
    if (config->mode == ROUTE_NAV_MODE_OFF || config->step_count == 0u) return true;
    if (config->menus != NULL) {
        route->nav_menus = *config->menus;
        route->nav_have_menus = true;
    }
    for (size_t i = 0u; i < config->step_count; i++) {
        const route_nav_step *step = &config->steps[i];
        if (step->at > step->to || step->to > route->total) {
            snprintf(error, error_size, "route nav step %zu: at=%llu to=%llu is not inside the record (%llu polls)", i + 1u,
                     (unsigned long long)step->at, (unsigned long long)step->to, (unsigned long long)route->total);
            return false;
        }
        if (config->mode == ROUTE_NAV_MODE_STRICT && route_nav_menu_find(route->nav_have_menus ? &route->nav_menus : NULL, step->menu) == NULL) {
            snprintf(error, error_size, "route nav step %zu names menu '%s' which the menu table does not define (--route-nav strict)", i + 1u,
                     step->menu);
            return false;
        }
        route->nav_steps[i] = *step;
        if (step->has_expect && !register_conditions(route, &route->nav_steps[i].expect_wait, &route->nav_runtime[i], 0u, error, error_size)) {
            return false;
        }
    }
    route->nav_count = config->step_count;
    return true;
}
