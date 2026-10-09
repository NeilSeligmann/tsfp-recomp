/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1613: the guarded guest memory poke (see guest_poke.h). FABRICATED-STATE census aid.
 *
 * GUARDS (each has a mutation test in tests/c/test_guest_poke.c):
 *   G1 refused entirely without --forced-state (guest_poke_apply forced_state argument)
 *   G2 at most GUEST_POKE_MAX pokes, widths 1/2/4 only (so at most 4 bytes, hard cap GUEST_POKE_MAX_BYTES), aligned
 *   G3 direct target and the indirect pointer dword inside 0..0x03FFFFFF, no 32-bit wrap, pointee+offset+width <= 2^32
 *   G4 nothing below 0x10000, nothing inside the XBE image except a writable non-executable section, never .text or
 *      .rdata by name, a missing or malformed XBE header refuses everything
 *   G5 whole request validated before the first write, one refusal writes nothing
 *   G6 read-back after every write, a mismatch stops the request and is recorded
 *   G7 every poke logged (address, old, new, label, present) in DIR/guestpoke.log and the dump header, DIR/FORCED_STATE
 *
 * RACE (honest): the guest runs on other host threads. A poke is one aligned 1/2/4 byte store through
 * kernel_guest_write_bytes (the same path and write tracking as every host side guest write). The old value read
 * before and the read-back after are not atomic with the store, a guest thread writing the same bytes in between shows
 * up as a read-back mismatch or an old value that was already stale. Acceptable for a census aid, never a model.
 */
#include "guest_poke.h"

#include "kernel_call.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SPACE_LIMIT 0x04000000ull

static void fail(char *error, size_t size, const char *reason, unsigned line)
{
    if (error != NULL && size > 0u) {
        snprintf(error, size, "line %u: %s", line, reason);
    }
}

static bool number(const char *begin, const char *end, uint64_t *out)
{
    if (begin == end || *begin < '0' || *begin > '9') {
        return false;
    }
    char text[24];
    const size_t length = (size_t)(end - begin);
    if (length >= sizeof text) {
        return false;
    }
    memcpy(text, begin, length);
    text[length] = '\0';
    char *stop = NULL;
    errno = 0;
    const unsigned long long value = strtoull(text, &stop, 0);
    if (errno != 0 || stop == text || *stop != '\0') {
        return false;
    }
    *out = value;
    return true;
}

static bool parse_line(const char *begin, const char *end, guest_poke_item *item, const char **reason)
{
    memset(item, 0, sizeof *item);
    item->indirect = *begin == '*';
    const char *address_begin = item->indirect ? begin + 1 : begin;
    const char *equals = memchr(address_begin, '=', (size_t)(end - address_begin));
    if (equals == NULL) {
        *reason = "not ADDR[+OFF]=VALUE[:WIDTH]";
        return false;
    }
    const char *plus = memchr(address_begin, '+', (size_t)(equals - address_begin));
    const char *address_end = plus != NULL ? plus : equals;
    uint64_t address = 0u, offset = 0u, value = 0u, width = 4u;
    if (!number(address_begin, address_end, &address) || address > UINT32_MAX) {
        *reason = "bad address";
        return false;
    }
    if (plus != NULL && (!number(plus + 1, equals, &offset) || offset > UINT32_MAX)) {
        *reason = "bad offset";
        return false;
    }
    const char *colon = memchr(equals + 1, ':', (size_t)(end - equals - 1));
    const char *value_end = colon != NULL ? colon : end;
    if (!number(equals + 1, value_end, &value) || value > UINT32_MAX) {
        *reason = "bad value (32-bit)";
        return false;
    }
    if (colon != NULL && (!number(colon + 1, end, &width) || (width != 1u && width != 2u && width != 4u))) {
        *reason = "width must be 1, 2 or 4";
        return false;
    }
    if (width < 4u && value >> (8u * width) != 0u) {
        *reason = "value does not fit the width";
        return false;
    }
    item->address = (uint32_t)address;
    item->offset = (uint32_t)offset;
    item->width = (unsigned)width;
    item->value = (uint32_t)value;
    return true;
}

bool guest_poke_parse(guest_poke_request *request, const char *text, char *error, size_t error_size)
{
    if (request == NULL || text == NULL) {
        fail(error, error_size, "no request", 0u);
        return false;
    }
    guest_poke_request work;
    memset(&work, 0, sizeof work);
    unsigned line_number = 0u;
    const char *cursor = text;
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        const char *stop = line_end != NULL ? line_end : cursor + strlen(cursor);
        line_number++;
        const char *begin = cursor;
        const char *end = stop;
        cursor = line_end != NULL ? line_end + 1 : stop;
        while (begin < end && isspace((unsigned char)*begin)) {
            begin++;
        }
        while (end > begin && isspace((unsigned char)end[-1])) {
            end--;
        }
        if (begin == end || *begin == '#') {
            continue;
        }
        if (work.count >= GUEST_POKE_MAX) {
            fail(error, error_size, "too many pokes (max 16)", line_number);
            return false;
        }
        const char *reason = NULL;
        if (!parse_line(begin, end, &work.items[work.count], &reason)) {
            fail(error, error_size, reason, line_number);
            return false;
        }
        work.count++;
    }
    if (work.count == 0u) {
        fail(error, error_size, "empty request", 0u);
        return false;
    }
    *request = work;
    return true;
}

const char *guest_poke_resolve(const guest_poke_item *item, uint32_t pointer, uint32_t *target)
{
    if (item == NULL || target == NULL) {
        return "no item";
    }
    if ((item->width != 1u && item->width != 2u && item->width != 4u) || item->width > GUEST_POKE_MAX_BYTES) {
        return "width must be 1, 2 or 4";
    }
    uint64_t final = 0u;
    if (item->indirect) {
        if ((uint64_t)item->address + 4u > SPACE_LIMIT) {
            return "pointer dword outside 0..0x03FFFFFF";
        }
        if ((item->address & 3u) != 0u) {
            return "pointer dword not 4-byte aligned";
        }
        final = (uint64_t)pointer + item->offset;
        if (final + item->width > 0x100000000ull) {
            return "pointee + offset wraps past 2^32";
        }
    } else {
        final = (uint64_t)item->address + item->offset;
        if (final + item->width > SPACE_LIMIT) {
            return "target outside 0..0x03FFFFFF";
        }
    }
    if ((final & (item->width - 1u)) != 0u) {
        return "target not aligned to its width";
    }
    *target = (uint32_t)final;
    return NULL;
}

static uint32_t le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

void guest_poke_load_image(guest_poke_image *image)
{
    memset(image, 0, sizeof *image);
    uint8_t header[0x124];
    if (!kernel_guest_read_bytes(0x10000u, header, sizeof header) || memcmp(header, "XBEH", 4) != 0) {
        return;
    }
    const uint32_t base = le32(header + 0x104), size = le32(header + 0x10C);
    const uint32_t count = le32(header + 0x11C), table = le32(header + 0x120);
    if (base != 0x10000u || size == 0u || (uint64_t)base + size > SPACE_LIMIT || count == 0u ||
        count > GUEST_POKE_MAX_SECTIONS) {
        return;
    }
    for (uint32_t index = 0u; index < count; index++) {
        uint8_t raw[0x38];
        if ((uint64_t)table + (uint64_t)(index + 1u) * 0x38u > 0x100000000ull ||
            !kernel_guest_read_bytes(table + index * 0x38u, raw, sizeof raw)) {
            return;
        }
        guest_poke_section *section = &image->sections[index];
        section->flags = le32(raw);
        section->address = le32(raw + 4);
        section->size = le32(raw + 8);
        uint8_t name[sizeof section->name] = {0};
        if (kernel_guest_read_bytes(le32(raw + 0x14), name, sizeof name - 1u)) {
            memcpy(section->name, name, sizeof section->name - 1u);
        }
    }
    image->base = base;
    image->size = size;
    image->count = count;
    image->valid = true;
}

const char *guest_poke_check_target(const guest_poke_image *image, uint32_t target, unsigned width)
{
    if (image == NULL || !image->valid) {
        return "XBE section table unavailable, refusing";
    }
    if (target < GUEST_POKE_LOW_LIMIT) {
        return "below the XBE image base";
    }
    const uint64_t end = (uint64_t)target + width;
    if (end <= image->base || target >= (uint64_t)image->base + image->size) {
        return NULL; /* outside the image: heap, stacks, kernel data */
    }
    for (unsigned index = 0u; index < image->count; index++) {
        const guest_poke_section *section = &image->sections[index];
        if (target >= section->address && end <= (uint64_t)section->address + section->size) {
            if (strncmp(section->name, ".text", sizeof section->name) == 0 ||
                strncmp(section->name, ".rdata", sizeof section->name) == 0) {
                return "inside a .text or .rdata section of the XBE image";
            }
            if ((section->flags & 1u) == 0u) {
                return "inside a read-only section of the XBE image";
            }
            if ((section->flags & 4u) != 0u) {
                return "inside an executable section of the XBE image";
            }
            return NULL;
        }
    }
    return "inside the XBE image but not entirely in one section (header or gap)";
}

void (*guest_poke_test_after_write)(uint32_t target);

static void set_status(guest_poke_result *result, const char *text)
{
    snprintf(result->status, sizeof result->status, "%s", text);
}

static bool read_value(uint32_t target, unsigned width, uint32_t *out)
{
    uint8_t raw[4] = {0, 0, 0, 0};
    if (!kernel_guest_read_bytes(target, raw, width)) {
        return false;
    }
    *out = le32(raw);
    return true;
}

bool guest_poke_apply(const guest_poke_request *request, bool forced_state, const guest_poke_image *image,
                      guest_poke_report *report)
{
    memset(report, 0, sizeof *report);
    if (request == NULL || request->count == 0u || request->count > GUEST_POKE_MAX) {
        snprintf(report->summary, sizeof report->summary, "REFUSED: bad request");
        return false;
    }
    report->count = request->count;
    for (unsigned index = 0u; index < request->count; index++) {
        report->results[index].item = request->items[index];
    }
    if (!forced_state) {
        snprintf(report->summary, sizeof report->summary, "REFUSED: host not started with --forced-state");
        for (unsigned index = 0u; index < report->count; index++) {
            set_status(&report->results[index], "refused: no --forced-state");
        }
        return false;
    }
    bool valid = true;
    for (unsigned index = 0u; index < request->count; index++) {
        guest_poke_result *result = &report->results[index];
        const guest_poke_item *item = &result->item;
        if (item->indirect) {
            uint32_t pointer = 0u;
            if ((uint64_t)item->address + 4u > SPACE_LIMIT || !read_value(item->address, 4u, &pointer)) {
                set_status(result, "refused: pointer dword not readable");
                valid = false;
                continue;
            }
            result->pointer = pointer;
        }
        const char *reason = guest_poke_resolve(item, result->pointer, &result->target);
        if (reason == NULL) {
            reason = guest_poke_check_target(image, result->target, item->width);
        }
        if (reason == NULL && !read_value(result->target, item->width, &result->old_value)) {
            reason = "target not readable (unmapped)";
        }
        if (reason != NULL) {
            snprintf(result->status, sizeof result->status, "refused: %s", reason);
            valid = false;
        } else {
            set_status(result, "validated");
        }
    }
    if (!valid) {
        for (unsigned index = 0u; index < report->count; index++) {
            if (strcmp(report->results[index].status, "validated") == 0) {
                set_status(&report->results[index], "not written: another poke of the request was refused");
            }
        }
        snprintf(report->summary, sizeof report->summary, "REFUSED: nothing written");
        return false;
    }
    for (unsigned index = 0u; index < request->count; index++) {
        guest_poke_result *result = &report->results[index];
        const uint8_t raw[4] = {(uint8_t)result->item.value, (uint8_t)(result->item.value >> 8),
                                (uint8_t)(result->item.value >> 16), (uint8_t)(result->item.value >> 24)};
        if (!kernel_guest_write_bytes(result->target, raw, result->item.width)) {
            set_status(result, "write failed (page not writable)");
        } else {
            result->written = true;
            report->written++;
            if (guest_poke_test_after_write != NULL) {
                guest_poke_test_after_write(result->target);
            }
            if (!read_value(result->target, result->item.width, &result->read_back)) {
                set_status(result, "read-back unreadable");
            } else if (result->read_back != result->item.value) {
                set_status(result, "READBACK-MISMATCH");
            } else {
                set_status(result, "OK");
            }
        }
        if (strcmp(result->status, "OK") != 0) {
            for (unsigned later = index + 1u; later < report->count; later++) {
                set_status(&report->results[later], "skipped: an earlier poke failed");
            }
            snprintf(report->summary, sizeof report->summary, "FAILED at poke %u (%u written)", index + 1u,
                     report->written);
            return false;
        }
    }
    report->ok = true;
    snprintf(report->summary, sizeof report->summary, "OK: %u poke(s) written and read back", report->written);
    return true;
}

void guest_poke_format(const guest_poke_report *report, const char *label, uint64_t present, const char *prefix,
                       char *out, size_t out_size)
{
    size_t used = 0u;
    out[0] = '\0';
    used += (size_t)snprintf(out + used, out_size - used,
                             "%sFORCED-STATE (T1613, FABRICATED) poke label=%s present=%llu: %s\n", prefix, label,
                             (unsigned long long)present, report->summary);
    for (unsigned index = 0u; index < report->count && used < out_size; index++) {
        const guest_poke_result *result = &report->results[index];
        used += (size_t)snprintf(
            out + used, out_size - used,
            "%spoke %u label=%s present=%llu addr=%s0x%08X+0x%X width=%u target=0x%08X old=0x%08X new=0x%08X "
            "readback=0x%08X status=%s\n",
            prefix, index, label, (unsigned long long)present, result->item.indirect ? "*" : "",
            (unsigned)result->item.address, (unsigned)result->item.offset, result->item.width,
            (unsigned)result->target, (unsigned)result->old_value, (unsigned)result->item.value,
            (unsigned)result->read_back, result->status);
    }
    if (used >= out_size) {
        out[out_size - 1u] = '\0';
    }
}

static bool clean_label(const char *label)
{
    if (label == NULL || label[0] == '\0' || strlen(label) > 63u) {
        return false;
    }
    for (const char *c = label; *c != '\0'; c++) {
        if (!isalnum((unsigned char)*c) && *c != '_' && *c != '-') {
            return false;
        }
    }
    return true;
}

static unsigned consumed_serial;

bool guest_poke_consume(const char *dir, const char *label, bool forced_state, uint64_t present, char *header,
                        size_t header_size)
{
    if (header != NULL && header_size > 0u) {
        header[0] = '\0';
    }
    char path[512];
    if (dir == NULL || !clean_label(label) || strlen(dir) > 380u) {
        return false;
    }
    snprintf(path, sizeof path, "%s/guestpoke.%s", dir, label);
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    char *text = calloc(1u, 8192u);
    char *log_text = calloc(1u, 8192u);
    guest_poke_report *report = calloc(1u, sizeof *report);
    if (text == NULL || log_text == NULL || report == NULL) {
        fclose(file);
        free(text);
        free(log_text);
        free(report);
        return true;
    }
    const size_t got = fread(text, 1u, 8191u, file);
    const bool truncated = got == 8191u && fgetc(file) != EOF;
    fclose(file);
    guest_poke_request request;
    char error[128] = "";
    if (truncated || !guest_poke_parse(&request, text, error, sizeof error)) {
        snprintf(report->summary, sizeof report->summary, "REFUSED: unparsable request");
        fprintf(stderr, "guest poke (T1613): label %s unparsable: %s\n", label, truncated ? "file over 8 KiB" : error);
    } else {
        guest_poke_image image;
        guest_poke_load_image(&image);
        guest_poke_apply(&request, forced_state, &image, report);
    }
    guest_poke_format(report, label, present, "", log_text, 8192u);
    char log_path[512];
    snprintf(log_path, sizeof log_path, "%s/guestpoke.log", dir);
    FILE *log = fopen(log_path, "a");
    if (log != NULL) {
        fputs(log_text, log);
        fclose(log);
    }
    if (report->written > 0u) {
        char marker_path[512];
        snprintf(marker_path, sizeof marker_path, "%s/FORCED_STATE", dir);
        FILE *marker = fopen(marker_path, "a");
        if (marker != NULL) {
            fprintf(marker,
                    "FABRICATED-STATE (T1613): guest memory was written by --forced-state pokes, see guestpoke.log. "
                    "Evidence from this folder supports naming at INFERRED only, never MEASURED, never proof.\n");
            fclose(marker);
        }
    }
    if (header != NULL && header_size > 0u) {
        guest_poke_format(report, label, present, "# ", header, header_size);
    }
    char done[560];
    snprintf(done, sizeof done, "%s.done.%u", path, ++consumed_serial);
    if (rename(path, done) != 0) {
        remove(path); /* never leave a request that could fire twice */
    }
    fprintf(stderr, "guest poke (T1613, FORCED-STATE): label %s -> %s\n", label, report->summary);
    free(text);
    free(log_text);
    free(report);
    return true;
}
