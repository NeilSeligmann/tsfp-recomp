/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_scaled_viewport.h. Every function carries the address of the original it ports.
 */

#include "d3d8_scaled_viewport.h"

#include <float.h>
#include <string.h>

#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "d3d8_scissor.h"
#include "d3d8_shader.h"
#include "d3d8_surface.h"
#include "d3d8_texture_dirty.h"
#include "d3d8_viewport_matrix.h"
#include "kernel_call.h"

#define ENTRY 0x003D7B80u
#define SET_VIEWPORT 0x003D3E40u

#define DEV_RENDER_TARGET 0x1A04u
#define DEV_SURFACE_FORMAT 0x1A0Cu
#define DEV_BACK_BUFFER 0x1A14u
#define DEV_FRONT_BUFFER 0x1A18u
#define DEV_DECLARATION 0x794u
#define DEV_TARGET_WIDTH 0x954u
#define DEV_TARGET_HEIGHT 0x958u
#define DEV_SCALE_X 0x95Cu
#define DEV_SCALE_Y 0x960u
#define DEV_SCALE_LAST 0x964u
#define DEV_SCALE_TABLE 0x968u
#define DEV_SAMPLE_SCALE_X 0x96Cu
#define DEV_SAMPLE_SCALE_Y 0x970u
#define DEV_DEVICE_FLAGS_SAMPLED 0x8000u

#define CONSTANT_TWO_POW_32 0x00475CCCu
#define CONSTANT_HALF 0x00475CD4u
#define CONSTANT_ONE 0x00475C78u
#define CONSTANT_EIGHT 0x0047D1E4u
#define GLOBAL_SAMPLE_SCALE_FACTOR 0x003E3F34u /* the float 0x40380 scales (render state 0x9C) */
#define GLOBAL_SAMPLE_ALPHA_HIGH 0x003E3F24u
#define GLOBAL_SAMPLE_ALPHA_LOW 0x003E3F38u
#define GLOBAL_ANTIALIAS 0x003E3F20u
#define SCALE_TABLE 0x003E1B48u
#define FLOAT_ONE_WORD 0x3F800000u
#define POINT_LIMIT 0x1FFu

static bool finite_word(uint32_t word)
{
    return (word & 0x7F800000u) != 0x7F800000u;
}

static float word_float(uint32_t word)
{
    float value;
    memcpy(&value, &word, sizeof(value));
    return value;
}

static uint32_t float_word(long double value)
{
    const float rounded = (float)value;
    uint32_t bits;
    memcpy(&bits, &rounded, sizeof(bits));
    return bits;
}

/* `cvttss2si` of the float a store rounds `value` to (0x003DB320): out of range and NaN give 0x80000000. */
static uint32_t truncated(long double value)
{
#if defined(__x86_64__) || defined(__i386__)
    const volatile float spilled = (float)value;
    uint32_t result;
    __asm__ volatile("cvttss2si %1, %0" : "=r"(result) : "m"(spilled));
    return result;
#else
    (void)value;
    d3d8_hle_fatal(ENTRY, "the scaled viewport requires x86 x87/SSE");
#endif
}

/* The two dimension reads that open every branch: the size word when set, else the log2 fields of the format. */
static uint32_t surface_width(uint32_t header)
{
    const uint32_t size = d3d8_guest_load32(header + D3D8_SURFACE_SIZE);
    return size != 0u ? (size & 0xFFFu) + 1u
                      : 1u << ((d3d8_guest_load32(header + D3D8_SURFACE_FORMAT) >> 20) & 0xFu);
}

static uint32_t surface_height(uint32_t header)
{
    const uint32_t size = d3d8_guest_load32(header + D3D8_SURFACE_SIZE);
    return size != 0u ? ((size >> 12) & 0xFFFu) + 1u
                      : 1u << ((d3d8_guest_load32(header + D3D8_SURFACE_FORMAT) >> 24) & 0xFu);
}

static uint32_t constant(uint32_t address)
{
    const uint32_t word = d3d8_guest_load32(address);
    if (!finite_word(word)) {
        d3d8_hle_fatal(ENTRY, "float constant %#x is %#x: NaN payload handling differs between the oracle and "
                              "physical hardware", (unsigned)address, (unsigned)word);
    }
    return word;
}

void d3d8_scaled_viewport_compute(d3d8_scaled_viewport *plan, uint32_t mode)
{
    d3d8_check_default_fp(ENTRY);
    if (LDBL_MANT_DIG != 64) {
        d3d8_hle_fatal(ENTRY, "the scaled viewport requires x87-width long double");
    }
    if (d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT) != D3D8_DEVICE_BASE) {
        d3d8_hle_fatal(ENTRY, "the scaled viewport requires the recovered device");
    }
    const uint32_t two_pow_32 = constant(CONSTANT_TWO_POW_32);
    const uint32_t half = constant(CONSTANT_HALF);
    const uint32_t one = constant(CONSTANT_ONE);
    const uint32_t eight = constant(CONSTANT_EIGHT);
    const long double half_value = word_float(half);

    const uint32_t target = d3d8_device_load32(DEV_RENDER_TARGET);
    const bool first_back_buffer = target == d3d8_device_load32(DEV_BACK_BUFFER);
    uint32_t width;
    uint32_t height;
    uint32_t scale_x_word;
    long double scale_y_value;
    if (first_back_buffer) {
        /* 0x003D7B9F: the sizes come from the front buffer (+0x1A18), scaled by the sample scales, then clamped to
         * the back buffer's. */
        scale_x_word = d3d8_device_load32(DEV_SAMPLE_SCALE_X);
        const uint32_t scale_y_word = d3d8_device_load32(DEV_SAMPLE_SCALE_Y);
        if (!finite_word(scale_x_word) || !finite_word(scale_y_word)) {
            d3d8_hle_fatal(ENTRY, "sample scale %#x %#x is not finite: NaN payload handling differs between the "
                                  "oracle and physical hardware",
                           (unsigned)scale_x_word, (unsigned)scale_y_word);
        }
        const uint32_t front = d3d8_device_load32(DEV_FRONT_BUFFER);
        const uint32_t back = d3d8_device_load32(DEV_BACK_BUFFER);
        const uint32_t scaled_width = d3d8_scaled_integer(surface_width(front), scale_x_word, half, two_pow_32);
        const uint32_t scaled_height = d3d8_scaled_integer(surface_height(front), scale_y_word, half, two_pow_32);
        const uint32_t back_width = surface_width(back);
        const uint32_t back_height = surface_height(back);
        width = scaled_width < back_width ? scaled_width : back_width;
        height = scaled_height < back_height ? scaled_height : back_height;
        scale_y_value = word_float(scale_y_word);
    } else {
        /* 0x003D7CE8: the target's own size, scale 1.0 (an immediate for x, the .rdata constant for y). */
        width = surface_width(target);
        height = surface_height(target);
        scale_x_word = FLOAT_ONE_WORD;
        scale_y_value = word_float(one);
    }

    uint32_t flags = d3d8_device_load32(D3D8_DEV_FLAGS) & ~DEV_DEVICE_FLAGS_SAMPLED;
    uint32_t format = d3d8_device_load32(DEV_SURFACE_FORMAT);
    if (mode != 0u && (format & 0x200u) == 0u) {
        flags |= DEV_DEVICE_FLAGS_SAMPLED;
        width = (width + 1u) >> 1;
        scale_x_word = float_word(word_float(scale_x_word) * half_value);
        if (mode == 2u) {
            scale_y_value *= half_value;
            height = (height + 1u) >> 1;
            format |= 0x2000u;
        } else {
            format |= 0x1000u;
        }
    }
    const long double scale_x_value = word_float(scale_x_word);
    const uint32_t scale_y_word = float_word(scale_y_value);
    /* FCOMP: the x scale is the sample scale when it is the smaller, else the y scale (equal included). */
    const uint32_t sample_word = scale_x_value < scale_y_value ? scale_x_word : scale_y_word;

    plan->width = width;
    plan->height = height;
    plan->scale_x = scale_x_word;
    plan->scale_y = scale_y_word;
    plan->sample_scale = sample_word;
    plan->device_flags = flags;
    plan->method_208 = format;
    plan->changed = !(word_float(d3d8_device_load32(DEV_SCALE_LAST)) == word_float(sample_word));
    plan->table_word = 0u;
    plan->method_380 = 0u;
    if (plan->changed) {
        /* 0x003D7DF3: the table entry at trunc(2 * scale + 0.5), and the point scale packet. */
        const long double sample = word_float(sample_word);
        const uint32_t index = truncated(sample + sample + half_value);
        const uint32_t address = SCALE_TABLE + index * 4u;
        if (!kernel_guest_read_bytes(address, &plan->table_word, sizeof(plan->table_word))) {
            d3d8_hle_fatal(ENTRY, "scale table index %#x is not mapped guest memory", (unsigned)index);
        }
        const long double point = word_float(d3d8_guest_load32(GLOBAL_SAMPLE_SCALE_FACTOR));
        const uint32_t clamp = truncated(point * sample * word_float(eight) + half_value);
        plan->method_380 = clamp > POINT_LIMIT ? POINT_LIMIT : clamp;
    }
    uint32_t mask = (d3d8_guest_load32(GLOBAL_SAMPLE_ALPHA_HIGH) << 16) | d3d8_guest_load32(GLOBAL_SAMPLE_ALPHA_LOW);
    if ((flags & DEV_DEVICE_FLAGS_SAMPLED) != 0u && d3d8_guest_load32(GLOBAL_ANTIALIAS) != 0u) {
        mask |= 1u;
    }
    plan->method_41d7c = mask;
}

void d3d8_scaled_viewport_store(const d3d8_scaled_viewport *plan)
{
    d3d8_device_store32(D3D8_DEV_FLAGS, plan->device_flags);
    d3d8_device_store32(DEV_SCALE_Y, plan->scale_y);
    d3d8_device_store32(DEV_SCALE_X, plan->scale_x);
    d3d8_device_store32(DEV_TARGET_WIDTH, plan->width);
    d3d8_device_store32(DEV_TARGET_HEIGHT, plan->height);
    if (plan->changed) {
        d3d8_device_store32(DEV_SCALE_LAST, plan->sample_scale);
        d3d8_device_store32(DEV_SCALE_TABLE, plan->table_word);
        d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK, d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | 0x10Fu);
    }
}

/* The bytes 0x003D7860 writes for the viewport (the offset and scale pair, or the offset alone) and the depth range. */
static uint32_t viewport_packet_bytes(uint32_t declaration_flags)
{
    const bool programmable = (declaration_flags & 0x12u) != 0u;
    const uint32_t viewport =
        programmable ? ((d3d8_device_load32(D3D8_DEV_FLAGS) & 0x200u) != 0u ? 0u : 40u) : 20u;
    return viewport + 12u;
}

static void refuse_nan_offset(uint32_t offset)
{
    const uint32_t word = d3d8_device_load32(offset);
    if (!finite_word(word) && (word & 0x7FFFFFFFu) != 0x7F800000u) {
        d3d8_hle_fatal(SET_VIEWPORT, "viewport offset %#x is NaN: payload handling differs between the oracle and "
                                     "physical hardware", (unsigned)word);
    }
}

void d3d8_scaled_viewport_run(uint32_t mode, uint32_t state_address)
{
    d3d8_scaled_viewport plan;
    d3d8_scaled_viewport_compute(&plan, mode);
    const uint32_t declaration_flags = d3d8_guest_load32(d3d8_device_load32(DEV_DECLARATION) + 4u);
    const d3d8_viewport_scissor scissor = {plan.scale_x, plan.scale_y, plan.width, plan.height};

    /* The plan: every site and every refusal, over a writer that carries the refill state. */
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_site(&sim, ENTRY, plan.changed ? 16u : 8u);
    d3d8_pushbuffer_sim_site(&sim, ENTRY, 8u);
    d3d8_plan_vertex_program_helper(&sim);
    d3d8_plan_viewport_scissor(&sim);
    d3d8_rebuild_viewport_matrix_check_scaled(plan.scale_x, plan.scale_y);
    refuse_nan_offset(0xEF8u);
    refuse_nan_offset(0xEFCu);
    d3d8_pushbuffer_sim_site(&sim, SET_VIEWPORT, viewport_packet_bytes(declaration_flags));

    d3d8_guest_store32(state_address, mode);
    d3d8_scaled_viewport_store(&plan);
    uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, 0x00040208u);
    d3d8_guest_store32(cursor + 4u, plan.method_208);
    cursor += 8u;
    if (plan.changed) {
        d3d8_guest_store32(cursor, 0x00040380u);
        d3d8_guest_store32(cursor + 4u, plan.method_380);
        cursor += 8u;
    }
    d3d8_pushbuffer_end(cursor);
    cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, 0x00041D7Cu);
    d3d8_guest_store32(cursor + 4u, plan.method_41d7c);
    d3d8_pushbuffer_end(cursor + 8u);
    d3d8_run_vertex_program_helper();

    /* SetViewport(NULL), 0x003D4033: the scissor of the viewport rectangle, the matrix, the viewport packets. */
    d3d8_set_viewport_scissor(&scissor);
    (void)d3d8_rebuild_viewport_matrix();
    cursor = d3d8_pushbuffer_begin();
    d3d8_pushbuffer_end(d3d8_viewport_emit(cursor, declaration_flags));
}

/* Direct 0x003D81F0/0x003D8220 adapter. Keep the legacy transactional run above
 * unchanged; its precomputed mutable values cannot stand in for these rereads. */
uint32_t d3d8_scaled_viewport_run_direct(uint32_t mode)
{
    d3d8_scaled_viewport direct_plan;
    d3d8_scaled_viewport_compute(&direct_plan, mode);
    const uint32_t initial_declaration = d3d8_guest_load32(d3d8_device_load32(DEV_DECLARATION) + 4u);
    d3d8_pushbuffer_sim direct_sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_site(&direct_sim, ENTRY, direct_plan.changed ? 16u : 8u);
    d3d8_pushbuffer_sim_site(&direct_sim, ENTRY, 8u);
    d3d8_plan_vertex_program_helper(&direct_sim);
    d3d8_plan_viewport_scissor(&direct_sim);
    d3d8_rebuild_viewport_matrix_check_scaled(direct_plan.scale_x, direct_plan.scale_y);
    refuse_nan_offset((0xEF8u));
    refuse_nan_offset(0xEFCu);
    d3d8_pushbuffer_sim_site(&direct_sim, SET_VIEWPORT, viewport_packet_bytes(initial_declaration));

    /* Original device prefix precedes the first entry refill. LAST/TABLE and
     * dirty writes belong to the later post-header changed-scale branch. */
    d3d8_device_store32(D3D8_DEV_FLAGS, direct_plan.device_flags);
    d3d8_device_store32(DEV_SCALE_Y, direct_plan.scale_y);
    d3d8_device_store32(DEV_SCALE_X, direct_plan.scale_x);
    d3d8_device_store32(DEV_TARGET_WIDTH, direct_plan.width);
    d3d8_device_store32(DEV_TARGET_HEIGHT, direct_plan.height);
    const uint32_t start = d3d8_pushbuffer_begin();
    d3d8_guest_store32(start, 0x00040208u);
    d3d8_guest_store32(start + 4u, direct_plan.method_208);
    uint32_t cursor = start + 8u;
    if (!(word_float(d3d8_device_load32(DEV_SCALE_LAST)) == word_float(direct_plan.sample_scale))) {
        d3d8_device_store32(DEV_SCALE_LAST, direct_plan.sample_scale);
        const long double sample = word_float(direct_plan.sample_scale);
        const uint32_t half = constant(CONSTANT_HALF);
        const uint32_t index = truncated(sample + sample + word_float(half));
        const uint32_t table_word = d3d8_guest_load32(SCALE_TABLE + index * 4u);
        d3d8_device_store32(DEV_SCALE_TABLE, table_word);
        const long double point = word_float(d3d8_guest_load32(GLOBAL_SAMPLE_SCALE_FACTOR));
        const uint32_t current_dirty = d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK);
        d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK, current_dirty | 0x10Fu);
        const uint32_t clamp = truncated(point * sample * word_float(constant(CONSTANT_EIGHT)) +
                                          word_float(constant(CONSTANT_HALF)));
        d3d8_guest_store32(cursor, 0x00040380u);
        d3d8_guest_store32(cursor + 4u, clamp > POINT_LIMIT ? POINT_LIMIT : clamp);
        cursor += 8u;
    }
    d3d8_pushbuffer_end_at(D3D8_DEVICE_BASE, start, cursor);
    /* Original captures this mask before the second refill, not before the first. */
    const uint32_t current_flags = d3d8_device_load32(D3D8_DEV_FLAGS);
    const uint32_t current_high = d3d8_guest_load32(GLOBAL_SAMPLE_ALPHA_HIGH);
    const uint32_t current_low = d3d8_guest_load32(GLOBAL_SAMPLE_ALPHA_LOW);
    uint32_t mask = (current_high << 16) | current_low;
    if ((current_flags & DEV_DEVICE_FLAGS_SAMPLED) != 0u &&
        d3d8_guest_load32(GLOBAL_ANTIALIAS) != 0u) {
        mask |= 1u;
    }
    cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, 0x00041D7Cu);
    d3d8_guest_store32(cursor + 4u, mask);
    d3d8_pushbuffer_end_at(D3D8_DEVICE_BASE, cursor, cursor + 8u);
    (d3d8_run_vertex_program_helper)();

    const d3d8_viewport_scissor direct_scissor = {
        d3d8_device_load32(DEV_SCALE_X), d3d8_device_load32(DEV_SCALE_Y),
        d3d8_device_load32(DEV_TARGET_WIDTH), d3d8_device_load32(DEV_TARGET_HEIGHT)
    };
    d3d8_set_viewport_scissor(&direct_scissor);
    (void)d3d8_rebuild_viewport_matrix();
    cursor = d3d8_pushbuffer_begin();
    const uint32_t declaration = d3d8_guest_load32(d3d8_device_load32(DEV_DECLARATION) + 4u);
    const uint32_t advanced = d3d8_viewport_emit(cursor, declaration);
    d3d8_pushbuffer_end_at(D3D8_DEVICE_BASE, cursor, advanced);
    return advanced;
}
