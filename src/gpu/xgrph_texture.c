/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xgrph_texture.h"
#include "kernel_call.h"
#include "xgrph_hle.h"

#define TEXTURE_HEADER_ENTRY 0x003E6684u
#define CUBE_HEADER_ENTRY 0x003E66AEu
#define FORMAT_INFO 0x00402D70u

static uint32_t trailing_zeroes(uint32_t value)
{
    uint32_t count = 0u;
    while ((value & 1u) == 0u) { value >>= 1u; count++; }
    return count;
}

static uint32_t set_header(uint32_t entry, bool cube, uint32_t width_pixels,
    uint32_t height_pixels, uint32_t levels, uint32_t usage, uint32_t format,
    uint32_t pool, uint32_t header, uint32_t data, uint32_t pitch)
{
    (void)pool;
    /* 0x003E6499 indexes the XGRPH table with the whole format argument.
     * The recovered format domain ends at 0x3F; do not mask an invalid index. */
    if (format > 0x3Fu)
        xgrph_hle_fatal(entry, "unsupported texture format %#x", format);
    uint8_t information;
    if (!kernel_guest_read_u8(FORMAT_INFO + format, &information))
        xgrph_hle_fatal(entry, "texture format metadata is unreadable");
    /* Quiescent guest mappings: identical-byte permission probe precedes the
     * only publishing write, including headers spanning page boundaries. */
    uint8_t old_header[20];
    if (!kernel_guest_read_bytes(header, old_header, sizeof(old_header)) ||
        !kernel_guest_write_bytes(header, old_header, sizeof(old_header)))
        xgrph_hle_fatal(entry, "texture header %#x is not readable/writable", header);
    const uint32_t bits = information & 0x3Cu;
    const bool compressed = format == 0xCu || format == 0xEu || format == 0xFu;
    const bool swizzled = (information & 1u) != 0u || compressed;
    uint32_t width_log = 0u;
    uint32_t height_log = 0u;
    uint32_t size_word = 0u;
    uint32_t allocation = 0u;
    if (swizzled) {
        if (width_pixels == 0u || height_pixels == 0u)
            xgrph_hle_fatal(entry, "zero texture dimension has undefined original BSF result");
        width_log = trailing_zeroes(width_pixels);
        height_log = trailing_zeroes(height_pixels);
        if (levels == 0u) levels = (width_log > height_log ? width_log : height_log) + 1u;
        uint32_t width = width_log;
        uint32_t height = height_log;
        const uint32_t floor = compressed ? 2u : 0u;
        /* After at most 31 steps all three exponents have reached zero. The
         * remaining identical additions can be multiplied modulo 2^32. */
        const uint32_t steps = levels < 32u ? levels : 32u;
        uint32_t level_bytes = 0u;
        for (uint32_t i = 0u; i < steps; i++) {
            const uint32_t w = width > floor ? width : floor;
            const uint32_t h = height > floor ? height : floor;
            const uint32_t texels = 1u << ((w + h) & 31u);
            level_bytes = (texels * bits) >> 3u;
            allocation += level_bytes;
            if (width != 0u) width--;
            if (height != 0u) height--;
        }
        if (levels > steps) allocation += (levels - steps) * level_bytes;
        if (cube) allocation = ((allocation + 127u) & ~127u) * 6u;
    } else {
        if (levels == 0u) levels = 1u;
        if (pitch == 0u) pitch = (((bits * width_pixels) >> 3u) + 63u) & ~63u;
        allocation = height_pixels * pitch;
        if (width_pixels != 0u) {
            size_word = (((pitch >> 6u) - 1u) << 12u) | (height_pixels - 1u);
            size_word = (size_word << 12u) | (width_pixels - 1u);
        }
    }
    /* Deliberately retain the original unmasked OR/shift packing, including
     * level counts that spill beyond a nibble and integer wraparound. */
    uint32_t format_word = height_log;
    format_word = (format_word << 4u) | width_log;
    format_word = (format_word << 4u) | levels;
    format_word = (format_word << 8u) | format;
    format_word = (format_word << 4u) | 2u;
    format_word = (format_word << 4u) | (cube ? 4u : 0u) | 9u;
    if ((usage & 0x10000u) != 0u) format_word &= ~8u;
    const uint32_t words[5] = {0x40001u, data, 0u, format_word, size_word};
    if (!kernel_guest_write_bytes(header, words, sizeof(words)))
        xgrph_hle_fatal(entry, "texture header %#x is not writable for 20 bytes", header);
    return allocation;
}

uint32_t xgrph_set_cube_texture_header(uint32_t edge, uint32_t levels, uint32_t usage,
    uint32_t format, uint32_t pool, uint32_t header, uint32_t data, uint32_t pitch)
{
    return set_header(CUBE_HEADER_ENTRY, true, edge, edge, levels, usage, format,
                      pool, header, data, pitch);
}
uint32_t xgrph_set_texture_header(uint32_t width, uint32_t height, uint32_t levels,
    uint32_t usage, uint32_t format, uint32_t pool, uint32_t header,
    uint32_t data, uint32_t pitch)
{
    return set_header(TEXTURE_HEADER_ENTRY, false, width, height, levels, usage,
                      format, pool, header, data, pitch);
}

static uint32_t cube_header_handler(void *context)
{
    uint32_t arguments[8];
    for (unsigned i = 0u; i < 8u; i++) {
        if (!kernel_frame_arg((const kernel_call_frame *)context, i, &arguments[i]))
            xgrph_hle_fatal(CUBE_HEADER_ENTRY, "cube texture argument %u is unreadable", i);
    }
    return xgrph_set_cube_texture_header(arguments[0], arguments[1], arguments[2], arguments[3],
                                         arguments[4], arguments[5], arguments[6], arguments[7]);
}
static uint32_t texture_header_handler(void *context)
{
    uint32_t a[9];
    for (unsigned i = 0u; i < 9u; i++)
        if (!kernel_frame_arg((const kernel_call_frame *)context, i, &a[i]))
            xgrph_hle_fatal(TEXTURE_HEADER_ENTRY, "texture argument %u is unreadable", i);
    return xgrph_set_texture_header(a[0],a[1],a[2],a[3],a[4],a[5],a[6],a[7],a[8]);
}
size_t xgrph_texture_register(void)
{
    size_t count = xgrph_hle_register(CUBE_HEADER_ENTRY, cube_header_handler) ? 1u : 0u;
    return count + (xgrph_hle_register(TEXTURE_HEADER_ENTRY, texture_header_handler) ? 1u : 0u);
}
