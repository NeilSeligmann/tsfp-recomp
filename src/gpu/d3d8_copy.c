/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_copy.h. Every function carries the address of the original it ports.
 */

#include "d3d8_copy.h"

#include <float.h>
#include <string.h>

#include "d3d8_gpu.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_immediate.h"
#include "d3d8_pushbuffer.h"
#include "d3d8_resource.h"
#include "d3d8_set_state.h"
#include "d3d8_shader.h"
#include "kernel_call.h"

#define COPY_RENDER_STATES 0x003E1C40u /* 20 pairs, state then value */
#define COPY_STAGE_STATES 0x003E1BE8u  /* 11 pairs, stage 0 state then value */
#define COPY_PROGRAM 0x003E1CE0u       /* the two instruction vertex program, 8 dwords */
#define RENDER_STATE_COUNT 20u
#define STAGE_STATE_COUNT 11u
#define SHADOW_STATES 0x39u
#define PROGRAM_DWORDS 8u

#define GLOBAL_PS_TEXTURE_MODES 0x003E3EE0u
#define GLOBAL_FILTER_STATE 0x003E3EB8u /* render state 0x7E */
#define GLOBAL_STAGE0_MAG 0x003E3ACCu
#define GLOBAL_STAGE0_MIN 0x003E3AD0u
#define GLOBAL_STAGE1_OPERATION 0x003E3B70u

#define DEV_SHADER 0x0784u
#define DEV_SHADER_HANDLE 0x0798u
#define DEV_DECLARATION 0x0794u
#define DEV_VERTEX_INDEX 0x079Cu
#define DEV_RENDER_TARGET 0x1A04u
#define DEV_SCALE_X 0x096Cu
#define DEV_SCALE_Y 0x0970u
#define DEV_SAVED_PROGRAM 0x10A8u
#define FLAG_FORCE_STATES 0x10u

#define CONSTANT_TWO 0x00475D10u
#define CONSTANT_FOUR 0x00475DB4u
#define CONSTANT_TWO_POW_32 0x00475CCCu

static uint32_t stage_state_address(uint32_t state)
{
    return D3D8_TS_SHADOW + state * 4u;
}

void d3d8_copy_snapshot(uint32_t *frame)
{
    frame[0] = d3d8_device_load32(DEV_SHADER);
    frame[1] = d3d8_device_load32(DEV_SHADER_HANDLE);
    frame[2] = d3d8_guest_load32(GLOBAL_STAGE1_OPERATION);
    for (uint32_t index = 0u; index < RENDER_STATE_COUNT; index++) {
        const uint32_t state = d3d8_guest_load32(COPY_RENDER_STATES + index * 8u);
        frame[4u + index] = d3d8_guest_load32(D3D8_GLOBAL_RS_SHADOW + state * 4u);
    }
    for (uint32_t index = 0u; index < STAGE_STATE_COUNT; index++) {
        const uint32_t state = d3d8_guest_load32(COPY_STAGE_STATES + index * 8u);
        frame[0x18u + index] = d3d8_guest_load32(stage_state_address(state));
    }
    if (frame[0] != 0u) {
        for (uint32_t index = 0u; index < SHADOW_STATES; index++) {
            frame[0x23u + index] = d3d8_guest_load32(D3D8_GLOBAL_RS_SHADOW + index * 4u);
        }
        frame[3] = d3d8_guest_load32(GLOBAL_PS_TEXTURE_MODES);
    }
}

/* 0x003D5C10 and 0x003D55C0: load a vertex program of `count` dwords from guest memory at `source`.
 * One reservation, the 0x41E9C slot, then chunks of at most 32 dwords each with its own header. */
static void push_program(uint32_t source, uint32_t count)
{
    uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, 0x00041E9Cu);
    d3d8_guest_store32(cursor + 4u, 0u);
    cursor += 8u;
    while (count != 0u) {
        const uint32_t chunk = count > 0x1Fu ? 0x20u : count;
        d3d8_guest_store32(cursor, chunk * 0x40000u + 0xB00u);
        for (uint32_t index = 0u; index < chunk; index++) {
            d3d8_guest_store32(cursor + 4u + index * 4u, d3d8_guest_load32(source + index * 4u));
        }
        source += chunk * 4u;
        count -= chunk;
        cursor += (chunk + 1u) * 4u;
    }
    d3d8_pushbuffer_end(cursor);
}

static uint32_t device_flags_byte(void)
{
    return d3d8_device_load32(D3D8_DEV_FLAGS) & 0xFFu;
}

void d3d8_copy_setup(void)
{
    for (uint32_t index = 0u; index < RENDER_STATE_COUNT; index++) {
        const int32_t state = (int32_t)d3d8_guest_load32(COPY_RENDER_STATES + index * 8u);
        const uint32_t value = d3d8_guest_load32(COPY_RENDER_STATES + index * 8u + 4u);
        const uint32_t shadow = d3d8_guest_load32(D3D8_GLOBAL_RS_SHADOW + (uint32_t)state * 4u);
        if ((state < 0x5C && (device_flags_byte() & FLAG_FORCE_STATES) != 0u) || value != shadow) {
            d3d8_set_render_state_notinline(state, value);
        }
    }
    for (uint32_t index = 0u; index < STAGE_STATE_COUNT; index++) {
        const uint32_t state = d3d8_guest_load32(COPY_STAGE_STATES + index * 8u);
        const uint32_t value = d3d8_guest_load32(COPY_STAGE_STATES + index * 8u + 4u);
        if (value != d3d8_guest_load32(stage_state_address(state))) {
            d3d8_set_texture_stage_state_notinline(0u, (int32_t)state, value);
        }
    }
    (void)d3d8_set_pixel_shader_v(0u);
    const uint32_t filter = d3d8_guest_load32(GLOBAL_FILTER_STATE);
    if (filter != d3d8_guest_load32(GLOBAL_STAGE0_MIN)) {
        d3d8_set_texture_stage_state_notinline(0u, 4, filter);
    }
    if (filter != d3d8_guest_load32(GLOBAL_STAGE0_MAG)) {
        d3d8_set_texture_stage_state_notinline(0u, 3, filter);
    }
    if (d3d8_guest_load32(GLOBAL_STAGE1_OPERATION) != 1u) {
        d3d8_set_texture_stage_state_notinline(1u, (int32_t)D3D8_TS_COLOR_OP, 1u);
    }
    d3d8_run_dirty_cascade();
    push_program(COPY_PROGRAM, PROGRAM_DWORDS);
    const uint32_t cursor = d3d8_pushbuffer_begin();
    const uint32_t words[8] = {0x00081E94u, 6u, 0u, 0x00041EA0u, 0u, 0x00080394u, 0u, 0x4B7FFFFFu};
    for (uint32_t index = 0u; index < 8u; index++) {
        d3d8_guest_store32(cursor + index * 4u, words[index]);
    }
    d3d8_pushbuffer_end(cursor + 32u);
}

static float float_of(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t bits_of(long double value)
{
    const float rounded = (float)value;
    uint32_t bits;
    memcpy(&bits, &rounded, sizeof(bits));
    return bits;
}

void d3d8_copy_draw_triangle(void)
{
#if LDBL_MANT_DIG != 64
    d3d8_hle_fatal(0x003D8990u, "the copy triangle requires x87 extended precision");
#endif
    const float scale_x = float_of(d3d8_device_load32(DEV_SCALE_X));
    const float scale_y = float_of(d3d8_device_load32(DEV_SCALE_Y));
    const uint32_t target = d3d8_device_load32(DEV_RENDER_TARGET);
    const uint32_t size = d3d8_guest_load32(target + 0x10u);
    const uint32_t width = size == 0u ? 1u << ((d3d8_guest_load32(target + 0xCu) >> 0x14) & 0xFu)
                                      : (size & 0xFFFu) + 1u;
    const uint32_t height = size == 0u ? 1u << (d3d8_guest_load8(target + 0xFu) & 0xFu)
                                       : ((size >> 12) & 0xFFFu) + 1u;
    const float two = float_of(d3d8_guest_load32(CONSTANT_TWO));
    const float four = float_of(d3d8_guest_load32(CONSTANT_FOUR));
    const float two_pow_32 = float_of(d3d8_guest_load32(CONSTANT_TWO_POW_32));
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    const uint32_t minification = d3d8_guest_load32(GLOBAL_STAGE0_MIN);
    if (minification == 4u || minification == 5u) {
        if (scale_x == two) offset_x = 0.5f;
        if (scale_y == two) offset_y = 0.5f;
    }
    const uint32_t x_bits = bits_of(offset_x);
    const uint32_t y_bits = bits_of(offset_y);
    d3d8_begin(5u);
    d3d8_set_vertex_data_2f(9u, x_bits, y_bits);
    d3d8_set_vertex_data_2f(0u, 0u, 0u);
    long double wide = (long double)(int32_t)width;
    if ((int32_t)width < 0) wide += (long double)two_pow_32;
    const float wide_float = (float)wide;
    d3d8_set_vertex_data_2f(9u,
                            bits_of((long double)wide_float * (long double)scale_x *
                                        (long double)four +
                                    (long double)offset_x),
                            y_bits);
    d3d8_set_vertex_data_2f(0u, bits_of((long double)wide_float * (long double)four), 0u);
    long double tall = (long double)(int32_t)height;
    if ((int32_t)height < 0) tall += (long double)two_pow_32;
    const float tall_float = (float)tall;
    d3d8_set_vertex_data_2f(9u, x_bits,
                            bits_of(tall * (long double)scale_y * (long double)four +
                                    (long double)offset_y));
    d3d8_set_vertex_data_2f(0u, 0u, bits_of((long double)tall_float * (long double)four));
    d3d8_end();
}

void d3d8_copy_restore(const uint32_t *frame)
{
    (void)d3d8_set_pixel_shader_v(frame[0]);
    const uint32_t flags = d3d8_device_load32(D3D8_DEV_FLAGS);
    if (frame[2] != d3d8_guest_load32(GLOBAL_STAGE1_OPERATION)) {
        d3d8_set_texture_stage_state_notinline(1u, (int32_t)D3D8_TS_COLOR_OP, frame[2]);
    }
    for (uint32_t index = 0u; index < RENDER_STATE_COUNT; index++) {
        const int32_t state = (int32_t)d3d8_guest_load32(COPY_RENDER_STATES + index * 8u);
        if ((state > 0x5B || (device_flags_byte() & FLAG_FORCE_STATES) == 0u) &&
            frame[4u + index] !=
                d3d8_guest_load32(D3D8_GLOBAL_RS_SHADOW + (uint32_t)state * 4u)) {
            d3d8_set_render_state_notinline(state, frame[4u + index]);
        }
    }
    for (uint32_t index = 0u; index < STAGE_STATE_COUNT; index++) {
        const uint32_t state = d3d8_guest_load32(COPY_STAGE_STATES + index * 8u);
        if (frame[0x18u + index] != d3d8_guest_load32(stage_state_address(state))) {
            d3d8_set_texture_stage_state_notinline(0u, (int32_t)state, frame[0x18u + index]);
        }
    }
    if ((flags & FLAG_FORCE_STATES) == 0u) {
        if (frame[0] != 0u) {
            const uint32_t definition = d3d8_guest_load32(frame[0] + 8u);
            for (uint32_t state = 0u; state < SHADOW_STATES; state++) {
                if (d3d8_guest_load32(definition + 0x20u) != 0u ||
                    d3d8_guest_load32(definition + 0x24u) != 0u || state < 8u || state > 9u) {
                    d3d8_set_render_state_notinline((int32_t)state, frame[0x23u + state]);
                }
            }
            d3d8_set_render_state_notinline(0x88, frame[3]);
        }
        push_program(D3D8_DEVICE_BASE + DEV_SAVED_PROGRAM, PROGRAM_DWORDS);
    }
    const uint32_t cursor = d3d8_pushbuffer_begin();
    const uint32_t shader_flags = d3d8_guest_load32(d3d8_device_load32(DEV_DECLARATION) + 4u);
    uint32_t end;
    if ((shader_flags & 0x12u) != 0u) {
        d3d8_guest_store32(cursor, 0x00081E94u);
        d3d8_guest_store32(cursor + 4u, 6u);
        d3d8_guest_store32(cursor + 8u, shader_flags & 1u);
        d3d8_guest_store32(cursor + 12u, 0x00041EA0u);
        d3d8_guest_store32(cursor + 16u, d3d8_device_load32(DEV_VERTEX_INDEX));
        end = d3d8_viewport_emit(cursor + 20u, shader_flags);
    } else {
        d3d8_guest_store32(cursor, 0x00041E94u);
        d3d8_guest_store32(cursor + 4u, 4u);
        end = d3d8_viewport_emit(cursor + 8u, shader_flags);
    }
    d3d8_pushbuffer_end(end);
}

/* ---- CopyRects 0x003D3AD0 (T555) ------------------------------------------------------------------ */

#define RECTS_ENTRY 0x003D3AD0u
#define FORMAT_TABLE 0x003E1828u /* one byte per hardware format: bit 0 byte copy, bits 2 to 5 bits per pixel */
#define DEV_COPY_FORMAT 0x192Cu  /* a nonzero word overrides the blit's colour format */
#define DEV_REFILL_WORDS 0x0064u /* cursor to the last word the refill reads or writes */
#define HEADER_WORDS 6u
#define HEADER_BYTES 0x18u
#define RECT_BYTES 16u
#define POINT_BYTES 8u
#define SURFACE_DATA 1u /* header dword indices */
#define SURFACE_FORMAT 3u
#define SURFACE_SIZE 4u
#define SURFACE_PARENT 5u
#define SURFACE_LOCK_BYTES 8u
#define BLIT_STATE_BYTES 24u
#define BLIT_RECT_BYTES 16u
#define BYTE_PATH_LIMIT 0x1FC0u
#define BYTE_PATH_ROW 0x1000u
#define MAX_RECTS 4096u /* a named policy refusal: the original has no bound */

typedef struct {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
} copy_rect;

static uint32_t format_byte(const uint32_t *header)
{
    return (header[SURFACE_FORMAT] >> 8) & 0xFFu;
}

static uint32_t format_flags(uint32_t format)
{
    return d3d8_guest_load8(FORMAT_TABLE + format);
}

/* 0x003DB470: the compressed formats 0xC, 0xE and 0xF. */
static bool format_compressed(uint32_t format)
{
    return format == 0xCu || format == 0xEu || format == 0xFu;
}

/* 0x003D3640: the row pitch in bytes. A linear surface keeps it in its size word, a swizzled one derives it. */
static uint32_t surface_pitch(const uint32_t *header)
{
    const uint32_t size = header[SURFACE_SIZE];
    if (size != 0u) return ((size >> 24) + 1u) << 6;
    const uint32_t format = format_byte(header);
    const uint32_t width = 1u << ((header[SURFACE_FORMAT] >> 20) & 0xFu);
    if (format == 0xCu) return width * 2u;
    if (format > 0xDu && format <= 0xFu) return width * 4u;
    return ((format_flags(format) & 0x3Cu) * width) >> 3;
}

/* 0x003D36B0: the byte size of the surface. */
static uint32_t surface_bytes(const uint32_t *header)
{
    const uint32_t format = format_byte(header);
    const uint32_t bits = format_flags(format) & 0x3Cu;
    const uint32_t minimum = format_compressed(format) ? 2u : 0u;
    const uint32_t size = header[SURFACE_SIZE];
    if (size == 0u) {
        uint32_t wide = (header[SURFACE_FORMAT] >> 20) & 0xFu;
        uint32_t tall = (header[SURFACE_FORMAT] >> 24) & 0xFu;
        if (minimum > wide) wide = minimum;
        if (minimum > tall) tall = minimum;
        return ((1u << ((wide + tall) & 31u)) * bits) >> 3;
    }
    return (((size >> 12) & 0xFFFu) + 1u) * ((size >> 24) + 1u) << 6;
}

static bool spans_overlap(uint32_t first, uint32_t first_bytes, uint32_t second, uint32_t second_bytes)
{
    return first_bytes != 0u && second_bytes != 0u && (uint64_t)first < (uint64_t)second + second_bytes &&
           (uint64_t)second < (uint64_t)first + first_bytes;
}

/* What the copy's own writes and the refill's must not touch: the ring, the cursor and fence words, the
 * control page, the kick history and the copy format word. A guest array there is refused by name. */
static bool touches_library_state(uint32_t address, uint32_t bytes)
{
    const uint32_t ring = d3d8_device_load32(D3D8_DEV_PB_BASE);
    const uint32_t ring_end = d3d8_device_load32(D3D8_DEV_PB_END);
    const uint32_t history = d3d8_device_load32(0x48u);
    const uint64_t history_bytes = ((uint64_t)d3d8_device_load32(0x38u) + 1u) * 8u;
    return spans_overlap(address, bytes, ring, ring_end - ring) ||
           spans_overlap(address, bytes, D3D8_DEVICE_BASE, DEV_REFILL_WORDS) ||
           spans_overlap(address, bytes, D3D8_DEVICE_BASE + DEV_COPY_FORMAT, 4u) ||
           spans_overlap(address, bytes, D3D8_DEVICE_POINTER_SLOT, 4u) ||
           spans_overlap(address, bytes, d3d8_device_load32(0x30u) & ~0xFFFu, 0x1000u) ||
           history_bytes > UINT32_MAX || spans_overlap(address, bytes, history, (uint32_t)history_bytes);
}

static void probe_writable(uint32_t address, uint32_t bytes)
{
    uint8_t original[64];
    for (uint32_t done = 0u; done < bytes; done += sizeof(original)) {
        const uint32_t chunk = bytes - done < sizeof(original) ? bytes - done : (uint32_t)sizeof(original);
        if (!kernel_guest_read_bytes(address + done, original, chunk) ||
            !kernel_guest_write_bytes(address + done, original, chunk))
            d3d8_hle_fatal(RECTS_ENTRY, "CopyRects output %#x+%u is unreadable or unwritable", (unsigned)address,
                           (unsigned)bytes);
    }
}

static void read_input(uint32_t address, void *out, uint32_t bytes, const char *what)
{
    if (!kernel_guest_read_bytes(address, out, bytes))
        d3d8_hle_fatal(RECTS_ENTRY, "CopyRects %s %#x+%u is unreadable or overflows", what, (unsigned)address,
                       (unsigned)bytes);
}

/*
 * 0x003D3AD0, stdcall(source, rects, count, destination, points), ret 0x14. The 2D engine copy: one
 * 24 byte state packet (surface offsets, colour format, both pitches), then one 16 byte blit packet per
 * rectangle, each behind its own cursor-against-limit preamble, and the device's next fence value stored in
 * both surfaces' Lock word (their parent's when they have one). Returns the destination's parent, else the
 * fence value (what eax holds at the `ret`).
 *
 * TWO PATHS, chosen by the SOURCE's format. A format whose table bit 0 is set, or a compressed one, copies
 * as bytes: the surface is one row of its byte size (up to 0x1FC0, pitch rounded up to 64) or rows of
 * 0x1000, and every rectangle is rewritten to (0, 0, width, height), IN THE CALLER'S ARRAY when it gave
 * one. Any other format copies rectangles in its own pixels with the colour format picked from its bytes
 * per pixel (1, 4 or 0xA) unless device word 0x192C names one. A missing rectangle list is the whole
 * surface, and missing points put each rectangle where it came from.
 *
 * Everything the original would do mid-way is decided before the first write: a roll-over the model
 * refuses (the whole site sequence is planned over a simulated writer), an unreadable or unwritable
 * guest array, an array or surface word that aliases the ring or the refill's state, more than MAX_RECTS
 * rectangles, a missing surface and, on the byte path with a format override, a zero bytes per pixel
 * (the original divides by it).
 */
uint32_t d3d8_copy_rects(uint32_t source, uint32_t rects, uint32_t count, uint32_t destination, uint32_t points)
{
    if (source == 0u || destination == 0u)
        d3d8_hle_fatal(RECTS_ENTRY, "CopyRects with a null surface (the original dereferences it)");
    if (count == 0u) count = 1u;
    if (count > MAX_RECTS)
        d3d8_hle_fatal(RECTS_ENTRY, "CopyRects of %u rectangles is above the %u the port bounds", (unsigned)count,
                       MAX_RECTS);
    uint32_t from[HEADER_WORDS];
    uint32_t to[HEADER_WORDS];
    read_input(source, from, HEADER_BYTES, "source surface");
    read_input(destination, to, HEADER_BYTES, "destination surface");

    const uint32_t format = format_byte(from);
    const uint32_t flags = format_flags(format);
    const uint32_t bytes_per_pixel = (flags >> 3) & 7u;
    const uint32_t override_format = d3d8_device_load32(DEV_COPY_FORMAT);
    const bool byte_path = (flags & 1u) != 0u || format_compressed(format);
    uint32_t source_pitch = surface_pitch(from);
    uint32_t destination_pitch = surface_pitch(to);
    uint32_t blit_format = override_format;
    uint32_t byte_width = 0u;
    uint32_t byte_height = 0u;
    if (byte_path) {
        const uint32_t bytes = surface_bytes(from);
        if (bytes <= BYTE_PATH_LIMIT) {
            byte_width = bytes;
            byte_height = 1u;
            source_pitch = (bytes + 0x3Fu) & 0xFFFFFFC0u;
        } else {
            byte_width = BYTE_PATH_ROW;
            byte_height = bytes >> 12;
            source_pitch = BYTE_PATH_ROW;
        }
        destination_pitch = source_pitch;
        if (override_format == 0u) {
            blit_format = 1u;
        } else {
            if (bytes_per_pixel == 0u)
                d3d8_hle_fatal(RECTS_ENTRY, "CopyRects byte path with a format override divides by zero bytes per pixel");
            byte_width /= bytes_per_pixel;
        }
    } else if (override_format == 0u) {
        blit_format = bytes_per_pixel == 1u ? 1u : (bytes_per_pixel == 2u ? 4u : 0xAu);
    }

    copy_rect list[MAX_RECTS];
    int32_t where[MAX_RECTS][2];
    if (rects != 0u) read_input(rects, list, count * RECT_BYTES, "rectangle list");
    if (points != 0u) read_input(points, where, count * POINT_BYTES, "destination points");
    const uint32_t source_tag = (from[SURFACE_PARENT] != 0u ? from[SURFACE_PARENT] : source) + SURFACE_LOCK_BYTES;
    const uint32_t destination_tag =
        (to[SURFACE_PARENT] != 0u ? to[SURFACE_PARENT] : destination) + SURFACE_LOCK_BYTES;
    /* The words the function reads from each header, and every span it writes. */
    const uint32_t read_words[8] = {source + 4u, source + 0xCu, source + 0x10u, source + 0x14u,
                                    destination + 4u, destination + 0xCu, destination + 0x10u,
                                    destination + 0x14u};
    const uint32_t input_spans[][2] = {{source, HEADER_BYTES}, {destination, HEADER_BYTES}};
    const uint32_t array_spans[][2] = {{rects, rects != 0u ? count * RECT_BYTES : 0u},
                                       {points, points != 0u ? count * POINT_BYTES : 0u}};
    for (unsigned index = 0u; index < 2u; index++) {
        if (touches_library_state(input_spans[index][0], input_spans[index][1]))
            d3d8_hle_fatal(RECTS_ENTRY, "CopyRects surface header aliases the ring or the library's state");
        if (touches_library_state(array_spans[index][0], array_spans[index][1]))
            d3d8_hle_fatal(RECTS_ENTRY, "CopyRects rectangle or point array aliases the ring or the library's state");
    }
    if (touches_library_state(source_tag, 4u) || touches_library_state(destination_tag, 4u))
        d3d8_hle_fatal(RECTS_ENTRY, "CopyRects Lock word aliases the ring or the library's state");
    for (unsigned word = 0u; word < 8u; word++) {
        if (spans_overlap(source_tag, 4u, read_words[word], 4u) ||
            spans_overlap(destination_tag, 4u, read_words[word], 4u))
            d3d8_hle_fatal(RECTS_ENTRY, "CopyRects Lock word aliases a header word the copy reads");
    }
    for (unsigned first = 0u; first < 2u; first++) {
        for (unsigned second = 0u; second < 2u; second++) {
            if (first != second && spans_overlap(array_spans[first][0], array_spans[first][1], array_spans[second][0],
                                                 array_spans[second][1]))
                d3d8_hle_fatal(RECTS_ENTRY, "CopyRects rectangle list and points alias");
        }
        for (unsigned header = 0u; header < 2u; header++) {
            if (spans_overlap(array_spans[first][0], array_spans[first][1], input_spans[header][0],
                              input_spans[header][1]))
                d3d8_hle_fatal(RECTS_ENTRY, "CopyRects array aliases a surface header");
        }
        if (spans_overlap(array_spans[first][0], array_spans[first][1], source_tag, 4u) ||
            spans_overlap(array_spans[first][0], array_spans[first][1], destination_tag, 4u))
            d3d8_hle_fatal(RECTS_ENTRY, "CopyRects array aliases a Lock word");
    }

    /* The sites in the order the original reaches them: the state packet, then one blit per rectangle. */
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    uint32_t planned[MAX_RECTS + 1u];
    planned[0] = d3d8_pushbuffer_sim_site(&sim, RECTS_ENTRY, BLIT_STATE_BYTES);
    for (uint32_t index = 0u; index < count; index++)
        planned[1u + index] = d3d8_pushbuffer_sim_site(&sim, RECTS_ENTRY, BLIT_RECT_BYTES);
    probe_writable(source_tag, 4u);
    probe_writable(destination_tag, 4u);
    if (byte_path && rects != 0u) probe_writable(rects, count * RECT_BYTES);

    uint32_t cursor = d3d8_pushbuffer_begin();
    if (cursor != planned[0])
        d3d8_hle_fatal(RECTS_ENTRY, "the CopyRects state packet landed at %#x, not %#x", (unsigned)cursor,
                       (unsigned)planned[0]);
    d3d8_guest_store32(cursor, 0x00086308u);
    d3d8_guest_store32(cursor + 4u, from[SURFACE_DATA]);
    d3d8_guest_store32(cursor + 8u, to[SURFACE_DATA]);
    d3d8_guest_store32(cursor + 12u, 0x00086300u);
    d3d8_guest_store32(cursor + 16u, blit_format);
    d3d8_guest_store32(cursor + 20u, (destination_pitch << 16) | (source_pitch & 0xFFFFu));
    d3d8_pushbuffer_end(cursor + BLIT_STATE_BYTES);

    for (uint32_t index = 0u; index < count; index++) {
        copy_rect rectangle;
        if (rects != 0u) {
            rectangle = list[index];
        } else if (from[SURFACE_SIZE] == 0u) {
            rectangle.left = 0;
            rectangle.top = 0;
            rectangle.right = (int32_t)(1u << ((from[SURFACE_FORMAT] >> 20) & 0xFu));
            rectangle.bottom = (int32_t)(1u << ((from[SURFACE_FORMAT] >> 24) & 0xFu));
        } else {
            rectangle.left = 0;
            rectangle.top = 0;
            rectangle.right = (int32_t)((from[SURFACE_SIZE] & 0xFFFu) + 1u);
            rectangle.bottom = (int32_t)(((from[SURFACE_SIZE] >> 12) & 0xFFFu) + 1u);
        }
        const int32_t point_x = points != 0u ? where[index][0] : rectangle.left;
        const int32_t point_y = points != 0u ? where[index][1] : rectangle.top;
        if (byte_path) {
            rectangle.left = 0;
            rectangle.top = 0;
            rectangle.right = (int32_t)byte_width;
            rectangle.bottom = (int32_t)byte_height;
            if (rects != 0u) {
                const uint32_t written[4] = {0u, 0u, byte_width, byte_height};
                if (!kernel_guest_write_bytes(rects + index * RECT_BYTES, written, sizeof(written)))
                    d3d8_hle_fatal(RECTS_ENTRY, "CopyRects rectangle list changed during publication");
            }
        }
        cursor = d3d8_pushbuffer_begin();
        if (cursor != planned[1u + index])
            d3d8_hle_fatal(RECTS_ENTRY, "the CopyRects blit %u landed at %#x, not %#x", (unsigned)index,
                           (unsigned)cursor, (unsigned)planned[1u + index]);
        d3d8_guest_store32(cursor, 0x000C4300u);
        d3d8_guest_store32(cursor + 4u, ((uint32_t)rectangle.top << 16) | ((uint32_t)rectangle.left & 0xFFFFu));
        d3d8_guest_store32(cursor + 8u, ((uint32_t)point_y << 16) | ((uint32_t)point_x & 0xFFFFu));
        d3d8_guest_store32(cursor + 12u, (((uint32_t)rectangle.bottom - (uint32_t)rectangle.top) << 16) ^
                                             (((uint32_t)rectangle.right - (uint32_t)rectangle.left) & 0xFFFFu));
        d3d8_pushbuffer_end(cursor + BLIT_RECT_BYTES);
    }

    const uint32_t fence = d3d8_device_load32(D3D8_DEV_FENCE);
    d3d8_guest_store32(source_tag, fence);
    d3d8_guest_store32(destination_tag, fence);
    return to[SURFACE_PARENT] != 0u ? to[SURFACE_PARENT] : fence;
}

static uint32_t handler_copy_rects(void *context)
{
    uint32_t arguments[5];
    for (unsigned index = 0u; index < 5u; index++) {
        if (!kernel_frame_arg((const kernel_call_frame *)context, index, &arguments[index]))
            d3d8_hle_fatal(RECTS_ENTRY, "argument %u of CopyRects cannot be read", index);
    }
    return d3d8_copy_rects(arguments[0], arguments[1], arguments[2], arguments[3], arguments[4]);
}

size_t d3d8_copy_register(void)
{
    return d3d8_hle_register(RECTS_ENTRY, handler_copy_rects) ? 1u : 0u;
}
