/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T792: live texture decode, upload plan, invalidation, targets and refusal census. Device-free.
 * Optional argv[1]: write a decoded row-major DXT1 picture as PNG (the "look at it" evidence). */
#include "gpu_png.h"
#include "live_texture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            (void)fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

#define FORMAT(color, exponent_u, exponent_v) \
    (((uint32_t)(exponent_v) << 24) | ((uint32_t)(exponent_u) << 20) | 0x10000u | ((uint32_t)(color) << 8) | 0x29u)
#define LINEAR_SIZE(width, height, pitch) \
    ((uint32_t)((width) - 1u) | ((uint32_t)((height) - 1u) << 12) | ((uint32_t)((pitch) / 64u - 1u) << 24))

/* Independent DXT1 colour oracle. T950 corrects the historical replay's
 * Morton block order to xemu's row-major compressed storage. */
static void original_expand_565(uint16_t color, uint8_t out[4])
{
    const uint32_t red = (color >> 11) & 31u, green = (color >> 5) & 63u, blue = color & 31u;
    out[0] = (uint8_t)((red << 3) | (red >> 2));
    out[1] = (uint8_t)((green << 2) | (green >> 4));
    out[2] = (uint8_t)((blue << 3) | (blue >> 2));
    out[3] = 255u;
}

static void original_decode_dxt1(const uint8_t *dxt1_blocks, uint32_t size, uint8_t *rgba)
{
    for (uint32_t block_y = 0u; block_y < size / 4u; block_y++) {
        for (uint32_t block_x = 0u; block_x < size / 4u; block_x++) {
            const uint8_t *block = &dxt1_blocks[(size_t)(block_y * (size / 4u) + block_x) * 8u];
            const uint16_t color0 = (uint16_t)(block[0] | (block[1] << 8));
            const uint16_t color1 = (uint16_t)(block[2] | (block[3] << 8));
            uint8_t palette[4][4];
            original_expand_565(color0, palette[0]);
            original_expand_565(color1, palette[1]);
            for (uint32_t channel = 0u; channel < 3u; channel++) {
                if (color0 > color1) {
                    palette[2][channel] = (uint8_t)((2u * palette[0][channel] + palette[1][channel]) / 3u);
                    palette[3][channel] = (uint8_t)((palette[0][channel] + 2u * palette[1][channel]) / 3u);
                } else {
                    palette[2][channel] = (uint8_t)((palette[0][channel] + palette[1][channel]) / 2u);
                    palette[3][channel] = 0u;
                }
            }
            palette[2][3] = 255u;
            palette[3][3] = color0 > color1 ? 255u : 0u;
            for (uint32_t row = 0u; row < 4u; row++) {
                for (uint32_t column = 0u; column < 4u; column++) {
                    const uint32_t pick = (block[4u + row] >> (2u * column)) & 3u;
                    memcpy(&rgba[((size_t)(block_y * 4u + row) * size + block_x * 4u + column) * 4u], palette[pick], 4u);
                }
            }
        }
    }
}

static uint32_t random_state = 0x12345678u;
static uint8_t next_byte(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return (uint8_t)(random_state >> 24);
}

static uint8_t guest_memory[1u << 20];
static unsigned reads;
static size_t last_read_bytes;
static bool read_guest(void *context, uint32_t address, void *out, size_t bytes)
{
    (void)context;
    if ((uint64_t)address + bytes > sizeof guest_memory) {
        return false;
    }
    memcpy(out, &guest_memory[address], bytes);
    last_read_bytes = bytes;
    reads++;
    return true;
}

static void test_dxt1_matches_original(void)
{
    for (uint32_t size = 4u; size <= 512u; size *= 2u) {
        const size_t bytes = (size_t)(size / 4u) * (size / 4u) * 8u;
        uint8_t *blocks = malloc(bytes);
        uint8_t *expected = malloc((size_t)size * size * 4u);
        uint8_t *actual = malloc((size_t)size * size * 4u);
        CHECK(blocks != NULL && expected != NULL && actual != NULL);
        for (size_t index = 0u; index < bytes; index++) {
            blocks[index] = next_byte();
        }
        original_decode_dxt1(blocks, size, expected);
        live_texture_decode_dxt1_square(blocks, size, actual);
        CHECK(memcmp(expected, actual, (size_t)size * size * 4u) == 0);
        /* the general planner path gives the same bytes for the measured title format */
        live_texture_binding binding = {0u, FORMAT(0x0C, __builtin_ctz(size), __builtin_ctz(size)), 0u, 0x1000u, 0u};
        live_texture_plan plan;
        CHECK(live_texture_plan_binding(&binding, false, &plan));
        CHECK(plan.width == size && plan.height == size && plan.compressed && plan.swizzled && plan.measured);
        CHECK(plan.source_bytes == bytes);
        memset(actual, 0, (size_t)size * size * 4u);
        CHECK(live_texture_decode(&plan, blocks, bytes, actual));
        CHECK(memcmp(expected, actual, (size_t)size * size * 4u) == 0);
        CHECK(!live_texture_decode(&plan, blocks, bytes - 1u, actual));
        free(blocks);
        free(expected);
        free(actual);
    }
    live_texture_binding title = {0u, 0x09910C29u, 0u, 0u, 0u};
    live_texture_plan plan;
    CHECK(live_texture_plan_binding(&title, false, &plan) && plan.width == 512u && plan.source_bytes == 128u * 128u * 8u);
}

/* Literal block colours independently expose row-major versus Morton order.
 * Both 16x8 and 8x16 ensure the second row and unequal axis lengths differ. */
static void test_rectangular_compressed_order(void)
{
    const uint16_t colors[8] = {0xF800,0x07E0,0x001F,0xFFFF,0xFFE0,0xF81F,0x07FF,0};
    const uint8_t rgb[8][3] = {{255,0,0},{0,255,0},{0,0,255},{255,255,255},
                              {255,255,0},{255,0,255},{0,255,255},{0,0,0}};
    const uint32_t formats[3] = {0x0C,0x0E,0x0F};
    for (unsigned format = 0; format < 3; format++) {
        for (unsigned tall = 0; tall < 2; tall++) {
            const uint32_t width = tall ? 8u : 16u, height = tall ? 16u : 8u;
            const uint32_t bytes = format ? 16u : 8u;
            uint8_t source[128] = {0}, rgba[512];
            for (unsigned block = 0; block < 8; block++) {
                uint8_t *b = source + block * bytes;
                if (format == 1) memset(b, 0xFF, 8);
                if (format == 2) { b[0] = 255; b[1] = 0; }
                uint8_t *color = b + (format ? 8u : 0u);
                color[0] = colors[block] & 255u; color[1] = colors[block] >> 8;
            }
            live_texture_binding binding = {0u, FORMAT(formats[format], tall ? 3 : 4, tall ? 4 : 3),0u,0x1000u, 0u};
            live_texture_plan plan;
            CHECK(live_texture_plan_binding(&binding,true,&plan));
            CHECK(plan.source_bytes == 8u*bytes);
            CHECK(!live_texture_decode(&plan,source,plan.source_bytes-1u,rgba));
            CHECK(live_texture_decode(&plan,source,plan.source_bytes,rgba));
            for (unsigned y=0;y<height;y++) for(unsigned x=0;x<width;x++) {
                unsigned block=(y/4u)*(width/4u)+x/4u;
                const uint8_t *pixel=rgba+(y*width+x)*4u;
                CHECK(memcmp(pixel,rgb[block],3)==0 && pixel[3]==255);
            }
        }
    }
}

static void test_swizzle_offset(void)
{
    /* hand values: 4x4 Morton order, then a 8x2 grid (shared 1 bit, x bits above) and a 2x8 grid (y bits above) */
    CHECK(live_texture_swizzle_offset(1u, 0u, 4u, 4u) == 1u);
    CHECK(live_texture_swizzle_offset(0u, 1u, 4u, 4u) == 2u);
    CHECK(live_texture_swizzle_offset(3u, 3u, 4u, 4u) == 15u);
    CHECK(live_texture_swizzle_offset(2u, 0u, 4u, 4u) == 4u);
    CHECK(live_texture_swizzle_offset(2u, 1u, 8u, 2u) == 6u);
    CHECK(live_texture_swizzle_offset(7u, 1u, 8u, 2u) == 15u);
    CHECK(live_texture_swizzle_offset(1u, 7u, 2u, 8u) == 15u);
    CHECK(live_texture_swizzle_offset(0u, 2u, 2u, 8u) == 4u);
    CHECK(live_texture_swizzle_offset(5u, 0u, 8u, 1u) == 5u);
    /* a bijection on 16x4 */
    bool seen[64] = {false};
    for (uint32_t y = 0u; y < 4u; y++) {
        for (uint32_t x = 0u; x < 16u; x++) {
            const uint32_t offset = live_texture_swizzle_offset(x, y, 16u, 4u);
            CHECK(offset < 64u && !seen[offset]);
            seen[offset] = true;
        }
    }
}

/* naive encoder (independent of the decoder): texel (x, y) of a W x H swizzled image is stored at the Morton offset */
static void put_swizzled(uint8_t *memory, uint32_t width, uint32_t height, uint32_t bytes, uint32_t x, uint32_t y, const uint8_t *value)
{
    uint32_t shared = 0u, width_bits = 0u, height_bits = 0u, offset = 0u;
    while ((1u << width_bits) < width) width_bits++;
    while ((1u << height_bits) < height) height_bits++;
    shared = width_bits < height_bits ? width_bits : height_bits;
    uint32_t out_bit = 0u;
    for (uint32_t bit = 0u; bit < shared; bit++) {
        offset |= ((x >> bit) & 1u) << out_bit++;
        offset |= ((y >> bit) & 1u) << out_bit++;
    }
    for (uint32_t bit = shared; bit < width_bits; bit++) offset |= ((x >> bit) & 1u) << out_bit++;
    for (uint32_t bit = shared; bit < height_bits; bit++) offset |= ((y >> bit) & 1u) << out_bit++;
    memcpy(&memory[(size_t)offset * bytes], value, bytes);
}

typedef struct {
    uint32_t color;
    uint32_t bytes;
    bool swizzled;
    uint8_t texel[4]; /* memory bytes of the texel at (x=1, y=1) */
    uint8_t expect[4];
} texel_case;

static void test_uncompressed_formats(void)
{
    static const texel_case cases[] = {
        {0x06, 4, true, {0x10, 0x20, 0x30, 0x40}, {0x30, 0x20, 0x10, 0x40}},  /* B G R A in memory */
        {0x07, 4, true, {0x10, 0x20, 0x30, 0x40}, {0x30, 0x20, 0x10, 0xFF}},
        {0x05, 2, true, {0x1F, 0xF8}, {0xFF, 0x00, 0xFF, 0xFF}},              /* 0xF81F: red 31, blue 31 */
        {0x02, 2, true, {0x00, 0xFC}, {0xFF, 0x00, 0x00, 0xFF}},              /* 0xFC00: A=1, red 31 */
        {0x02, 2, true, {0x00, 0x7C}, {0xFF, 0x00, 0x00, 0x00}},              /* A=0 */
        {0x03, 2, true, {0x00, 0x7C}, {0xFF, 0x00, 0x00, 0xFF}},
        {0x04, 2, true, {0x21, 0xF4}, {0x44, 0x22, 0x11, 0xFF}},              /* 0xF421: A=F R=4 G=2 B=1 */
        {0x00, 1, true, {0x80, 0, 0, 0}, {0x80, 0x80, 0x80, 0xFF}},
        {0x01, 1, true, {0x80, 0, 0, 0}, {0x80, 0x80, 0x80, 0x80}},
        {0x12, 4, false, {0x10, 0x20, 0x30, 0x40}, {0x30, 0x20, 0x10, 0x40}},
        {0x1E, 4, false, {0x10, 0x20, 0x30, 0x40}, {0x30, 0x20, 0x10, 0xFF}},
        {0x11, 2, false, {0x1F, 0xF8}, {0xFF, 0x00, 0xFF, 0xFF}},
        {0x10, 2, false, {0x00, 0xFC}, {0xFF, 0x00, 0x00, 0xFF}},
        {0x1C, 2, false, {0x00, 0x7C}, {0xFF, 0x00, 0x00, 0xFF}},
        {0x1D, 2, false, {0x21, 0xF4}, {0x44, 0x22, 0x11, 0xFF}},
        {0x13, 1, false, {0x80, 0, 0, 0}, {0x80, 0x80, 0x80, 0xFF}},
        /* T859, xemu kelvin colour map (INFERRED from the source) */
        {0x19, 1, true, {0x80, 0, 0, 0}, {0xFF, 0xFF, 0xFF, 0x80}},
        {0x1F, 1, false, {0x80, 0, 0, 0}, {0xFF, 0xFF, 0xFF, 0x80}},
        {0x1A, 2, true, {0x30, 0x90, 0, 0}, {0x30, 0x30, 0x30, 0x90}},
        {0x20, 2, false, {0x30, 0x90, 0, 0}, {0x30, 0x30, 0x30, 0x90}},
        {0x1B, 1, false, {0x80, 0, 0, 0}, {0x80, 0x80, 0x80, 0x80}},
        {0x28, 2, true, {0x11, 0x22, 0, 0}, {0x11, 0x22, 0x11, 0x22}},
        {0x17, 2, false, {0x11, 0x22, 0, 0}, {0x11, 0x22, 0x11, 0x22}},
        {0x29, 2, true, {0x11, 0x22, 0, 0}, {0x22, 0x11, 0x11, 0x22}},
    };
    CHECK(sizeof cases / sizeof cases[0] == 24u);
    for (size_t index = 0u; index < sizeof cases / sizeof cases[0]; index++) {
        const texel_case *item = &cases[index];
        const uint32_t width = 8u, height = 4u; /* non square on purpose */
        uint8_t memory[8 * 4 * 4 + 512];
        uint8_t rgba[8 * 4 * 4];
        memset(memory, 0, sizeof memory);
        live_texture_binding binding = {0u, 0u, 0u, 0u, 0u};
        uint32_t pitch = 0u;
        if (item->swizzled) {
            binding.format = FORMAT(item->color, 3, 2);
            put_swizzled(memory, width, height, item->bytes, 1u, 1u, item->texel);
        } else {
            pitch = 64u;
            binding.format = FORMAT(item->color, 0, 0);
            binding.size_word = LINEAR_SIZE(width, height, pitch);
            memcpy(&memory[pitch + 1u * item->bytes], item->texel, item->bytes);
        }
        live_texture_plan plan;
        CHECK(!live_texture_plan_binding(&binding, false, &plan) || plan.measured);
        CHECK(live_texture_plan_binding(&binding, true, &plan));
        CHECK(plan.width == width && plan.height == height);
        CHECK(live_texture_decode(&plan, memory, sizeof memory, rgba));
        const uint8_t *got = &rgba[(1u * width + 1u) * 4u];
        CHECK(memcmp(got, item->expect, 4u) == 0);
        if (memcmp(got, item->expect, 4u) != 0) {
            (void)fprintf(stderr, "case colour 0x%02X got %02X%02X%02X%02X\n", (unsigned)item->color, got[0], got[1], got[2], got[3]);
        }
    }
}

static void test_dxt3_dxt5(void)
{
    /* one 4x4 block: colour block red (0xF800) and blue (0x001F), 4 colour mode, all indices 0 except texel 1 -> index 1 */
    uint8_t dxt3[16] = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x00, 0xF8, 0x1F, 0x00, 0x04, 0x00, 0x00, 0x00};
    live_texture_binding binding = {0u, FORMAT(0x0E, 2, 2), 0u, 0u, 0u};
    live_texture_plan plan;
    uint8_t rgba[64];
    CHECK(!live_texture_plan_binding(&binding, false, &plan) && strcmp(plan.refusal, "inferred format") == 0);
    CHECK(live_texture_plan_binding(&binding, true, &plan));
    CHECK(plan.source_bytes == 16u && plan.width == 4u);
    CHECK(live_texture_decode(&plan, dxt3, sizeof dxt3, rgba));
    CHECK(rgba[0] == 255u && rgba[1] == 0u && rgba[2] == 0u); /* texel 0 index 0 = colour0 red */
    CHECK(rgba[4] == 0u && rgba[5] == 0u && rgba[6] == 255u); /* texel 1 index 1 = colour1 blue */
    for (uint32_t texel = 0u; texel < 16u; texel++) {
        const uint32_t nibble = texel & 15u; /* alpha bytes 0x10 0x32 ... store nibbles 0,1,2,3,... */
        CHECK(rgba[texel * 4u + 3u] == nibble * 17u);
    }
    /* DXT3 colour0 < colour1 still interpolates 4 colours (no transparent mode): index 3 is 1/3 colour0 + 2/3 colour1 */
    uint8_t reversed[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x00, 0x00, 0xF8, 0x03, 0x00, 0x00, 0x00};
    CHECK(live_texture_decode(&plan, reversed, sizeof reversed, rgba));
    CHECK(rgba[3] == 255u && rgba[0] == (uint8_t)((0u + 2u * 255u) / 3u) && rgba[2] == (uint8_t)((255u + 0u) / 3u));
    /* DXT5: alpha0 200 > alpha1 40 -> 8 value ramp; indices 3 bits: texel0 = 0 (200), texel1 = 1 (40), texel2 = 2 (ramp[2]) */
    uint8_t dxt5[16] = {200, 40, 0x88 | 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00};
    dxt5[2] = (uint8_t)(0u | (1u << 3) | (2u << 6)); /* texel 0 = 0, texel 1 = 1, texel 2 low 2 bits */
    binding.format = FORMAT(0x0F, 2, 2);
    CHECK(live_texture_plan_binding(&binding, true, &plan));
    CHECK(live_texture_decode(&plan, dxt5, sizeof dxt5, rgba));
    CHECK(rgba[3] == 200u && rgba[7] == 40u && rgba[11] == (uint8_t)((6u * 200u + 40u) / 7u));
    /* texel 15 sits in bits 45..47 (byte 5 bits 5..7): index 5 of the 8 value ramp, (3*a0+4*a1)/7 */
    uint8_t dxt5_high[16] = {200, 40, 0, 0, 0, 0, 0, (uint8_t)(5u << 5), 0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0};
    CHECK(live_texture_decode(&plan, dxt5_high, sizeof dxt5_high, rgba));
    CHECK(rgba[15 * 4 + 3] == (uint8_t)((3u * 200u + 4u * 40u) / 7u) && rgba[3] == 200u);
    /* alpha0 <= alpha1: 6 value ramp plus 0 and 255; select index 6 and 7 for texels 0 and 1 */
    uint8_t dxt5_low[16] = {10, 100, (uint8_t)(6u | (7u << 3)), 0, 0, 0, 0, 0, 0x00, 0xF8, 0x1F, 0x00, 0, 0, 0, 0};
    CHECK(live_texture_decode(&plan, dxt5_low, sizeof dxt5_low, rgba));
    CHECK(rgba[3] == 0u && rgba[7] == 255u);
    dxt5_low[2] = (uint8_t)(2u | (4u << 3));
    CHECK(live_texture_decode(&plan, dxt5_low, sizeof dxt5_low, rgba));
    CHECK(rgba[3] == (uint8_t)((4u * 10u + 1u * 100u) / 5u) && rgba[7] == (uint8_t)((2u * 10u + 3u * 100u) / 5u));
}

static void expect_refusal(const live_texture_binding *binding, bool inferred, const char *category)
{
    live_texture_plan plan;
    const bool ok = live_texture_plan_binding(binding, inferred, &plan);
    CHECK(!ok && !plan.ok && plan.refusal != NULL && strcmp(plan.refusal, category) == 0 && plan.detail[0] != '\0');
    if (plan.refusal != NULL && strcmp(plan.refusal, category) != 0) {
        (void)fprintf(stderr, "wanted %s got %s\n", category, plan.refusal);
    }
}

static void test_plan_refusals(void)
{
    live_texture_binding binding = {0u, FORMAT(0x0C, 4, 4), 0u, 0u, 0u};
    live_texture_plan plan;
    CHECK(live_texture_plan_binding(&binding, false, &plan));
    binding.format = FORMAT(0x30, 4, 4);
    expect_refusal(&binding, true, "unsupported format");
    binding.format = FORMAT(0x0C, 4, 4) | 0x04u; /* cube map: T1490 plans it only with the inferred opt-in */
    expect_refusal(&binding, false, "inferred cube map");
    CHECK(live_texture_plan_binding(&binding, true, &plan) && plan.cube && plan.faces == 6u);
    binding.format = FORMAT(0x0C, 4, 4) & ~0xF0u; /* dimensionality 0 */
    expect_refusal(&binding, true, "unsupported format");
    binding.format = FORMAT(0x0C, 4, 4) | 0x20000u; /* two mip levels */
    binding.format = (binding.format & ~0xF0000u) | 0x20000u;
    expect_refusal(&binding, false, "inferred mip chain");
    CHECK(live_texture_plan_binding(&binding, true, &plan) && plan.levels == 2u);
    binding.format = FORMAT(0x0C, 4, 4);
    binding.size_word = 1u;
    expect_refusal(&binding, true, "unsupported layout"); /* swizzled with a Size word */
    binding.size_word = 0u;
    binding.format = FORMAT(0x0C, 1, 1);
    expect_refusal(&binding, true, "unsupported layout"); /* DXT under 4 texels */
    binding.format = FORMAT(0x0C, 1, 2);
    expect_refusal(&binding, true, "unsupported layout");
    binding.format = FORMAT(0x0C, 2, 1);
    expect_refusal(&binding, true, "unsupported layout");
    binding.format = FORMAT(0x06, 12, 4);
    expect_refusal(&binding, true, "unsupported layout"); /* beyond 2048 */
    binding.format = FORMAT(0x06, 4, 12);
    expect_refusal(&binding, true, "unsupported layout");
    binding.format = FORMAT(0x06, 11, 11);
    CHECK(live_texture_plan_binding(&binding, true, &plan) && plan.width == 2048u);
    binding.format = (FORMAT(0x0C, 4, 4) & ~0xF0000u); /* zero levels */
    expect_refusal(&binding, true, "second level");
    binding.format = FORMAT(0x12, 0, 0);
    expect_refusal(&binding, true, "unsupported layout"); /* linear without Size word */
    binding.size_word = LINEAR_SIZE(64u, 4u, 128u);
    expect_refusal(&binding, true, "unsupported layout"); /* pitch 128 < 64 * 4 */
    binding.size_word = LINEAR_SIZE(64u, 4u, 256u);
    CHECK(live_texture_plan_binding(&binding, false, &plan) && plan.pitch == 256u && plan.source_bytes == 1024u && !plan.swizzled);
    binding.format = FORMAT(0x06, 4, 4);
    binding.size_word = 0u;
    expect_refusal(&binding, false, "inferred format");
    CHECK(live_texture_plan_binding(&binding, true, &plan) && !plan.measured);
}

static void fill_texture_memory(uint32_t address, size_t bytes, uint8_t seed)
{
    for (size_t index = 0u; index < bytes; index++) {
        guest_memory[address + index] = (uint8_t)(seed + index * 7u);
    }
}

static void test_cache_invalidation(void)
{
    live_texture_cache cache;
    live_texture_cache_init(&cache, false);
    const live_texture_binding binding = {0x100u, FORMAT(0x0C, 4, 4), 0u, 0x2000u, 0u}; /* 16x16 DXT1, 128 bytes */
    fill_texture_memory(0x2000u, 128u, 1u);
    live_texture_result first, second, third;
    reads = 0u;
    live_texture_lookup(&cache, &binding, read_guest, NULL, &first);
    CHECK(first.source == LIVE_TEXTURE_SOURCE_GUEST && first.rgba != NULL && first.width == 16u && first.height == 16u);
    CHECK(last_read_bytes == 128u);
    CHECK(first.needs_upload && first.upload_bytes == 16u * 16u * 4u && first.generation == 1u && reads == 1u);
    uint8_t first_pixels[16 * 16 * 4];
    memcpy(first_pixels, first.rgba, sizeof first_pixels);
    live_texture_mark_uploaded(&cache, first.entry, first.generation);
    live_texture_lookup(&cache, &binding, read_guest, NULL, &second);
    CHECK(second.source == LIVE_TEXTURE_SOURCE_GUEST && !second.needs_upload && second.upload_bytes == 0u && reads == 1u);
    CHECK(second.generation == 1u && cache.hits == 1u && cache.decodes == 1u);
    /* writes just outside [0x2000, 0x2080) leave it valid */
    CHECK(live_texture_note_write(&cache, 0x1F00u, 0x100u) == 0u);
    CHECK(live_texture_note_write(&cache, 0x2080u, 0x10u) == 0u);
    live_texture_lookup(&cache, &binding, read_guest, NULL, &second);
    CHECK(!second.needs_upload && reads == 1u);
    /* one byte inside the last byte, then the first byte, each invalidates */
    fill_texture_memory(0x2000u, 128u, 99u);
    CHECK(live_texture_note_write(&cache, 0x207Fu, 1u) == 1u);
    live_texture_lookup(&cache, &binding, read_guest, NULL, &third);
    CHECK(third.needs_upload && third.generation == 2u && reads == 2u && cache.invalidations == 1u && cache.decodes == 2u);
    CHECK(memcmp(first_pixels, third.rgba, sizeof first_pixels) != 0);
    live_texture_mark_uploaded(&cache, third.entry, third.generation);
    CHECK(live_texture_note_write(&cache, 0x1FFFu, 2u) == 1u);
    live_texture_lookup(&cache, &binding, read_guest, NULL, &third);
    CHECK(third.needs_upload && third.generation == 3u);
    /* an overlapping write spanning the whole range of the entry */
    CHECK(live_texture_note_write(&cache, 0x0u, 0x10000u) == 1u);
    /* the same Data and Size word under another Format is another texture, not the cached one */
    const live_texture_binding bigger = {0x100u, FORMAT(0x0C, 5, 5), 0u, 0x2000u, 0u};
    live_texture_result other;
    live_texture_lookup(&cache, &binding, read_guest, NULL, &other); /* valid again, so only the key can tell them apart */
    CHECK(other.width == 16u);
    live_texture_lookup(&cache, &bigger, read_guest, NULL, &other);
    CHECK(other.source == LIVE_TEXTURE_SOURCE_GUEST && other.width == 32u && other.height == 32u);
    /* an unreadable address is a named refusal, not a stale or stand-in image */
    const live_texture_binding bad = {0u, FORMAT(0x0C, 4, 4), 0u, 0xFFFFF80u, 0u};
    live_texture_result refused;
    live_texture_lookup(&cache, &bad, read_guest, NULL, &refused);
    CHECK(refused.source == LIVE_TEXTURE_SOURCE_REFUSED && refused.rgba == NULL && strcmp(refused.refusal, "unreadable bytes") == 0);
    live_texture_cache_free(&cache);
}

/* T1255: a frame binds up to 75 distinct textures. With 64 cache entries the LRU over that cycle missed on every bind (a decode and an
 * upload each), the cutscene frame job of the Story replay ran 2x slower. */
static void test_cache_holds_a_frames_textures(void)
{
    live_texture_cache cache;
    live_texture_cache_init(&cache, false);
    enum { TEXTURES = 75 };
    CHECK(LIVE_TEXTURE_CACHE_ENTRIES >= TEXTURES);
    for (uint32_t index = 0u; index < TEXTURES; index++) {
        fill_texture_memory(0x4000u + index * 0x80u, 128u, (uint8_t)(index + 1u));
    }
    for (unsigned pass = 0u; pass < 3u; pass++) {
        for (uint32_t index = 0u; index < TEXTURES; index++) {
            const live_texture_binding binding = {0x100u, FORMAT(0x0C, 4, 4), 0u, 0x4000u + index * 0x80u, 0u};
            live_texture_result result;
            live_texture_lookup(&cache, &binding, read_guest, NULL, &result);
            CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST && result.rgba != NULL);
        }
    }
    CHECK(cache.decodes == TEXTURES && cache.hits == 2u * TEXTURES);
    live_texture_cache_free(&cache);
}

static uint64_t resolver_reads;
static bool counting_resolver(void *context, const live_texture_binding *binding, uint32_t bytes, uint32_t *address, uint64_t *identity,
                              const char **refusal)
{
    (void)context; (void)bytes; (void)refusal;
    *address = binding->data;
    *identity = 7u;
    return true;
}
static bool counting_reader(void *context, uint32_t address, void *out, size_t bytes)
{
    resolver_reads++;
    return read_guest(context, address, out, bytes);
}

/* T1255: within one frame job (input_serial nonzero) a binding already compared with the frame's capture is not read and compared again. */
static void test_input_serial_skips_repeat_compares(void)
{
    live_texture_cache cache;
    live_texture_cache_init(&cache, false);
    const live_texture_binding binding = {0x100u, FORMAT(0x0C, 4, 4), 0u, 0x2000u, 0u};
    fill_texture_memory(0x2000u, 128u, 3u);
    live_texture_result result;
    resolver_reads = 0u;
    live_texture_lookup_resolved(&cache, &binding, counting_reader, NULL, counting_resolver, NULL, &result); /* serial 0: always reads */
    live_texture_lookup_resolved(&cache, &binding, counting_reader, NULL, counting_resolver, NULL, &result);
    CHECK(resolver_reads == 2u && cache.compares_skipped == 0u && cache.decodes == 1u && cache.hits == 1u);
    cache.input_serial = 5u;
    live_texture_lookup_resolved(&cache, &binding, counting_reader, NULL, counting_resolver, NULL, &result); /* first at serial 5: compares */
    CHECK(resolver_reads == 3u && cache.compares_skipped == 0u);
    live_texture_lookup_resolved(&cache, &binding, counting_reader, NULL, counting_resolver, NULL, &result); /* repeat: skipped */
    live_texture_lookup_resolved(&cache, &binding, counting_reader, NULL, counting_resolver, NULL, &result);
    CHECK(resolver_reads == 3u && cache.compares_skipped == 2u && cache.decodes == 1u && cache.hits == 4u);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST && result.rgba != NULL);
    fill_texture_memory(0x2000u, 128u, 90u); /* the next frame capture holds other bytes: a new serial compares and decodes again */
    cache.input_serial = 6u;
    live_texture_lookup_resolved(&cache, &binding, counting_reader, NULL, counting_resolver, NULL, &result);
    CHECK(resolver_reads == 4u && cache.invalidations == 1u && cache.decodes == 2u && result.generation == 2u);
    live_texture_cache_free(&cache);
}

static void test_targets_and_census(void)
{
    live_texture_cache cache;
    live_texture_cache_init(&cache, false);
    CHECK(live_texture_register_target(&cache, 7u, 0x40000u, 64u, 32u, 256u));
    live_texture_binding binding = {0x300u, 0x00011229u, LINEAR_SIZE(64u, 32u, 256u), 0x40000u, 0u};
    live_texture_result result;
    live_texture_lookup(&cache, &binding, read_guest, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_TARGET && result.target_id == 7u && result.width == 64u && result.height == 32u);
    CHECK(result.rgba == NULL && !result.needs_upload && cache.target_samples == 1u);
    /* a dirty write to the target bytes must not touch the target path */
    CHECK(live_texture_note_write(&cache, 0x40000u, 16u) == 0u);
    /* size mismatch on an exact Data match */
    binding.size_word = LINEAR_SIZE(32u, 32u, 256u);
    live_texture_lookup(&cache, &binding, read_guest, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED && strcmp(result.refusal, "unsupported layout") == 0);
    /* same size and Data, other pitch */
    binding.size_word = LINEAR_SIZE(64u, 32u, 320u);
    live_texture_lookup(&cache, &binding, read_guest, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED && strcmp(result.refusal, "unsupported layout") == 0);
    /* alias: same bytes at another offset */
    binding.size_word = LINEAR_SIZE(64u, 32u, 256u);
    binding.data = 0x3FF00u; /* starts below the target and runs into it */
    live_texture_lookup(&cache, &binding, read_guest, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED && strcmp(result.refusal, "alias") == 0);
    binding.data = 0x40100u;
    live_texture_lookup(&cache, &binding, read_guest, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED && strcmp(result.refusal, "alias") == 0);
    /* a swizzled texture at the target's Data */
    live_texture_binding swizzled = {0u, FORMAT(0x0C, 4, 4), 0u, 0x40000u, 0u};
    live_texture_lookup(&cache, &swizzled, read_guest, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED && strcmp(result.refusal, "unsupported layout") == 0);
    {
        /* a swizzled A8R8G8B8 of the target's size at the target's Data (inferred formats enabled) */
        live_texture_cache loose;
        live_texture_cache_init(&loose, true);
        CHECK(live_texture_register_target(&loose, 3u, 0x40000u, 64u, 32u, 256u));
        live_texture_binding same = {0u, FORMAT(0x06, 6, 5), 0u, 0x40000u, 0u};
        live_texture_lookup(&loose, &same, read_guest, NULL, &result);
        CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED && strcmp(result.refusal, "unsupported layout") == 0);
        live_texture_cache_free(&loose);
    }
    {
        /* T1489: a swizzled target is sampled by a swizzled A8R8G8B8 header of its own size (normalised coordinates), by nothing else */
        live_texture_cache swz;
        live_texture_cache_init(&swz, true);
        CHECK(live_texture_register_swizzled_target(&swz, 5u, 0x80000u, 64u, 32u));
        live_texture_binding same = {0u, FORMAT(0x06, 6, 5), 0u, 0x80000u, 0u};
        live_texture_lookup(&swz, &same, read_guest, NULL, &result);
        CHECK(result.source == LIVE_TEXTURE_SOURCE_TARGET && result.target_id == 5u && result.target_swizzled && result.width == 64u &&
              result.height == 32u && result.rgba == NULL && !result.needs_upload);
        live_texture_binding other_size = {0u, FORMAT(0x06, 5, 5), 0u, 0x80000u, 0u};
        live_texture_lookup(&swz, &other_size, read_guest, NULL, &result);
        CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED && strcmp(result.refusal, "unsupported layout") == 0);
        live_texture_binding linear_header = {0x300u, 0x00011229u, LINEAR_SIZE(64u, 32u, 256u), 0x80000u, 0u};
        live_texture_lookup(&swz, &linear_header, read_guest, NULL, &result);
        CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED && strcmp(result.refusal, "unsupported layout") == 0);
        /* the linear target keeps refusing the swizzled header and is not flagged swizzled */
        CHECK(live_texture_register_target(&swz, 6u, 0x90000u, 64u, 32u, 256u));
        live_texture_binding to_linear = {0x300u, 0x00011229u, LINEAR_SIZE(64u, 32u, 256u), 0x90000u, 0u};
        live_texture_lookup(&swz, &to_linear, read_guest, NULL, &result);
        CHECK(result.source == LIVE_TEXTURE_SOURCE_TARGET && !result.target_swizzled);
        live_texture_binding swz_over_linear = {0u, FORMAT(0x06, 6, 5), 0u, 0x90000u, 0u};
        live_texture_lookup(&swz, &swz_over_linear, read_guest, NULL, &result);
        CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED && strcmp(result.refusal, "unsupported layout") == 0);
        live_texture_cache_free(&swz);
    }
    /* ambiguous: two targets on one Data */
    CHECK(live_texture_register_target(&cache, 8u, 0x40000u, 64u, 32u, 256u));
    binding.data = 0x40000u;
    live_texture_lookup(&cache, &binding, read_guest, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED && strcmp(result.refusal, "ambiguous mapping") == 0);
    live_texture_clear_targets(&cache);
    /* a refused binding repeated is one census line with a count */
    live_texture_binding odd = {0x400u, FORMAT(0x30, 3, 3), 0u, 0x5000u, 0u};
    for (unsigned repeat = 0u; repeat < 3u; repeat++) {
        live_texture_lookup(&cache, &odd, read_guest, NULL, &result);
        CHECK(result.source == LIVE_TEXTURE_SOURCE_REFUSED);
    }
    char line[400];
    bool found = false;
    for (size_t index = 0u; live_texture_census_line(&cache, index, line, sizeof line); index++) {
        if (strstr(line, "unsupported format") != NULL && strstr(line, "Data 0x0005000 x3") != NULL && strstr(line, "Format 0x03313029") != NULL) {
            found = true;
        }
    }
    CHECK(found);
    CHECK(cache.census_count >= 4u && cache.refusals == 9u);
    CHECK(!live_texture_census_line(&cache, cache.census_count, line, sizeof line));
    live_texture_cache_free(&cache);
}

/* a recognisable picture for the PNG evidence: encode it as row-major DXT1 with a min/max endpoint encoder */
static void write_evidence(const char *path)
{
    enum { SIZE = 128 };
    static uint8_t picture[SIZE * SIZE * 4];
    static uint8_t memory[(SIZE / 4) * (SIZE / 4) * 8];
    static uint8_t decoded[SIZE * SIZE * 4];
    for (int y = 0; y < SIZE; y++) {
        for (int x = 0; x < SIZE; x++) {
            uint8_t *pixel = &picture[(y * SIZE + x) * 4];
            const int dx = x - SIZE / 2, dy = y - SIZE / 2;
            const bool disc = dx * dx + dy * dy < 40 * 40;
            pixel[0] = disc ? 250 : (uint8_t)(x * 2);
            pixel[1] = disc ? 200 : (uint8_t)(y * 2);
            pixel[2] = disc ? 20 : 160;
            pixel[3] = 255;
            if (y < 8 && x < 8) { /* top-left marker proves row 0 is on top */
                pixel[0] = pixel[1] = pixel[2] = 255;
            }
        }
    }
    for (int block_y = 0; block_y < SIZE / 4; block_y++) {
        for (int block_x = 0; block_x < SIZE / 4; block_x++) {
            int low = 0, high = 0, low_sum = 1 << 30, high_sum = -1;
            for (int texel = 0; texel < 16; texel++) {
                const uint8_t *pixel = &picture[((block_y * 4 + texel / 4) * SIZE + block_x * 4 + texel % 4) * 4];
                const int sum = pixel[0] + pixel[1] + pixel[2];
                if (sum < low_sum) { low_sum = sum; low = texel; }
                if (sum > high_sum) { high_sum = sum; high = texel; }
            }
            uint16_t endpoint[2];
            for (int which = 0; which < 2; which++) {
                const uint8_t *pixel = &picture[((block_y * 4 + (which ? low : high) / 4) * SIZE + block_x * 4 + (which ? low : high) % 4) * 4];
                endpoint[which] = (uint16_t)(((pixel[0] >> 3) << 11) | ((pixel[1] >> 2) << 5) | (pixel[2] >> 3));
            }
            bool flipped = false;
            if (endpoint[0] == endpoint[1]) {
                if (endpoint[1] > 0u) { endpoint[1]--; } else { endpoint[0]++; }
            } else if (endpoint[0] < endpoint[1]) {
                const uint16_t swap = endpoint[0]; endpoint[0] = endpoint[1]; endpoint[1] = swap;
                flipped = true;
            }
            uint8_t block[8] = {(uint8_t)endpoint[0], (uint8_t)(endpoint[0] >> 8), (uint8_t)endpoint[1], (uint8_t)(endpoint[1] >> 8), 0, 0, 0, 0};
            for (int texel = 0; texel < 16; texel++) {
                const uint8_t *pixel = &picture[((block_y * 4 + texel / 4) * SIZE + block_x * 4 + texel % 4) * 4];
                const int sum = pixel[0] + pixel[1] + pixel[2];
                const int span = high_sum - low_sum + 1;
                int pick = (high_sum - sum) * 3 / span; /* 0 = colour0 (bright) .. 3 = colour1 (dark), in palette order 0,2,3,1 */
                static const int order[4] = {0, 2, 3, 1};
                pick = order[pick < 0 ? 0 : (pick > 3 ? 3 : pick)];
                if (flipped) { pick = order[3 - (pick == 0 ? 0 : pick == 2 ? 1 : pick == 3 ? 2 : 3)]; }
                block[4 + texel / 4] |= (uint8_t)(pick << (2 * (texel % 4)));
            }
            memcpy(&memory[((size_t)block_y * (SIZE / 4u) + (size_t)block_x) * 8u], block, 8u);
        }
    }
    const live_texture_binding binding = {0u, FORMAT(0x0C, 7, 7), 0u, 0u, 0u};
    live_texture_plan plan;
    CHECK(live_texture_plan_binding(&binding, false, &plan));
    CHECK(live_texture_decode(&plan, memory, sizeof memory, decoded));
    /* lossy encoder, so check the marker and the disc centre instead of equality */
    CHECK(decoded[0] > 240u && decoded[1] > 240u && decoded[2] > 240u);
    const uint8_t *centre = &decoded[((SIZE / 2) * SIZE + SIZE / 2) * 4];
    CHECK(centre[0] > 200u && centre[1] > 150u && centre[2] < 90u);
    if (path != NULL) {
        CHECK(gpu_png_write_rgba(path, decoded, SIZE, SIZE, SIZE * 4u));
    }
}

static bool forbidden_resolver(void *context, const live_texture_binding *binding,
                               uint32_t bytes, uint32_t *address, uint64_t *identity,
                               const char **refusal)
{
    (void)context; (void)binding; (void)bytes; (void)address; (void)identity;
    *refusal="must not resolve guest memory for an actual target image";
    return false;
}

static void test_target_bypasses_guest_resolution(void)
{
    live_texture_cache cache; live_texture_cache_init(&cache,false);
    CHECK(live_texture_register_target(&cache,42u,0x2000u,16u,16u,64u));
    live_texture_binding binding={0x100u,FORMAT(0x12,0,0),LINEAR_SIZE(16,16,64),0x2000u, 0u};
    live_texture_result result;
    live_texture_lookup_resolved(&cache,&binding,NULL,NULL,forbidden_resolver,NULL,&result);
    CHECK(result.source==LIVE_TEXTURE_SOURCE_TARGET && result.target_id==42u);
    binding.data=0x4000u;
    live_texture_lookup_resolved(&cache,&binding,NULL,NULL,forbidden_resolver,NULL,&result);
    CHECK(result.source==LIVE_TEXTURE_SOURCE_REFUSED && strcmp(result.refusal,"resource resolution")==0);
    live_texture_cache_free(&cache);
}

/* Literal footprints and level-local Morton order, independent of production offset calculation. */
static void test_mip_chains(void)
{
    live_texture_binding binding = {.format = 0x02341929u}; /* A8 8x4, four levels */
    live_texture_plan plan;
    CHECK(live_texture_plan_binding(&binding, true, &plan));
    CHECK(plan.levels == 4u && plan.source_bytes == 43u && plan.rgba_bytes == 172u);
    const uint32_t lengths[] = {32u, 8u, 2u, 1u};
    const uint32_t offsets[] = {0u, 32u, 40u, 42u};
    const uint32_t rgba_offsets[] = {0u, 128u, 160u, 168u};
    const uint8_t morton8x4[] = {0,1,4,5,16,17,20,21,2,3,6,7,18,19,22,23,
                                8,9,12,13,24,25,28,29,10,11,14,15,26,27,30,31};
    const uint8_t morton4x2[] = {0,1,4,5,2,3,6,7};
    uint8_t source[43], rgba[176];
    for (uint32_t i = 0u; i < 43u; i++) source[i] = (uint8_t)(i + 17u);
    memset(rgba, 0xA5, sizeof rgba);
    CHECK(!live_texture_decode(&plan, source, 42u, rgba));
    CHECK(live_texture_decode(&plan, source, sizeof source, rgba));
    for (uint32_t level = 0u; level < 4u; level++) {
        CHECK(plan.mip[level].source_bytes == lengths[level] && plan.mip[level].source_offset == offsets[level]);
        CHECK(plan.mip[level].rgba_offset == rgba_offsets[level]);
        for (uint32_t pixel = 0u; pixel < lengths[level]; pixel++) {
            const uint32_t index = level == 0u ? morton8x4[pixel] : level == 1u ? morton4x2[pixel] : pixel;
            const uint8_t *out = rgba + rgba_offsets[level] + pixel * 4u;
            CHECK(out[0] == 255u && out[1] == 255u && out[2] == 255u && out[3] == source[offsets[level] + index]);
        }
    }
    CHECK(rgba[172] == 0xA5 && rgba[175] == 0xA5);
    binding.format = 0x07761929u;
    CHECK(live_texture_plan_binding(&binding, true, &plan));
    CHECK(plan.levels == 6u && plan.source_bytes == 21840u && plan.rgba_bytes == 87360u);
    binding.mip_limit = 3u;
    CHECK(live_texture_plan_binding(&binding, true, &plan) && plan.levels == 3u && plan.source_bytes == 21504u);
    binding.mip_limit = 0u;
    for (uint32_t color = 0x0Cu; color <= 0x0Fu; color++) {
        if (color == 0x0Du) continue;
        binding.format = (FORMAT(color, 2, 2) & ~0xF0000u) | 0x30000u;
        CHECK(live_texture_plan_binding(&binding, true, &plan));
        const uint32_t block_bytes = color == 0x0Cu ? 8u : 16u;
        CHECK(plan.levels == 3u && plan.source_bytes == 3u * block_bytes && plan.rgba_bytes == 84u);
        uint8_t blocks[48] = {0}, output[88];
        for (uint32_t level = 0u; level < 3u; level++) {
            uint8_t *block = blocks + level * block_bytes;
            if (color == 0x0Eu) memset(block, 0xFF, 8u);
            if (color == 0x0Fu) block[0] = 255u;
            block += color == 0x0Cu ? 0u : 8u;
            block[1] = 0xF8u; /* red 565 endpoint, selector zero */
        }
        memset(output, 0xA5, sizeof output);
        CHECK(live_texture_decode(&plan, blocks, plan.source_bytes, output));
        for (uint32_t pixel = 0u; pixel < 21u; pixel++)
            CHECK(output[pixel * 4u] == 255u && output[pixel * 4u + 1u] == 0u &&
                  output[pixel * 4u + 2u] == 0u && output[pixel * 4u + 3u] == 255u);
        CHECK(output[84] == 0xA5 && output[87] == 0xA5);
    }
}

static const uint8_t *cube_guest;
static bool cube_reader(void *context, uint32_t address, void *out, size_t bytes)
{
    (void)context;
    if (address < 0x100u || address + bytes > 0x800u) return false;
    memcpy(out, cube_guest + address, bytes);
    return true;
}

/* T1490: a cube map is six square faces +X -X +Y -Y +Z -Z, each a mip chain, 128 byte aligned in guest memory (xemu pgraph/texture.c
 * pgraph_get_texture_length, gl/texture.c): the plan, the decode (face f at f * face_rgba_bytes) and the cache lookup. */
static void test_cube_maps(void)
{
    live_texture_binding binding = {0u, FORMAT(0x06, 2, 2) | 0x04u, 0u, 0x0100000u, 0u};
    live_texture_plan plan;
    CHECK(!live_texture_plan_binding(&binding, false, &plan) && strcmp(plan.refusal, "inferred cube map") == 0);
    CHECK(live_texture_plan_binding(&binding, true, &plan) && plan.cube && plan.faces == 6u && plan.width == 4u && plan.height == 4u);
    CHECK(plan.face_source_stride == 128u && plan.face_rgba_bytes == 64u && plan.rgba_bytes == 6u * 64u && plan.source_bytes == 5u * 128u + 64u);
    /* face f texel (x, y) of a swizzled 4 x 4 A8R8G8B8: bytes B G R A with B = 10 f + x, G = 100 + y, R = f */
    uint8_t source[6u * 128u];
    memset(source, 0xEE, sizeof source); /* the alignment padding must never reach the image */
    for (uint32_t face = 0u; face < 6u; face++) {
        for (uint32_t y = 0u; y < 4u; y++) {
            for (uint32_t x = 0u; x < 4u; x++) {
                uint8_t *texel = source + face * 128u + live_texture_swizzle_offset(x, y, 4u, 4u) * 4u;
                texel[0] = (uint8_t)(10u * face + x);
                texel[1] = (uint8_t)(100u + y);
                texel[2] = (uint8_t)face;
                texel[3] = 255u;
            }
        }
    }
    uint8_t rgba[6u * 64u];
    CHECK(live_texture_decode(&plan, source, plan.source_bytes, rgba));
    bool exact = true;
    for (uint32_t face = 0u; face < 6u; face++) {
        for (uint32_t y = 0u; y < 4u; y++) {
            for (uint32_t x = 0u; x < 4u; x++) {
                const uint8_t *pixel = rgba + face * plan.face_rgba_bytes + (y * 4u + x) * 4u;
                exact = exact && pixel[0] == face && pixel[1] == 100u + y && pixel[2] == 10u * face + x && pixel[3] == 255u;
            }
        }
    }
    CHECK(exact);
    CHECK(!live_texture_decode(&plan, source, plan.source_bytes - 1u, rgba)); /* the last face is short */
    /* two mip levels per face: the chain of a face is 64 + 16 bytes, the next face starts at 128 */
    binding.format = (FORMAT(0x06, 2, 2) | 0x04u) + 0x10000u;
    CHECK(live_texture_plan_binding(&binding, true, &plan) && plan.levels == 2u && plan.face_source_stride == 128u);
    CHECK(plan.face_rgba_bytes == 64u + 16u && plan.rgba_bytes == 6u * 80u && plan.source_bytes == 5u * 128u + 80u && plan.mip[1].rgba_offset == 64u);
    /* a 16 x 16 face is 1024 bytes, already a multiple of 128 */
    binding.format = FORMAT(0x06, 4, 4) | 0x04u;
    CHECK(live_texture_plan_binding(&binding, true, &plan) && plan.face_source_stride == 1024u && plan.source_bytes == 6u * 1024u);
    /* not square, linear, and a Size word are refused by name */
    binding.format = FORMAT(0x06, 3, 2) | 0x04u;
    expect_refusal(&binding, true, "unsupported layout");
    binding.format = FORMAT(0x12, 2, 2) | 0x04u;
    binding.size_word = LINEAR_SIZE(4u, 4u, 64u);
    expect_refusal(&binding, true, "unsupported layout");
    /* through the cache: one lookup decodes six faces, the image is flagged a cube, a write into the LAST face invalidates it */
    live_texture_cache cache;
    live_texture_cache_init(&cache, true);
    binding = (live_texture_binding){0u, FORMAT(0x06, 2, 2) | 0x04u, 0u, 0x0000100u, 0u};
    uint8_t guest[0x800];
    memset(guest, 0, sizeof guest);
    memcpy(guest + 0x100u, source, sizeof source);
    cube_guest = guest;
    live_texture_result result;
    live_texture_lookup(&cache, &binding, cube_reader, NULL, &result);
    CHECK(result.source == LIVE_TEXTURE_SOURCE_GUEST && result.cube && result.needs_upload && result.upload_bytes == 6u * 64u);
    CHECK(result.rgba != NULL && result.rgba[5u * 64u + 2u] == 50u); /* face 5 texel (0, 0): R = B of the guest = 10 * 5 */
    CHECK(live_texture_note_write(&cache, 0x100u + 5u * 128u + 60u, 4u) == 1u); /* the final bytes of the final face */
    CHECK(live_texture_note_write(&cache, 0x100u + 5u * 128u + 64u, 4u) == 0u); /* the padding after the last face is not part of the texture */
    live_texture_cache_free(&cache);
}

int main(int argc, char **argv)
{
    test_mip_chains();
    test_target_bypasses_guest_resolution();
    test_dxt1_matches_original();
    test_rectangular_compressed_order();
    test_swizzle_offset();
    test_uncompressed_formats();
    test_dxt3_dxt5();
    test_plan_refusals();
    test_cube_maps();
    test_cache_invalidation();
    test_cache_holds_a_frames_textures();
    test_input_serial_skips_repeat_compares();
    test_targets_and_census();
    write_evidence(argc > 1 ? argv[1] : NULL);
    if (failures != 0) {
        (void)fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    (void)puts("live_texture: all checks pass");
    return 0;
}
