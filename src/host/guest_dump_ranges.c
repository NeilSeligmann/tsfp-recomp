/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1599: parsing and bounds of --dump-guest-range. Pure, no guest memory access.
 */
#include "guest_dump.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fail(char *error, size_t size, const char *reason)
{
    if (error != NULL && size > 0u) {
        snprintf(error, size, "%s", reason);
    }
}

static bool parse_number(const char *begin, const char *end, uint64_t *out)
{
    if (begin == end || *begin < '0' || *begin > '9') { /* no sign, no blank, no empty */
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

bool guest_dump_parse_append(guest_dump_set *set, const char *text, char *error, size_t error_size)
{
    if (set == NULL || text == NULL || text[0] == '\0') {
        fail(error, error_size, "empty range list");
        return false;
    }
    guest_dump_set work = *set;
    const char *item = text;
    for (;;) {
        const char *comma = strchr(item, ',');
        const char *item_end = comma != NULL ? comma : item + strlen(item);
        const char *colon = memchr(item, ':', (size_t)(item_end - item));
        if (colon == NULL) {
            fail(error, error_size, "range is not ADDR:LEN, *ADDR+OFF:LEN or **ADDR+OFF1+OFF2:LEN");
            return false;
        }
        /* T1759: `**ADDR+OFF1+OFF2:LEN` is the two level form, both offsets are mandatory (so `**ADDR:LEN` stays refused) */
        const bool twice = item[0] == '*' && item[1] == '*';
        const bool indirect = item[0] == '*';
        const char *address_begin = twice ? item + 2 : indirect ? item + 1 : item;
        const char *plus = indirect ? memchr(address_begin, '+', (size_t)(colon - address_begin)) : NULL;
        const char *address_end = plus != NULL ? plus : colon;
        const char *plus2 = twice && plus != NULL ? memchr(plus + 1, '+', (size_t)(colon - plus - 1)) : NULL;
        uint64_t address = 0u;
        uint64_t length = 0u;
        uint64_t offset = 0u;
        uint64_t offset2 = 0u;
        if (twice && plus2 == NULL) {
            fail(error, error_size, "two level range needs **ADDR+OFF1+OFF2:LEN");
            return false;
        }
        if (!parse_number(address_begin, address_end, &address) || !parse_number(colon + 1, item_end, &length) ||
            (plus != NULL && !parse_number(plus + 1, twice ? plus2 : colon, &offset)) ||
            (twice && !parse_number(plus2 + 1, colon, &offset2))) {
            fail(error, error_size, "address or length is not an unsigned number");
            return false;
        }
        if (length == 0u) {
            fail(error, error_size, "length is zero");
            return false;
        }
        if (address >= GUEST_DUMP_ADDRESS_LIMIT) {
            fail(error, error_size, "address is outside the guest address space 0..0x03FFFFFF");
            return false;
        }
        /* 64-bit sum: a 32-bit address+length that wraps cannot pass as a small end */
        if (!indirect && (length > GUEST_DUMP_ADDRESS_LIMIT || address + length > GUEST_DUMP_ADDRESS_LIMIT)) {
            fail(error, error_size, "range leaves the guest address space (or wraps)");
            return false;
        }
        if (indirect) {
            /* the pointer dword must lie inside the space, the pointee offset and length must fit on their own */
            if (address + 4u > GUEST_DUMP_ADDRESS_LIMIT) {
                fail(error, error_size, "pointer dword leaves the guest address space");
                return false;
            }
            /* length is bounded first: a 64-bit offset+length could otherwise wrap */
            if (offset > GUEST_DUMP_ADDRESS_LIMIT || length > GUEST_DUMP_ADDRESS_LIMIT ||
                (!twice && offset + length > GUEST_DUMP_ADDRESS_LIMIT)) {
                fail(error, error_size, "offset+length leaves the guest address space (or wraps)");
                return false;
            }
            if (twice && (offset2 > GUEST_DUMP_ADDRESS_LIMIT || offset2 + length > GUEST_DUMP_ADDRESS_LIMIT)) {
                fail(error, error_size, "offset2+length leaves the guest address space (or wraps)");
                return false;
            }
        }
        if (work.count >= GUEST_DUMP_MAX_RANGES) {
            fail(error, error_size, "too many ranges");
            return false;
        }
        work.ranges[work.count].address = (uint32_t)address;
        work.ranges[work.count].length = (uint32_t)length;
        work.ranges[work.count].indirect = indirect;
        work.ranges[work.count].offset = (uint32_t)offset;
        work.ranges[work.count].twice = twice;
        work.ranges[work.count].offset2 = (uint32_t)offset2;
        work.count++;
        work.total_bytes += length;
        if (comma == NULL) {
            break;
        }
        item = comma + 1;
    }
    *set = work;
    return true;
}

const char *guest_dump_indirect_target(const guest_dump_range *range, uint32_t pointer, uint32_t *final)
{
    if (range == NULL || final == NULL || !range->indirect) {
        return "not an indirect range";
    }
    /* The pointee is NOT bound to 0..0x03FFFFFF: the title's heap pointers are above it (measured 0x42F9AE20), the guest
     * is identity mapped in 32 bits and the read itself says whether a page is mapped. 64-bit sum: no 32-bit wrap. */
    if ((uint64_t)pointer + range->offset + range->length > GUEST_DUMP_POINTEE_LIMIT) {
        return "pointer+offset+length wraps past 0xFFFFFFFF";
    }
    *final = pointer + range->offset;
    return NULL;
}

const char *guest_dump_twice_slot(const guest_dump_range *range, uint32_t first, uint32_t *slot)
{
    if (range == NULL || slot == NULL || !range->indirect || !range->twice) {
        return "not a two level range";
    }
    if (first == 0u) {
        return "first pointer is null";
    }
    if ((uint64_t)first + range->offset + 4u > GUEST_DUMP_POINTEE_LIMIT) {
        return "first pointer+offset wraps past 0xFFFFFFFF";
    }
    *slot = first + range->offset;
    return NULL;
}

const char *guest_dump_twice_target(const guest_dump_range *range, uint32_t second, uint32_t *final)
{
    if (range == NULL || final == NULL || !range->indirect || !range->twice) {
        return "not a two level range";
    }
    if (second == 0u) {
        return "second pointer is null";
    }
    if ((uint64_t)second + range->offset2 + range->length > GUEST_DUMP_POINTEE_LIMIT) {
        return "second pointer+offset2+length wraps past 0xFFFFFFFF";
    }
    *final = second + range->offset2;
    return NULL;
}

bool guest_dump_check_total(const guest_dump_set *set, uint64_t max_bytes)
{
    return set != NULL && set->count > 0u && set->total_bytes <= max_bytes && max_bytes <= GUEST_DUMP_HARD_MAX_BYTES;
}
