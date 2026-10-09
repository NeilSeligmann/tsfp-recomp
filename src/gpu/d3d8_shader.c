/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_shader.h"
#include "d3d8_pixel_constants.h"
#include "d3d8_pixel_bind.h"
#include "d3d8_shader_bind.h"
#include "d3d8_vertex_program.h"

#include <float.h>
#include <string.h>

#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "kernel_call.h"

#define DEV_PIXEL_SHADER 0x0784u
#define DEV_PIXEL_SHADER_INPUTS 0x0788u
#define GLOBAL_TEXTURE_FACTOR 0x003E3F10u

static uint32_t pixel_shader_apply(const d3d8_pixel_bind_plan *plan)
{
    const uint32_t wrapper = plan->wrapper;
    if (wrapper != 0u) {
        const uint32_t *words = plan->words;
        const uint32_t previous = plan->previous;
        const uint32_t inputs = words[8] | words[9];
        uint32_t dirty = d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | 0x4000u;
        if (inputs == 0u && d3d8_device_load32(DEV_PIXEL_SHADER_INPUTS) != 0u) dirty |= 0x2000u;
        d3d8_device_store32(DEV_PIXEL_SHADER, wrapper);
        d3d8_device_store32(DEV_PIXEL_SHADER_INPUTS, inputs);
        d3d8_device_store32(0x78Cu, words[59] & 0x100u);
        d3d8_device_store32(0x790u, words[54]);
        d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK, dirty);
        uint32_t cursor = d3d8_pushbuffer_begin();
        /* The refill of 0x003D93FC is the real one: it must land where the plan put the site. */
        if (cursor != plan->site[0])
            d3d8_hle_fatal(0x003D9320u, "the pixel binder's refill did not land where it was previewed");
        if ((d3d8_device_load32(D3D8_DEV_FLAGS) & 0x10u) == 0u) {
            for (uint32_t i = 0u; i < 57u; i++) d3d8_guest_store32(0x003E3CC0u + i * 4u, words[i]);
            d3d8_guest_store32(0x003E3EE0u, words[54]);
        }
        /* When enabling a shader from the null binding, 0x003D77F0 first selects
         * the programmable texture-state rows. These are cached guest state. */
        if (previous == 0u) {
            for (uint32_t row = 0u; row < 3u; row++) {
                d3d8_guest_store32(cursor, 0x00181B68u + row * 0x40u);
                for (uint32_t i = 0u; i < 6u; i++) {
                    d3d8_guest_store32(cursor + 4u + i * 4u,
                        plan->rows[row * 6u + i]);
                }
                cursor += 28u;
            }
        }
        /* The shader writer copies the program's contiguous register blocks; word
         * 54 is cached for the separate texture-stage emitter, never sent to 0x1D90. */
        const uint32_t headers[] = {0x00200260u, 0x00800A60u, 0x000417F8u,
                                    0x00081E20u, 0x00241E40u, 0x00081E74u};
        const uint32_t offsets[] = {0u, 10u, 42u, 43u, 45u, 55u};
        const uint32_t counts[] = {8u, 32u, 1u, 2u, 9u, 2u};
        for (uint32_t block = 0u; block < 6u; block++) {
            d3d8_guest_store32(cursor, headers[block]);
            cursor += 4u;
            for (uint32_t i = 0u; i < counts[block]; i++) {
                d3d8_guest_store32(cursor, words[offsets[block] + i]);
                cursor += 4u;
            }
        }
        if (inputs != 0u) {
            d3d8_guest_store32(cursor, 0x00080288u);
            d3d8_guest_store32(cursor + 4u, words[8]);
            d3d8_guest_store32(cursor + 8u, words[9]);
            cursor += 12u;
        }
        d3d8_pushbuffer_end(cursor);
        return cursor;
    }
    d3d8_device_store32(DEV_PIXEL_SHADER, 0u);
    uint32_t dirty = d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | 0x4800u;
    if (d3d8_device_load32(DEV_PIXEL_SHADER_INPUTS) != 0u) {
        dirty |= 0x2000u;
    }
    d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK, dirty);

    /* 0x003D7150: restore the fixed-function texture factor, 16 consecutive factors. */
    uint32_t cursor = d3d8_pushbuffer_begin();
    if (cursor != plan->site[0])
        d3d8_hle_fatal(0x003D7150u, "the pixel binder's refill did not land where it was previewed");
    d3d8_guest_store32(cursor, 0x00400A60u);
    const uint32_t factor = plan->factor;
    for (uint32_t i = 0u; i < 16u; i++) {
        d3d8_guest_store32(cursor + 4u + i * 4u, factor);
    }
    cursor += 68u;
    d3d8_guest_store32(GLOBAL_TEXTURE_FACTOR, factor);
    d3d8_pushbuffer_end(cursor);

    /* 0x003D77F0 selects the fixed-function rows after +0x784 has been cleared.
     * The original emits three headers and six values per row (method stride 0x40,
     * source stride 0x80), then null shader sets the other-stage input word. */
    cursor = d3d8_pushbuffer_begin();
    if (cursor != plan->site[1])
        d3d8_hle_fatal(0x003D9374u, "the pixel binder's refill did not land where it was previewed");
    for (uint32_t row = 0u; row < 3u; row++) {
        d3d8_guest_store32(cursor, 0x00181B68u + row * 0x40u);
        for (uint32_t i = 0u; i < 6u; i++) {
            d3d8_guest_store32(cursor + 4u + i * 4u,
                              plan->rows[row * 6u + i]);
        }
        cursor += 28u;
    }
    d3d8_guest_store32(cursor, 0x00041E78u);
    d3d8_guest_store32(cursor + 4u, 0x00210000u);
    cursor += 8u;
    d3d8_pushbuffer_end(cursor);
    return cursor;
}

uint32_t d3d8_set_pixel_shader_v(uint32_t wrapper)
{
    d3d8_pixel_bind_plan plan;
    d3d8_pixel_bind_prepare(0x003D9320u, wrapper, false, &plan);
    return pixel_shader_apply(&plan);
}
uint32_t d3d8_set_pixel_shader(uint32_t definition)
{
    d3d8_pixel_bind_plan plan;
    d3d8_pixel_bind_prepare(0x003D92E0u, definition, true, &plan);
    if (definition != 0u) {
        d3d8_guest_store32(plan.wrapper, 1u);
        d3d8_guest_store32(plan.wrapper + 4u, 0u);
        d3d8_guest_store32(plan.wrapper + 8u, definition);
    }
    return pixel_shader_apply(&plan);
}

uint32_t d3d8_upload_vertex_shader_constants(uint32_t packed, uint32_t index)
{
    return d3d8_upload_vertex_program(packed, index);
}

/* Float32 stores are explicit: the original evaluates in x87 extended precision,
 * then rounds at fstp dword. Do not contract these expressions or change the locals
 * below to float: 0x003D6D20 rounds half-width/height before adding their offsets. */
static long double device_float(uint32_t offset)
{
    const uint32_t bits = d3d8_device_load32(offset);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return (long double)value;
}

static uint32_t float_word(long double value)
{
    const float rounded = (float)value;
    uint32_t bits;
    memcpy(&bits, &rounded, sizeof(bits));
    return bits;
}

static long double rounded_float(long double value)
{
    return (long double)(float)value;
}

static uint32_t vertex_viewport_emit(uint32_t cursor, uint32_t shader_flags)
{
    if (LDBL_MANT_DIG != 64) {
        d3d8_hle_fatal(0x003D7860u, "viewport emitter requires x87-width long double");
    }
    const uint32_t flags = d3d8_device_load32(D3D8_DEV_FLAGS);
    long double x = (long double)d3d8_device_load32(0xEE0u) * device_float(0x95Cu);
    long double y = (long double)d3d8_device_load32(0xEE4u) * device_float(0x960u);
    x += device_float(0xEF8u);
    y += device_float(0xEFCu);
    if ((flags & 0x8000u) != 0u && d3d8_guest_load32(0x003E3F20u) != 0u) {
        x -= 0.5L;
        y -= 0.5L;
    }
    const bool programmable = (shader_flags & 0x12u) != 0u;
    if (programmable) {
        /* 0x003D6D20's four stack float temporaries, then offset and scale vectors. */
        const long double half_width = rounded_float(
            (long double)d3d8_device_load32(0xEE8u) * device_float(0x95Cu) * 0.5L);
        const long double half_height = rounded_float(
            (long double)d3d8_device_load32(0xEECu) * device_float(0x960u) * -0.5L);
        const long double depth_offset = rounded_float(device_float(0xEF0u) * device_float(0x948u));
        const long double depth_scale = rounded_float(
            (device_float(0xEF4u) - device_float(0xEF0u)) * device_float(0x948u));
        if ((flags & 0x200u) == 0u) {
            const uint32_t offset[4] = {float_word(x + half_width), float_word(y - half_height),
                                        float_word(depth_offset), 0u};
            const uint32_t scale[4] = {float_word(half_width), float_word(half_height),
                                       float_word(depth_scale), 0u};
            d3d8_guest_store32(cursor, 0x00100A20u);
            d3d8_guest_store32(cursor + 20u, 0x00100AF0u);
            for (uint32_t i = 0u; i < 4u; i++) {
                d3d8_guest_store32(cursor + 4u + i * 4u, offset[i]);
                d3d8_guest_store32(cursor + 24u + i * 4u, scale[i]);
            }
            cursor += 40u;
        }
    } else {
        d3d8_guest_store32(cursor, 0x00100A20u);
        d3d8_guest_store32(cursor + 4u, float_word(x));
        d3d8_guest_store32(cursor + 8u, float_word(y));
        d3d8_guest_store32(cursor + 12u, 0u);
        d3d8_guest_store32(cursor + 16u, 0u);
        cursor += 20u;
    }
    const uint32_t low = programmable ? 0x1C10u : 0x1C08u;
    const uint32_t high = low + 4u;
    const uint32_t cache_flag = programmable ? 2u : 1u;
    if ((d3d8_device_load32(0x1C18u) & cache_flag) == 0u) {
        if (d3d8_guest_load32(0x003E3EFCu) == 2u) {
            long double minimum = device_float(0x944u) * device_float(0x93Cu);
            minimum *= device_float(0x948u);
            d3d8_device_store32(low, float_word(minimum));
            d3d8_device_store32(high, d3d8_device_load32(0x948u));
        } else if (programmable) {
            d3d8_device_store32(low, 0u);
            d3d8_device_store32(high, d3d8_device_load32(0x948u));
        } else {
            d3d8_device_store32(low, float_word(device_float(0xEF0u) * device_float(0x948u)));
            d3d8_device_store32(high, float_word(device_float(0xEF4u) * device_float(0x948u)));
        }
    }
    /* 0x003D7A24..0x003D7A3C captures both cache values before packet
     * publication. The push cursor may alias those cache words. */
    const uint32_t cached_low = d3d8_device_load32(low);
    const uint32_t cached_high = d3d8_device_load32(high);
    d3d8_guest_store32(cursor + 4u, cached_low);
    d3d8_guest_store32(cursor, 0x00080394u);
    d3d8_guest_store32(cursor + 8u, cached_high);
    return cursor + 12u;
}

uint32_t d3d8_viewport_emit(uint32_t cursor, uint32_t shader_flags)
{
    return vertex_viewport_emit(cursor, shader_flags);
}

void d3d8_shader_create(void)
{
    /* Measured original CreateDevice defaults: built-in fixed-function declaration. */
    d3d8_device_store32(0x794u, 0x003E2D80u);
    d3d8_device_store32(0x798u, 2u);
    d3d8_device_store32(0x79Cu, 0u);

    /* 0x003DB0B1 (T870): after its two built-in SetVertexShader calls the original zeroes the 0x46 dwords at
     * 0x003E2C68 (the title's declaration header and its 64 dword attribute cache) and stores flags 0x10 at
     * 0x003E2C6C, header word 1. The title binds that header at 0x003D5630, so the binder's
     * `(flags ^ previous) & 0x13` sees 0x10 against the built-in declaration's 0 and writes the program mode 6.
     * Without these two stores the flags stay 0 for the whole run and every copy restore (0x003D8xxx, flags & 0x12
     * == 0) leaves the execution mode at 4. */
    for (uint32_t offset = 0u; offset < 0x46u * 4u; offset += 4u) {
        d3d8_guest_store32(0x003E2C68u + offset, 0u);
    }
    d3d8_guest_store32(0x003E2C6Cu, 0x10u);

    /* CreateDevice emits the default transform execution mode before the first draw.
     * Its declaration flags are zero at creation, so the second data word is flags & 1 = 0.
     * Keep this in shader creation, which initialise_device calls immediately after creating
     * the pushbuffer, matching the original packet's position in the init stream. */
    const uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, 0x00081E94u);
    d3d8_guest_store32(cursor + 4u, 6u);
    d3d8_guest_store32(cursor + 8u, 0u);
    d3d8_pushbuffer_end(cursor + 12u);
}

static uint32_t vertex_shader_handle_apply(uint32_t tagged, uint32_t index)
{
    if (tagged != 0u) {
        const uint32_t old = d3d8_device_load32(0x794u);
        const uint32_t header = tagged - 1u;
        const uint32_t flags = d3d8_guest_load32(header + 4u);
        const uint32_t previous = d3d8_guest_load32(old + 4u);
        d3d8_device_store32(0x794u, header);
        d3d8_device_store32(0x798u, tagged);
        d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK,
                           d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | 0x70u);
        if (((flags ^ previous) & 0x13u) != 0u) {
            d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK,
                               d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | 0x1000u);
            uint32_t position = vertex_viewport_emit(d3d8_pushbuffer_begin(), flags);
            d3d8_guest_store32(position, 0x00081E94u);
            d3d8_guest_store32(position + 4u, 6u);
            d3d8_guest_store32(position + 8u, flags & 1u);
            d3d8_pushbuffer_end(position + 12u);
        }
    }
    const uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, 0x00041EA0u);
    d3d8_guest_store32(cursor + 4u, index);
    d3d8_pushbuffer_end(cursor + 8u);
    d3d8_device_store32(0x79Cu, index);
    return cursor + 8u;
}

uint32_t d3d8_set_vertex_shader_handle(uint32_t tagged, uint32_t index)
{
    d3d8_shader_bind_prepare(0x003D5A50u, tagged, 0u, NULL);
    return vertex_shader_handle_apply(tagged, index);
}
uint32_t d3d8_set_vertex_shader(uint32_t declaration, uint32_t index)
{
    uint32_t words[64];
    const uint32_t tagged = declaration ? 0x003E2C69u : 0u;
    d3d8_shader_bind_prepare(0x003D5630u, tagged, declaration, words);
    if (declaration != 0u) {
        for (uint32_t i = 0u; i < 64u; i++)
            d3d8_guest_store32(0x003E2C7Cu + i * 4u, words[i]);
    }
    return vertex_shader_handle_apply(tagged, index);
}

/* MINPS/MAXPS use the second operand for unordered inputs: all NaNs clamp to 1.
 * Compute float32 *255 rounding and CVTSS2SI round-to-nearest-even with integers,
 * independent of the host floating-point environment. Values below exponent 118
 * cannot round to a nonzero byte, including every subnormal. */
static uint32_t color_byte(uint32_t bits)
{
    const uint32_t exponent = (bits >> 23) & 0xFFu;
    const uint32_t fraction = bits & 0x7FFFFFu;
    if (exponent == 0xFFu && fraction != 0u) return 255u;
    if ((bits & 0x80000000u) != 0u) return 0u;
    if (bits >= 0x3F800000u) return 255u;
    if (exponent < 118u) return 0u;
    const uint64_t exact = (uint64_t)(fraction | 0x800000u) * 255u;
    uint32_t shift = 0u;
    while ((exact >> shift) > 0xFFFFFFu) shift++;
    uint64_t mantissa = exact >> shift;
    if (shift != 0u) {
        const uint64_t remainder = exact & ((UINT64_C(1) << shift) - 1u);
        const uint64_t half = UINT64_C(1) << (shift - 1u);
        if (remainder > half || (remainder == half && (mantissa & 1u) != 0u)) mantissa++;
    }
    if (mantissa == 0x1000000u) {
        mantissa >>= 1;
        shift++;
    }
    const uint32_t fractional_bits = 150u - exponent - shift;
    uint32_t integer = (uint32_t)(mantissa >> fractional_bits);
    const uint64_t remainder = mantissa & ((UINT64_C(1) << fractional_bits) - 1u);
    const uint64_t half = UINT64_C(1) << (fractional_bits - 1u);
    if (remainder > half || (remainder == half && (integer & 1u) != 0u)) integer++;
    return integer;
}

uint32_t d3d8_set_pixel_shader_constants(uint32_t index, uint32_t data, uint32_t count)
{
    d3d8_pixel_constants_plan plan;
    d3d8_pixel_constants_prepare(index, data, count, &plan);
    for (uint32_t vector = 0u; vector < count; vector++, index++, data += 16u) {
        const uint32_t packed = (color_byte(plan.source[vector][0]) << 16) |
                               (color_byte(plan.source[vector][1]) << 8) |
                               color_byte(plan.source[vector][2]) |
                               (color_byte(plan.source[vector][3]) << 24);
        d3d8_device_store32(0x8E4u + index * 4u, packed);
        uint32_t cursor = d3d8_pushbuffer_begin();
        /* The refill of 0x003D95BA is the real one: it must land where the plan put the vector. */
        if (cursor != plan.site[vector])
            d3d8_hle_fatal(0x003D9520u, "the pixel constants' refill did not land where it was previewed");
        const uint32_t usage = plan.usage[vector];
        for (uint32_t factor = 0u; factor < 3u; factor++) {
            const uint32_t map = usage ^ plan.maps[factor];
            const uint32_t stages = factor == 2u ? 2u : 8u;
            const uint32_t method = factor == 2u ? 0x1E20u : 0x0A60u + factor * 0x20u;
            const uint32_t shadow = factor == 2u ? 0x003E3D6Cu : 0x003E3CE8u + factor * 0x20u;
            for (uint32_t stage = 0u; stage < stages; stage++) {
                if (((map >> (stage * 4u)) & 0xFu) == 0u) {
                    d3d8_guest_store32(shadow + stage * 4u, packed);
                    d3d8_guest_store32(cursor, 0x00040000u | (method + stage * 4u));
                    d3d8_guest_store32(cursor + 4u, packed);
                    cursor += 8u;
                }
            }
        }
        if (index == 0u) {
            d3d8_guest_store32(cursor, 0x000C181Cu);
            for (uint32_t component = 0u; component < 3u; component++) {
                d3d8_guest_store32(cursor + 4u + component * 4u,
                                   plan.source[vector][component]);
            }
            cursor += 16u;
        }
        d3d8_pushbuffer_end(cursor);
    }
    /* The original's final dec eax leaves zero, including count==0. */
    return 0u;
}

static uint32_t argument(void *context, uint32_t address)
{
    uint32_t value = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, 0u, &value)) {
        d3d8_hle_fatal(address, "pixel shader argument cannot be read");
    }
    return value;
}

static uint32_t handler_shader(void *context)
{
    return d3d8_set_pixel_shader(argument(context, 0x003D92E0u));
}

static uint32_t handler_shader_v(void *context)
{
    return d3d8_set_pixel_shader_v(argument(context, 0x003D9320u));
}

static uint32_t handler_upload_constants(void *context)
{
    uint32_t index = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, 1u, &index)) {
        d3d8_hle_fatal(0x003D59E0u, "packed constant register index cannot be read");
    }
    return d3d8_upload_vertex_shader_constants(argument(context, 0x003D59E0u), index);
}

static uint32_t vertex_index(void *context, uint32_t address)
{
    uint32_t index = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, 1u, &index)) {
        d3d8_hle_fatal(address, "vertex shader index cannot be read");
    }
    return index;
}

static uint32_t handler_vertex_shader(void *context)
{
    return d3d8_set_vertex_shader(argument(context, 0x003D5630u),
                                 vertex_index(context, 0x003D5630u));
}

static uint32_t handler_vertex_shader_handle(void *context)
{
    return d3d8_set_vertex_shader_handle(argument(context, 0x003D5A50u),
                                        vertex_index(context, 0x003D5A50u));
}

static uint32_t handler_pixel_constants(void *context)
{
    uint32_t data = 0u, count = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, 1u, &data) ||
        !kernel_frame_arg((const kernel_call_frame *)context, 2u, &count)) {
        d3d8_hle_fatal(0x003D9520u, "pixel constant data/count cannot be read");
    }
    return d3d8_set_pixel_shader_constants(argument(context, 0x003D9520u), data, count);
}

size_t d3d8_shader_register(void)
{
    return (size_t)d3d8_hle_register(0x003D92E0u, handler_shader) +
           (size_t)d3d8_hle_register(0x003D9320u, handler_shader_v) +
           (size_t)d3d8_hle_register(0x003D59E0u, handler_upload_constants) +
           (size_t)d3d8_hle_register(0x003D5630u, handler_vertex_shader) +
           (size_t)d3d8_hle_register(0x003D5A50u, handler_vertex_shader_handle) +
           (size_t)d3d8_hle_register(0x003D9520u, handler_pixel_constants);
}
