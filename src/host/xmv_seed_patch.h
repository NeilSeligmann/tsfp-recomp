/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_HOST_XMV_SEED_PATCH_H
#define TSFP_HOST_XMV_SEED_PATCH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* T611: one edit of the guest state at the entry of the XMV codec wrapper, "TARGET=HEXBYTES".
 *   dec+OFF=HEX        bytes at decoder object + OFF (the object is the wrapper's first stack argument)
 *   esp+OFF=HEX        bytes at ESP + OFF (ESP points at the return address)
 *   dec@PTR+OFF=HEX    bytes at (dword at decoder + PTR) + OFF, a buffer the decoder object points to
 * OFF and PTR are decimal or 0x hex below 0x10000000, HEX is 1 to 128 bytes. The host and
 * tools/diagnostics/replay_xmv_seeded.py read the same grammar. Header only so the option parser and
 * the seam share it without a library dependency. */
#define XMV_SEED_PATCH_BYTES 128u
typedef enum { XMV_SEED_DEC, XMV_SEED_ESP, XMV_SEED_DEC_POINTER } xmv_seed_base;
typedef struct {
    xmv_seed_base base;
    uint32_t pointer_offset;
    uint32_t offset;
    uint8_t bytes[XMV_SEED_PATCH_BYTES];
    size_t length;
} xmv_seed_patch;

static inline bool xmv_seed_number(const char *text, uint32_t *value, const char **rest)
{
    char *end = NULL;
    if (text[0] < '0' || text[0] > '9') return false;
    const bool hex = text[0] == '0' && (text[1] == 'x' || text[1] == 'X') && text[2] != '\0';
    const unsigned long long number = strtoull(hex ? text + 2 : text, &end, hex ? 16 : 10);
    if (end == (hex ? text + 2 : text) || number >= 0x10000000ull) return false;
    *value = (uint32_t)number;
    *rest = end;
    return true;
}

static inline int xmv_seed_hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline bool xmv_seed_patch_parse(const char *text, xmv_seed_patch *out)
{
    const char *rest = NULL;
    memset(out, 0, sizeof *out);
    if (strncmp(text, "dec@", 4u) == 0) {
        if (!xmv_seed_number(text + 4, &out->pointer_offset, &rest) || rest[0] != '+') return false;
        out->base = XMV_SEED_DEC_POINTER;
        if (!xmv_seed_number(rest + 1, &out->offset, &rest)) return false;
    } else if (strncmp(text, "dec+", 4u) == 0 || strncmp(text, "esp+", 4u) == 0) {
        out->base = text[0] == 'd' ? XMV_SEED_DEC : XMV_SEED_ESP;
        if (!xmv_seed_number(text + 4, &out->offset, &rest)) return false;
    } else {
        return false;
    }
    if (rest[0] != '=') return false;
    rest++;
    const size_t digits = strlen(rest);
    if (digits == 0u || digits % 2u != 0u || digits / 2u > XMV_SEED_PATCH_BYTES) return false;
    for (size_t i = 0u; i < digits; i += 2u) {
        const int high = xmv_seed_hex_digit(rest[i]), low = xmv_seed_hex_digit(rest[i + 1u]);
        if (high < 0 || low < 0) return false;
        out->bytes[i / 2u] = (uint8_t)(high * 16 + low);
    }
    out->length = digits / 2u;
    return true;
}
#endif
