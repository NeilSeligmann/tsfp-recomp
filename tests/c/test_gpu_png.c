/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Tests for the dependency-free PNG writer. Test 2 independently re-implements
 * zlib stored-block inflation and Adler-32 so a bug shared between writer and
 * checker cannot hide the real defect.
 */

#include "gpu_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                      \
    } while (0)

/* ---- Independent CRC-32 / Adler-32 / chunk parsing, kept separate from the
 * implementation under test on purpose. ---- */

static uint32_t reference_crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;
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
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t reference_adler32(const uint8_t *data, size_t length)
{
    const uint32_t modulus = 65521u;
    uint32_t low = 1u;
    uint32_t high = 0u;
    size_t offset = 0;
    while (offset < length) {
        size_t remaining = length - offset;
        size_t block = remaining < 5552u ? remaining : 5552u;
        for (size_t i = 0; i < block; i++) {
            low += data[offset + i];
            high += low;
        }
        low %= modulus;
        high %= modulus;
        offset += block;
    }
    return (high << 16) | low;
}

static uint32_t read_u32_be(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static uint16_t read_u16_le(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | (uint16_t)((uint16_t)data[1] << 8));
}

/* Whole file slurped into memory, plus the parsed-out location of each chunk
 * the tests care about. */
struct loaded_png {
    uint8_t *bytes;
    size_t length;
};

static bool load_file(const char *path, struct loaded_png *out_file)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long signed_length = ftell(file);
    if (signed_length < 0) {
        fclose(file);
        return false;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    size_t length = (size_t)signed_length;
    uint8_t *bytes = malloc(length > 0u ? length : 1u);
    if (bytes == NULL) {
        fclose(file);
        return false;
    }
    if (length > 0u && fread(bytes, 1, length, file) != length) {
        free(bytes);
        fclose(file);
        return false;
    }
    fclose(file);
    out_file->bytes = bytes;
    out_file->length = length;
    return true;
}

/* A single parsed chunk: type, data pointer into the file buffer, data
 * length, and the CRC the file claims for it. */
struct parsed_chunk {
    char type[5];
    const uint8_t *data;
    uint32_t data_length;
    uint32_t stored_crc;
};

/* Finds the n-th chunk (0-indexed) after the 8-byte signature. Returns false
 * if the file is truncated or there are fewer than n+1 chunks. */
static bool parse_chunk(const struct loaded_png *file, size_t chunk_index,
                        struct parsed_chunk *out_chunk)
{
    size_t offset = 8; /* skip the signature */
    for (size_t current = 0;; current++) {
        if (offset + 8 > file->length) {
            return false;
        }
        uint32_t data_length = read_u32_be(file->bytes + offset);
        const uint8_t *type_pointer = file->bytes + offset + 4;
        size_t data_offset = offset + 8;
        if (data_offset + (size_t)data_length + 4 > file->length) {
            return false;
        }
        const uint8_t *data_pointer = file->bytes + data_offset;
        uint32_t stored_crc = read_u32_be(data_pointer + data_length);

        if (current == chunk_index) {
            memcpy(out_chunk->type, type_pointer, 4);
            out_chunk->type[4] = '\0';
            out_chunk->data = data_pointer;
            out_chunk->data_length = data_length;
            out_chunk->stored_crc = stored_crc;
            return true;
        }

        offset = data_offset + (size_t)data_length + 4;
    }
}

static uint32_t crc_of_chunk(const struct parsed_chunk *chunk)
{
    uint8_t *combined = malloc((size_t)chunk->data_length + 4u);
    memcpy(combined, chunk->type, 4);
    if (chunk->data_length > 0u) {
        memcpy(combined + 4, chunk->data, chunk->data_length);
    }
    uint32_t crc = reference_crc32(combined, (size_t)chunk->data_length + 4u);
    free(combined);
    return crc;
}

/* Independently inflates the stored-block zlib stream found in an IDAT
 * chunk's data. Returns the decoded bytes (caller frees) and the Adler-32
 * recorded at the end of the stream, or NULL on a malformed stream. */
static uint8_t *inflate_stored(const uint8_t *zlib_data, size_t zlib_length,
                               size_t *out_decoded_length, uint32_t *out_trailer_adler)
{
    if (zlib_length < 2u + 4u) {
        return NULL;
    }
    if (zlib_data[0] != 0x78u || zlib_data[1] != 0x01u) {
        return NULL;
    }

    size_t capacity = zlib_length; /* stored blocks can only shrink the data, never grow it */
    uint8_t *decoded = malloc(capacity > 0u ? capacity : 1u);
    size_t decoded_length = 0;

    size_t offset = 2;
    bool saw_final = false;
    while (offset + 5 <= zlib_length - 4u) {
        uint8_t block_header = zlib_data[offset];
        uint16_t block_length = read_u16_le(zlib_data + offset + 1);
        uint16_t block_length_complement = read_u16_le(zlib_data + offset + 3);
        if ((uint16_t)(~block_length) != block_length_complement) {
            free(decoded);
            return NULL;
        }
        offset += 5;
        if (offset + block_length > zlib_length - 4u) {
            free(decoded);
            return NULL;
        }
        memcpy(decoded + decoded_length, zlib_data + offset, block_length);
        decoded_length += block_length;
        offset += block_length;

        if ((block_header & 1u) != 0u) {
            saw_final = true;
            break;
        }
    }

    if (!saw_final || offset != zlib_length - 4u) {
        free(decoded);
        return NULL;
    }

    *out_trailer_adler = read_u32_be(zlib_data + offset);
    *out_decoded_length = decoded_length;
    return decoded;
}

/* Strips the per-scanline filter byte (must be 0, "None") and returns the raw
 * RGBA bytes, which should be exactly width*4*height long. */
static uint8_t *strip_filter_bytes(const uint8_t *filtered, size_t filtered_length,
                                   uint32_t width, uint32_t height)
{
    size_t row_bytes = (size_t)width * 4u;
    size_t filtered_row_bytes = row_bytes + 1u;
    if (filtered_length != filtered_row_bytes * (size_t)height) {
        return NULL;
    }
    uint8_t *raw = malloc(row_bytes * (size_t)height);
    for (uint32_t row = 0; row < height; row++) {
        const uint8_t *filtered_row = filtered + (size_t)row * filtered_row_bytes;
        if (filtered_row[0] != 0u) {
            free(raw);
            return NULL; /* only filter type None is produced by this writer */
        }
        memcpy(raw + (size_t)row * row_bytes, filtered_row + 1, row_bytes);
    }
    return raw;
}

/* ---- Test fixtures ---- */

static uint8_t *make_test_pixels(uint32_t width, uint32_t height)
{
    uint8_t *pixels = malloc((size_t)width * 4u * (size_t)height);
    for (uint32_t row = 0; row < height; row++) {
        for (uint32_t column = 0; column < width; column++) {
            size_t index = ((size_t)row * width + column) * 4u;
            pixels[index + 0] = (uint8_t)((row * 13u + column * 7u) & 0xFFu);
            pixels[index + 1] = (uint8_t)((row * 29u + column * 3u) & 0xFFu);
            pixels[index + 2] = (uint8_t)((row * 5u + column * 17u) & 0xFFu);
            pixels[index + 3] = (uint8_t)(0xFFu - ((row + column) & 0xFFu));
        }
    }
    return pixels;
}

static void test_round_trip_header_and_crcs(void)
{
    printf("test_round_trip_header_and_crcs\n");
    const uint8_t pixels[2 * 2 * 4] = {
        255, 0, 0, 255, 0, 255, 0, 255,
        0, 0, 255, 255, 255, 255, 255, 128,
    };
    const char *path = "/tmp/test_gpu_png_2x2.png";

    CHECK(gpu_png_write_rgba(path, pixels, 2, 2, 2 * 4));

    struct loaded_png file;
    CHECK(load_file(path, &file));
    if (file.bytes == NULL) {
        return;
    }

    static const uint8_t expected_signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(file.length >= 8 && memcmp(file.bytes, expected_signature, 8) == 0);

    struct parsed_chunk ihdr;
    CHECK(parse_chunk(&file, 0, &ihdr));
    CHECK(memcmp(ihdr.type, "IHDR", 4) == 0);
    CHECK(ihdr.data_length == 13u);
    CHECK(read_u32_be(ihdr.data + 0) == 2u);  /* width */
    CHECK(read_u32_be(ihdr.data + 4) == 2u);  /* height */
    CHECK(ihdr.data[8] == 8u);                /* bit depth */
    CHECK(ihdr.data[9] == 6u);                /* colour type RGBA */
    CHECK(ihdr.data[10] == 0u);               /* compression */
    CHECK(ihdr.data[11] == 0u);               /* filter method */
    CHECK(ihdr.data[12] == 0u);               /* interlace */
    CHECK(crc_of_chunk(&ihdr) == ihdr.stored_crc);

    struct parsed_chunk idat;
    CHECK(parse_chunk(&file, 1, &idat));
    CHECK(memcmp(idat.type, "IDAT", 4) == 0);
    CHECK(crc_of_chunk(&idat) == idat.stored_crc);

    struct parsed_chunk iend;
    CHECK(parse_chunk(&file, 2, &iend));
    CHECK(memcmp(iend.type, "IEND", 4) == 0);
    CHECK(iend.data_length == 0u);
    CHECK(crc_of_chunk(&iend) == iend.stored_crc);

    free(file.bytes);
}

static void test_idat_decodes_to_exact_pixels(void)
{
    printf("test_idat_decodes_to_exact_pixels\n");
    uint8_t pixels[3 * 3 * 4];
    for (size_t i = 0; i < sizeof pixels; i++) {
        pixels[i] = (uint8_t)(i * 31u + 7u);
    }
    const char *path = "/tmp/test_gpu_png_decode.png";

    CHECK(gpu_png_write_rgba(path, pixels, 3, 3, 3 * 4));

    struct loaded_png file;
    CHECK(load_file(path, &file));
    if (file.bytes == NULL) {
        return;
    }

    struct parsed_chunk idat;
    CHECK(parse_chunk(&file, 1, &idat));

    size_t decoded_length = 0;
    uint32_t trailer_adler = 0;
    uint8_t *decoded = inflate_stored(idat.data, idat.data_length, &decoded_length, &trailer_adler);
    CHECK(decoded != NULL);
    if (decoded == NULL) {
        free(file.bytes);
        return;
    }

    CHECK(trailer_adler == reference_adler32(decoded, decoded_length));

    uint8_t *raw = strip_filter_bytes(decoded, decoded_length, 3, 3);
    CHECK(raw != NULL);
    if (raw != NULL) {
        CHECK(memcmp(raw, pixels, sizeof pixels) == 0);
        free(raw);
    }

    free(decoded);
    free(file.bytes);
}

static void test_stride_padding_is_not_written(void)
{
    printf("test_stride_padding_is_not_written\n");
    const uint32_t width = 3;
    const uint32_t height = 2;
    const uint32_t stride = 64;
    uint8_t *padded = malloc((size_t)stride * height);
    memset(padded, 0xAA, (size_t)stride * height); /* junk padding, must never appear in output */

    uint8_t *expected = malloc((size_t)width * 4u * height);
    for (uint32_t row = 0; row < height; row++) {
        for (uint32_t column = 0; column < width; column++) {
            size_t padded_index = (size_t)row * stride + (size_t)column * 4u;
            size_t expected_index = ((size_t)row * width + column) * 4u;
            uint8_t red = (uint8_t)(row * 50u + column * 10u);
            uint8_t green = (uint8_t)(row * 20u + column * 40u);
            uint8_t blue = (uint8_t)(row + column);
            uint8_t alpha = 200u;
            padded[padded_index + 0] = red;
            padded[padded_index + 1] = green;
            padded[padded_index + 2] = blue;
            padded[padded_index + 3] = alpha;
            expected[expected_index + 0] = red;
            expected[expected_index + 1] = green;
            expected[expected_index + 2] = blue;
            expected[expected_index + 3] = alpha;
        }
    }

    const char *path = "/tmp/test_gpu_png_stride.png";
    CHECK(gpu_png_write_rgba(path, padded, width, height, stride));

    struct loaded_png file;
    CHECK(load_file(path, &file));
    if (file.bytes != NULL) {
        struct parsed_chunk idat;
        CHECK(parse_chunk(&file, 1, &idat));

        size_t decoded_length = 0;
        uint32_t trailer_adler = 0;
        uint8_t *decoded = inflate_stored(idat.data, idat.data_length, &decoded_length, &trailer_adler);
        CHECK(decoded != NULL);
        if (decoded != NULL) {
            uint8_t *raw = strip_filter_bytes(decoded, decoded_length, width, height);
            CHECK(raw != NULL);
            if (raw != NULL) {
                CHECK(memcmp(raw, expected, (size_t)width * 4u * height) == 0);
                free(raw);
            }
            free(decoded);
        }
        free(file.bytes);
    }

    free(padded);
    free(expected);
}

static void test_multi_block_image_round_trips(void)
{
    printf("test_multi_block_image_round_trips\n");
    /* 256x128 RGBA is 131072 bytes of pixel data plus 128 filter bytes, i.e.
     * 131200 bytes of filtered payload -- more than one 65535-byte stored
     * block, which is exactly what this test exercises. */
    const uint32_t width = 256;
    const uint32_t height = 128;
    uint8_t *pixels = make_test_pixels(width, height);
    const char *path = "/tmp/test_gpu_png_multiblock.png";

    CHECK(gpu_png_write_rgba(path, pixels, width, height, width * 4u));

    struct loaded_png file;
    CHECK(load_file(path, &file));
    if (file.bytes != NULL) {
        struct parsed_chunk idat;
        CHECK(parse_chunk(&file, 1, &idat));
        CHECK(crc_of_chunk(&idat) == idat.stored_crc);

        size_t decoded_length = 0;
        uint32_t trailer_adler = 0;
        uint8_t *decoded = inflate_stored(idat.data, idat.data_length, &decoded_length, &trailer_adler);
        CHECK(decoded != NULL);
        if (decoded != NULL) {
            CHECK(trailer_adler == reference_adler32(decoded, decoded_length));
            uint8_t *raw = strip_filter_bytes(decoded, decoded_length, width, height);
            CHECK(raw != NULL);
            if (raw != NULL) {
                CHECK(memcmp(raw, pixels, (size_t)width * 4u * height) == 0);
                free(raw);
            }
            free(decoded);
        }
        free(file.bytes);
    }

    free(pixels);
}

static void test_argument_validation_rejects_bad_input(void)
{
    printf("test_argument_validation_rejects_bad_input\n");
    uint8_t pixels[4 * 4] = {0};

    CHECK(!gpu_png_write_rgba(NULL, pixels, 1, 1, 4));
    CHECK(!gpu_png_write_rgba("/tmp/test_gpu_png_null_pixels.png", NULL, 1, 1, 4));
    CHECK(!gpu_png_write_rgba("/tmp/test_gpu_png_zero_width.png", pixels, 0, 1, 4));
    CHECK(!gpu_png_write_rgba("/tmp/test_gpu_png_zero_height.png", pixels, 1, 0, 4));
    CHECK(!gpu_png_write_rgba("/tmp/test_gpu_png_bad_stride.png", pixels, 2, 1, 4)); /* needs 8 */
}

int main(void)
{
    printf("gpu_png tests\n");
    test_round_trip_header_and_crcs();
    test_idat_decodes_to_exact_pixels();
    test_stride_padding_is_not_written();
    test_multi_block_image_round_trips();
    test_argument_validation_rejects_bad_input();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
