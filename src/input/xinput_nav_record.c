/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xinput_nav_record.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GATE_VALUES_MAX 32u
#define SLOTS_MAX 64u
#define DPAD_MASK 0x000Fu

struct nav_recorder {
    route_nav_menu_table table;
    nav_record_hooks hooks;
    route_nav scratch;
    /* the cheap early-out: every menu starts its active= with `ADDR:W==VALUE` on the same ADDR (the game mode word) */
    bool gate_valid;
    route_cond gate;
    uint32_t gate_values[GATE_VALUES_MAX];
    unsigned gate_count;
    /* what was last logged */
    bool seeded;
    int last_menu;
    bool last_cursor_ok, last_count_ok, last_itemid_ok, last_ready;
    uint32_t last_cursor, last_count, last_itemid;
    /* the page instance */
    int instance_menu;
    uint32_t instance_page;
    bool have_first_press;
    uint64_t first_press;
    /* the previous recorded state, the activation waiting for the select button to go up */
    xinput_pad_state previous;
    bool have_previous;
    bool act_pending;
    uint64_t act_at, act_edge;
    int act_menu;
    char act_body[260];
    uint64_t gated, scanned;
    unsigned emitted, skipped;
};

static bool select_down(const xinput_pad_state *state, const route_nav_menu *menu)
{
    if (menu->select_analog >= 0) return state->analog[menu->select_analog] >= ROUTE_NAV_SELECT_DOWN;
    return (state->digital_buttons & menu->select_digital) != 0u;
}

static void say(nav_recorder *recorder, const char *format, ...) __attribute__((format(printf, 2, 3)));
static void say(nav_recorder *recorder, const char *format, ...)
{
    if (recorder->hooks.log == NULL) return;
    char line[300];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    recorder->hooks.log(line, recorder->hooks.log_user);
}

nav_recorder *nav_recorder_create(const route_nav_menu_table *table, const nav_record_hooks *hooks)
{
    if (table == NULL || table->count == 0u || hooks == NULL || hooks->probe.read_mem == NULL || hooks->add == NULL) return NULL;
    nav_recorder *recorder = calloc(1u, sizeof *recorder);
    if (recorder == NULL) return NULL;
    recorder->table = *table;
    recorder->hooks = *hooks;
    recorder->last_menu = -2;
    recorder->instance_menu = -1;
    recorder->gate_valid = true;
    for (size_t i = 0u; i < table->count && recorder->gate_valid; i++) {
        const route_nav_menu *menu = &table->menus[i];
        const route_nav_ref *first = NULL;
        for (unsigned c = 0u; c < menu->active_count && first == NULL; c++)
            if (!menu->active[c].page && !menu->active[c].cond.indirect && menu->active[c].cond.cmp == RCMP_EQ) first = &menu->active[c];
        if (first == NULL || (i != 0u && (first->cond.address != recorder->gate.address || first->cond.width != recorder->gate.width ||
                                           first->cond.mask != recorder->gate.mask))) {
            recorder->gate_valid = false;
            break;
        }
        if (i == 0u) recorder->gate = first->cond;
        bool known = false;
        for (unsigned v = 0u; v < recorder->gate_count; v++) known = known || recorder->gate_values[v] == first->cond.value;
        if (!known) {
            if (recorder->gate_count >= GATE_VALUES_MAX) recorder->gate_valid = false;
            else recorder->gate_values[recorder->gate_count++] = first->cond.value;
        }
    }
    return recorder;
}

void nav_recorder_free(nav_recorder *recorder) { free(recorder); }

void nav_recorder_stats(const nav_recorder *recorder, uint64_t *gated, uint64_t *scanned, unsigned *emitted, unsigned *skipped)
{
    if (gated != NULL) *gated = recorder->gated;
    if (scanned != NULL) *scanned = recorder->scanned;
    if (emitted != NULL) *emitted = recorder->emitted;
    if (skipped != NULL) *skipped = recorder->skipped;
}

/* Which menu of the table is on screen: a menu with a cursor and ready first, then one with a cursor, then a cursorless one; the first of the
 * table among equals. -1: none. */
static int sample(nav_recorder *recorder, route_nav_view *best_view)
{
    const route_nav_probe *probe = &recorder->hooks.probe;
    if (recorder->gate_valid) {
        uint32_t value = 0u;
        bool allowed = false;
        if (probe->read_mem(recorder->gate.address, recorder->gate.width, &value, probe->user)) {
            value &= recorder->gate.mask;
            for (unsigned i = 0u; i < recorder->gate_count; i++) allowed = allowed || recorder->gate_values[i] == value;
        }
        if (!allowed) {
            recorder->gated++;
            return -1;
        }
    }
    recorder->scanned++;
    uint32_t builders[SLOTS_MAX];
    unsigned slots = 0u;
    const route_nav_widgets *widgets = &recorder->table.widgets;
    if (widgets->present) {
        slots = widgets->count < SLOTS_MAX ? widgets->count : SLOTS_MAX;
        for (unsigned slot = 0u; slot < slots; slot++) {
            const uint32_t address = widgets->table + slot * widgets->stride;
            uint32_t used = 0u, builder = 0u;
            builders[slot] = 0u;
            if (probe->read_mem(address + ROUTE_NAV_PAGE_USED_OFFSET, 4u, &used, probe->user) && used != 0u &&
                probe->read_mem(address + ROUTE_NAV_PAGE_BUILDER_OFFSET, 4u, &builder, probe->user))
                builders[slot] = builder;
        }
    }
    int best = -1, best_rank = 0;
    for (size_t i = 0u; i < recorder->table.count; i++) {
        const route_nav_menu *menu = &recorder->table.menus[i];
        if (menu->page_builder) {
            bool present = false;
            for (unsigned slot = 0u; slot < slots && !present; slot++) present = builders[slot] == menu->builder;
            if (!present) continue;
        }
        route_nav_view view;
        route_nav_view_read(&recorder->scratch, probe, menu, &view);
        if (!view.active) continue;
        const int rank = menu->has_cursor ? (view.ready ? 3 : 2) : 1;
        if (rank > best_rank) {
            best_rank = rank;
            best = (int)i;
            *best_view = view;
        }
    }
    return best;
}

static void log_change(nav_recorder *recorder, int menu_index, const route_nav_view *view)
{
    const route_nav_menu *menu = menu_index >= 0 ? &recorder->table.menus[menu_index] : NULL;
    const bool cursor_ok = menu != NULL && menu->has_cursor && view->cursor_ok;
    const bool count_ok = menu != NULL && menu->has_cursor && view->count_ok;
    const bool itemid_ok = menu != NULL && menu->has_itemid && view->itemid_ok;
    const bool ready = menu != NULL && view->ready;
    if (recorder->seeded && recorder->last_menu == menu_index && recorder->last_cursor_ok == cursor_ok && recorder->last_count_ok == count_ok &&
        recorder->last_itemid_ok == itemid_ok && recorder->last_ready == ready && (!cursor_ok || recorder->last_cursor == view->cursor) &&
        (!count_ok || recorder->last_count == view->count) && (!itemid_ok || recorder->last_itemid == view->itemid))
        return;
    recorder->seeded = true;
    recorder->last_menu = menu_index;
    recorder->last_cursor_ok = cursor_ok;
    recorder->last_count_ok = count_ok;
    recorder->last_itemid_ok = itemid_ok;
    recorder->last_ready = ready;
    recorder->last_cursor = view->cursor;
    recorder->last_count = view->count;
    recorder->last_itemid = view->itemid;
    if (menu == NULL) {
        say(recorder, "nav menu=none");
        return;
    }
    char cursor[16], count[16], id[16];
    if (!menu->has_cursor) snprintf(cursor, sizeof cursor, "-"), snprintf(count, sizeof count, "-");
    else {
        if (cursor_ok) snprintf(cursor, sizeof cursor, "%u", (unsigned)view->cursor);
        else snprintf(cursor, sizeof cursor, "?");
        if (count_ok) snprintf(count, sizeof count, "%u", (unsigned)view->count);
        else snprintf(count, sizeof count, "?");
    }
    if (!menu->has_itemid) snprintf(id, sizeof id, "-");
    else if (itemid_ok) snprintf(id, sizeof id, "0x%X", (unsigned)view->itemid);
    else snprintf(id, sizeof id, "?");
    say(recorder, "nav menu=%s cursor=%s count=%s id=%s ready=%d", menu->id, cursor, count, id, ready ? 1 : 0);
}

/* The nav grammar's percent encoding of an item text: everything but [A-Za-z0-9._-] as %XX. Empty when it does not fit the 111 characters of a select=. */
static void percent_encode(const char *text, char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t used = 0u;
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0'; c++) {
        const bool plain = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '-';
        if (used + (plain ? 1u : 3u) + 1u > out_size || used + (plain ? 1u : 3u) > ROUTE_NAV_SELECT_TEXT - 1u - 5u) {
            out[0] = '\0';
            return;
        }
        if (plain) out[used++] = (char)*c;
        else {
            out[used++] = '%';
            out[used++] = hex[*c >> 4];
            out[used++] = hex[*c & 15u];
        }
    }
    out[used] = '\0';
}

static void emit_activation(nav_recorder *recorder, uint64_t to)
{
    const route_nav_menu *menu = &recorder->table.menus[recorder->act_menu];
    const char *reason = recorder->hooks.add(recorder->act_at, to, recorder->act_edge, recorder->act_body, menu->select_analog, menu->select_digital,
                                             recorder->hooks.add_user);
    recorder->act_pending = false;
    if (reason == NULL) {
        recorder->emitted++;
        say(recorder, "nav-line at=%llu to=%llu edge=%llu %s", (unsigned long long)recorder->act_at, (unsigned long long)to,
            (unsigned long long)recorder->act_edge, recorder->act_body);
    } else {
        recorder->skipped++;
        say(recorder, "nav-skip menu=%s reason=%s at=%llu to=%llu", menu->id, reason, (unsigned long long)recorder->act_at, (unsigned long long)to);
    }
}

static void skip(nav_recorder *recorder, const route_nav_menu *menu, const char *reason, uint32_t row, uint32_t flags, bool with_row)
{
    recorder->skipped++;
    if (with_row) say(recorder, "nav-skip menu=%s reason=%s row=%u flags=0x%X", menu->id, reason, (unsigned)row, (unsigned)flags);
    else say(recorder, "nav-skip menu=%s reason=%s", menu->id, reason);
}

void nav_recorder_observe(nav_recorder *recorder, uint64_t poll_index, const xinput_pad_state *recorded)
{
    route_nav_view view;
    memset(&view, 0, sizeof view);
    const int menu_index = sample(recorder, &view);
    const route_nav_menu *menu = menu_index >= 0 ? &recorder->table.menus[menu_index] : NULL;
    log_change(recorder, menu_index, &view);
    /* the page instance: a new menu or a new page widget starts it over */
    const uint32_t page = menu != NULL && view.page_found ? view.page_base : 0u;
    if (menu_index != recorder->instance_menu || page != recorder->instance_page) {
        recorder->instance_menu = menu_index;
        recorder->instance_page = page;
        recorder->have_first_press = false;
    }
    const xinput_pad_state before = recorder->have_previous ? recorder->previous : (xinput_pad_state){0};
    /* an activation waits for its select button to go up */
    if (recorder->act_pending && !select_down(recorded, &recorder->table.menus[recorder->act_menu])) emit_activation(recorder, poll_index);
    if (menu != NULL) {
        if (((recorded->digital_buttons & (uint16_t)~before.digital_buttons) & DPAD_MASK) != 0u && !recorder->have_first_press) {
            recorder->have_first_press = true;
            recorder->first_press = poll_index;
        }
        if (!recorder->act_pending && select_down(recorded, menu) && !select_down(&before, menu)) {
            route_nav_row_info row;
            const char *why = NULL;
            bool with_row = false;
            memset(&row, 0, sizeof row);
            if (!view.ready) why = "not-ready";
            else if (menu->has_cursor && (!view.cursor_ok || !view.count_ok || view.cursor == ROUTE_NAV_CURSOR_NONE || view.cursor >= view.count))
                why = "no-cursor-row";
            else if (menu->has_cursor && !route_nav_view_row(&recorder->scratch, &recorder->hooks.probe, menu, &view, menu->selectby == ROUTE_NAV_BY_NAME, &row))
                why = "rows-unreadable";
            else if (row.known && row.skipped) { why = "skipped-row"; with_row = true; }
            else if (row.known && row.greyed) { why = "greyed-row"; with_row = true; }
            if (why != NULL) {
                skip(recorder, menu, why, view.cursor, row.flags, with_row);
            } else {
                recorder->act_pending = true;
                recorder->act_menu = menu_index;
                recorder->act_edge = poll_index;
                /* a menu that does not take the d-pad (dpad=0) has no presses to replace: its range is the select press only */
                recorder->act_at = recorder->have_first_press && !menu->dpad_off ? recorder->first_press : poll_index;
                char encoded[200];
                encoded[0] = '\0';
                if (menu->has_cursor && menu->selectby == ROUTE_NAV_BY_NAME && row.known && row.unique_text) percent_encode(row.text, encoded, sizeof encoded);
                if (encoded[0] != '\0')
                    snprintf(recorder->act_body, sizeof recorder->act_body, "menu=%s select=name:%s activate", menu->id, encoded);
                else if (menu->selectby != ROUTE_NAV_BY_INDEX && menu->has_cursor && row.known && row.unique_id)
                    snprintf(recorder->act_body, sizeof recorder->act_body, "menu=%s select=id:%u activate", menu->id, (unsigned)row.id);
                else
                    snprintf(recorder->act_body, sizeof recorder->act_body, "menu=%s select=index:%u activate", menu->id,
                             menu->has_cursor ? (unsigned)view.cursor : 0u);
                recorder->have_first_press = false;
            }
        }
    }
    recorder->previous = *recorded;
    recorder->have_previous = true;
}

void nav_recorder_close(nav_recorder *recorder, uint64_t total_polls)
{
    if (recorder->act_pending) emit_activation(recorder, total_polls);
}
