/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gpu_phase_timing.h"
#include "live_texture.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t color_byte;
    live_texture_kind kind;
    bool swizzled;
    uint32_t bytes; /* per texel, or per 4x4 block for the DXT kinds */
    bool measured;  /* a T632/T633 xemu fixture pinned it, otherwise INFERRED (xemu's kelvin colour list) */
} format_row;

static const format_row formats[] = {
    {0x00u, LIVE_TEXTURE_Y8, true, 1u, false},         {0x01u, LIVE_TEXTURE_AY8, true, 1u, false},
    {0x02u, LIVE_TEXTURE_A1R5G5B5, true, 2u, false},   {0x03u, LIVE_TEXTURE_X1R5G5B5, true, 2u, false},
    {0x04u, LIVE_TEXTURE_A4R4G4B4, true, 2u, false},   {0x05u, LIVE_TEXTURE_R5G6B5, true, 2u, false},
    {0x06u, LIVE_TEXTURE_A8R8G8B8, true, 4u, false},   {0x07u, LIVE_TEXTURE_X8R8G8B8, true, 4u, false},
    {0x0Cu, LIVE_TEXTURE_DXT1, true, 8u, true},        {0x0Eu, LIVE_TEXTURE_DXT3, true, 16u, false},
    {0x0Fu, LIVE_TEXTURE_DXT5, true, 16u, false},      {0x10u, LIVE_TEXTURE_A1R5G5B5, false, 2u, false},
    {0x11u, LIVE_TEXTURE_R5G6B5, false, 2u, false},    {0x12u, LIVE_TEXTURE_A8R8G8B8, false, 4u, true},
    {0x13u, LIVE_TEXTURE_Y8, false, 1u, false},        {0x1Cu, LIVE_TEXTURE_X1R5G5B5, false, 2u, false},
    {0x1Du, LIVE_TEXTURE_A4R4G4B4, false, 2u, false},  {0x1Eu, LIVE_TEXTURE_X8R8G8B8, false, 4u, false},
    /* T859: the xemu kelvin colour map (hw/xbox/nv2a/pgraph/gl/constants.h) gives the channel mapping, INFERRED from it */
    {0x19u, LIVE_TEXTURE_A8, true, 1u, false},         {0x1Fu, LIVE_TEXTURE_A8, false, 1u, false},
    {0x1Au, LIVE_TEXTURE_A8Y8, true, 2u, false},       {0x20u, LIVE_TEXTURE_A8Y8, false, 2u, false},
    {0x1Bu, LIVE_TEXTURE_AY8, false, 1u, false},       {0x17u, LIVE_TEXTURE_G8B8, false, 2u, false},
    {0x28u, LIVE_TEXTURE_G8B8, true, 2u, false},       {0x29u, LIVE_TEXTURE_R8B8, true, 2u, false},
};

static bool is_compressed(live_texture_kind kind)
{
    return kind == LIVE_TEXTURE_DXT1 || kind == LIVE_TEXTURE_DXT3 || kind == LIVE_TEXTURE_DXT5;
}

static bool refuse(live_texture_plan *plan, const char *category, const char *format, ...) __attribute__((format(printf, 3, 4)));


static bool refuse(live_texture_plan *plan, const char *category, const char *format, ...)
{
    va_list arguments;
    plan->ok = false;
    plan->refusal = category;
    va_start(arguments, format);
    (void)vsnprintf(plan->detail, sizeof plan->detail, format, arguments);
    va_end(arguments);
    return false;
}

static uint32_t log2_of(uint32_t value)
{
    uint32_t bits = 0u;
    while ((1u << bits) < value) {
        bits++;
    }
    return bits;
}

bool live_texture_plan_binding(const live_texture_binding *binding, bool allow_inferred, live_texture_plan *plan)
{
    memset(plan, 0, sizeof *plan);
    const uint32_t format = binding->format;
    const uint32_t color = (format >> 8) & 0xFFu;
    const uint32_t levels = (format >> 16) & 0xFu;
    const uint32_t exponent_u = (format >> 20) & 0xFu;
    const uint32_t exponent_v = (format >> 24) & 0xFu;
    plan->color_byte = color;
    const format_row *row = NULL;
    for (size_t index = 0u; index < sizeof formats / sizeof formats[0]; index++) {
        if (formats[index].color_byte == color) {
            row = &formats[index];
        }
    }
    if (row == NULL) {
        return refuse(plan, "unsupported format", "colour byte 0x%02X (Format 0x%08X) has no decoder", (unsigned)color, (unsigned)format);
    }
    const bool cube = ((format >> 2) & 1u) != 0u;
    if (((format >> 4) & 0xFu) != 2u || ((format >> 28) & 0xFu) != 0u || ((format & 3u) != 1u && (format & 3u) != 2u)) {
        return refuse(plan, "unsupported format", "Format 0x%08X is not a plain 2D or cube map (no 3D) texture of the decoded kind", (unsigned)format);
    }
    if (cube && !allow_inferred) {
        return refuse(plan, "inferred cube map", "Format 0x%08X is a cube map, INFERRED from xemu, opt-in only", (unsigned)format);
    }
    if (levels == 0u) {
        return refuse(plan, "second level", "Format 0x%08X declares zero mipmap levels", (unsigned)format);
    }
    if (levels > 1u && !allow_inferred) {
        return refuse(plan, "inferred mip chain", "Format 0x%08X mip storage/sampling is INFERRED, opt-in only", (unsigned)format);
    }
    if (!row->measured && !allow_inferred) {
        return refuse(plan, "inferred format", "colour byte 0x%02X is INFERRED (no xemu fixture), opt-in only", (unsigned)color);
    }
    if (cube && !row->swizzled) {
        return refuse(plan, "unsupported layout", "a linear colour 0x%02X cube map (xemu asserts not linear)", (unsigned)color);
    }
    plan->kind = row->kind;
    plan->swizzled = row->swizzled;
    plan->compressed = is_compressed(row->kind);
    plan->measured = row->measured && levels == 1u;
    plan->levels = levels;
    if (row->swizzled) {
        if (binding->size_word != 0u) {
            return refuse(plan, "unsupported layout", "swizzled colour 0x%02X carries a Size word 0x%08X", (unsigned)color, (unsigned)binding->size_word);
        }
        if (exponent_u > 11u || exponent_v > 11u || exponent_u < (plan->compressed ? 2u : 0u) || exponent_v < (plan->compressed ? 2u : 0u)) {
            return refuse(plan, "unsupported layout", "swizzled size exponents %u x %u are outside the decoded range", (unsigned)exponent_u, (unsigned)exponent_v);
        }
        plan->width = 1u << exponent_u;
        plan->height = 1u << exponent_v;
        const uint32_t legal_levels = (exponent_u > exponent_v ? exponent_u : exponent_v) + 1u;
        if (plan->levels > legal_levels) plan->levels = legal_levels;
        if (binding->mip_limit != 0u && plan->levels > binding->mip_limit) plan->levels = binding->mip_limit;
    } else {
        if (levels != 1u) {
            return refuse(plan, "unsupported layout", "linear textures do not have mip chains");
        }
        if (binding->size_word == 0u) {
            return refuse(plan, "unsupported layout", "linear colour 0x%02X has no Size word", (unsigned)color);
        }
        plan->measured = row->measured;
        plan->width = (binding->size_word & 0xFFFu) + 1u;
        plan->height = ((binding->size_word >> 12) & 0xFFFu) + 1u;
        plan->pitch = (((binding->size_word >> 24) & 0xFFu) + 1u) << 6;
        if (plan->width > LIVE_TEXTURE_MAX_DIMENSION || plan->height > LIVE_TEXTURE_MAX_DIMENSION || plan->pitch < plan->width * row->bytes) {
            return refuse(plan, "unsupported layout", "linear %ux%u pitch %u does not hold its texels", (unsigned)plan->width, (unsigned)plan->height, (unsigned)plan->pitch);
        }
    }
    if (cube && plan->width != plan->height) {
        return refuse(plan, "unsupported layout", "a cube map face is %ux%u, not square", (unsigned)plan->width, (unsigned)plan->height);
    }
    uint32_t width = plan->width, height = plan->height;
    for (uint32_t level = 0u; level < plan->levels; ++level) {
        live_texture_level *mip = &plan->mip[level];
        mip->width = width;
        mip->height = height;
        mip->source_offset = plan->source_bytes;
        mip->rgba_offset = plan->rgba_bytes;
        mip->source_bytes = !plan->swizzled ? plan->pitch * height : plan->compressed
            ? ((width + 3u) / 4u) * ((height + 3u) / 4u) * row->bytes : width * height * row->bytes;
        mip->rgba_bytes = width * height * 4u;
        plan->source_bytes += mip->source_bytes;
        plan->rgba_bytes += mip->rgba_bytes;
        width = width > 1u ? width / 2u : 1u;
        height = height > 1u ? height / 2u : 1u;
    }
    plan->faces = 1u;
    if (cube) {
        /* xemu pgraph/texture.c pgraph_get_texture_length and gl/texture.c: the chain of one face rounded up to 128 bytes, six faces */
        plan->cube = true;
        plan->faces = 6u;
        plan->face_source_stride = (plan->source_bytes + 127u) & ~127u;
        plan->face_rgba_bytes = plan->rgba_bytes;
        plan->rgba_bytes *= 6u;
        plan->source_bytes = 5u * plan->face_source_stride + plan->source_bytes;
    }
    plan->ok = true;
    return true;
}

uint32_t live_texture_swizzle_offset(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    const uint32_t width_bits = log2_of(width);
    const uint32_t height_bits = log2_of(height);
    const uint32_t shared = width_bits < height_bits ? width_bits : height_bits;
    uint32_t offset = 0u;
    for (uint32_t bit = 0u; bit < shared; bit++) {
        offset |= ((x >> bit) & 1u) << (2u * bit);
        offset |= ((y >> bit) & 1u) << (2u * bit + 1u);
    }
    if (width_bits > shared) {
        offset |= (x >> shared) << (2u * shared);
    } else if (height_bits > shared) {
        offset |= (y >> shared) << (2u * shared);
    }
    return offset;
}

static void expand_565(uint16_t color, uint8_t out[4])
{
    const uint32_t red = (color >> 11) & 31u;
    const uint32_t green = (color >> 5) & 63u;
    const uint32_t blue = color & 31u;
    out[0] = (uint8_t)((red << 3) | (red >> 2));
    out[1] = (uint8_t)((green << 2) | (green >> 4));
    out[2] = (uint8_t)((blue << 3) | (blue >> 2));
    out[3] = 255u;
}

/* One 4x4 colour block (8 bytes) into 16 RGBA texels in `texels[row * 4 + column]`. DXT1 keeps the 3 colour transparent mode,
 * DXT3 and DXT5 always interpolate four colours (alpha is explicit there). */
static void decode_colour_block(const uint8_t *block, bool dxt1, uint8_t texels[16][4])
{
    const uint16_t color0 = (uint16_t)(block[0] | (block[1] << 8));
    const uint16_t color1 = (uint16_t)(block[2] | (block[3] << 8));
    const bool four = !dxt1 || color0 > color1;
    uint8_t palette[4][4];
    expand_565(color0, palette[0]);
    expand_565(color1, palette[1]);
    for (uint32_t channel = 0u; channel < 3u; channel++) {
        if (four) {
            palette[2][channel] = (uint8_t)((2u * palette[0][channel] + palette[1][channel]) / 3u);
            palette[3][channel] = (uint8_t)((palette[0][channel] + 2u * palette[1][channel]) / 3u);
        } else {
            palette[2][channel] = (uint8_t)((palette[0][channel] + palette[1][channel]) / 2u);
            palette[3][channel] = 0u;
        }
    }
    palette[2][3] = 255u;
    palette[3][3] = four ? 255u : 0u;
    for (uint32_t row = 0u; row < 4u; row++) {
        for (uint32_t column = 0u; column < 4u; column++) {
            memcpy(texels[row * 4u + column], palette[(block[4u + row] >> (2u * column)) & 3u], 4u);
        }
    }
}

static void decode_alpha_block(const uint8_t *block, live_texture_kind kind, uint8_t alpha[16])
{
    if (kind == LIVE_TEXTURE_DXT3) {
        for (uint32_t texel = 0u; texel < 16u; texel++) {
            const uint32_t nibble = (block[texel / 2u] >> (4u * (texel & 1u))) & 15u;
            alpha[texel] = (uint8_t)(nibble * 17u);
        }
        return;
    }
    uint32_t ramp[8];
    ramp[0] = block[0];
    ramp[1] = block[1];
    if (ramp[0] > ramp[1]) {
        for (uint32_t step = 1u; step < 7u; step++) {
            ramp[1u + step] = ((7u - step) * ramp[0] + step * ramp[1]) / 7u;
        }
    } else {
        for (uint32_t step = 1u; step < 5u; step++) {
            ramp[1u + step] = ((5u - step) * ramp[0] + step * ramp[1]) / 5u;
        }
        ramp[6] = 0u;
        ramp[7] = 255u;
    }
    uint64_t bits = 0u;
    for (uint32_t byte = 0u; byte < 6u; byte++) {
        bits |= (uint64_t)block[2u + byte] << (8u * byte);
    }
    for (uint32_t texel = 0u; texel < 16u; texel++) {
        alpha[texel] = (uint8_t)ramp[(bits >> (3u * texel)) & 7u];
    }
}

static void decode_compressed(const live_texture_plan *plan, const uint8_t *source, uint8_t *rgba)
{
    const uint32_t block_bytes = plan->kind == LIVE_TEXTURE_DXT1 ? 8u : 16u;
    const uint32_t blocks_x = (plan->width + 3u) / 4u;
    const uint32_t blocks_y = (plan->height + 3u) / 4u;
    for (uint32_t block_y = 0u; block_y < blocks_y; block_y++) {
        for (uint32_t block_x = 0u; block_x < blocks_x; block_x++) {
            /* Compressed blocks are row-major, not Morton swizzled: xemu
             * pgraph/s3tc.c::s3tc_decompress_2d (478b4f49). The format still
             * supplies exponent dimensions and normalised coordinates. */
            const uint8_t *block = &source[((size_t)block_y * blocks_x + block_x) * block_bytes];
            uint8_t texels[16][4];
            decode_colour_block(plan->kind == LIVE_TEXTURE_DXT1 ? block : block + 8, plan->kind == LIVE_TEXTURE_DXT1, texels);
            if (plan->kind != LIVE_TEXTURE_DXT1) {
                uint8_t alpha[16];
                decode_alpha_block(block, plan->kind, alpha);
                for (uint32_t texel = 0u; texel < 16u; texel++) {
                    texels[texel][3] = alpha[texel];
                }
            }
            for (uint32_t row = 0u; row < 4u && block_y * 4u + row < plan->height; row++) {
                const uint32_t columns = plan->width - block_x * 4u < 4u ? plan->width - block_x * 4u : 4u;
                memcpy(&rgba[((size_t)(block_y * 4u + row) * plan->width + block_x * 4u) * 4u], texels[row * 4u], columns * 4u);
            }
        }
    }
}

static void convert_texel(live_texture_kind kind, const uint8_t *in, uint8_t out[4])
{
    const uint32_t word = kind == LIVE_TEXTURE_A8R8G8B8 || kind == LIVE_TEXTURE_X8R8G8B8
                              ? (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24)
                              : (kind == LIVE_TEXTURE_Y8 || kind == LIVE_TEXTURE_AY8 || kind == LIVE_TEXTURE_A8 ? in[0] : (uint32_t)in[0] | ((uint32_t)in[1] << 8));
    switch (kind) {
    case LIVE_TEXTURE_A8R8G8B8:
    case LIVE_TEXTURE_X8R8G8B8:
        out[0] = (uint8_t)(word >> 16);
        out[1] = (uint8_t)(word >> 8);
        out[2] = (uint8_t)word;
        out[3] = kind == LIVE_TEXTURE_A8R8G8B8 ? (uint8_t)(word >> 24) : 255u;
        break;
    case LIVE_TEXTURE_R5G6B5:
        expand_565((uint16_t)word, out);
        break;
    case LIVE_TEXTURE_A1R5G5B5:
    case LIVE_TEXTURE_X1R5G5B5: {
        const uint32_t red = (word >> 10) & 31u, green = (word >> 5) & 31u, blue = word & 31u;
        out[0] = (uint8_t)((red << 3) | (red >> 2));
        out[1] = (uint8_t)((green << 3) | (green >> 2));
        out[2] = (uint8_t)((blue << 3) | (blue >> 2));
        out[3] = kind == LIVE_TEXTURE_A1R5G5B5 && (word & 0x8000u) == 0u ? 0u : 255u;
        break;
    }
    case LIVE_TEXTURE_A4R4G4B4:
        out[0] = (uint8_t)(((word >> 8) & 15u) * 17u);
        out[1] = (uint8_t)(((word >> 4) & 15u) * 17u);
        out[2] = (uint8_t)((word & 15u) * 17u);
        out[3] = (uint8_t)(((word >> 12) & 15u) * 17u);
        break;
    case LIVE_TEXTURE_Y8:
        out[0] = out[1] = out[2] = (uint8_t)word;
        out[3] = 255u;
        break;
    case LIVE_TEXTURE_A8: /* xemu {ONE, ONE, ONE, RED} */
        out[0] = out[1] = out[2] = 255u;
        out[3] = (uint8_t)word;
        break;
    case LIVE_TEXTURE_A8Y8: /* xemu {RED, RED, RED, GREEN}: byte 0 is Y, byte 1 is A */
        out[0] = out[1] = out[2] = (uint8_t)word;
        out[3] = (uint8_t)(word >> 8);
        break;
    case LIVE_TEXTURE_G8B8: /* xemu {RED, GREEN, RED, GREEN} */
        out[0] = out[2] = (uint8_t)word;
        out[1] = out[3] = (uint8_t)(word >> 8);
        break;
    case LIVE_TEXTURE_R8B8: /* xemu {GREEN, RED, RED, GREEN} */
        out[0] = (uint8_t)(word >> 8);
        out[1] = out[2] = (uint8_t)word;
        out[3] = (uint8_t)(word >> 8);
        break;
    default: /* AY8 */
        out[0] = out[1] = out[2] = out[3] = (uint8_t)word;
        break;
    }
}

bool live_texture_decode(const live_texture_plan *plan, const uint8_t *source, size_t source_length, uint8_t *rgba)
{
    if (!plan->ok || plan->levels > LIVE_TEXTURE_MAX_LEVELS || source_length < plan->source_bytes) {
        return false;
    }
    if (plan->cube) {
        live_texture_plan face = *plan;
        face.cube = false;
        face.faces = 1u;
        face.rgba_bytes = plan->face_rgba_bytes;
        face.source_bytes = plan->source_bytes - 5u * plan->face_source_stride;
        for (uint32_t index = 0u; index < plan->faces; index++) {
            if (!live_texture_decode(&face, source + (size_t)index * plan->face_source_stride, face.source_bytes,
                                     rgba + (size_t)index * plan->face_rgba_bytes)) return false;
        }
        return true;
    }
    if (plan->levels > 1u) {
        for (uint32_t level = 0u; level < plan->levels; ++level) {
            live_texture_plan single = *plan;
            const live_texture_level *mip = &plan->mip[level];
            single.levels = 1u;
            single.width = mip->width;
            single.height = mip->height;
            single.source_bytes = mip->source_bytes;
            if (!live_texture_decode(&single, source + mip->source_offset, mip->source_bytes,
                                     rgba + mip->rgba_offset)) return false;
        }
        return true;
    }
    if (plan->compressed) {
        decode_compressed(plan, source, rgba);
        return true;
    }
    uint32_t bytes = 4u;
    for (size_t index = 0u; index < sizeof formats / sizeof formats[0]; index++) {
        if (formats[index].color_byte == plan->color_byte) {
            bytes = formats[index].bytes;
        }
    }
    for (uint32_t y = 0u; y < plan->height; y++) {
        for (uint32_t x = 0u; x < plan->width; x++) {
            const size_t offset = plan->swizzled ? (size_t)live_texture_swizzle_offset(x, y, plan->width, plan->height) * bytes
                                                 : (size_t)y * plan->pitch + (size_t)x * bytes;
            convert_texel(plan->kind, &source[offset], &rgba[((size_t)y * plan->width + x) * 4u]);
        }
    }
    return true;
}

void live_texture_decode_dxt1_square(const uint8_t *blocks, uint32_t size, uint8_t *rgba)
{
    live_texture_plan plan;
    memset(&plan, 0, sizeof plan);
    plan.ok = true;
    plan.kind = LIVE_TEXTURE_DXT1;
    plan.compressed = true;
    plan.swizzled = true;
    plan.width = plan.height = size;
    plan.source_bytes = (size / 4u) * (size / 4u) * 8u;
    decode_compressed(&plan, blocks, rgba);
}

/* --- cache ------------------------------------------------------------------------------------------------------ */

void live_texture_cache_init(live_texture_cache *cache, bool allow_inferred)
{
    memset(cache, 0, sizeof *cache);
    cache->allow_inferred = allow_inferred;
}

void live_texture_cache_free(live_texture_cache *cache)
{
    for (size_t index = 0u; index < LIVE_TEXTURE_CACHE_ENTRIES; index++) {
        free(cache->entries[index].rgba);
        free(cache->entries[index].source);
    }
    memset(cache, 0, sizeof *cache);
}

size_t live_texture_note_write(live_texture_cache *cache, uint32_t address, size_t bytes)
{
    size_t stale = 0u;
    cache->writes_noted++;
    for (size_t index = 0u; index < LIVE_TEXTURE_CACHE_ENTRIES; index++) {
        live_texture_entry *entry = &cache->entries[index];
        if (entry->in_use && entry->valid &&
            (((uint64_t)address < (uint64_t)entry->read_address + entry->plan.source_bytes &&
              (uint64_t)entry->read_address < (uint64_t)address + bytes) ||
             ((uint64_t)address < (uint64_t)entry->binding.data + entry->plan.source_bytes &&
              (uint64_t)entry->binding.data < (uint64_t)address + bytes))) {
            entry->valid = false;
            stale++;
            cache->invalidations++;
        }
    }
    return stale;
}

bool live_texture_register_target(live_texture_cache *cache, uint32_t id, uint32_t data, uint32_t width, uint32_t height, uint32_t pitch)
{
    for (size_t index = 0u; index < LIVE_TEXTURE_TARGETS; index++) {
        if (!cache->targets[index].used) {
            cache->targets[index] = (live_texture_target){true, true, data, width, height, pitch, id, false};
            return true;
        }
    }
    return false;
}

bool live_texture_register_swizzled_target(live_texture_cache *cache, uint32_t id, uint32_t data, uint32_t width, uint32_t height)
{
    if (!live_texture_register_target(cache, id, data, width, height, width * 4u)) {
        return false;
    }
    for (size_t index = 0u; index < LIVE_TEXTURE_TARGETS; index++) {
        if (cache->targets[index].used && cache->targets[index].id == id) {
            cache->targets[index].swizzled = true;
        }
    }
    return true;
}

void live_texture_clear_targets(live_texture_cache *cache)
{
    memset(cache->targets, 0, sizeof cache->targets);
}

static void note_refusal(live_texture_cache *cache, const live_texture_binding *binding, const char *category, const char *detail)
{
    cache->refusals++;
    for (size_t index = 0u; index < cache->census_count; index++) {
        live_texture_census_entry *entry = &cache->census[index];
        if (entry->category == category && entry->format == binding->format && entry->size_word == binding->size_word &&
            entry->data == binding->data) {
            entry->count++;
            return;
        }
    }
    if (cache->census_count == LIVE_TEXTURE_CENSUS_ENTRIES) {
        cache->census_overflow++;
        return;
    }
    live_texture_census_entry *entry = &cache->census[cache->census_count++];
    entry->category = category;
    entry->format = binding->format;
    entry->size_word = binding->size_word;
    entry->data = binding->data;
    entry->count = 1u;
    (void)snprintf(entry->detail, sizeof entry->detail, "%s", detail);
}

static void fail(live_texture_cache *cache, const live_texture_binding *binding, live_texture_result *result, const char *category,
    const char *detail)
{
    result->source = LIVE_TEXTURE_SOURCE_REFUSED;
    result->refusal = category;
    (void)snprintf(result->detail, sizeof result->detail, "%s", detail);
    note_refusal(cache, binding, category, detail);
}

static bool entry_matches(const live_texture_entry *candidate, const live_texture_binding *binding, uint32_t read_address,
                          uint64_t identity)
{
    return candidate->in_use && candidate->binding.data == binding->data && candidate->binding.format == binding->format &&
           candidate->binding.size_word == binding->size_word && candidate->binding.header == binding->header &&
           candidate->binding.mip_limit == binding->mip_limit && candidate->read_address == read_address &&
           candidate->backing_identity == identity;
}

void live_texture_lookup_resolved(live_texture_cache *cache, const live_texture_binding *binding, live_texture_reader reader, void *context,
    live_texture_resolver resolver, void *resolver_context, live_texture_result *result)
{
    memset(result, 0, sizeof *result);
    live_texture_plan plan;
    if (!live_texture_plan_binding(binding, cache->allow_inferred, &plan)) {
        fail(cache, binding, result, plan.refusal, plan.detail);
        return;
    }
    /* T510 render target rules: an exact Data match is the target, a partial overlap or two matches is refused */
    size_t matches = 0u, match = 0u;
    bool alias = false;
    for (size_t index = 0u; index < LIVE_TEXTURE_TARGETS; index++) {
        const live_texture_target *target = &cache->targets[index];
        if (!target->used) {
            continue;
        }
        if (target->data == binding->data) {
            matches++;
            match = index;
        } else if ((uint64_t)binding->data < (uint64_t)target->data + (uint64_t)target->pitch * target->height &&
                   (uint64_t)target->data < (uint64_t)binding->data + plan.source_bytes) {
            alias = true;
        }
    }
    if (matches > 1u) {
        fail(cache, binding, result, "ambiguous mapping", "Data is drawn into by more than one registered target");
        return;
    }
    if (matches == 1u) {
        const live_texture_target *target = &cache->targets[match];
        /* T1489: a swizzled target is sampled by a swizzled header of its own size (no pitch: the Morton layout has none), a linear one
         * by a linear header with its pitch. The mixed pairs are refused. */
        if (plan.swizzled != target->swizzled || plan.kind != LIVE_TEXTURE_A8R8G8B8 || plan.width != target->width ||
            plan.height != target->height || (!target->swizzled && plan.pitch != target->pitch)) {
            fail(cache, binding, result, "unsupported layout", "Data equals a render target whose format, size or pitch the binding does not match");
            return;
        }
        result->source = LIVE_TEXTURE_SOURCE_TARGET;
        result->width = target->width;
        result->height = target->height;
        result->levels = 1u;
        result->target_id = target->id;
        result->target_swizzled = target->swizzled;
        cache->target_samples++;
        return;
    }
    if (alias) {
        fail(cache, binding, result, "alias", "binding overlaps a render target at a different offset, only an exact Data match is taken");
        return;
    }
    uint32_t read_address = binding->data;
    uint64_t identity = 0u;
    const char *refusal = NULL;
    if (resolver != NULL && !resolver(resolver_context, binding, plan.source_bytes,
                                     &read_address, &identity, &refusal)) {
        fail(cache, binding, result, "resource resolution", refusal != NULL ? refusal : "resource resolution failed");
        return;
    }
    uint8_t *fresh = NULL;
    bool compared_this_serial = false;
    if (resolver != NULL && cache->input_serial != 0u) {
        for (size_t index = 0u; index < LIVE_TEXTURE_CACHE_ENTRIES && !compared_this_serial; index++) {
            const live_texture_entry *candidate = &cache->entries[index];
            compared_this_serial = candidate->in_use && candidate->valid && candidate->checked_serial == cache->input_serial &&
                                   entry_matches(candidate, binding, read_address, identity);
        }
    }
    if (resolver != NULL && !compared_this_serial) {
        fresh = malloc(plan.source_bytes);
        if (fresh == NULL || reader == NULL || !reader(context, read_address, fresh, plan.source_bytes)) {
            free(fresh);
            fail(cache, binding, result, "unreadable bytes", "resolved resource bytes cannot be read");
            return;
        }
    }
    live_texture_entry *entry = NULL;
    for (size_t index = 0u; index < LIVE_TEXTURE_CACHE_ENTRIES; index++) {
        if (entry_matches(&cache->entries[index], binding, read_address, identity)) {
            entry = &cache->entries[index];
        }
    }
    if (entry == NULL) {
        for (size_t index = 0u; index < LIVE_TEXTURE_CACHE_ENTRIES && entry == NULL; index++) {
            if (!cache->entries[index].in_use) {
                entry = &cache->entries[index];
            }
        }
        if (entry == NULL) {
            entry = &cache->entries[0];
            for (size_t index = 1u; index < LIVE_TEXTURE_CACHE_ENTRIES; index++) {
                if (cache->entries[index].last_use < entry->last_use) {
                    entry = &cache->entries[index];
                }
            }
        }
        entry->in_use = true;
        entry->valid = false;
        entry->uploaded_generation = 0u;
        entry->checked_serial = 0u;
    }
    entry->last_use = ++cache->clock;
    if (entry->valid && fresh != NULL &&
        (entry->source == NULL || memcmp(entry->source, fresh, plan.source_bytes) != 0)) {
        entry->valid = false;
        cache->invalidations++;
    }
    if (entry->valid) {
        cache->hits++;
        if (compared_this_serial) cache->compares_skipped++;
    } else {
        uint8_t *source = fresh != NULL ? fresh : malloc(plan.source_bytes);
        uint8_t *rgba = realloc(entry->rgba, plan.rgba_bytes);
        if (rgba != NULL) {
            entry->rgba = rgba;
        }
        if (source == NULL || rgba == NULL ||
            (fresh == NULL && (reader == NULL || !reader(context, read_address, source, plan.source_bytes)))) {
            free(source);
            entry->in_use = entry->valid = false;
            fail(cache, binding, result, "unreadable bytes", "the texture bytes cannot be read from guest memory");
            return;
        }
        const uint64_t decode_start = gpu_phase_now();
        (void)live_texture_decode(&plan, source, plan.source_bytes, entry->rgba);
        gpu_phase_add(GPU_PHASE_TEXTURE_DECODE, decode_start); /* T1289: for the guest frame trace */
        free(entry->source);
        entry->source = fresh;
        if (fresh == NULL) free(source);
        fresh = NULL;
        entry->read_address = read_address;
        entry->backing_identity = identity;
        entry->binding = *binding;
        entry->plan = plan;
        entry->valid = true;
        entry->generation++;
        cache->decodes++;
    }
    entry->checked_serial = resolver != NULL ? cache->input_serial : 0u;
    free(fresh);
    result->source = LIVE_TEXTURE_SOURCE_GUEST;
    result->rgba = entry->rgba;
    result->width = entry->plan.width;
    result->height = entry->plan.height;
    result->levels = entry->plan.levels;
    result->cube = entry->plan.cube;
    result->generation = entry->generation;
    result->entry = (uint32_t)(entry - cache->entries);
    result->needs_upload = entry->uploaded_generation != entry->generation;
    result->upload_bytes = result->needs_upload ? entry->plan.rgba_bytes : 0u;
}

void live_texture_lookup(live_texture_cache *cache, const live_texture_binding *binding,
                         live_texture_reader reader, void *context, live_texture_result *result)
{
    live_texture_lookup_resolved(cache, binding, reader, context, NULL, NULL, result);
}

void live_texture_mark_uploaded(live_texture_cache *cache, uint32_t entry, uint32_t generation)
{
    if (entry < LIVE_TEXTURE_CACHE_ENTRIES) {
        cache->entries[entry].uploaded_generation = generation;
    }
}

bool live_texture_census_line(const live_texture_cache *cache, size_t index, char *out, size_t out_bytes)
{
    if (index >= cache->census_count) {
        return false;
    }
    const live_texture_census_entry *entry = &cache->census[index];
    (void)snprintf(out, out_bytes, "refused texture binding: %s Format 0x%08X Size 0x%08X Data 0x%07X x%llu: %s", entry->category,
        (unsigned)entry->format, (unsigned)entry->size_word, (unsigned)entry->data, (unsigned long long)entry->count, entry->detail);
    return true;
}
