/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xinput_route_spec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool number(const char *text, size_t length, uint64_t limit, uint64_t *out)
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

static bool mem_condition(const char *text, xinput_route_wait *out)
{
    const char *eq = strstr(text, "==");
    if (eq == NULL) return false;
    uint64_t address, value, mask = 0xFFFFFFFFull, width = 4u;
    const char *colon = memchr(text, ':', (size_t)(eq - text));
    const char *address_end = colon != NULL ? colon : eq;
    if (!number(text, (size_t)(address_end - text), 0xFFFFFFFFull, &address)) return false;
    if (colon != NULL && (!number(colon + 1, (size_t)(eq - colon - 1), 4u, &width) || (width != 1u && width != 2u && width != 4u)))
        return false;
    const char *value_start = eq + 2;
    const char *amp = strchr(value_start, '&');
    const size_t value_length = amp != NULL ? (size_t)(amp - value_start) : strlen(value_start);
    if (!number(value_start, value_length, 0xFFFFFFFFull, &value)) return false;
    if (amp != NULL && !number(amp + 1, strlen(amp + 1), 0xFFFFFFFFull, &mask)) return false;
    if (address % width != 0u || address + width > 0x04000000ull) return false;
    if (width < 4u) mask &= (1ull << (8u * width)) - 1u;
    if ((value & ~mask) != 0u) return false; /* a value the mask hides can never match */
    out->has_mem = true;
    out->address = (uint32_t)address;
    out->width = (unsigned)width;
    out->value = (uint32_t)value;
    out->mask = (uint32_t)mask;
    return true;
}

/* ---- T1633 event grammar ---- */

static bool lower_text(const char *begin, size_t length, char *out, size_t out_size)
{
    if (length == 0u || length >= out_size) return false;
    for (size_t i = 0u; i < length; i++) {
        unsigned char c = (unsigned char)begin[i];
        if (c < 0x20u || c > 0x7Eu || c == ',') return false;
        out[i] = (char)((c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c);
    }
    out[length] = '\0';
    return true;
}

/* mem=[*]ADDR[+OFF][:W]OP VALUE[&MASK] */
static bool mem_event_condition(const char *text, size_t length, route_cond *out)
{
    char copy[96];
    if (length == 0u || length >= sizeof copy) return false;
    memcpy(copy, text, length);
    copy[length] = '\0';
    const bool indirect = copy[0] == '*';
    const char *start = copy + (indirect ? 1 : 0);
    const char *op = NULL;
    size_t op_length = 0u;
    route_cmp cmp = RCMP_EQ;
    for (const char *scan = start; *scan != '\0' && op == NULL; scan++) {
        if (scan[0] == '=' && scan[1] == '=') { op = scan; op_length = 2u; cmp = RCMP_EQ; }
        else if (scan[0] == '!' && scan[1] == '=') { op = scan; op_length = 2u; cmp = RCMP_NE; }
        else if (scan[0] == '<' && scan[1] == '=') { op = scan; op_length = 2u; cmp = RCMP_LE; }
        else if (scan[0] == '>' && scan[1] == '=') { op = scan; op_length = 2u; cmp = RCMP_GE; }
        else if (scan[0] == '<') { op = scan; op_length = 1u; cmp = RCMP_LT; }
        else if (scan[0] == '>') { op = scan; op_length = 1u; cmp = RCMP_GT; }
    }
    if (op == NULL) return false;
    uint64_t address, offset = 0u, value, mask = 0xFFFFFFFFull, width = 4u;
    const char *colon = memchr(start, ':', (size_t)(op - start));
    const char *address_end = colon != NULL ? colon : op;
    const char *plus = indirect ? memchr(start, '+', (size_t)(address_end - start)) : NULL;
    if (plus != NULL) {
        if (!number(plus + 1, (size_t)(address_end - plus - 1), 0xFFFFFFFFull, &offset)) return false;
        address_end = plus;
    }
    if (!number(start, (size_t)(address_end - start), 0xFFFFFFFFull, &address)) return false;
    if (colon != NULL && (!number(colon + 1, (size_t)(op - colon - 1), 4u, &width) || (width != 1u && width != 2u && width != 4u)))
        return false;
    const char *value_start = op + op_length;
    const char *amp = strchr(value_start, '&');
    const size_t value_length = amp != NULL ? (size_t)(amp - value_start) : strlen(value_start);
    if (!number(value_start, value_length, 0xFFFFFFFFull, &value)) return false;
    if (amp != NULL && !number(amp + 1, strlen(amp + 1), 0xFFFFFFFFull, &mask)) return false;
    if (width < 4u) mask &= (1ull << (8u * width)) - 1u;
    if (indirect) {
        if (address % 4u != 0u || address + 4u > 0x04000000ull) return false; /* the pointer dword */
    } else if (address % width != 0u || address + width > 0x04000000ull) {
        return false;
    }
    if ((cmp == RCMP_EQ) && (value & ~mask) != 0u) return false; /* a value the mask hides can never match */
    memset(out, 0, sizeof *out);
    out->kind = RCOND_MEM;
    out->indirect = indirect;
    out->address = (uint32_t)address;
    out->offset = (uint32_t)offset;
    out->width = (unsigned)width;
    out->cmp = cmp;
    out->value = (uint32_t)value;
    out->mask = (uint32_t)mask;
    return true;
}

/* T1640: one memory condition on its own (the menu table and the nav steps reuse the T1633 `mem=` grammar). */
bool xinput_route_mem_cond_parse(const char *text, route_cond *out)
{
    return text != NULL && mem_event_condition(text, strlen(text), out);
}

/* T1640: a memory reference without comparison, `[*]ADDR[+OFF][:W][&MASK]` (a cursor or count variable). Parsed as `==0`. */
bool xinput_route_mem_ref_parse(const char *text, route_cond *out)
{
    char spec[96];
    if (text == NULL || strchr(text, '=') != NULL || strchr(text, '<') != NULL || strchr(text, '>') != NULL || strchr(text, '!') != NULL)
        return false;
    const char *amp = strchr(text, '&');
    const size_t head = amp != NULL ? (size_t)(amp - text) : strlen(text);
    if (head == 0u || head + 3u + (amp != NULL ? strlen(amp) : 0u) >= sizeof spec) return false;
    memcpy(spec, text, head);
    memcpy(spec + head, "==0", 3u);
    if (amp != NULL) strcpy(spec + head + 3u, amp);
    else spec[head + 3u] = '\0';
    return mem_event_condition(spec, strlen(spec), out);
}

bool xinput_route_mem_address(const route_cond *cond, xinput_route_read_fn read, void *user, uint32_t *address)
{
    if (read == NULL) return false;
    if (!cond->indirect) {
        *address = cond->address;
        return true;
    }
    uint32_t pointer = 0u;
    if (!read(cond->address, 4u, &pointer, user) || pointer == 0u) return false;
    *address = pointer + cond->offset;
    return true;
}

bool xinput_route_mem_read(const route_cond *cond, xinput_route_read_fn read, void *user, uint32_t *value)
{
    uint32_t address = 0u;
    if (!xinput_route_mem_address(cond, read, user, &address)) return false;
    return read(address, cond->width, value, user);
}

bool xinput_route_compare(route_cmp cmp, uint32_t left, uint32_t right)
{
    switch (cmp) {
    case RCMP_EQ: return left == right;
    case RCMP_NE: return left != right;
    case RCMP_LT: return left < right;
    case RCMP_LE: return left <= right;
    case RCMP_GT: return left > right;
    case RCMP_GE: return left >= right;
    }
    return false;
}

static bool add_cond(xinput_route_wait *wait, const route_cond *cond)
{
    if (wait->cond_count >= ROUTE_COND_MAX) return false;
    wait->conds[wait->cond_count++] = *cond;
    return true;
}

/* One field of an event wait. Sets *good_out; false only for an error already described in `error`. */
static bool event_field(const char *at, size_t length, xinput_route_wait *out, bool *good_out, char *error, size_t error_size)
{
    *good_out = false;
    (void)error;
    (void)error_size;
    route_cond cond;
    memset(&cond, 0, sizeof cond);
    uint64_t amount;
    if (length >= 4u && memcmp(at, "mem=", 4u) == 0) {
        if (mem_event_condition(at + 4, length - 4u, &cond)) *good_out = add_cond(out, &cond);
    } else if (length >= 10u && memcmp(at, "file-open=", 10u) == 0) {
        cond.kind = RCOND_FILE_OPEN;
        if (lower_text(at + 10, length - 10u, cond.text, sizeof cond.text)) *good_out = add_cond(out, &cond);
    } else if (length >= 10u && memcmp(at, "file-read=", 10u) == 0) {
        const char *text = at + 10;
        size_t text_length = length - 10u;
        const char *at_sign = memchr(text, '@', text_length);
        cond.kind = RCOND_FILE_READ;
        cond.amount = 1u;
        if (at_sign != NULL) {
            if (!number(at_sign + 1, (size_t)(text + text_length - at_sign - 1), 0xFFFFFFFFull, &amount) || amount == 0u) return true;
            cond.amount = amount;
            text_length = (size_t)(at_sign - text);
        }
        if (lower_text(text, text_length, cond.text, sizeof cond.text)) *good_out = add_cond(out, &cond);
    } else if (length >= 10u && memcmp(at, "file-idle=", 10u) == 0) {
        const char *text = at + 10;
        size_t text_length = length - 10u;
        const char *at_sign = memchr(text, '@', text_length);
        const size_t ms_length = at_sign != NULL ? (size_t)(at_sign - text) : text_length;
        cond.kind = RCOND_FILE_IDLE;
        if (!number(text, ms_length, XINPUT_ROUTE_TIMEOUT_MS_LIMIT, &amount) || amount == 0u) return true;
        cond.amount = amount;
        if (at_sign == NULL || lower_text(at_sign + 1, (size_t)(text + text_length - at_sign - 1), cond.text, sizeof cond.text))
            *good_out = add_cond(out, &cond);
    } else if (length >= 5u && memcmp(at, "call=", 5u) == 0) {
        const char *text = at + 5;
        const size_t text_length = length - 5u;
        const char *at_sign = memchr(text, '@', text_length);
        const size_t va_length = at_sign != NULL ? (size_t)(at_sign - text) : text_length;
        uint64_t va;
        cond.kind = RCOND_CALL;
        cond.amount = 1u;
        if (!number(text, va_length, 0x03FFFFFFull, &va) || va == 0u) return true;
        if (at_sign != NULL && (!number(at_sign + 1, (size_t)(text + text_length - at_sign - 1), 0xFFFFFFull, &amount) || amount == 0u))
            return true;
        if (at_sign != NULL) cond.amount = amount;
        cond.va = (uint32_t)va;
        *good_out = add_cond(out, &cond);
    } else if (length == 12u && memcmp(at, "frame-change", 12u) == 0) {
        cond.kind = RCOND_FRAME_CHANGE;
        *good_out = add_cond(out, &cond);
    } else if (length >= 13u && memcmp(at, "frame-stable=", 13u) == 0) {
        cond.kind = RCOND_FRAME_STABLE;
        if (number(at + 13, length - 13u, XINPUT_ROUTE_TIMEOUT_MS_LIMIT, &amount) && amount != 0u) {
            cond.amount = amount;
            *good_out = add_cond(out, &cond);
        }
    } else if (length >= 4u && memcmp(at, "min=", 4u) == 0) {
        if (out->min_polls == 0u && number(at + 4, length - 4u, XINPUT_ROUTE_MAX_POLLS_LIMIT, &amount) && amount != 0u) {
            out->min_polls = amount;
            *good_out = true;
        }
    } else if (length >= 7u && memcmp(at, "min-ms=", 7u) == 0) {
        if (out->min_ms == 0u && number(at + 7, length - 7u, XINPUT_ROUTE_TIMEOUT_MS_LIMIT, &amount) && amount != 0u) {
            out->min_ms = amount;
            *good_out = true;
        }
    } else if (length >= 4u && memcmp(at, "max=", 4u) == 0) {
        if (out->max_polls == 0u && number(at + 4, length - 4u, XINPUT_ROUTE_MAX_POLLS_LIMIT, &amount) && amount != 0u) {
            out->max_polls = amount;
            *good_out = true;
        }
    } else if (length >= 8u && memcmp(at, "timeout=", 8u) == 0) {
        if (out->timeout_ms == 0u && number(at + 8, length - 8u, XINPUT_ROUTE_TIMEOUT_MS_LIMIT, &amount) && amount != 0u) {
            out->timeout_ms = amount;
            *good_out = true;
        }
    } else if (length == 3u && memcmp(at, "any", 3u) == 0) {
        if (!out->any) *good_out = out->any = true;
    }
    return true;
}

static bool wait_parse(const char *spec, xinput_route_wait *out, bool events, char *error, size_t error_size)
{
    memset(out, 0, sizeof *out);
    out->max_polls = events ? 0u : XINPUT_ROUTE_DEFAULT_MAX_POLLS;
    out->event = events;
    if (spec == NULL || strncmp(spec, "mark", 4) != 0) {
        snprintf(error, error_size, "route wait '%s': want markK:%s", spec != NULL ? spec : "",
                 events ? "file-open=...,mem=...,timeout=MS" : "mem=...,min=N,max=N");
        return false;
    }
    uint64_t mark;
    const char *colon = strchr(spec, ':');
    if (colon == NULL || !number(spec + 4, (size_t)(colon - spec - 4), XINPUT_ROUTE_MAX_MARKS, &mark) || mark == 0u) {
        snprintf(error, error_size, "route wait '%s': the mark must be mark1..mark%u", spec, XINPUT_ROUTE_MAX_MARKS);
        return false;
    }
    out->mark = (unsigned)mark;
    bool have_min = false;
    for (const char *at = colon + 1; *at != '\0';) {
        const char *end = at;
        while (*end != '\0' && *end != ',') end++;
        uint64_t polls;
        bool good = false;
        if (events) {
            if (!event_field(at, (size_t)(end - at), out, &good, error, error_size)) return false;
        } else if (strncmp(at, "mem=", 4) == 0 && !out->has_mem) {
            char condition[96];
            const size_t length = (size_t)(end - at - 4);
            if (length != 0u && length < sizeof condition) {
                memcpy(condition, at + 4, length);
                condition[length] = '\0';
                good = mem_condition(condition, out);
            }
        } else if (strncmp(at, "min=", 4) == 0 && !have_min &&
                   number(at + 4, (size_t)(end - at - 4), XINPUT_ROUTE_MAX_POLLS_LIMIT, &polls)) {
            out->min_polls = polls;
            have_min = good = true;
        } else if (strncmp(at, "max=", 4) == 0 && number(at + 4, (size_t)(end - at - 4), XINPUT_ROUTE_MAX_POLLS_LIMIT, &polls) &&
                   polls > 0u) {
            out->max_polls = polls;
            good = true;
        }
        if (!good) {
            snprintf(error, error_size, "route wait '%s': bad, repeated or too many fields near '%.24s'", spec, at);
            return false;
        }
        at = *end == ',' ? end + 1 : end;
    }
    if (events) {
        if (out->cond_count == 0u && out->min_polls == 0u && out->min_ms == 0u) {
            snprintf(error, error_size, "route wait '%s': give at least one condition (mem=, file-open=, ...) or min=/min-ms=, else it never waits",
                     spec);
            return false;
        }
        if (out->any && out->cond_count < 2u) {
            snprintf(error, error_size, "route wait '%s': 'any' needs two or more conditions", spec);
            return false;
        }
        if (out->timeout_ms == 0u && out->max_polls == 0u) out->timeout_ms = XINPUT_ROUTE_DEFAULT_TIMEOUT_MS;
        if (out->timeout_ms != 0u && out->min_ms > out->timeout_ms) {
            snprintf(error, error_size, "route wait '%s': min-ms is above timeout", spec);
            return false;
        }
        if (out->max_polls != 0u && out->min_polls > out->max_polls) {
            snprintf(error, error_size, "route wait '%s': min is above max", spec);
            return false;
        }
        return true;
    }
    if (!out->has_mem && !have_min) {
        snprintf(error, error_size, "route wait '%s': give mem=ADDR==VALUE or min=N, else it never waits", spec);
        return false;
    }
    if (out->min_polls > out->max_polls) {
        snprintf(error, error_size, "route wait '%s': min is above max", spec);
        return false;
    }
    return true;
}

bool xinput_route_wait_parse(const char *spec, xinput_route_wait *out, char *error, size_t error_size)
{
    return wait_parse(spec, out, false, error, error_size);
}

bool xinput_route_event_wait_parse(const char *spec, xinput_route_wait *out, char *error, size_t error_size)
{
    return wait_parse(spec, out, true, error, error_size);
}

size_t xinput_route_cond_format(const route_cond *cond, char *out, size_t out_size)
{
    static const char *const ops[] = {"", "==", "!=", "<", "<=", ">", ">="};
    int n = 0;
    switch (cond->kind) {
    case RCOND_MEM:
        n = snprintf(out, out_size, "mem=%s0x%X%s%s0x%X:%u%s0x%X", cond->indirect ? "*" : "", (unsigned)cond->address,
                     cond->indirect ? "+" : "", cond->indirect ? "" : "", cond->indirect ? (unsigned)cond->offset : 0u,
                     cond->width, ops[cond->cmp], (unsigned)cond->value);
        if (!cond->indirect) n = snprintf(out, out_size, "mem=0x%X:%u%s0x%X", (unsigned)cond->address, cond->width, ops[cond->cmp], (unsigned)cond->value);
        break;
    case RCOND_FILE_OPEN: n = snprintf(out, out_size, "file-open=%s", cond->text); break;
    case RCOND_FILE_READ: n = snprintf(out, out_size, "file-read=%s@%llu", cond->text, (unsigned long long)cond->amount); break;
    case RCOND_FILE_IDLE:
        n = cond->text[0] != '\0' ? snprintf(out, out_size, "file-idle=%llu@%s", (unsigned long long)cond->amount, cond->text)
                                  : snprintf(out, out_size, "file-idle=%llu", (unsigned long long)cond->amount);
        break;
    case RCOND_CALL: n = snprintf(out, out_size, "call=0x%X@%llu", (unsigned)cond->va, (unsigned long long)cond->amount); break;
    case RCOND_FRAME_CHANGE: n = snprintf(out, out_size, "frame-change"); break;
    case RCOND_FRAME_STABLE: n = snprintf(out, out_size, "frame-stable=%llu", (unsigned long long)cond->amount); break;
    }
    return n < 0 ? 0u : (size_t)n;
}

bool poke_trigger_parse(const char *spec, poke_trigger_entry *out, char *error, size_t error_size)
{
    memset(out, 0, sizeof *out);
    const char *colon = spec != NULL ? strchr(spec, ':') : NULL;
    if (colon == NULL) {
        snprintf(error, error_size, "poke trigger '%s': want WHERE:LABEL (N, markK or replay-end)", spec != NULL ? spec : "");
        return false;
    }
    const size_t where_length = (size_t)(colon - spec);
    uint64_t value;
    if (where_length == 10u && strncmp(spec, "replay-end", 10) == 0) {
        out->where = POKE_AT_END;
    } else if (where_length > 4u && strncmp(spec, "mark", 4) == 0 &&
               number(spec + 4, where_length - 4u, XINPUT_ROUTE_MAX_MARKS, &value) && value != 0u) {
        out->where = POKE_AT_MARK;
        out->value = value;
    } else if (number(spec, where_length, UINT32_MAX, &value) && value != 0u) {
        out->where = POKE_AT_POLL;
        out->value = value;
    } else {
        snprintf(error, error_size, "poke trigger '%s': WHERE must be a poll number >= 1, mark1..mark%u or replay-end", spec,
                 XINPUT_ROUTE_MAX_MARKS);
        return false;
    }
    const char *label = colon + 1;
    const size_t length = strlen(label);
    bool clean = length != 0u && length < sizeof out->label;
    for (size_t i = 0u; clean && i < length; i++) {
        const char c = label[i];
        clean = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }
    if (!clean) {
        snprintf(error, error_size, "poke trigger '%s': the label must be 1..63 of [A-Za-z0-9_-]", spec);
        return false;
    }
    memcpy(out->label, label, length + 1u);
    return true;
}

const char *poke_trigger_take(poke_trigger_set *set, poke_where where, uint64_t value)
{
    for (unsigned i = 0u; i < set->count; i++) {
        poke_trigger_entry *entry = &set->entries[i];
        if (entry->fired || entry->where != where) continue;
        const bool due = where == POKE_AT_POLL ? value >= entry->value : (where == POKE_AT_MARK ? value == entry->value : true);
        if (!due) continue;
        entry->fired = true;
        return entry->label;
    }
    return NULL;
}
