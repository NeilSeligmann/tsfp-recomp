/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_overlay_image.h. Depends only on the C library.
 */

#include "d3d8_overlay_image.h"

#include <stdio.h>
#include <string.h>

/* The constants the original wrapper 0x445DF7 loads: mm5 words 0x54FB and 0x4000 (their sum is
 * the luma gain), mm7 words -16, mm6 dwords -128 and the four floats of xmm4 to xmm7. */
#define LUMA_BIAS 16
#define CHROMA_BIAS 128
#define LUMA_GAIN (0x54FB + 0x4000)
#define V_TO_RED 52298
#define V_TO_GREEN (-26640)
#define U_TO_GREEN (-12812)
#define U_TO_BLUE 66126
#define FIXED_SHIFT 15
/* Half of one source pixel in the 12.20 step arithmetic, so the sample is the nearest pixel. */
#define HALF_STEP 0x80000u

/* The xemu matrix (T770, 40 of 40 flat cells exact): BT.601 studio range, integer, rounded. */
#define XEMU_LUMA_GAIN 298
#define XEMU_V_TO_RED 409
#define XEMU_U_TO_GREEN 100
#define XEMU_V_TO_GREEN 208
#define XEMU_U_TO_BLUE 516
#define XEMU_ROUND 128
#define XEMU_SHIFT 8
/* The scale's weight has 8 bits: the second texel gets `weight`, the first `256 - weight`. */
#define XEMU_WEIGHT_ONE 256

static uint8_t clamp_byte(int32_t value)
{
    return value < 0 ? 0u : (value > 255 ? 255u : (uint8_t)value);
}

void d3d8_overlay_image_yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t rgb[3])
{
    const int32_t luma = ((int32_t)y - LUMA_BIAS) * LUMA_GAIN;
    const int32_t blue_difference = (int32_t)u - CHROMA_BIAS;
    const int32_t red_difference = (int32_t)v - CHROMA_BIAS;
    rgb[0] = clamp_byte((luma + V_TO_RED * red_difference) >> FIXED_SHIFT);
    rgb[1] = clamp_byte((luma + V_TO_GREEN * red_difference + U_TO_GREEN * blue_difference) >>
                        FIXED_SHIFT);
    rgb[2] = clamp_byte((luma + U_TO_BLUE * blue_difference) >> FIXED_SHIFT);
}

void d3d8_overlay_image_yuv_to_rgb_xemu(uint8_t y, uint8_t u, uint8_t v, uint8_t rgb[3])
{
    const int32_t luma = ((int32_t)y - LUMA_BIAS) * XEMU_LUMA_GAIN + XEMU_ROUND;
    const int32_t blue_difference = (int32_t)u - CHROMA_BIAS;
    const int32_t red_difference = (int32_t)v - CHROMA_BIAS;
    rgb[0] = clamp_byte((luma + XEMU_V_TO_RED * red_difference) >> XEMU_SHIFT);
    rgb[1] = clamp_byte((luma - XEMU_V_TO_GREEN * red_difference -
                         XEMU_U_TO_GREEN * blue_difference) >> XEMU_SHIFT);
    rgb[2] = clamp_byte((luma + XEMU_U_TO_BLUE * blue_difference) >> XEMU_SHIFT);
}

size_t d3d8_overlay_image_yuy2_row_bytes(uint32_t width)
{
    return 2u * (size_t)((width + 1u) & ~1u);
}

static bool yuy2_input_valid(const uint8_t *yuy2, size_t pitch, uint32_t width, uint32_t height)
{
    return yuy2 != NULL && width != 0u && height != 0u &&
           pitch >= d3d8_overlay_image_yuy2_row_bytes(width);
}

typedef void (*yuv_converter)(uint8_t y, uint8_t u, uint8_t v, uint8_t rgb[3]);

static bool yuy2_rows_to_rgb(const uint8_t *yuy2, size_t pitch, uint32_t width, uint32_t height,
                             uint8_t *rgb, yuv_converter convert)
{
    if (!yuy2_input_valid(yuy2, pitch, width, height) || rgb == NULL) {
        return false;
    }
    for (uint32_t row = 0u; row < height; row++) {
        const uint8_t *line = yuy2 + (size_t)row * pitch;
        uint8_t *out = rgb + (size_t)row * width * 3u;
        for (uint32_t column = 0u; column < width; column++) {
            const uint8_t *pair = line + 4u * (column / 2u);
            convert(line[2u * column], pair[1], pair[3], out + 3u * column);
        }
    }
    return true;
}

bool d3d8_overlay_image_yuy2_to_rgb(const uint8_t *yuy2, size_t pitch, uint32_t width,
                                    uint32_t height, uint8_t *rgb)
{
    return yuy2_rows_to_rgb(yuy2, pitch, width, height, rgb, d3d8_overlay_image_yuv_to_rgb);
}

bool d3d8_overlay_image_yuy2_to_rgb_xemu(const uint8_t *yuy2, size_t pitch, uint32_t width,
                                         uint32_t height, uint8_t *rgb)
{
    return yuy2_rows_to_rgb(yuy2, pitch, width, height, rgb, d3d8_overlay_image_yuv_to_rgb_xemu);
}

bool d3d8_overlay_image_yuy2_to_planes(const uint8_t *yuy2, size_t pitch, uint32_t width,
                                       uint32_t height, uint8_t *y, uint8_t *u, uint8_t *v)
{
    if (!yuy2_input_valid(yuy2, pitch, width, height) || y == NULL || u == NULL || v == NULL) {
        return false;
    }
    const uint32_t chroma_width = (width + 1u) / 2u;
    for (uint32_t row = 0u; row < height; row++) {
        const uint8_t *line = yuy2 + (size_t)row * pitch;
        for (uint32_t column = 0u; column < width; column++) {
            y[(size_t)row * width + column] = line[2u * column];
        }
        for (uint32_t pair = 0u; pair < chroma_width; pair++) {
            u[(size_t)row * chroma_width + pair] = line[4u * pair + 1u];
            v[(size_t)row * chroma_width + pair] = line[4u * pair + 3u];
        }
    }
    return true;
}

bool d3d8_overlay_image_scale(const uint8_t *source, uint32_t source_width, uint32_t source_height,
                              uint32_t horizontal_step, uint32_t vertical_step, uint8_t *destination,
                              uint32_t destination_width, uint32_t destination_height)
{
    if (source == NULL || destination == NULL || source_width == 0u || source_height == 0u ||
        destination_width == 0u || destination_height == 0u) {
        return false;
    }
    for (uint32_t row = 0u; row < destination_height; row++) {
        uint64_t source_row = ((uint64_t)row * vertical_step + HALF_STEP) >> 20;
        if (source_row >= source_height) {
            source_row = source_height - 1u;
        }
        for (uint32_t column = 0u; column < destination_width; column++) {
            uint64_t source_column = ((uint64_t)column * horizontal_step + HALF_STEP) >> 20;
            if (source_column >= source_width) {
                source_column = source_width - 1u;
            }
            memcpy(destination + ((size_t)row * destination_width + column) * 3u,
                   source + ((size_t)source_row * source_width + (size_t)source_column) * 3u, 3u);
        }
    }
    return true;
}

/* One axis of the xemu scale: output pixel `index` samples texel coordinate index * source / out and the
 * filter is centred half a texel left of it, so the first texel is floor((2 * index * source - out) / (2 * out))
 * and the weight of the second is the remainder, quantised to 8 bits (round half up). Exact integers. */
static void xemu_axis(uint32_t index, uint32_t source, uint32_t out, int64_t *first,
                      uint32_t *weight)
{
    const int64_t numerator = 2 * (int64_t)index * source - (int64_t)out;
    const int64_t denominator = 2 * (int64_t)out;
    int64_t base = numerator / denominator;
    if (numerator % denominator < 0) {
        base--; /* floor, not truncation, for the texel left of the first */
    }
    *first = base;
    *weight = (uint32_t)(((numerator - base * denominator) * XEMU_WEIGHT_ONE + out) / denominator);
}

static uint32_t repeat_index(int64_t index, uint32_t size)
{
    const int64_t wrapped = index % (int64_t)size;
    return (uint32_t)(wrapped < 0 ? wrapped + (int64_t)size : wrapped);
}

static uint32_t mix_weighted(uint32_t first, uint32_t second, uint32_t weight)
{
    return (first * (XEMU_WEIGHT_ONE - weight) + second * weight + XEMU_ROUND) >> XEMU_SHIFT;
}

bool d3d8_overlay_image_scale_xemu(const uint8_t *source, uint32_t source_width,
                                   uint32_t source_height, uint8_t *destination,
                                   uint32_t destination_width, uint32_t destination_height)
{
    if (source == NULL || destination == NULL || source_width == 0u || source_height == 0u ||
        destination_width == 0u || destination_height == 0u) {
        return false;
    }
    for (uint32_t row = 0u; row <= destination_height; row++) {
        int64_t top;
        uint32_t vertical;
        xemu_axis(row, source_height, destination_height, &top, &vertical);
        const uint8_t *upper = source + (size_t)repeat_index(top, source_height) * source_width * 3u;
        const uint8_t *lower =
            source + (size_t)repeat_index(top + 1, source_height) * source_width * 3u;
        for (uint32_t column = 0u; column <= destination_width; column++) {
            int64_t left;
            uint32_t horizontal;
            xemu_axis(column, source_width, destination_width, &left, &horizontal);
            const size_t first_texel = (size_t)repeat_index(left, source_width) * 3u;
            const size_t second_texel = (size_t)repeat_index(left + 1, source_width) * 3u;
            uint8_t *out = destination + ((size_t)row * (destination_width + 1u) + column) * 3u;
            for (size_t channel = 0u; channel < 3u; channel++) {
                const uint32_t above = mix_weighted(upper[first_texel + channel],
                                                    upper[second_texel + channel], horizontal);
                const uint32_t below = mix_weighted(lower[first_texel + channel],
                                                    lower[second_texel + channel], horizontal);
                out[channel] = (uint8_t)mix_weighted(above, below, vertical);
            }
        }
    }
    return true;
}

/* --- PNG ---------------------------------------------------------------------------------- */

uint32_t d3d8_overlay_image_crc32(uint32_t crc, const uint8_t *data, size_t length)
{
    crc = ~crc;
    for (size_t index = 0u; index < length; index++) {
        crc ^= data[index];
        for (unsigned bit = 0u; bit < 8u; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

uint32_t d3d8_overlay_image_adler32(uint32_t adler, const uint8_t *data, size_t length)
{
    uint32_t low = adler & 0xFFFFu;
    uint32_t high = adler >> 16;
    for (size_t index = 0u; index < length; index++) {
        low = (low + data[index]) % 65521u;
        high = (high + low) % 65521u;
    }
    return (high << 16) | low;
}

static void put_u32_be(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

/* One chunk: length, type, data, CRC over type and data. */
static bool write_chunk(FILE *file, const char type[4], const uint8_t *data, uint32_t length)
{
    uint8_t head[8];
    uint8_t tail[4];
    put_u32_be(head, length);
    memcpy(head + 4, type, 4);
    uint32_t crc = d3d8_overlay_image_crc32(0u, (const uint8_t *)type, 4u);
    crc = d3d8_overlay_image_crc32(crc, data, length);
    put_u32_be(tail, crc);
    return fwrite(head, 1u, sizeof(head), file) == sizeof(head) &&
           (length == 0u || fwrite(data, 1u, length, file) == length) &&
           fwrite(tail, 1u, sizeof(tail), file) == sizeof(tail);
}

#define STORED_BLOCK_MAX 65535u

bool d3d8_overlay_image_write_png(const char *path, const uint8_t *rgb, uint32_t width,
                                  uint32_t height)
{
    if (path == NULL || rgb == NULL || width == 0u || height == 0u || width > 0x10000u ||
        height > 0x10000u) {
        return false;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        return false;
    }
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t header[13];
    put_u32_be(header, width);
    put_u32_be(header + 4, height);
    header[8] = 8u;  /* bit depth */
    header[9] = 2u;  /* colour type: RGB */
    header[10] = 0u; /* deflate */
    header[11] = 0u; /* filter method 0 */
    header[12] = 0u; /* no interlace */
    bool good = fwrite(signature, 1u, sizeof(signature), file) == sizeof(signature) &&
                write_chunk(file, "IHDR", header, sizeof(header));

    /* The raw stream is one filter byte (0, none) before each row. It is written as a zlib stream
     * of stored blocks, so the IDAT payload is known up front and the chunk CRC can run over it
     * in pieces. */
    const size_t row_bytes = 1u + (size_t)width * 3u;
    const uint64_t raw_total = (uint64_t)row_bytes * height;
    const uint64_t blocks = (raw_total + STORED_BLOCK_MAX - 1u) / STORED_BLOCK_MAX;
    const uint64_t idat_total = 2u + raw_total + 5u * blocks + 4u;
    if (!good || idat_total > 0x7FFFFFFFu) {
        (void)fclose(file);
        return false;
    }
    uint8_t length_and_type[8];
    put_u32_be(length_and_type, (uint32_t)idat_total);
    memcpy(length_and_type + 4, "IDAT", 4);
    uint32_t crc = d3d8_overlay_image_crc32(0u, (const uint8_t *)"IDAT", 4u);
    uint32_t adler = 1u;
    good = fwrite(length_and_type, 1u, sizeof(length_and_type), file) == sizeof(length_and_type);
    const uint8_t zlib_header[2] = {0x78u, 0x01u};
    good = good && fwrite(zlib_header, 1u, sizeof(zlib_header), file) == sizeof(zlib_header);
    crc = d3d8_overlay_image_crc32(crc, zlib_header, sizeof(zlib_header));

    uint8_t block[STORED_BLOCK_MAX + 5u];
    uint64_t row = 0u;
    size_t column_byte = 0u; /* position in the current raw row, 0 is the filter byte */
    uint64_t remaining = raw_total;
    while (good && remaining != 0u) {
        const uint32_t length = remaining > STORED_BLOCK_MAX ? STORED_BLOCK_MAX : (uint32_t)remaining;
        block[0] = remaining == length ? 1u : 0u;
        block[1] = (uint8_t)length;
        block[2] = (uint8_t)(length >> 8);
        block[3] = (uint8_t)~length;
        block[4] = (uint8_t)(~length >> 8);
        for (uint32_t index = 0u; index < length; index++) {
            if (column_byte == 0u) {
                block[5u + index] = 0u;
            } else {
                block[5u + index] = rgb[row * (size_t)width * 3u + column_byte - 1u];
            }
            if (++column_byte == row_bytes) {
                column_byte = 0u;
                row++;
            }
        }
        adler = d3d8_overlay_image_adler32(adler, block + 5, length);
        crc = d3d8_overlay_image_crc32(crc, block, length + 5u);
        good = fwrite(block, 1u, length + 5u, file) == length + 5u;
        remaining -= length;
    }
    uint8_t adler_bytes[4];
    put_u32_be(adler_bytes, adler);
    crc = d3d8_overlay_image_crc32(crc, adler_bytes, sizeof(adler_bytes));
    uint8_t crc_bytes[4];
    put_u32_be(crc_bytes, crc);
    good = good && fwrite(adler_bytes, 1u, sizeof(adler_bytes), file) == sizeof(adler_bytes) &&
           fwrite(crc_bytes, 1u, sizeof(crc_bytes), file) == sizeof(crc_bytes) &&
           write_chunk(file, "IEND", NULL, 0u);
    return fclose(file) == 0 && good;
}
