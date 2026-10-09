/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * Minimal PNG encoder with zero external dependencies. This exists purely so
 * the Vulkan renderer can dump an offscreen framebuffer somewhere a human can
 * look at it, so it deliberately skips real DEFLATE compression and emits
 * "stored" (uncompressed) deflate blocks instead -- still a legal zlib
 * stream, just a larger file.
 */

#include "gpu_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Largest payload a single stored deflate block may carry -- the block's LEN
 * field is a 16-bit count, so 65535 is the hard ceiling. */
#define GPU_PNG_STORED_BLOCK_MAX 65535u

static void write_u32_be(uint8_t destination[4], uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)(value);
}

static void write_u16_le(uint8_t destination[2], uint16_t value)
{
    destination[0] = (uint8_t)(value & 0xFFu);
    destination[1] = (uint8_t)(value >> 8);
}

/* Standard reflected CRC-32 (polynomial 0xEDB88320), the one PNG and zlib
 * both use. `crc` is the running value with no pre/post inversion applied --
 * callers that need the final chunk CRC invert before and after, see
 * crc32_of() below. */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 1u) != 0u) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc = crc >> 1;
            }
        }
    }
    return crc;
}

static uint32_t crc32_of(const uint8_t *type, const uint8_t *data, uint32_t data_length)
{
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, type, 4);
    if (data_length > 0u) {
        crc = crc32_update(crc, data, data_length);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Adler-32 as used by zlib. Sums are reduced modulo 65521 every NMAX bytes
 * (5552) rather than after every byte, which is the standard trick to avoid
 * needing to detect overflow on every addition. */
static void adler32_accumulate(uint32_t *low, uint32_t *high, const uint8_t *data, size_t length)
{
    const uint32_t modulus = 65521u;
    const size_t max_block = 5552u;
    size_t offset = 0;
    while (offset < length) {
        size_t remaining = length - offset;
        size_t block = remaining < max_block ? remaining : max_block;
        for (size_t i = 0; i < block; i++) {
            *low += data[offset + i];
            *high += *low;
        }
        *low %= modulus;
        *high %= modulus;
        offset += block;
    }
}

static bool write_bytes(FILE *file, const void *data, size_t length)
{
    if (length == 0u) {
        return true;
    }
    return fwrite(data, 1, length, file) == length;
}

static bool write_chunk(FILE *file, const char type[4], const uint8_t *data, uint32_t data_length)
{
    uint8_t length_bytes[4];
    write_u32_be(length_bytes, data_length);
    if (!write_bytes(file, length_bytes, sizeof length_bytes)) {
        return false;
    }
    if (!write_bytes(file, type, 4)) {
        return false;
    }
    if (data_length > 0u && !write_bytes(file, data, data_length)) {
        return false;
    }

    uint32_t crc = crc32_of((const uint8_t *)type, data, data_length);
    uint8_t crc_bytes[4];
    write_u32_be(crc_bytes, crc);
    return write_bytes(file, crc_bytes, sizeof crc_bytes);
}

/* Builds the full zlib stream (header + stored deflate blocks + Adler-32) for
 * the filtered scanlines, without ever materialising the filtered scanlines
 * as a separate buffer. Each row is "None"-filtered (a single 0x00 byte)
 * followed by `row_bytes` pixel bytes copied out of the caller's framebuffer,
 * respecting `stride_bytes` so padding between rows is never read.
 *
 * Returns the heap-allocated zlib stream and its length via *out_length, or
 * NULL on allocation failure or if the result would not fit a chunk length
 * field.
 */
static uint8_t *build_idat_payload(const uint8_t *pixels, uint32_t width, uint32_t height,
                                   uint32_t stride_bytes, size_t *out_length)
{
    uint64_t row_bytes64 = (uint64_t)width * 4u;
    uint64_t filtered_row_bytes64 = row_bytes64 + 1u;
    uint64_t total_filtered_size64 = filtered_row_bytes64 * (uint64_t)height;

    uint64_t block_count64 = (total_filtered_size64 + (GPU_PNG_STORED_BLOCK_MAX - 1u))
                              / GPU_PNG_STORED_BLOCK_MAX;
    if (block_count64 == 0u) {
        block_count64 = 1u; /* a zero-length image still needs one empty block */
    }

    uint64_t idat_size64 = 2u + block_count64 * 5u + total_filtered_size64 + 4u;
    if (idat_size64 > (uint64_t)SIZE_MAX || idat_size64 > (uint64_t)UINT32_MAX) {
        return NULL;
    }

    size_t idat_size = (size_t)idat_size64;
    size_t row_bytes = (size_t)row_bytes64;
    size_t filtered_row_bytes = (size_t)filtered_row_bytes64;
    uint64_t remaining = total_filtered_size64;

    uint8_t *payload = malloc(idat_size);
    if (payload == NULL) {
        return NULL;
    }

    size_t offset = 0;
    payload[offset++] = 0x78u;
    payload[offset++] = 0x01u;

    uint32_t adler_low = 1u;
    uint32_t adler_high = 0u;

    uint32_t current_row = 0;
    size_t pos_in_filtered_row = 0; /* 0 means the next byte is the filter byte */

    while (remaining > 0u) {
        uint32_t block_length = remaining < (uint64_t)GPU_PNG_STORED_BLOCK_MAX
                                     ? (uint32_t)remaining
                                     : GPU_PNG_STORED_BLOCK_MAX;
        bool is_last = (uint64_t)block_length == remaining;

        payload[offset++] = (uint8_t)(is_last ? 1u : 0u);
        uint8_t length_le[2];
        write_u16_le(length_le, (uint16_t)block_length);
        payload[offset++] = length_le[0];
        payload[offset++] = length_le[1];
        uint16_t complement = (uint16_t)(~(uint16_t)block_length);
        uint8_t complement_le[2];
        write_u16_le(complement_le, complement);
        payload[offset++] = complement_le[0];
        payload[offset++] = complement_le[1];

        uint8_t *block_start = payload + offset;
        size_t filled = 0;
        while (filled < block_length) {
            if (pos_in_filtered_row == 0u) {
                payload[offset + filled] = 0x00u; /* filter type None */
                filled++;
                pos_in_filtered_row = 1u;
                continue;
            }
            const uint8_t *source_row = pixels + (size_t)current_row * stride_bytes;
            size_t pixel_offset = pos_in_filtered_row - 1u;
            size_t row_remaining = row_bytes - pixel_offset;
            size_t block_remaining = (size_t)block_length - filled;
            size_t copy_length = row_remaining < block_remaining ? row_remaining : block_remaining;
            memcpy(payload + offset + filled, source_row + pixel_offset, copy_length);
            filled += copy_length;
            pos_in_filtered_row += copy_length;
            if (pos_in_filtered_row == filtered_row_bytes) {
                pos_in_filtered_row = 0u;
                current_row++;
            }
        }

        adler32_accumulate(&adler_low, &adler_high, block_start, block_length);

        offset += block_length;
        remaining -= block_length;
    }

    uint32_t adler_value = (adler_high << 16) | adler_low;
    write_u32_be(payload + offset, adler_value);
    offset += 4u;

    *out_length = offset;
    return payload;
}

bool gpu_png_write_rgba(const char *path, const uint8_t *pixels,
                        uint32_t width, uint32_t height, uint32_t stride_bytes)
{
    if (path == NULL || pixels == NULL || width == 0u || height == 0u) {
        return false;
    }

    uint64_t row_bytes64 = (uint64_t)width * 4u;
    if ((uint64_t)stride_bytes < row_bytes64) {
        return false;
    }

    size_t idat_length = 0;
    uint8_t *idat_payload = build_idat_payload(pixels, width, height, stride_bytes, &idat_length);
    if (idat_payload == NULL) {
        return false;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        free(idat_payload);
        return false;
    }

    bool ok = true;

    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    ok = ok && write_bytes(file, signature, sizeof signature);

    uint8_t ihdr_data[13];
    write_u32_be(ihdr_data + 0, width);
    write_u32_be(ihdr_data + 4, height);
    ihdr_data[8] = 8u;  /* bit depth */
    ihdr_data[9] = 6u;  /* colour type: RGBA */
    ihdr_data[10] = 0u; /* compression method */
    ihdr_data[11] = 0u; /* filter method */
    ihdr_data[12] = 0u; /* interlace method */
    ok = ok && write_chunk(file, "IHDR", ihdr_data, (uint32_t)sizeof ihdr_data);

    ok = ok && write_chunk(file, "IDAT", idat_payload, (uint32_t)idat_length);
    ok = ok && write_chunk(file, "IEND", NULL, 0u);

    free(idat_payload);

    if (fclose(file) != 0) {
        ok = false;
    }

    return ok;
}
