/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xinput_route_nav_spec.h"
#include "xinput_source.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool parse_u64(const char *text, size_t length, uint64_t limit, uint64_t *out)
{
    char copy[32];
    if (length == 0u || length >= sizeof copy || text[0] == '-' || text[0] == '+') return false;
    memcpy(copy, text, length);
    copy[length] = '\0';
    char *end = NULL;
    const unsigned long long value = strtoull(copy, &end, 0);
    if (end == NULL || *end != '\0' || value > limit) return false;
    *out = value;
    return true;
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* ---- timing and mode ---- */

route_nav_timing route_nav_timing_default(void)
{
    const route_nav_timing timing = {ROUTE_NAV_HOLD_POLLS, ROUTE_NAV_GAP_POLLS, ROUTE_NAV_LOST_EXTRA_POLLS};
    return timing;
}

bool route_nav_timing_parse(const char *spec, route_nav_timing *out, char *error, size_t error_size)
{
    const char *colon = spec != NULL ? strchr(spec, ':') : NULL;
    const char *second = colon != NULL ? strchr(colon + 1, ':') : NULL;
    uint64_t hold, gap, extra = ROUTE_NAV_LOST_EXTRA_POLLS;
    const size_t gap_length = colon != NULL ? (second != NULL ? (size_t)(second - colon - 1) : strlen(colon + 1)) : 0u;
    if (colon == NULL || !parse_u64(spec, (size_t)(colon - spec), ROUTE_NAV_TIMING_LIMIT, &hold) || hold == 0u ||
        !parse_u64(colon + 1, gap_length, ROUTE_NAV_TIMING_LIMIT, &gap) || gap == 0u ||
        (second != NULL && !parse_u64(second + 1, strlen(second + 1), ROUTE_NAV_TIMING_LIMIT, &extra))) {
        snprintf(error, error_size, "route nav timing '%s': want HOLD:GAP[:EXTRA], polls 1..%u (EXTRA 0..%u)", spec != NULL ? spec : "",
                 ROUTE_NAV_TIMING_LIMIT, ROUTE_NAV_TIMING_LIMIT);
        return false;
    }
    out->hold = (unsigned)hold;
    out->gap = (unsigned)gap;
    out->extra = (unsigned)extra;
    return true;
}

bool route_nav_mode_parse(const char *text, route_nav_mode *out)
{
    if (text == NULL) return false;
    if (strcmp(text, "on") == 0) *out = ROUTE_NAV_MODE_ON;
    else if (strcmp(text, "off") == 0) *out = ROUTE_NAV_MODE_OFF;
    else if (strcmp(text, "strict") == 0) *out = ROUTE_NAV_MODE_STRICT;
    else return false;
    return true;
}

/* ---- nav record line ---- */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool route_nav_percent_decode(const char *text, size_t length, char *out, size_t out_size, char *error, size_t error_size)
{
    size_t used = 0u;
    for (size_t i = 0u; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '%') {
            const int hi = i + 1u < length ? hex_value(text[i + 1u]) : -1;
            const int lo = i + 2u < length ? hex_value(text[i + 2u]) : -1;
            if (hi < 0 || lo < 0) {
                snprintf(error, error_size, "bad percent escape in the name near '%.8s' (want %%XX with XX hex)", text + i);
                return false;
            }
            c = (unsigned char)(hi * 16 + lo);
            i += 2u;
        }
        if (c < 0x20u || c > 0x7Eu) {
            snprintf(error, error_size, "the name has a byte 0x%02X outside printable ASCII", c);
            return false;
        }
        if (used + 1u >= out_size) {
            snprintf(error, error_size, "the name is longer than %zu bytes", out_size - 1u);
            return false;
        }
        out[used++] = (char)c;
    }
    out[used] = '\0';
    if (used == 0u) {
        snprintf(error, error_size, "the name is empty");
        return false;
    }
    return true;
}

typedef struct {
    bool at, to, menu, select, activate, expect, timeout, retry;
} seen_keys;

static bool duplicate(bool *flag, const char *key, char *error, size_t error_size)
{
    if (*flag) {
        snprintf(error, error_size, "'%s' given twice", key);
        return true;
    }
    *flag = true;
    return false;
}

bool route_nav_line_parse(const char *text, route_nav_step *out, char *error, size_t error_size)
{
    memset(out, 0, sizeof *out);
    out->timeout_ms = ROUTE_NAV_DEFAULT_TIMEOUT_MS;
    out->retry = ROUTE_NAV_DEFAULT_RETRY;
    if (text == NULL || strlen(text) > ROUTE_NAV_LINE_MAX) {
        snprintf(error, error_size, "the nav line is longer than %u characters", ROUTE_NAV_LINE_MAX);
        return false;
    }
    seen_keys seen;
    memset(&seen, 0, sizeof seen);
    const char *at = text;
    while (*at != '\0') {
        while (is_space(*at)) at++;
        if (*at == '\0') break;
        const char *end = at;
        while (*end != '\0' && !is_space(*end)) end++;
        const size_t length = (size_t)(end - at);
        const char *equals = memchr(at, '=', length);
        const size_t key_length = equals != NULL ? (size_t)(equals - at) : length;
        const char *value = equals != NULL ? equals + 1 : end;
        const size_t value_length = (size_t)(end - value);
        uint64_t number;
        if (key_length == 2u && memcmp(at, "at", 2u) == 0 && equals != NULL) {
            if (duplicate(&seen.at, "at", error, error_size)) return false;
            if (!parse_u64(value, value_length, UINT32_MAX * 16ull, &number)) {
                snprintf(error, error_size, "bad number in at=%.*s", (int)value_length, value);
                return false;
            }
            out->at = number;
        } else if (key_length == 2u && memcmp(at, "to", 2u) == 0 && equals != NULL) {
            if (duplicate(&seen.to, "to", error, error_size)) return false;
            if (!parse_u64(value, value_length, UINT32_MAX * 16ull, &number)) {
                snprintf(error, error_size, "bad number in to=%.*s", (int)value_length, value);
                return false;
            }
            out->to = number;
        } else if (key_length == 4u && memcmp(at, "menu", 4u) == 0 && equals != NULL) {
            if (duplicate(&seen.menu, "menu", error, error_size)) return false;
            bool clean = value_length != 0u && value_length < ROUTE_NAV_ID_MAX;
            for (size_t i = 0u; clean && i < value_length; i++) {
                const char c = value[i];
                clean = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            }
            if (!clean) {
                snprintf(error, error_size, "bad menu id '%.*s' (1..%u of [A-Za-z0-9_.-])", (int)value_length, value, ROUTE_NAV_ID_MAX - 1u);
                return false;
            }
            memcpy(out->menu, value, value_length);
            out->menu[value_length] = '\0';
        } else if (key_length == 6u && memcmp(at, "select", 6u) == 0 && equals != NULL) {
            if (duplicate(&seen.select, "select", error, error_size)) return false;
            if (value_length >= sizeof out->select_text) {
                snprintf(error, error_size, "select= is longer than %zu characters", sizeof out->select_text - 1u);
                return false;
            }
            memcpy(out->select_text, value, value_length);
            out->select_text[value_length] = '\0';
            if (value_length > 6u && memcmp(value, "index:", 6u) == 0) {
                if (!parse_u64(value + 6, value_length - 6u, 0xFFFFFFull, &number)) {
                    snprintf(error, error_size, "bad number in select=%.*s", (int)value_length, value);
                    return false;
                }
                out->kind = ROUTE_NAV_SELECT_INDEX;
                out->index = (uint32_t)number;
            } else if (value_length > 3u && memcmp(value, "id:", 3u) == 0) {
                if (!parse_u64(value + 3, value_length - 3u, 0xFFFFFFFFull, &number)) {
                    snprintf(error, error_size, "bad number in select=%.*s", (int)value_length, value);
                    return false;
                }
                out->kind = ROUTE_NAV_SELECT_ID;
                out->index = (uint32_t)number;
            } else if (value_length > 5u && memcmp(value, "name:", 5u) == 0) {
                char reason[120];
                if (!route_nav_percent_decode(value + 5, value_length - 5u, out->name, sizeof out->name, reason, sizeof reason)) {
                    snprintf(error, error_size, "select=%.*s: %s", (int)value_length, value, reason);
                    return false;
                }
                out->kind = ROUTE_NAV_SELECT_NAME;
            } else {
                snprintf(error, error_size, "select=%.*s: want index:N, id:N or name:TEXT", (int)value_length, value);
                return false;
            }
        } else if (length == 8u && memcmp(at, "activate", 8u) == 0) {
            if (duplicate(&seen.activate, "activate", error, error_size)) return false;
            out->activate = true;
        } else if (key_length == 6u && memcmp(at, "expect", 6u) == 0 && equals != NULL) {
            if (duplicate(&seen.expect, "expect", error, error_size)) return false;
            char spec[ROUTE_NAV_EXPECT_MAX + 8u], reason[200];
            if (value_length == 0u || value_length >= ROUTE_NAV_EXPECT_MAX) {
                snprintf(error, error_size, "expect= must be 1..%u characters", ROUTE_NAV_EXPECT_MAX - 1u);
                return false;
            }
            memcpy(out->expect, value, value_length);
            out->expect[value_length] = '\0';
            snprintf(spec, sizeof spec, "mark1:%s", out->expect);
            if (!xinput_route_event_wait_parse(spec, &out->expect_wait, reason, sizeof reason)) {
                snprintf(error, error_size, "expect=%s: %s", out->expect, reason);
                return false;
            }
            out->has_expect = true;
        } else if (key_length == 7u && memcmp(at, "timeout", 7u) == 0 && equals != NULL) {
            if (duplicate(&seen.timeout, "timeout", error, error_size)) return false;
            if (!parse_u64(value, value_length, XINPUT_ROUTE_TIMEOUT_MS_LIMIT, &number) || number == 0u) {
                snprintf(error, error_size, "bad number in timeout=%.*s (1..%u ms)", (int)value_length, value, XINPUT_ROUTE_TIMEOUT_MS_LIMIT);
                return false;
            }
            out->timeout_ms = number;
        } else if (key_length == 5u && memcmp(at, "retry", 5u) == 0 && equals != NULL) {
            if (duplicate(&seen.retry, "retry", error, error_size)) return false;
            if (!parse_u64(value, value_length, ROUTE_NAV_RETRY_LIMIT, &number)) {
                snprintf(error, error_size, "bad number in retry=%.*s (0..%u)", (int)value_length, value, ROUTE_NAV_RETRY_LIMIT);
                return false;
            }
            out->retry = (unsigned)number;
        } else {
            snprintf(error, error_size, "unknown field '%.*s'", (int)(length > 40u ? 40u : length), at);
            return false;
        }
        at = end;
    }
    if (!seen.at) { snprintf(error, error_size, "missing at="); return false; }
    if (!seen.to) { snprintf(error, error_size, "missing to="); return false; }
    if (!seen.menu) { snprintf(error, error_size, "missing menu="); return false; }
    if (!seen.select) { snprintf(error, error_size, "missing select="); return false; }
    if (out->at > out->to) {
        snprintf(error, error_size, "at=%llu is above to=%llu", (unsigned long long)out->at, (unsigned long long)out->to);
        return false;
    }
    if (out->has_expect && !out->activate) {
        snprintf(error, error_size, "expect= is the post-condition of the activation, give `activate` too");
        return false;
    }
    return true;
}

bool route_nav_steps_check(const route_nav_step *steps, size_t count, const uint64_t *marks, size_t mark_count, uint64_t total,
                           char *error, size_t error_size)
{
    if (count > ROUTE_NAV_MAX_STEPS) {
        snprintf(error, error_size, "%zu nav lines, a route holds at most %u", count, ROUTE_NAV_MAX_STEPS);
        return false;
    }
    for (size_t i = 0u; i < count; i++) {
        const route_nav_step *step = &steps[i];
        if (step->to > total) {
            snprintf(error, error_size, "nav step %zu: to=%llu is past the record (%llu polls)", i + 1u, (unsigned long long)step->to,
                     (unsigned long long)total);
            return false;
        }
        if (i != 0u && step->at < steps[i - 1u].to) {
            snprintf(error, error_size, "nav step %zu: at=%llu overlaps or precedes step %zu (to=%llu), steps must be ascending", i + 1u,
                     (unsigned long long)step->at, i, (unsigned long long)steps[i - 1u].to);
            return false;
        }
        for (size_t m = 0u; m < mark_count; m++) {
            if (marks[m] > step->at && marks[m] < step->to) {
                snprintf(error, error_size, "nav step %zu: mark %zu at %llu lies inside (at=%llu, to=%llu), a mark may sit at either end only",
                         i + 1u, m + 1u, (unsigned long long)marks[m], (unsigned long long)step->at, (unsigned long long)step->to);
                return false;
            }
        }
    }
    return true;
}

/* ---- menu table ---- */

/* `@+OFF[...]` (a page spec) or a plain mem spec; `cond` selects the comparison form. */
static bool parse_ref_item(const char *text, bool cond, route_nav_ref *out)
{
    memset(out, 0, sizeof *out);
    if (text[0] == '@') {
        if (text[1] != '+' || text[2] == '\0') return false;
        out->page = true;
        text += 2;
    }
    const bool good = cond ? xinput_route_mem_cond_parse(text, &out->cond) : xinput_route_mem_ref_parse(text, &out->cond);
    return good && !(out->page && out->cond.indirect);
}

static bool parse_cond_list(const char *text, size_t length, route_nav_ref *conds, unsigned *count, char *error, size_t error_size,
                            const char *key)
{
    *count = 0u;
    const char *at = text;
    const char *const end = text + length;
    while (at <= end) {
        const char *comma = memchr(at, ',', (size_t)(end - at));
        const size_t part = comma != NULL ? (size_t)(comma - at) : (size_t)(end - at);
        char copy[96];
        if (part == 0u || part >= sizeof copy || *count >= ROUTE_COND_MAX) {
            snprintf(error, error_size, "%s=: empty, too long or more than %u conditions", key, ROUTE_COND_MAX);
            return false;
        }
        memcpy(copy, at, part);
        copy[part] = '\0';
        if (!parse_ref_item(copy, true, &conds[*count])) {
            snprintf(error, error_size, "%s=: bad condition '%s' (want [*]ADDR[+OFF][:W]OP VALUE[&MASK] or @+OFF[:W]OP VALUE[&MASK])", key, copy);
            return false;
        }
        (*count)++;
        if (comma == NULL) break;
        at = comma + 1;
    }
    return true;
}

static bool parse_ref(const char *text, size_t length, route_nav_ref *out, char *error, size_t error_size, const char *key)
{
    char copy[96];
    if (length == 0u || length >= sizeof copy) {
        snprintf(error, error_size, "%s=: empty or too long", key);
        return false;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    if (!parse_ref_item(copy, false, out)) {
        snprintf(error, error_size, "%s=: bad memory reference '%s' (want [*]ADDR[+OFF][:W][&MASK] or @+OFF[:W][&MASK])", key, copy);
        return false;
    }
    return true;
}

static bool parse_names(const char *text, size_t length, route_nav_names *out, char *error, size_t error_size)
{
    char copy[160];
    if (length == 0u || length >= sizeof copy) {
        snprintf(error, error_size, "names=: empty or too long");
        return false;
    }
    memcpy(copy, text, length);
    copy[length] = '\0';
    char *parts[4];
    size_t found = 0u;
    for (char *scan = copy;; ) {
        if (found == 4u) { found = 5u; break; }
        parts[found++] = scan;
        char *slash = strchr(scan, '/');
        if (slash == NULL) break;
        *slash = '\0';
        scan = slash + 1;
    }
    uint64_t stride, max_len;
    if (found != 4u || !parse_u64(parts[1], strlen(parts[1]), 4096u, &stride) || stride == 0u ||
        !parse_u64(parts[3], strlen(parts[3]), ROUTE_NAV_NAME_MAX - 1u, &max_len) || max_len == 0u) {
        snprintf(error, error_size, "names=: want MEM/STRIDE/ascii|utf16|ptr-ascii|ptr-utf16/MAXLEN (STRIDE 1..4096, MAXLEN 1..%u)",
                 ROUTE_NAV_NAME_MAX - 1u);
        return false;
    }
    if (strcmp(parts[2], "ascii") == 0) out->kind = ROUTE_NAV_NAMES_ASCII;
    else if (strcmp(parts[2], "utf16") == 0) out->kind = ROUTE_NAV_NAMES_UTF16;
    else if (strcmp(parts[2], "ptr-ascii") == 0) out->kind = ROUTE_NAV_NAMES_PTR_ASCII;
    else if (strcmp(parts[2], "ptr-utf16") == 0) out->kind = ROUTE_NAV_NAMES_PTR_UTF16;
    else {
        snprintf(error, error_size, "names=: unknown encoding '%s' (ascii, utf16, ptr-ascii or ptr-utf16)", parts[2]);
        return false;
    }
    if (!parse_ref_item(parts[0], false, &out->base)) {
        snprintf(error, error_size, "names=: bad memory reference '%s'", parts[0]);
        return false;
    }
    out->stride = (unsigned)stride;
    out->max_len = (unsigned)max_len;
    return true;
}

/* "+0x14" -> offset. */
static bool plus_offset(const char *text, size_t length, uint32_t *out)
{
    uint64_t value;
    if (length < 2u || text[0] != '+' || !parse_u64(text + 1, length - 1u, 0xFFFFu, &value)) return false;
    *out = (uint32_t)value;
    return true;
}

/* items=walk(HEAD;next=+N;type=+N:T1|T2..;id=+N;text=+N;flags=+N;skip=MASK;grey=MASK) (the text inside the value, parentheses included). */
static bool parse_items(const char *text, size_t length, route_nav_items *out, char *error, size_t error_size)
{
    char copy[300];
    if (length < 7u || length >= sizeof copy || memcmp(text, "walk(", 5u) != 0 || text[length - 1u] != ')') {
        snprintf(error, error_size, "items=: want walk(HEAD;next=+N;type=+N:T1|T2;id=+N;text=+N;flags=+N;skip=MASK;grey=MASK)");
        return false;
    }
    memcpy(copy, text + 5, length - 6u);
    copy[length - 6u] = '\0';
    memset(out, 0, sizeof *out);
    out->id_off = out->text_off = out->flags_off = 0xFFFFFFFFu;
    bool have_next = false, have_type = false;
    char *scan = copy;
    bool first = true;
    while (scan != NULL) {
        char *semicolon = strchr(scan, ';');
        if (semicolon != NULL) *semicolon = '\0';
        const size_t part = strlen(scan);
        if (first) {
            if (!parse_ref_item(scan, false, &out->head)) {
                snprintf(error, error_size, "items=walk: bad head '%s'", scan);
                return false;
            }
            first = false;
        } else {
            const char *equals = strchr(scan, '=');
            uint64_t number;
            if (equals == NULL) {
                snprintf(error, error_size, "items=walk: '%s' is not key=value", scan);
                return false;
            }
            const size_t key_length = (size_t)(equals - scan);
            const char *value = equals + 1;
            const size_t value_length = part - key_length - 1u;
            if (key_length == 4u && memcmp(scan, "next", 4u) == 0) {
                have_next = plus_offset(value, value_length, &out->next_off);
                if (!have_next) { snprintf(error, error_size, "items=walk: bad next '%s'", value); return false; }
            } else if (key_length == 4u && memcmp(scan, "type", 4u) == 0) {
                const char *colon = memchr(value, ':', value_length);
                if (colon == NULL || !plus_offset(value, (size_t)(colon - value), &out->type_off)) {
                    snprintf(error, error_size, "items=walk: bad type '%s' (want +OFF:T1|T2)", value);
                    return false;
                }
                const char *type = colon + 1;
                const char *const type_end = value + value_length;
                while (type <= type_end) {
                    const char *bar = memchr(type, '|', (size_t)(type_end - type));
                    const size_t type_length = bar != NULL ? (size_t)(bar - type) : (size_t)(type_end - type);
                    if (out->type_count >= ROUTE_NAV_MAX_TYPES || !parse_u64(type, type_length, 0xFFFFFFFFull, &number)) {
                        snprintf(error, error_size, "items=walk: bad type list '%s' (1..%u numbers)", value, ROUTE_NAV_MAX_TYPES);
                        return false;
                    }
                    out->types[out->type_count++] = (uint32_t)number;
                    if (bar == NULL) break;
                    type = bar + 1;
                }
                have_type = true;
            } else if (key_length == 2u && memcmp(scan, "id", 2u) == 0) {
                if (!plus_offset(value, value_length, &out->id_off)) { snprintf(error, error_size, "items=walk: bad id '%s'", value); return false; }
            } else if (key_length == 4u && memcmp(scan, "text", 4u) == 0) {
                if (!plus_offset(value, value_length, &out->text_off)) { snprintf(error, error_size, "items=walk: bad text '%s'", value); return false; }
            } else if (key_length == 5u && memcmp(scan, "flags", 5u) == 0) {
                if (!plus_offset(value, value_length, &out->flags_off)) { snprintf(error, error_size, "items=walk: bad flags '%s'", value); return false; }
            } else if (key_length == 4u && memcmp(scan, "skip", 4u) == 0) {
                if (!parse_u64(value, value_length, 0xFFFFFFFFull, &number)) { snprintf(error, error_size, "items=walk: bad skip '%s'", value); return false; }
                out->skip_mask = (uint32_t)number;
            } else if (key_length == 4u && memcmp(scan, "grey", 4u) == 0) {
                if (!parse_u64(value, value_length, 0xFFFFFFFFull, &number)) { snprintf(error, error_size, "items=walk: bad grey '%s'", value); return false; }
                out->grey_mask = (uint32_t)number;
            } else {
                snprintf(error, error_size, "items=walk: unknown key '%.*s'", (int)key_length, scan);
                return false;
            }
        }
        scan = semicolon != NULL ? semicolon + 1 : NULL;
    }
    if (!have_next || !have_type || ((out->skip_mask != 0u || out->grey_mask != 0u) && out->flags_off == 0xFFFFFFFFu)) {
        snprintf(error, error_size, "items=walk: needs next=, type= and flags= when skip= or grey= is given");
        return false;
    }
    out->present = true;
    return true;
}

static bool parse_button(const char *text, size_t length, int *analog_out, uint16_t *digital_out, char *name, size_t name_size)
{
    static const char *const analog[] = {"A", "B", "X", "Y", "BLACK", "WHITE"};
    if (length == 0u || length >= name_size) return false;
    for (int i = 0; i < 6; i++) {
        if (strlen(analog[i]) == length && memcmp(analog[i], text, length) == 0) {
            *analog_out = i;
            *digital_out = 0u;
            memcpy(name, text, length);
            name[length] = '\0';
            return true;
        }
    }
    if (length == 5u && memcmp(text, "START", 5u) == 0) *digital_out = XINPUT_BUTTON_START;
    else if (length == 4u && memcmp(text, "BACK", 4u) == 0) *digital_out = XINPUT_BUTTON_BACK;
    else return false;
    *analog_out = -1;
    memcpy(name, text, length);
    name[length] = '\0';
    return true;
}

typedef struct {
    bool page, active, cursor, count, axis, wrap, ready, names, select, back, conf, itemid, items, dpad, selectby;
} menu_keys;

static bool parse_menu_line(const char *line, size_t length, unsigned number, route_nav_menu *menu, char *error, size_t error_size)
{
    memset(menu, 0, sizeof *menu);
    menu->select_analog = 0;
    memcpy(menu->select_name, "A", 2u);
    menu->line = number;
    menu_keys seen;
    memset(&seen, 0, sizeof seen);
    const char *at = line;
    const char *const end = line + length;
    while (at < end && is_space(*at)) at++;
    if (end - at < 5 || memcmp(at, "menu", 4u) != 0 || !is_space(at[4])) {
        snprintf(error, error_size, "line %u: expected `menu ID key=value ...`", number);
        return false;
    }
    at += 4;
    while (at < end && is_space(*at)) at++;
    const char *id_end = at;
    while (id_end < end && !is_space(*id_end)) id_end++;
    bool clean = id_end > at && (size_t)(id_end - at) < ROUTE_NAV_ID_MAX;
    for (const char *scan = at; clean && scan < id_end; scan++)
        clean = (*scan >= 'a' && *scan <= 'z') || (*scan >= 'A' && *scan <= 'Z') || (*scan >= '0' && *scan <= '9') || *scan == '_' ||
                *scan == '-' || *scan == '.';
    if (!clean) {
        snprintf(error, error_size, "line %u: bad menu id (1..%u of [A-Za-z0-9_.-])", number, ROUTE_NAV_ID_MAX - 1u);
        return false;
    }
    memcpy(menu->id, at, (size_t)(id_end - at));
    at = id_end;
    char reason[220];
    while (at < end) {
        while (at < end && is_space(*at)) at++;
        if (at >= end) break;
        const char *token_end = at;
        while (token_end < end && !is_space(*token_end)) token_end++;
        const char *equals = memchr(at, '=', (size_t)(token_end - at));
        if (equals == NULL) {
            snprintf(error, error_size, "line %u: '%.*s' is not key=value", number, (int)(token_end - at > 40 ? 40 : token_end - at), at);
            return false;
        }
        const size_t key_length = (size_t)(equals - at);
        const char *value = equals + 1;
        const size_t value_length = (size_t)(token_end - value);
        bool *flag = NULL;
        bool good = false;
        reason[0] = '\0';
#define KEY_IS(name) (key_length == sizeof(name) - 1u && memcmp(at, name, key_length) == 0)
        if (KEY_IS("page")) {
            flag = &seen.page;
            if (!*flag) {
                uint64_t builder;
                if (value_length == 4u && memcmp(value, "none", 4u) == 0) {
                    good = true;
                } else if (value_length > 8u && memcmp(value, "builder:", 8u) == 0 && parse_u64(value + 8, value_length - 8u, 0xFFFFFFFFull, &builder) && builder != 0u) {
                    menu->page_builder = true;
                    menu->builder = (uint32_t)builder;
                    good = true;
                } else {
                    snprintf(reason, sizeof reason, "page= wants none or builder:0xVA");
                }
            }
        } else if (KEY_IS("active")) {
            flag = &seen.active;
            if (!*flag) good = parse_cond_list(value, value_length, menu->active, &menu->active_count, reason, sizeof reason, "active");
        } else if (KEY_IS("ready")) {
            flag = &seen.ready;
            if (!*flag) good = parse_cond_list(value, value_length, menu->ready, &menu->ready_count, reason, sizeof reason, "ready");
        } else if (KEY_IS("cursor")) {
            flag = &seen.cursor;
            if (!*flag) good = parse_ref(value, value_length, &menu->cursor, reason, sizeof reason, "cursor");
        } else if (KEY_IS("count")) {
            flag = &seen.count;
            if (!*flag) good = parse_ref(value, value_length, &menu->count, reason, sizeof reason, "count");
        } else if (KEY_IS("itemid")) {
            flag = &seen.itemid;
            if (!*flag) {
                good = parse_ref(value, value_length, &menu->itemid, reason, sizeof reason, "itemid");
                menu->has_itemid = good;
            }
        } else if (KEY_IS("items")) {
            flag = &seen.items;
            if (!*flag) good = parse_items(value, value_length, &menu->items, reason, sizeof reason);
        } else if (KEY_IS("axis")) {
            flag = &seen.axis;
            if (!*flag) {
                good = value_length == 1u && (value[0] == 'v' || value[0] == 'h');
                menu->vertical = value[0] == 'v';
                if (!good) snprintf(reason, sizeof reason, "axis= wants v or h");
            }
        } else if (KEY_IS("wrap")) {
            flag = &seen.wrap;
            if (!*flag) {
                good = value_length == 1u && (value[0] == '0' || value[0] == '1');
                menu->wrap = value[0] == '1';
                if (!good) snprintf(reason, sizeof reason, "wrap= wants 0 or 1");
            }
        } else if (KEY_IS("names")) {
            flag = &seen.names;
            if (!*flag) good = parse_names(value, value_length, &menu->names, reason, sizeof reason);
        } else if (KEY_IS("select")) {
            flag = &seen.select;
            if (!*flag) {
                good = parse_button(value, value_length, &menu->select_analog, &menu->select_digital, menu->select_name, sizeof menu->select_name);
                if (!good) snprintf(reason, sizeof reason, "select= wants A B X Y BLACK WHITE START or BACK");
            }
        } else if (KEY_IS("back")) {
            flag = &seen.back;
            if (!*flag) {
                int analog;
                uint16_t digital;
                good = parse_button(value, value_length, &analog, &digital, menu->back_name, sizeof menu->back_name);
                if (!good) snprintf(reason, sizeof reason, "back= wants A B X Y BLACK WHITE START or BACK");
            }
        } else if (KEY_IS("dpad")) {
            flag = &seen.dpad;
            if (!*flag) {
                good = value_length == 1u && (value[0] == '0' || value[0] == '1');
                menu->dpad_off = value[0] == '0';
                if (!good) snprintf(reason, sizeof reason, "dpad= wants 0 or 1");
            }
        } else if (KEY_IS("selectby")) {
            flag = &seen.selectby;
            if (!*flag) {
                good = true;
                if (value_length == 2u && memcmp(value, "id", 2u) == 0) menu->selectby = ROUTE_NAV_BY_ID;
                else if (value_length == 4u && memcmp(value, "name", 4u) == 0) menu->selectby = ROUTE_NAV_BY_NAME;
                else if (value_length == 5u && memcmp(value, "index", 5u) == 0) menu->selectby = ROUTE_NAV_BY_INDEX;
                else {
                    good = false;
                    snprintf(reason, sizeof reason, "selectby= wants id, name or index");
                }
            }
        } else if (KEY_IS("conf")) {
            flag = &seen.conf;
            if (!*flag) {
                good = value_length != 0u && value_length < sizeof menu->conf;
                if (good) {
                    memcpy(menu->conf, value, value_length);
                    menu->conf[value_length] = '\0';
                } else {
                    snprintf(reason, sizeof reason, "conf= wants 1..%zu characters", sizeof menu->conf - 1u);
                }
            }
        } else {
            snprintf(error, error_size, "line %u: unknown key '%.*s'", number, (int)(key_length > 40u ? 40u : key_length), at);
            return false;
        }
#undef KEY_IS
        if (*flag && !good && reason[0] == '\0') {
            snprintf(error, error_size, "line %u: '%.*s' given twice", number, (int)key_length, at);
            return false;
        }
        if (!good) {
            snprintf(error, error_size, "line %u: %s", number, reason);
            return false;
        }
        *flag = true;
        at = token_end;
    }
    menu->has_cursor = seen.cursor;
    if (!seen.active) {
        snprintf(error, error_size, "line %u: menu %s is missing active=", number, menu->id);
        return false;
    }
    if (seen.cursor != seen.count) {
        snprintf(error, error_size, "line %u: menu %s needs cursor= and count= together", number, menu->id);
        return false;
    }
    if (seen.cursor && (!seen.axis || !seen.wrap)) {
        snprintf(error, error_size, "line %u: menu %s is missing %s=", number, menu->id, !seen.axis ? "axis" : "wrap");
        return false;
    }
    if (!seen.cursor && (seen.itemid || seen.items || seen.names || seen.dpad || seen.selectby)) {
        snprintf(error, error_size, "line %u: menu %s has itemid=, items=, names=, dpad= or selectby= without a cursor", number, menu->id);
        return false;
    }
    bool uses_page = false;
    for (unsigned i = 0u; i < menu->active_count; i++) uses_page = uses_page || menu->active[i].page;
    for (unsigned i = 0u; i < menu->ready_count; i++) uses_page = uses_page || menu->ready[i].page;
    uses_page = uses_page || (seen.cursor && (menu->cursor.page || menu->count.page)) || (seen.itemid && menu->itemid.page) ||
                (seen.items && menu->items.head.page) || (seen.names && menu->names.base.page);
    if (uses_page && !menu->page_builder) {
        snprintf(error, error_size, "line %u: menu %s uses a @+OFF page spec but has no page=builder:VA", number, menu->id);
        return false;
    }
    return true;
}

/* widgets table=ADDR stride=N count=N [closing=S,S] */
static bool parse_widgets_line(const char *line, size_t length, unsigned number, route_nav_widgets *out, char *error, size_t error_size)
{
    const char *at = line + 7; /* after "widgets" */
    const char *const end = line + length;
    bool have_table = false, have_stride = false, have_count = false, have_closing = false;
    memset(out, 0, sizeof *out);
    while (at < end) {
        while (at < end && is_space(*at)) at++;
        if (at >= end) break;
        const char *token_end = at;
        while (token_end < end && !is_space(*token_end)) token_end++;
        const char *equals = memchr(at, '=', (size_t)(token_end - at));
        const size_t key_length = equals != NULL ? (size_t)(equals - at) : 0u;
        const char *value = equals != NULL ? equals + 1 : at;
        const size_t value_length = equals != NULL ? (size_t)(token_end - value) : 0u;
        uint64_t number_value;
        if (equals != NULL && key_length == 5u && memcmp(at, "table", 5u) == 0 && !have_table &&
            parse_u64(value, value_length, 0xFFFFFFFFull, &number_value) && number_value != 0u) {
            out->table = (uint32_t)number_value;
            have_table = true;
        } else if (equals != NULL && key_length == 6u && memcmp(at, "stride", 6u) == 0 && !have_stride &&
                   parse_u64(value, value_length, 0x10000ull, &number_value) && number_value >= 0x1B4u) {
            out->stride = (uint32_t)number_value;
            have_stride = true;
        } else if (equals != NULL && key_length == 5u && memcmp(at, "count", 5u) == 0 && !have_count &&
                   parse_u64(value, value_length, 256ull, &number_value) && number_value != 0u) {
            out->count = (uint32_t)number_value;
            have_count = true;
        } else if (equals != NULL && key_length == 7u && memcmp(at, "closing", 7u) == 0 && !have_closing) {
            const char *state = value;
            const char *const state_end = value + value_length;
            while (state <= state_end) {
                const char *comma = memchr(state, ',', (size_t)(state_end - state));
                const size_t state_length = comma != NULL ? (size_t)(comma - state) : (size_t)(state_end - state);
                if (out->closing_count >= ROUTE_NAV_MAX_CLOSING || !parse_u64(state, state_length, 0xFFFFFFFFull, &number_value)) {
                    snprintf(error, error_size, "line %u: widgets closing= wants 1..%u state numbers", number, ROUTE_NAV_MAX_CLOSING);
                    return false;
                }
                out->closing[out->closing_count++] = (uint32_t)number_value;
                if (comma == NULL) break;
                state = comma + 1;
            }
            have_closing = true;
        } else {
            snprintf(error, error_size, "line %u: widgets: bad, repeated or unknown field '%.*s' (want table=ADDR stride=N count=N [closing=S,S])", number,
                     (int)(token_end - at > 40 ? 40 : token_end - at), at);
            return false;
        }
        at = token_end;
    }
    if (!have_table || !have_stride || !have_count) {
        snprintf(error, error_size, "line %u: widgets needs table=, stride= and count=", number);
        return false;
    }
    out->present = true;
    return true;
}

bool route_nav_menus_parse(const char *text, size_t length, route_nav_menu_table *out, char *error, size_t error_size)
{
    memset(out, 0, sizeof *out);
    size_t at = 0u;
    unsigned number = 0u;
    while (at < length) {
        size_t end = at;
        while (end < length && text[end] != '\n') end++;
        number++;
        size_t content = at;
        size_t stop = end;
        for (size_t i = at; i < end; i++) {
            if (text[i] == '#') { stop = i; break; }
        }
        while (content < stop && is_space(text[content])) content++;
        while (stop > content && is_space(text[stop - 1u])) stop--;
        if (stop > content) {
            if (stop - content >= 8u && memcmp(text + content, "widgets", 7u) == 0 && is_space(text[content + 7u])) {
                if (out->widgets.present) {
                    snprintf(error, error_size, "line %u: a second widgets line", number);
                    return false;
                }
                if (!parse_widgets_line(text + content, stop - content, number, &out->widgets, error, error_size)) return false;
            } else {
                if (out->count >= ROUTE_NAV_MAX_MENUS) {
                    snprintf(error, error_size, "line %u: more than %u menus", number, ROUTE_NAV_MAX_MENUS);
                    return false;
                }
                route_nav_menu *menu = &out->menus[out->count];
                if (!parse_menu_line(text + content, stop - content, number, menu, error, error_size)) return false;
                for (size_t i = 0u; i < out->count; i++) {
                    if (strcmp(out->menus[i].id, menu->id) == 0) {
                        snprintf(error, error_size, "line %u: menu %s is already defined on line %u", number, menu->id, out->menus[i].line);
                        return false;
                    }
                }
                out->count++;
            }
        }
        at = end + 1u;
    }
    for (size_t i = 0u; i < out->count; i++) {
        if (out->menus[i].page_builder && !out->widgets.present) {
            snprintf(error, error_size, "line %u: menu %s has page=builder: but the table has no widgets line", out->menus[i].line, out->menus[i].id);
            return false;
        }
        out->menus[i].widgets = out->widgets;
    }
    return true;
}

const route_nav_menu *route_nav_menu_find(const route_nav_menu_table *table, const char *id)
{
    if (table == NULL || id == NULL) return NULL;
    for (size_t i = 0u; i < table->count; i++)
        if (strcmp(table->menus[i].id, id) == 0) return &table->menus[i];
    return NULL;
}
