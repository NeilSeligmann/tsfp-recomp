/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xinput_route_nav.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define ROUTE_NAV_COUNT_LIMIT 4096u
#define ROUTE_NAV_GREY_GRACE_MS 3000u /* a greyed target row is waited for this long, then the step FAILS */

typedef struct {
    bool found;
    uint32_t base;
} page_ctx;

typedef struct {
    page_ctx page;
    bool active, ready, cursor_ok, count_ok, itemid_ok;
    uint32_t cursor, count, itemid;
    char unmet[480];
} snapshot;

static void append(char *buffer, size_t size, const char *format, ...) __attribute__((format(printf, 3, 4)));
static void append(char *buffer, size_t size, const char *format, ...)
{
    const size_t used = strlen(buffer);
    if (used + 1u >= size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(buffer + used, size - used, format, args);
    va_end(args);
}

size_t route_nav_step_label(const route_nav_step *step, char *out, size_t out_size)
{
    const int n = snprintf(out, out_size, "menu=%s select=%s", step->menu, step->select_text);
    return n < 0 ? 0u : (size_t)n;
}

/* ---- guest reads ---- */

static bool read32(const route_nav *machine, uint32_t address, uint32_t *value)
{
    return machine->probe.read_mem(address, 4u, value, machine->probe.user);
}

/* The page widget a `page=builder:VA` menu is: among the live slots (dword +0 non zero) whose builder (dword +8) is VA and whose
 * state (dword +0x1B0) is not a closing one, the highest state, then the highest slot. */
static bool locate_page(const route_nav *machine, uint32_t *base)
{
    const route_nav_widgets *widgets = &machine->menu->widgets;
    bool found = false;
    uint32_t best_state = 0u;
    for (uint32_t slot = 0u; slot < widgets->count; slot++) {
        const uint32_t address = widgets->table + slot * widgets->stride;
        uint32_t used = 0u, builder = 0u, state = 0u;
        if (!read32(machine, address + ROUTE_NAV_PAGE_USED_OFFSET, &used) || used == 0u) continue;
        if (!read32(machine, address + ROUTE_NAV_PAGE_BUILDER_OFFSET, &builder) || builder != machine->menu->builder) continue;
        if (!read32(machine, address + ROUTE_NAV_PAGE_STATE_OFFSET, &state)) continue;
        bool closing = false;
        for (unsigned i = 0u; i < widgets->closing_count; i++) closing = closing || state == widgets->closing[i];
        if (closing) continue;
        if (!found || state >= best_state) {
            found = true;
            best_state = state;
            *base = address;
        }
    }
    return found;
}

static bool ref_address(const route_nav *machine, const page_ctx *page, const route_nav_ref *ref, uint32_t *address)
{
    if (ref->page) {
        if (!page->found) return false;
        *address = page->base + ref->cond.address;
        return true;
    }
    return xinput_route_mem_address(&ref->cond, machine->probe.read_mem, machine->probe.user, address);
}

static bool read_ref(const route_nav *machine, const page_ctx *page, const route_nav_ref *ref, uint32_t *value)
{
    uint32_t address = 0u, raw = 0u;
    if (!ref_address(machine, page, ref, &address)) return false;
    if (!machine->probe.read_mem(address, ref->cond.width, &raw, machine->probe.user)) return false;
    *value = raw & ref->cond.mask;
    return true;
}

/* All conditions hold? `unmet` collects "label: mem=... (read 0x..)" of those that do not, `; ` separated. */
static bool eval_conds(const route_nav *machine, const page_ctx *page, const route_nav_ref *conds, unsigned count, const char *label, char *unmet,
                       size_t unmet_size)
{
    bool all = true;
    for (unsigned i = 0u; i < count; i++) {
        uint32_t value = 0u;
        const bool readable = read_ref(machine, page, &conds[i], &value);
        if (readable && xinput_route_compare(conds[i].cond.cmp, value, conds[i].cond.value)) continue;
        all = false;
        char text[80];
        xinput_route_cond_format(&conds[i].cond, text, sizeof text);
        if (unmet[0] != '\0') append(unmet, unmet_size, "; ");
        if (readable) append(unmet, unmet_size, "%s %s%s (read 0x%X)", label, conds[i].page ? "page+" : "", text, (unsigned)value);
        else append(unmet, unmet_size, "%s %s%s (unreadable)", label, conds[i].page ? "page+" : "", text);
    }
    return all;
}

static void take_snapshot(const route_nav *machine, snapshot *snap)
{
    memset(snap, 0, sizeof *snap);
    const route_nav_menu *menu = machine->menu;
    if (menu == NULL) return;
    if (menu->page_builder) {
        snap->page.found = locate_page(machine, &snap->page.base);
        if (!snap->page.found) {
            append(snap->unmet, sizeof snap->unmet, "page builder:0x%X is not on screen (no live widget slot)", menu->builder);
            return;
        }
    }
    snap->active = eval_conds(machine, &snap->page, menu->active, menu->active_count, "active", snap->unmet, sizeof snap->unmet);
    snap->ready = eval_conds(machine, &snap->page, menu->ready, menu->ready_count, "ready", snap->unmet, sizeof snap->unmet);
    if (!menu->has_cursor) return;
    snap->cursor_ok = read_ref(machine, &snap->page, &menu->cursor, &snap->cursor);
    snap->count_ok = read_ref(machine, &snap->page, &menu->count, &snap->count);
    if (menu->has_itemid) snap->itemid_ok = read_ref(machine, &snap->page, &menu->itemid, &snap->itemid);
}

/* ---- rows and names ---- */

static bool name_char(uint8_t low, uint8_t high, char *out)
{
    *out = high == 0u && low >= 0x20u && low <= 0x7Eu ? (char)low : '?';
    return low != 0u || high != 0u;
}

/* A NUL terminated 8 bit string at `address`, cut at ROUTE_NAV_NAME_MAX - 1 characters. */
static bool read_cstring(const route_nav *machine, uint32_t address, char *out)
{
    size_t used = 0u;
    out[0] = '\0';
    while (used + 1u < ROUTE_NAV_NAME_MAX) {
        uint8_t chunk[16];
        size_t want = ROUTE_NAV_NAME_MAX - 1u - used;
        if (want > sizeof chunk) want = sizeof chunk;
        bool got = false;
        if (machine->probe.read_bytes != NULL) {
            got = machine->probe.read_bytes(address + (uint32_t)used, chunk, want, machine->probe.user);
        } else {
            got = true;
            for (size_t i = 0u; i < want && got; i++) {
                uint32_t byte = 0u;
                got = machine->probe.read_mem(address + (uint32_t)(used + i), 1u, &byte, machine->probe.user);
                chunk[i] = (uint8_t)byte;
            }
        }
        if (!got) return used != 0u; /* a string running into unreadable memory ends there */
        for (size_t i = 0u; i < want; i++) {
            char c;
            if (!name_char(chunk[i], 0u, &c)) return true;
            out[used++] = c;
            out[used] = '\0';
        }
    }
    return true;
}

/* The rows of an `items=walk` menu: id, flags and (with_text) text of every cursor row, and the stops the cursor can rest on. */
static bool load_rows(route_nav *machine, const page_ctx *page, bool with_text, char *why, size_t why_size)
{
    const route_nav_items *items = &machine->menu->items;
    machine->row_count = 0u;
    machine->stop_count = 0u;
    uint32_t node = 0u;
    if (!read_ref(machine, page, &items->head, &node)) {
        snprintf(why, why_size, "the row list head is unreadable");
        return false;
    }
    for (unsigned guard = 0u; node != 0u; guard++) {
        if (guard >= ROUTE_NAV_MAX_NODES) {
            snprintf(why, why_size, "the row list is longer than %u nodes (cyclic?)", ROUTE_NAV_MAX_NODES);
            return false;
        }
        uint32_t type = 0u, next = 0u;
        if (!read32(machine, node + items->type_off, &type)) {
            snprintf(why, why_size, "the row node at 0x%X is unreadable", (unsigned)node);
            return false;
        }
        bool wanted = false;
        for (unsigned i = 0u; i < items->type_count; i++) wanted = wanted || type == items->types[i];
        if (wanted) {
            if (machine->row_count >= ROUTE_NAV_MAX_ITEMS) {
                snprintf(why, why_size, "the row list has more than %u rows", ROUTE_NAV_MAX_ITEMS);
                return false;
            }
            const unsigned k = machine->row_count++;
            machine->row_id[k] = 0u;
            machine->row_flags[k] = 0u;
            machine->row_text[k][0] = '\0';
            if (items->id_off != 0xFFFFFFFFu && !read32(machine, node + items->id_off, &machine->row_id[k])) {
                snprintf(why, why_size, "the id of row %u (node 0x%X) is unreadable", k, (unsigned)node);
                return false;
            }
            if (items->flags_off != 0xFFFFFFFFu && !read32(machine, node + items->flags_off, &machine->row_flags[k])) {
                snprintf(why, why_size, "the flags of row %u (node 0x%X) are unreadable", k, (unsigned)node);
                return false;
            }
            if (with_text && items->text_off != 0xFFFFFFFFu) {
                uint32_t pointer = 0u;
                if (!read32(machine, node + items->text_off, &pointer) || (pointer != 0u && !read_cstring(machine, pointer, machine->row_text[k]))) {
                    snprintf(why, why_size, "the text of row %u (node 0x%X) is unreadable", k, (unsigned)node);
                    return false;
                }
            }
            if ((machine->row_flags[k] & items->skip_mask) == 0u) machine->stops[machine->stop_count++] = k;
        }
        if (!read32(machine, node + items->next_off, &next)) {
            snprintf(why, why_size, "the next link of node 0x%X is unreadable", (unsigned)node);
            return false;
        }
        node = next;
    }
    return true;
}

static bool read_name(const route_nav *machine, uint32_t base, uint32_t index, char *out)
{
    const route_nav_names *names = &machine->menu->names;
    uint32_t address = base + index * names->stride;
    const bool wide = names->kind == ROUTE_NAV_NAMES_UTF16 || names->kind == ROUTE_NAV_NAMES_PTR_UTF16;
    out[0] = '\0';
    if (names->kind == ROUTE_NAV_NAMES_PTR_ASCII || names->kind == ROUTE_NAV_NAMES_PTR_UTF16) {
        uint32_t pointer = 0u;
        if (!read32(machine, address, &pointer)) return false;
        if (pointer == 0u) return true; /* an empty slot */
        address = pointer;
    }
    uint8_t raw[ROUTE_NAV_NAME_MAX * 2u];
    const size_t bytes = (size_t)names->max_len * (wide ? 2u : 1u);
    if (machine->probe.read_bytes == NULL || !machine->probe.read_bytes(address, raw, bytes, machine->probe.user)) return false;
    size_t used = 0u;
    for (unsigned i = 0u; i < names->max_len; i++) {
        char c;
        if (!name_char(raw[wide ? 2u * i : i], wide ? raw[2u * i + 1u] : 0u, &c)) break;
        out[used++] = c;
    }
    out[used] = '\0';
    return true;
}

static bool ascii_equal_blind(const char *left, const char *right)
{
    for (;; left++, right++) {
        char a = *left, b = *right;
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) return false;
        if (a == '\0') return true;
    }
}

/* The item names of a menu without a row list (names=), index 0..count-1, into row_text. `why` says what failed. */
static bool read_all_names(route_nav *machine, const page_ctx *page, uint32_t count, char *why, size_t why_size)
{
    const route_nav_names *spec = &machine->menu->names;
    if (spec->kind == ROUTE_NAV_NAMES_NONE) {
        snprintf(why, why_size, "menu %s has no names= or items= entry, so select=name: cannot be resolved", machine->menu->id);
        return false;
    }
    if (machine->probe.read_bytes == NULL) {
        snprintf(why, why_size, "no guest byte reader for names");
        return false;
    }
    if (count > ROUTE_NAV_MAX_ITEMS) {
        snprintf(why, why_size, "%u items are more than the %u a name lookup reads", count, ROUTE_NAV_MAX_ITEMS);
        return false;
    }
    uint32_t base = 0u;
    if (!ref_address(machine, page, &spec->base, &base)) {
        snprintf(why, why_size, "the names table address is unreadable");
        return false;
    }
    for (uint32_t i = 0u; i < count; i++) {
        if (!read_name(machine, base, i, machine->row_text[i])) {
            snprintf(why, why_size, "the name of item %u is unreadable (0x%X)", i, (unsigned)(base + i * spec->stride));
            return false;
        }
    }
    machine->row_count = count;
    return true;
}

/* The cursor stops: with a row list the non skipped rows in order, else every index. */
static uint32_t stops_total(const route_nav *machine, uint32_t count) { return machine->menu->items.present ? machine->stop_count : count; }
static uint32_t stop_at(const route_nav *machine, uint32_t position) { return machine->menu->items.present ? machine->stops[position] : position; }
/* The position of cursor row `row` among the stops, or -1 when the cursor cannot rest on it. */
static int64_t stop_pos(const route_nav *machine, uint32_t row)
{
    if (!machine->menu->items.present) return (int64_t)row;
    for (unsigned i = 0u; i < machine->stop_count; i++)
        if (machine->stops[i] == row) return (int64_t)i;
    return -1;
}

/* ---- messages ---- */

static void describe_seen(route_nav *machine, char *out, size_t out_size)
{
    out[0] = '\0';
    snapshot snap;
    take_snapshot(machine, &snap);
    if (!machine->menu->has_cursor) {
        append(out, out_size, "seen active=%s ready=%s", snap.active ? "yes" : "no", snap.ready ? "yes" : "no");
        if (machine->menu->page_builder) append(out, out_size, " page=%s", snap.page.found ? "on screen" : "absent");
        return;
    }
    if (machine->target_known) append(out, out_size, "expected cursor=%u of count=%u", machine->target, machine->count);
    else append(out, out_size, "target not resolved yet");
    append(out, out_size, ", seen cursor=");
    if (snap.cursor_ok) append(out, out_size, "%u", snap.cursor);
    else append(out, out_size, "unreadable");
    append(out, out_size, " count=");
    if (snap.count_ok) append(out, out_size, "%u", snap.count);
    else append(out, out_size, "unreadable");
    append(out, out_size, " active=%s ready=%s", snap.active ? "yes" : "no", snap.ready ? "yes" : "no");
    if (machine->menu->page_builder) append(out, out_size, " page=%s", snap.page.found ? "on screen" : "absent");
    if ((machine->step->kind == ROUTE_NAV_SELECT_INDEX && !machine->list_names) || !snap.active || !snap.count_ok || snap.count > ROUTE_NAV_MAX_ITEMS) return;
    char why[120];
    const bool have_rows = machine->menu->items.present ? load_rows(machine, &snap.page, true, why, sizeof why)
                                                        : read_all_names(machine, &snap.page, snap.count, why, sizeof why);
    if (!have_rows) return;
    append(out, out_size, " names=[");
    for (uint32_t i = 0u; i < machine->row_count; i++) {
        append(out, out_size, "%s%u:'%s'", i != 0u ? ", " : "", i, machine->row_text[i]);
        if (machine->menu->items.present && machine->menu->items.id_off != 0xFFFFFFFFu) append(out, out_size, "#0x%X", (unsigned)machine->row_id[i]);
        if (machine->menu->items.present && (machine->row_flags[i] & machine->menu->items.skip_mask) != 0u) append(out, out_size, "(skipped)");
        else if (machine->menu->items.present && (machine->row_flags[i] & machine->menu->items.grey_mask) != 0u) append(out, out_size, "(greyed)");
    }
    append(out, out_size, "]");
}

static route_nav_status finish(route_nav *machine, route_nav_status status)
{
    machine->status = status;
    machine->state = ROUTE_NAV_END;
    return status;
}

static route_nav_status fail_text(route_nav *machine, const char *reason)
{
    char label[ROUTE_NAV_SELECT_TEXT + ROUTE_NAV_ID_MAX + 16u], seen[ROUTE_NAV_MESSAGE_MAX / 2u];
    route_nav_step_label(machine->step, label, sizeof label);
    if (machine->menu == NULL) {
        snprintf(machine->message, sizeof machine->message, "route FAILED: nav step %u (%s) %s", machine->number, label, reason);
        return finish(machine, ROUTE_NAV_FAILED);
    }
    describe_seen(machine, seen, sizeof seen);
    snprintf(machine->message, sizeof machine->message, "route FAILED: nav step %u (%s) %s; %s", machine->number, label, reason, seen);
    return finish(machine, ROUTE_NAV_FAILED);
}

static route_nav_status fail(route_nav *machine, const char *format, ...) __attribute__((format(printf, 2, 3)));
static route_nav_status fail(route_nav *machine, const char *format, ...)
{
    char reason[420];
    va_list args;
    va_start(args, format);
    vsnprintf(reason, sizeof reason, format, args);
    va_end(args);
    return fail_text(machine, reason);
}

/* The step cannot run. Falls back to the recorded presses when mode `on` and nothing was pressed yet, else FAILS. */
static route_nav_status give_up(route_nav *machine, const char *format, ...) __attribute__((format(printf, 2, 3)));
static route_nav_status give_up(route_nav *machine, const char *format, ...)
{
    char reason[360];
    va_list args;
    va_start(args, format);
    vsnprintf(reason, sizeof reason, format, args);
    va_end(args);
    if (machine->mode != ROUTE_NAV_MODE_STRICT && machine->presses == 0u && !machine->act_started) {
        snprintf(machine->message, sizeof machine->message, "route nav: step %u falling back to the recorded presses (%s)", machine->number,
                 reason);
        return finish(machine, ROUTE_NAV_FALLBACK);
    }
    char full[420];
    if (machine->mode == ROUTE_NAV_MODE_STRICT) snprintf(full, sizeof full, "cannot run (strict): %s", reason);
    else snprintf(full, sizeof full, "cannot continue after %u press(es), no fallback any more: %s", machine->presses, reason);
    return fail_text(machine, full);
}

static void phase_text(const route_nav *machine, char *out, size_t out_size)
{
    out[0] = '\0';
    switch (machine->state) {
    case ROUTE_NAV_PLAN:
        if (machine->unmet[0] != '\0') append(out, out_size, "waiting for the menu, unmet: %s", machine->unmet);
        else append(out, out_size, "planning (cursor %u, target %u)", machine->cursor, machine->target);
        break;
    case ROUTE_NAV_PRESS:
    case ROUTE_NAV_RELEASE:
        append(out, out_size, "moving the cursor %u -> %u, %u press(es), %u lost", machine->cursor, machine->target, machine->presses,
               machine->lost);
        break;
    case ROUTE_NAV_ACT_PRESS:
    case ROUTE_NAV_ACT_RELEASE:
        append(out, out_size, "pressing %s on the target", machine->menu != NULL ? machine->menu->select_name : "A");
        break;
    case ROUTE_NAV_VERIFY:
        append(out, out_size, "waiting for the post-condition%s%s", machine->unmet[0] != '\0' ? ": " : "", machine->unmet);
        break;
    case ROUTE_NAV_END: break;
    }
}

static void maybe_progress(route_nav *machine, uint64_t now)
{
    if (now - machine->last_progress_ms < ROUTE_NAV_PROGRESS_MS) return;
    machine->last_progress_ms = now;
    if (machine->probe.progress == NULL) return;
    char label[ROUTE_NAV_SELECT_TEXT + ROUTE_NAV_ID_MAX + 16u], phase[560], line[760];
    route_nav_step_label(machine->step, label, sizeof label);
    phase_text(machine, phase, sizeof phase);
    snprintf(line, sizeof line, "route nav: step %u (%s) still working after %.1f s: %s", machine->number, label,
             (double)(now - machine->start_ms) / 1000.0, phase);
    machine->probe.progress(line, machine->probe.user);
}

/* ---- the machine ---- */

void route_nav_begin(route_nav *machine, const route_nav_step *step, unsigned number, const route_nav_menu *menu, route_nav_mode mode,
                     const route_nav_timing *timing, const route_nav_probe *probe)
{
    memset(machine, 0, sizeof *machine);
    machine->step = step;
    machine->menu = menu;
    machine->number = number;
    machine->mode = mode;
    machine->timing = timing != NULL ? *timing : route_nav_timing_default();
    machine->probe = *probe;
    machine->state = ROUTE_NAV_PLAN;
    machine->start_ms = machine->last_progress_ms = probe->now_ms(probe->user);
}

const char *route_nav_message(const route_nav *machine) { return machine->message; }

/* Which way along the stops: increasing (DOWN / RIGHT) or decreasing. A wrapping menu takes the shorter way, a tie goes increasing. */
static bool prefer_increasing(bool wrap, uint32_t from, uint32_t to, uint32_t total)
{
    if (!wrap) return to > from;
    const uint32_t forward = (to + total - from) % total;
    const uint32_t backward = total - forward;
    return forward <= backward;
}

/* The cursor row after one press from stop position `position` (-1: none yet, the first press lands on the first or last stop). */
static uint32_t expected_after_press(const route_nav *machine, int64_t position, bool increasing, uint32_t total)
{
    if (position < 0) return stop_at(machine, increasing ? 0u : total - 1u);
    const uint32_t at = (uint32_t)position;
    if (increasing) return stop_at(machine, at + 1u < total ? at + 1u : (machine->menu->wrap ? 0u : at));
    return stop_at(machine, at > 0u ? at - 1u : (machine->menu->wrap ? total - 1u : at));
}

/* Resolve select= to a cursor row (index:, id: or name:). */
static route_nav_status resolve_target(route_nav *machine, const page_ctx *page, uint32_t count)
{
    const route_nav_step *step = machine->step;
    const route_nav_items *items = &machine->menu->items;
    char why[200];
    if (step->kind == ROUTE_NAV_SELECT_INDEX) {
        machine->target = step->index;
        machine->target_known = true;
        return ROUTE_NAV_RUNNING;
    }
    unsigned matches = 0u;
    uint32_t first = 0u;
    char indexes[96];
    indexes[0] = '\0';
    if (step->kind == ROUTE_NAV_SELECT_ID) {
        if (!items->present || items->id_off == 0xFFFFFFFFu)
            return give_up(machine, "menu %s has no items= with id=, so select=id: cannot be resolved", machine->menu->id);
        for (uint32_t i = 0u; i < machine->row_count; i++) {
            if (machine->row_id[i] != step->index) continue;
            if (matches++ == 0u) first = i;
            append(indexes, sizeof indexes, "%s%u", matches > 1u ? ", " : "", i);
        }
        if (matches == 0u) return fail(machine, "no row has the id 0x%X among the menu's %u row(s)", (unsigned)step->index, machine->row_count);
        if (matches > 1u) return fail(machine, "the id 0x%X is ambiguous, it matches rows %s", (unsigned)step->index, indexes);
        machine->target = first;
        machine->target_known = true;
        return ROUTE_NAV_RUNNING;
    }
    if (items->present) {
        if (items->text_off == 0xFFFFFFFFu)
            return give_up(machine, "menu %s has no text= in its items= walk, so select=name: cannot be resolved", machine->menu->id);
    } else if (!read_all_names(machine, page, count, why, sizeof why)) {
        return give_up(machine, "%s", why);
    }
    for (uint32_t i = 0u; i < machine->row_count; i++) {
        if (items->present && (machine->row_flags[i] & items->skip_mask) != 0u) continue; /* a row the cursor skips (a title) is not an item */
        if (!ascii_equal_blind(machine->row_text[i], step->name)) continue;
        if (matches++ == 0u) first = i;
        append(indexes, sizeof indexes, "%s%u", matches > 1u ? ", " : "", i);
    }
    if (matches == 0u) return fail(machine, "name '%s' is not among the menu's %u item(s)", step->name, machine->row_count);
    if (matches > 1u) return fail(machine, "name '%s' is ambiguous, it matches items %s", step->name, indexes);
    machine->target = first;
    machine->target_known = true;
    return ROUTE_NAV_RUNNING;
}

static void fill_button(const route_nav *machine, xinput_pad_state *pad)
{
    if (machine->menu->select_analog >= 0) pad->analog[machine->menu->select_analog] = 255u;
    else pad->digital_buttons = machine->menu->select_digital;
}

static void begin_activation(route_nav *machine, uint32_t count, xinput_pad_state *pad)
{
    machine->act_started = true;
    machine->act_start_ms = machine->probe.now_ms(machine->probe.user);
    machine->initial_count = count;
    machine->act_polls = 1u;
    machine->phase_polls = 1u;
    machine->state = ROUTE_NAV_ACT_PRESS;
    if (machine->step->has_expect && machine->probe.expect_arm != NULL) machine->probe.expect_arm(machine->number, machine->probe.user);
    fill_button(machine, pad);
}

static route_nav_status plan(route_nav *machine, xinput_pad_state *pad)
{
    if (machine->menu == NULL) return give_up(machine, "unknown menu '%s', the menu table has no such entry", machine->step->menu);
    const route_nav_menu *menu = machine->menu;
    snapshot snap;
    take_snapshot(machine, &snap);
    if (!snap.active || !snap.ready) {
        snprintf(machine->unmet, sizeof machine->unmet, "%s", snap.unmet);
        return ROUTE_NAV_RUNNING;
    }
    machine->unmet[0] = '\0';
    if (!machine->ready_seen) {
        machine->ready_seen = true;
        machine->ready_polls = machine->polls;
    }
    if (!menu->has_cursor) { /* a screen that is only waited for and activated */
        if (!machine->step->activate) return finish(machine, ROUTE_NAV_DONE);
        begin_activation(machine, 0u, pad);
        return ROUTE_NAV_RUNNING;
    }
    if (!snap.cursor_ok || !snap.count_ok)
        return give_up(machine, "the menu is active but its %s is unreadable (null pointer or bad address)", snap.cursor_ok ? "count" : "cursor");
    if (snap.count == 0u || snap.count > ROUTE_NAV_COUNT_LIMIT) return give_up(machine, "the item count %u is not plausible", snap.count);
    const bool none = snap.cursor == ROUTE_NAV_CURSOR_NONE;
    if (!none && snap.cursor >= snap.count) return give_up(machine, "the cursor %u is not below the item count %u", snap.cursor, snap.count);
    char why[200];
    if (menu->items.present) {
        if (!load_rows(machine, &snap.page, machine->step->kind == ROUTE_NAV_SELECT_NAME && !machine->target_known, why, sizeof why))
            return give_up(machine, "%s", why);
        if (machine->row_count != snap.count) return give_up(machine, "the count is %u but the row list has %u rows", snap.count, machine->row_count);
    }
    machine->cursor = snap.cursor;
    machine->count = snap.count;
    if (!machine->target_known) {
        const route_nav_status resolved = resolve_target(machine, &snap.page, snap.count);
        if (resolved != ROUTE_NAV_RUNNING) return resolved;
    }
    if (machine->target >= snap.count)
        return fail(machine, "the index %u is beyond the menu's %u item(s) (valid 0..%u)", machine->target, snap.count, snap.count - 1u);
    const uint32_t total = stops_total(machine, snap.count);
    if (total == 0u) return give_up(machine, "the menu has no row the cursor can rest on");
    const int64_t target_position = stop_pos(machine, machine->target);
    if (target_position < 0) {
        (void)load_rows(machine, &snap.page, true, why, sizeof why); /* the texts, for the message */
        return fail(machine, "row %u '%s' is skipped by the cursor (flags 0x%X), it cannot be selected", machine->target,
                    machine->row_text[machine->target], (unsigned)machine->row_flags[machine->target]);
    }
    int64_t cursor_position = -1;
    if (!none) {
        cursor_position = stop_pos(machine, snap.cursor);
        if (cursor_position < 0) return give_up(machine, "the cursor %u is on a row the cursor skips", snap.cursor);
    }
    if (!none && snap.cursor == machine->target) {
        machine->dpad_since_ms = 0u;
        if (!machine->step->activate) return finish(machine, ROUTE_NAV_DONE);
        const route_nav_items *items = &menu->items;
        if (items->present && items->flags_off != 0xFFFFFFFFu && (machine->row_flags[machine->target] & items->grey_mask) != 0u) {
            const uint64_t now = machine->probe.now_ms(machine->probe.user);
            (void)load_rows(machine, &snap.page, true, why, sizeof why); /* the texts, for the messages */
            if (machine->grey_since_ms == 0u) machine->grey_since_ms = now + 1u;
            if (now + 1u - machine->grey_since_ms >= ROUTE_NAV_GREY_GRACE_MS)
                return fail(machine, "the target row %u '%s' is greyed (flags 0x%X) and ignores the select press", machine->target,
                            machine->row_text[machine->target], (unsigned)machine->row_flags[machine->target]);
            snprintf(machine->unmet, sizeof machine->unmet, "the target row %u '%s' is greyed (flags 0x%X)", machine->target,
                     machine->row_text[machine->target], (unsigned)machine->row_flags[machine->target]);
            return ROUTE_NAV_RUNNING;
        }
        machine->grey_since_ms = 0u;
        if (menu->has_itemid && items->present && items->id_off != 0xFFFFFFFFu && snap.itemid_ok && snap.itemid != machine->row_id[machine->target])
            return fail(machine, "the cursor is on the item id 0x%X but the target row %u has the id 0x%X", (unsigned)snap.itemid, machine->target,
                        (unsigned)machine->row_id[machine->target]);
        begin_activation(machine, snap.count, pad);
        return ROUTE_NAV_RUNNING;
    }
    if (menu->dpad_off) {
        /* the menu does not take the d-pad: never press it, wait for the cursor to get on the target by itself (the recorded movement before the step) */
        const uint64_t now = machine->probe.now_ms(machine->probe.user);
        char at_text[16];
        if (none) snprintf(at_text, sizeof at_text, "none");
        else snprintf(at_text, sizeof at_text, "%u", snap.cursor);
        if (machine->dpad_since_ms == 0u) machine->dpad_since_ms = now + 1u;
        if (now + 1u - machine->dpad_since_ms >= ROUTE_NAV_DPAD_GRACE_MS) {
            machine->list_names = true;
            return fail(machine, "menu %s does not take the d-pad (pointer driven), cursor is at %s, wanted %u: the recorded stick movement before the step did not reach the item",
                        menu->id, at_text, machine->target);
        }
        snprintf(machine->unmet, sizeof machine->unmet, "menu %s does not take the d-pad: the cursor is at %s, waiting for it to reach %u", menu->id, at_text,
                 machine->target);
        return ROUTE_NAV_RUNNING;
    }
    const bool increasing = none ? true : prefer_increasing(menu->wrap, (uint32_t)cursor_position, (uint32_t)target_position, total);
    machine->press_bit = menu->vertical ? (increasing ? XINPUT_BUTTON_DPAD_DOWN : XINPUT_BUTTON_DPAD_UP)
                                        : (increasing ? XINPUT_BUTTON_DPAD_RIGHT : XINPUT_BUTTON_DPAD_LEFT);
    machine->press_from = snap.cursor;
    machine->expected_to = expected_after_press(machine, cursor_position, increasing, total);
    machine->moved = false;
    machine->presses++;
    machine->phase_polls = 1u;
    machine->state = ROUTE_NAV_PRESS;
    pad->digital_buttons = machine->press_bit;
    return ROUTE_NAV_RUNNING;
}

static void observe_cursor(route_nav *machine)
{
    page_ctx page;
    memset(&page, 0, sizeof page);
    if (machine->menu->page_builder) page.found = locate_page(machine, &page.base);
    uint32_t cursor = 0u;
    if (!read_ref(machine, &page, &machine->menu->cursor, &cursor)) return;
    if (cursor != machine->press_from) {
        machine->moved = true;
        machine->moved_to = cursor;
    }
}

/* The end of the release window: did the press move the cursor? */
static route_nav_status settle_press(route_nav *machine)
{
    const route_nav_timing *timing = &machine->timing;
    if (machine->phase_polls < timing->gap) return ROUTE_NAV_RUNNING;
    if (machine->moved) {
        machine->consecutive_lost = 0u;
        machine->state = ROUTE_NAV_PLAN;
        if (machine->moved_to != machine->expected_to) {
            machine->anomalies++;
            if (machine->anomalies > machine->step->retry)
                return fail(machine, "the menu does not follow the pad: %u presses moved the cursor to a different item than expected (last %u -> %u, expected %u)",
                            machine->anomalies, machine->press_from, machine->moved_to, machine->expected_to);
        }
        return ROUTE_NAV_RUNNING;
    }
    if (machine->phase_polls < timing->gap + timing->extra) return ROUTE_NAV_RUNNING;
    machine->lost++;
    if (++machine->consecutive_lost > machine->step->retry)
        return fail(machine, "the press was lost %u times in a row (retry=%u): the cursor stayed at %u", machine->consecutive_lost,
                    machine->step->retry, machine->press_from);
    machine->state = ROUTE_NAV_PLAN;
    return ROUTE_NAV_RUNNING;
}

static route_nav_status verify(route_nav *machine, uint64_t now, xinput_pad_state *pad)
{
    const route_nav_step *step = machine->step;
    const uint64_t spent = now - machine->act_start_ms;
    if (step->has_expect) {
        char note[400];
        note[0] = '\0';
        const bool holds = machine->probe.expect_holds != NULL &&
                           machine->probe.expect_holds(machine->number, machine->act_polls, machine->act_start_ms, note, sizeof note, machine->probe.user);
        snprintf(machine->unmet, sizeof machine->unmet, "%s", note);
        if (holds) return finish(machine, ROUTE_NAV_DONE);
        const xinput_route_wait *wait = &step->expect_wait;
        if ((wait->max_polls != 0u && machine->act_polls >= wait->max_polls) || (wait->timeout_ms != 0u && spent >= wait->timeout_ms))
            return fail(machine, "the expect=%s post-condition did not hold %llu ms / %llu polls after the activation: %s", step->expect,
                        (unsigned long long)spent, (unsigned long long)machine->act_polls, note);
        return ROUTE_NAV_RUNNING;
    }
    /* No expect=: the menu must have reacted. It did when it left the screen (the page is gone or closing, the mode changed), when
     * something took the input (a child page: no longer ready) or when the cursor or the item count changed. */
    snapshot snap;
    take_snapshot(machine, &snap);
    const bool changed = !snap.active || !snap.ready ||
                         (machine->menu->has_cursor && (!snap.cursor_ok || !snap.count_ok || snap.cursor != machine->target || snap.count != machine->initial_count));
    if (changed) return finish(machine, ROUTE_NAV_DONE);
    if (spent < ROUTE_NAV_ACTIVATE_GRACE_MS) return ROUTE_NAV_RUNNING;
    if (machine->represses < ROUTE_NAV_ACTIVATE_REPRESS) {
        /* The menu is still there, ready, on the target: the press may have been lost. Press the select button once more. */
        machine->represses++;
        machine->act_start_ms = now;
        machine->act_polls = 1u;
        machine->phase_polls = 1u;
        machine->state = ROUTE_NAV_ACT_PRESS;
        fill_button(machine, pad);
        if (machine->probe.progress != NULL) {
            char label[ROUTE_NAV_SELECT_TEXT + ROUTE_NAV_ID_MAX + 16u], line[400];
            route_nav_step_label(step, label, sizeof label);
            snprintf(line, sizeof line, "route nav: step %u (%s) the menu did not react to %s within %u ms, pressing it once more", machine->number, label,
                     machine->menu->select_name, ROUTE_NAV_ACTIVATE_GRACE_MS);
            machine->probe.progress(line, machine->probe.user);
        }
        return ROUTE_NAV_RUNNING;
    }
    return fail(machine, "the activation had no visible effect: the menu is still active and ready on the target %llu ms after the last press "
                         "(%u press(es) of %s; give expect= for an activation that leaves the menu unchanged)",
                (unsigned long long)spent, machine->represses + 1u, machine->menu->select_name);
}

route_nav_status route_nav_poll(route_nav *machine, uint64_t poll_index, xinput_pad_state *pad)
{
    (void)poll_index;
    memset(pad, 0, sizeof *pad);
    if (machine->status != ROUTE_NAV_RUNNING) return machine->status;
    machine->polls++;
    const uint64_t now = machine->probe.now_ms(machine->probe.user);
    const uint64_t spent = now - machine->start_ms;
    if (spent >= machine->step->timeout_ms) {
        char phase[560];
        phase_text(machine, phase, sizeof phase);
        return fail(machine, "timed out after %llu ms (limit %llu ms) while %s", (unsigned long long)spent,
                    (unsigned long long)machine->step->timeout_ms, phase[0] != '\0' ? phase : "starting");
    }
    maybe_progress(machine, now);
    if (machine->act_started) machine->act_polls++; /* the polls since the activation press began (that poll is 1) */
    route_nav_status status = ROUTE_NAV_RUNNING;
    switch (machine->state) {
    case ROUTE_NAV_PLAN: status = plan(machine, pad); break;
    case ROUTE_NAV_PRESS:
        if (machine->phase_polls < machine->timing.hold) {
            machine->phase_polls++;
            pad->digital_buttons = machine->press_bit;
            observe_cursor(machine);
            break;
        }
        machine->state = ROUTE_NAV_RELEASE;
        machine->phase_polls = 0u;
        /* fall through */
    case ROUTE_NAV_RELEASE:
        machine->phase_polls++;
        observe_cursor(machine);
        status = settle_press(machine);
        break;
    case ROUTE_NAV_ACT_PRESS:
        if (machine->phase_polls < machine->timing.hold) {
            machine->phase_polls++;
            fill_button(machine, pad);
            break;
        }
        machine->state = ROUTE_NAV_ACT_RELEASE;
        machine->phase_polls = 0u;
        /* fall through */
    case ROUTE_NAV_ACT_RELEASE:
        machine->phase_polls++;
        if (machine->phase_polls >= machine->timing.gap) machine->state = ROUTE_NAV_VERIFY;
        break;
    case ROUTE_NAV_VERIFY:
        status = verify(machine, now, pad);
        break;
    case ROUTE_NAV_END: status = machine->status; break;
    }
    if (status != ROUTE_NAV_RUNNING) memset(pad, 0, sizeof *pad);
    return status;
}

/* ---- the recorder side: a menu seen without a step ---- */

void route_nav_view_read(route_nav *scratch, const route_nav_probe *probe, const route_nav_menu *menu, route_nav_view *view)
{
    scratch->probe = *probe;
    scratch->menu = menu;
    snapshot snap;
    take_snapshot(scratch, &snap);
    view->page_found = snap.page.found;
    view->page_base = snap.page.base;
    view->active = snap.active;
    view->ready = snap.ready;
    view->cursor_ok = snap.cursor_ok;
    view->count_ok = snap.count_ok;
    view->itemid_ok = snap.itemid_ok;
    view->cursor = snap.cursor;
    view->count = snap.count;
    view->itemid = snap.itemid;
}

bool route_nav_view_row(route_nav *scratch, const route_nav_probe *probe, const route_nav_menu *menu, const route_nav_view *view, bool with_text,
                        route_nav_row_info *info)
{
    memset(info, 0, sizeof *info);
    if (!menu->has_cursor || !menu->items.present) return true;
    scratch->probe = *probe;
    scratch->menu = menu;
    page_ctx page = {view->page_found, view->page_base};
    char why[120];
    if (!load_rows(scratch, &page, with_text, why, sizeof why) || !view->cursor_ok || view->cursor >= scratch->row_count) return false;
    const uint32_t row = view->cursor;
    info->known = true;
    info->id = scratch->row_id[row];
    info->flags = scratch->row_flags[row];
    info->skipped = (info->flags & menu->items.skip_mask) != 0u;
    info->greyed = (info->flags & menu->items.grey_mask) != 0u;
    info->unique_id = menu->items.id_off != 0xFFFFFFFFu && info->id != 0xFFFFFFFFu;
    for (uint32_t i = 0u; info->unique_id && i < scratch->row_count; i++)
        if (i != row && scratch->row_id[i] == info->id) info->unique_id = false;
    if (with_text && menu->items.text_off != 0xFFFFFFFFu) {
        snprintf(info->text, sizeof info->text, "%s", scratch->row_text[row]);
        info->unique_text = info->text[0] != '\0';
        for (uint32_t i = 0u; info->unique_text && i < scratch->row_count; i++) {
            if (i == row || (scratch->row_flags[i] & menu->items.skip_mask) != 0u) continue;
            if (ascii_equal_blind(scratch->row_text[i], info->text)) info->unique_text = false;
        }
    }
    return true;
}
