/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "d3d8_dirty.h"
#include <float.h>
#include <string.h>
#include "d3d8_guest.h"
#include "d3d8_matrix_inverse.h"
#include "d3d8_pushbuffer.h"
#include "d3d8_hle.h"

static float scalar(uint32_t address)
{
    const uint32_t bits = d3d8_guest_load32(address);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static void emit_float(uint32_t address, long double value)
{
    const float rounded = (float)value;
    uint32_t bits;
    memcpy(&bits, &rounded, sizeof(bits));
    d3d8_guest_store32(address, bits);
}
uint32_t d3d8_emit_point_state(void)
{
#if LDBL_MANT_DIG != 64
    d3d8_hle_fatal(0x003DD7C0u, "D3D point-state emitter requires x87 extended precision");
#endif
    /* 0x003DD7CB: the reservation preamble comes first, before any state is read. */
    uint32_t cursor = d3d8_pushbuffer_begin();
    const float minimum = scalar(0x003E3E94u);
    const float maximum = scalar(0x003E3EACu);
    const uint32_t attenuation = d3d8_guest_load32(0x003E3E9Cu);
    long double size = scalar(0x003E3E90u);
    if (attenuation == 0u) {
        size *= (long double)scalar(D3D8_DEVICE_BASE + 0x964u);
        if (size < (long double)minimum) size = minimum;
        if (size > (long double)maximum) size = maximum;
        const float hardware_maximum = scalar(0x003E2954u);
        if (size > (long double)hardware_maximum) size = hardware_maximum;
    } else {
        /* fstp at 0x3DD86B rounds the range before its subsequent divisions. */
        const float range = (float)((long double)maximum - (long double)minimum);
        const uint32_t height = d3d8_device_load32(0xEECu);
        long double ratio = (long double)range / ((long double)height * size);
        ratio *= ratio;
        d3d8_guest_store32(cursor, 0x00200A30u);
        emit_float(cursor + 4u, (long double)scalar(0x003E3EA0u) * ratio);
        emit_float(cursor + 8u, (long double)scalar(0x003E3EA4u) * ratio);
        emit_float(cursor + 12u, (long double)scalar(0x003E3EA8u) * ratio);
        for (uint32_t offset = 16u; offset <= 24u; offset += 4u)
            emit_float(cursor + offset, range);
        emit_float(cursor + 28u, -((long double)minimum / (long double)range));
        emit_float(cursor + 32u, minimum);
        cursor += 36u;
    }
    d3d8_guest_store32(cursor, 0x00080318u);
    d3d8_guest_store32(cursor + 4u, attenuation);
    d3d8_guest_store32(cursor + 8u, d3d8_guest_load32(0x003E3E98u));
    /* Original fstp float32 followed by cvttss2si, then an unsigned cap. */
    const float rounded = (float)(size * 8.0L + 0.5L);
    const uint32_t integer = rounded >= -2147483648.0L && rounded < 2147483648.0L
                             ? (uint32_t)(int32_t)rounded : 0x80000000u;
    const uint32_t hardware_size = integer > 511u ? 511u : integer;
    d3d8_guest_store32(cursor + 12u, 0x0004043Cu);
    d3d8_guest_store32(cursor + 16u, hardware_size);
    d3d8_pushbuffer_end(cursor + 20u);
    return hardware_size;
}

static uint32_t adapt_texture_mode(uint32_t mode, uint32_t texture)
{
    /* Negative cache marks an unbound stage; these four modes do not require
     * that stage's texture. This test precedes the dimension adaptations. */
    if ((texture & 0x80000000u) != 0u && mode != 4u && mode != 5u &&
        mode != 10u && mode != 17u) return 0u;
    if (mode >= 1u && mode <= 3u) {
        if ((texture & 4u) != 0u) return 3u;
        return ((texture & 0x40000000u) != 0u || (texture & 0xF0u) == 0x30u) ? 2u : 1u;
    }
    if (mode == 13u || mode == 14u) return (texture & 4u) != 0u ? 14u : 13u;
    return mode;
}

/* 0x003DD930 with no pixel shader bound: one 5 bit sampler type per stage from the stage's bound
 * texture word (device+0xC+4*stage). A stage above stage 0 looks at the colour operation of the stage
 * BELOW it (texture states 0xC of stages 0 to 2): 0x19 and 0x1A select the bump types 6 and 7. */
static uint32_t fixed_function_stage_type(uint32_t texture, uint32_t previous_operation, bool chained)
{
    if ((texture & 0x80000000u) != 0u) return 0u;
    if (chained && previous_operation == 0x19u) return 6u;
    if (chained && previous_operation == 0x1Au) return 7u;
    if ((texture & 4u) != 0u) return 3u;
    return ((texture & 0x40000000u) != 0u || (texture & 0xF0u) == 0x30u) ? 2u : 1u;
}

static uint32_t fixed_function_stage_program(void)
{
    uint32_t program = 0u;
    for (uint32_t stage = 4u; stage-- > 0u;) {
        const uint32_t texture = d3d8_device_load32(0xCu + stage * 4u);
        const uint32_t previous =
            stage > 0u ? d3d8_guest_load32(0x003E3AC0u + (stage - 1u) * 0x80u + 0x30u) : 0u;
        program = (program << 5) | fixed_function_stage_type(texture, previous, stage > 0u);
    }
    return program;
}

uint32_t d3d8_emit_shader_stage_program(void)
{
    if (d3d8_device_load32(0x784u) == 0u) {
        const uint32_t program = fixed_function_stage_program();
        const uint32_t cursor = d3d8_pushbuffer_begin();
        d3d8_guest_store32(cursor, 0x00041E70u);
        d3d8_guest_store32(cursor + 4u, program);
        d3d8_pushbuffer_end(cursor + 8u);
        return cursor + 8u;
    }
    const uint32_t cached = d3d8_device_load32(0x790u);
    uint32_t program = cached;
    if (d3d8_device_load32(0x78Cu) != 0u) {
        program = 0u;
        for (uint32_t stage = 0u; stage < 4u; stage++) {
            const uint32_t mode = (cached >> (stage * 5u)) & 31u;
            const uint32_t texture = d3d8_device_load32(0xCu + stage * 4u);
            program |= adapt_texture_mode(mode, texture) << (stage * 5u);
        }
    }
    /* 0x003DDC8F: the program is formed first and the reservation precedes only the stores. */
    const uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, 0x00041E70u);
    d3d8_guest_store32(cursor + 4u, program);
    d3d8_pushbuffer_end(cursor + 8u);
    return cursor + 8u;
}

void d3d8_validate_default_lighting(uint32_t dirty)
{
    if ((dirty & 0x1000u) != 0u) {
        const uint32_t header = d3d8_device_load32(0x794u);
        const uint32_t flags = d3d8_guest_load8(header + 4u);
        if ((flags & 0x12u) == 0u && d3d8_guest_load32(0x003E3E58u) != 0u &&
            (d3d8_device_load32(0x1928u) & ~0x10u) != 1u)
            d3d8_hle_fatal(0x003DE830u, "enabled fixed-function lighting is not recovered");
    }
    if ((dirty & 0xFF8000u) != 0u) {
        for (uint32_t offset = 0x7ACu; offset <= 0x7C8u; offset += 4u)
            if (d3d8_device_load32(offset) != 0u)
                d3d8_hle_fatal(0x003DE830u, "non-null light emitter is not recovered");
    }
}

uint32_t d3d8_emit_default_lighting_state(uint32_t dirty)
{
    /* Unsupported branches are rejected before the reservation, which may roll the ring over. */
    d3d8_validate_default_lighting(dirty);
    const uint32_t start = d3d8_pushbuffer_begin();
    const bool defaults = (dirty & 0x1000u) != 0u;
    const bool lights = (dirty & 0xFF8000u) != 0u;
    uint32_t fog = 0u;
    if (defaults)
        fog = (d3d8_guest_load32(0x003E3E5Cu) != 0u ||
               (d3d8_device_load32(8u) & 0x40u) != 0u) ? 1u : 0u;
    uint32_t cursor = start;
    if (defaults) {
        const uint32_t packet[] = {0x40314u, 0u, 0x403B8u, fog, 0x40294u, 0x20001u};
        for (uint32_t i = 0u; i < 6u; i++)
            d3d8_guest_store32(cursor + i * 4u, packet[i]);
        cursor += 24u;
    }
    if (lights) {
        d3d8_guest_store32(cursor, 0x403BCu);
        d3d8_guest_store32(cursor + 4u, d3d8_device_load32(0x7CCu));
        cursor += 8u;
    }
    d3d8_pushbuffer_end(cursor);
    /* The original loop leaves the last pointer-slot address in EAX, even when
     * every pointer was null; otherwise the entry cursor remains in EAX. */
    return lights ? D3D8_DEVICE_BASE + 0x7C8u : start;
}

void d3d8_plan_point_state(d3d8_pushbuffer_sim *sim)
{
#if LDBL_MANT_DIG != 64
    d3d8_hle_fatal(0x003DD7C0u, "D3D point-state emitter requires x87 extended precision");
#endif
    d3d8_pushbuffer_sim_site(sim, 0x003DD7C0u, d3d8_guest_load32(0x003E3E9Cu) == 0u ? 20u : 56u);
}

void d3d8_plan_shader_stage_program(d3d8_pushbuffer_sim *sim)
{
    d3d8_pushbuffer_sim_site(sim, 0x003DD930u, 8u);
}

bool d3d8_dirty_programmable(void)
{
    const uint32_t declaration = d3d8_device_load32(0x794u);
    return (d3d8_guest_load8(declaration + 4u) & 0x12u) != 0u ||
           (d3d8_device_load32(0x1928u) & ~0x10u) == 1u;
}

#define TRANSFORM_FLAGS 0x003E3B14u /* texture state 0x15 of stage 0, 0x80 bytes per stage */
#define TEXCOORD_INDEX_WORD 0x003E3B30u /* texture state 0x1C of stage 0, 0x80 bytes per stage */
#define TRANSFORM_PAIR 0x00040420u
#define TRANSFORM_MATRIX_HEADER 0x004006C0u /* 16 dwords at method 0x6C0, 0x40 more per stage */
#define TRANSFORM_SOURCE 0xCE0u              /* the stage matrices at device+0xCE0, 0x40 bytes each */
#define TRANSFORM_BYTE_TABLE 0x003DE1F4u
#define TRANSFORM_JUMP_TABLE 0x003DE400u

/* The shapes 0x003DE080 writes for a stage whose transform flags are not zero. The original forms
 * code = ((dimension << 4 | count) << 4) | projected, with the dimension from the texture coordinate
 * index word (3 when it carries a mode, else a byte of the declaration, 2 when that is zero), then
 * jumps: above 0x320 to the 4x4 shapes, 0x320 to its own and below through a byte table into four
 * labels. A byte past the four labels jumps through garbage, so the port refuses it. */
typedef enum {
    SHAPE_ZERO,
    SHAPE_2D,        /* 0x003DE187 */
    SHAPE_2D_Z,      /* 0x003DE1C2 */
    SHAPE_2D_W,      /* 0x003DE200 */
    SHAPE_2D_ZW,     /* 0x003DE250 */
    SHAPE_3X4,       /* 0x003DE2A8 */
    SHAPE_FULL,      /* 0x003DE2FE */
    SHAPE_FULL_W,    /* 0x003DE359, code 0x331 */
    SHAPE_FULL_Z     /* 0x003DE37F, code 0x330 */
} transform_shape;

static transform_shape stage_transform_shape(uint32_t stage)
{
    const uint32_t flags = d3d8_guest_load32(TRANSFORM_FLAGS + stage * 0x80u);
    if (flags == 0u) return SHAPE_ZERO;
    const uint32_t index_word = d3d8_guest_load32(TEXCOORD_INDEX_WORD + stage * 0x80u);
    uint32_t dimension = 3u;
    if ((index_word & 0xFFFF0000u) == 0u) {
        const uint32_t declaration = d3d8_device_load32(0x794u);
        dimension = (d3d8_guest_load32(declaration + 0x10u) >> (((index_word & 0xFFFFu) << 3) & 31u)) & 0xFFu;
        if (dimension == 0u) dimension = 2u;
    }
    const uint32_t code = (((dimension << 4) | (flags & 0xFFu)) << 4) | ((flags >> 8) & 1u);
    if (code > 0x320u) return code == 0x330u ? SHAPE_FULL_Z : code == 0x331u ? SHAPE_FULL_W : SHAPE_FULL;
    if (code == 0x320u) return SHAPE_3X4;
    const uint32_t entry = d3d8_guest_load8(TRANSFORM_BYTE_TABLE + code);
    switch (d3d8_guest_load32(TRANSFORM_JUMP_TABLE + entry * 4u)) {
    case 0x003DE187u: return SHAPE_2D;
    case 0x003DE1C2u: return SHAPE_2D_Z;
    case 0x003DE200u: return SHAPE_2D_W;
    case 0x003DE250u: return SHAPE_2D_ZW;
    default:
        d3d8_hle_fatal(0x003DE080u,
                       "stage %u texture transform code %#x jumps through the table entry %#x to "
                       "garbage in the original",
                       (unsigned)stage, (unsigned)code, (unsigned)entry);
    }
}

static uint32_t shape_bytes(transform_shape shape)
{
    return shape == SHAPE_ZERO ? 8u : 76u;
}

/* fld then fstp of a single is a copy, except for a NaN: the physical x87 quiets a signalling one and
 * the oracle's emulator does not, so a NaN word that goes through the FPU is a named refusal (the
 * same policy as the viewport matrix) rather than a guess. */
static uint32_t through_x87(uint32_t bits, bool *nan)
{
    if ((bits & 0x7FFFFFFFu) > 0x7F800000u) *nan = true;
    return bits;
}

/* The 16 dwords of a stage matrix packet: source[row * 4 + column] transposed and cut to the shape.
 * The first element of each row is an integer move, the rest go through the FPU. */
static void shape_matrix(transform_shape shape, uint32_t stage, uint32_t out[16], bool *nan)
{
    uint32_t m[16];
    for (uint32_t index = 0u; index < 16u; index++)
        m[index] = d3d8_device_load32(TRANSFORM_SOURCE + stage * 0x40u + index * 4u);
#define F(index) through_x87(m[index], nan)
    for (uint32_t index = 0u; index < 16u; index++) out[index] = 0u;
    out[15] = 0x3F800000u;
    const bool full = shape >= SHAPE_FULL;
    out[0] = m[0];
    out[1] = F(4);
    out[4] = m[1];
    out[5] = F(5);
    if (shape == SHAPE_3X4 || full) {
        out[2] = F(8);
        out[3] = F(12);
        out[6] = F(9);
        out[7] = F(13);
    } else {
        out[3] = F(8);
        out[7] = F(9);
    }
    switch (shape) {
    case SHAPE_2D_Z:
    case SHAPE_2D_ZW:
        out[8] = m[2];
        out[9] = F(6);
        out[11] = F(10);
        break;
    case SHAPE_2D_W:
        out[12] = m[2];
        out[13] = F(6);
        out[15] = F(10);
        break;
    case SHAPE_FULL:
        out[8] = m[2];
        out[9] = F(6);
        out[10] = F(10);
        out[11] = F(14);
        break;
    case SHAPE_FULL_W:
        out[12] = m[2];
        out[13] = F(6);
        out[14] = F(10);
        out[15] = F(14);
        break;
    case SHAPE_FULL_Z:
        out[8] = m[2];
        out[9] = F(6);
        out[10] = F(10);
        out[11] = F(14);
        break;
    default:
        break;
    }
    if (shape == SHAPE_2D_ZW) {
        out[12] = m[3];
        out[13] = F(7);
        out[15] = F(11);
    } else if (shape == SHAPE_FULL) {
        out[12] = m[3];
        out[13] = F(7);
        out[14] = F(11);
        out[15] = F(15);
    }
#undef F
}

static void validate_texture_transforms(void)
{
    for (uint32_t stage = 0u; stage < 4u; stage++) {
        const transform_shape shape = stage_transform_shape(stage);
        if (shape == SHAPE_ZERO) continue;
        uint32_t matrix[16];
        bool nan = false;
        shape_matrix(shape, stage, matrix, &nan);
        if (nan)
            d3d8_hle_fatal(0x003DE080u,
                           "stage %u texture transform matrix has a NaN word: x87 payload "
                           "handling differs between the oracle and physical hardware",
                           (unsigned)stage);
    }
}

void d3d8_emit_texture_transforms(void)
{
    if (d3d8_dirty_programmable()) return;
    validate_texture_transforms();
    for (uint32_t stage = 0u; stage < 4u; stage++) {
        /* 0x003DE0D9: each stage opens with its own reservation preamble. */
        const uint32_t cursor = d3d8_pushbuffer_begin();
        const transform_shape shape = stage_transform_shape(stage);
        d3d8_guest_store32(cursor, TRANSFORM_PAIR + stage * 4u);
        if (shape == SHAPE_ZERO) {
            d3d8_guest_store32(cursor + 4u, 0u);
        } else {
            uint32_t matrix[16];
            bool nan = false;
            shape_matrix(shape, stage, matrix, &nan);
            d3d8_guest_store32(cursor + 4u, 1u);
            d3d8_guest_store32(cursor + 8u, TRANSFORM_MATRIX_HEADER + stage * 0x40u);
            for (uint32_t index = 0u; index < 16u; index++)
                d3d8_guest_store32(cursor + 12u + index * 4u, matrix[index]);
        }
        d3d8_pushbuffer_end(cursor + shape_bytes(shape));
    }
}

void d3d8_plan_texture_transforms(d3d8_pushbuffer_sim *sim)
{
    if (d3d8_dirty_programmable()) return;
    validate_texture_transforms();
    for (uint32_t stage = 0u; stage < 4u; stage++)
        d3d8_pushbuffer_sim_site(sim, 0x003DE080u, shape_bytes(stage_transform_shape(stage)));
}

#define MATRIX_WORLD_VIEW 0xDE0u
#define MATRIX_PROJECTION 0xC60u
#define MATRIX_VIEWPORT 0x980u

/* 0x003D9B30: out = a * b over 4x4 float matrices. The original is SSE, each output row is
 * ((a0*b_row0 + a1*b_row1) + a2*b_row2) + a3*b_row3 in single precision. */
static void matrix_multiply(float out[16], const float a[16], const float b[16])
{
    for (uint32_t row = 0u; row < 4u; row++) {
        for (uint32_t column = 0u; column < 4u; column++) {
            volatile float sum = a[row * 4u] * b[column];
            volatile float term = a[row * 4u + 1u] * b[4u + column];
            sum = sum + term;
            term = a[row * 4u + 2u] * b[8u + column];
            sum = sum + term;
            term = a[row * 4u + 3u] * b[12u + column];
            sum = sum + term;
            out[row * 4u + column] = sum;
        }
    }
}

static void load_matrix(float out[16], uint32_t address)
{
    for (uint32_t index = 0u; index < 16u; index++) {
        const uint32_t bits = d3d8_guest_load32(address + index * 4u);
        memcpy(&out[index], &bits, sizeof(bits));
    }
}

/* 0x003D62F0: the packet header then the matrix transposed (column major). */
static void store_matrix_packet(uint32_t cursor, uint32_t header, const float matrix[16])
{
    d3d8_guest_store32(cursor, header);
    for (uint32_t column = 0u; column < 4u; column++) {
        for (uint32_t row = 0u; row < 4u; row++) {
            uint32_t bits;
            memcpy(&bits, &matrix[row * 4u + column], sizeof(bits));
            d3d8_guest_store32(cursor + 4u + (column * 4u + row) * 4u, bits);
        }
    }
}

static bool matrices_run(uint32_t dirty)
{
    return (dirty & 0x80000000u) == 0u && !d3d8_dirty_programmable();
}

#define MATRIX_BLEND 0xE20u        /* the three blend matrices, 0x40 bytes each */
#define BLEND_MATRICES 3u          /* 0x003DED55: edi from 0x4004C0 in steps of 0x40 while below 0x400580 */
#define PACKET_PROJECTION 0x00400480u
#define PACKET_VIEWPORT 0x00400680u
#define PACKET_BLEND 0x004004C0u
#define PACKET_LIT 0x00300580u     /* 12 dwords */
#define PACKET_LIT_BLEND 0x003005C0u
#define MATRIX_PACKET_BYTES 68u
#define LIT_PACKET_BYTES 52u
#define GLOBAL_LIGHTING_STATE 0x003E3E58u
#define GLOBAL_BLENDING 0x003E3EE4u
#define GLOBAL_NORMALIZE 0x003E3EF8u

/* Everything 0x003DEB80 writes, computed before the first write (T533): the world-view-projection product, the
 * product with the viewport, the inverses of the lit path (render state 0x66 or a wrapped texture stage,
 * device+0x950) and the three blend products with theirs. A matrix that cannot be inverted or whose inverse
 * would be NaN is a named refusal here (d3d8_matrix_inverse.h). */
typedef struct {
    bool lit;
    bool blend;
    float first[16];
    float viewport[16];
    float second[16];
    uint32_t lit_first[16];
    float blended[BLEND_MATRICES][16];
    uint32_t lit_blended[BLEND_MATRICES][16];
} matrix_plan;

static void float_words(const float matrix[16], uint32_t words[16])
{
    memcpy(words, matrix, 16u * sizeof(uint32_t));
}

/* The original calls 0x003D9DB0 and copies 12 dwords of its output buffer without testing the result: a singular
 * matrix leaves the buffer as it was on the stack. */
static void lit_words(const float matrix[16], uint32_t output[16])
{
    uint32_t words[16];
    float_words(matrix, words);
    if (!d3d8_matrix_inverse_compute(words, d3d8_guest_load32(GLOBAL_NORMALIZE) == 0u, output)) {
        d3d8_hle_fatal(0x003D9DB0u,
                       "a singular matrix leaves the lit packet as uninitialised stack in the original");
    }
}

static void prepare_matrices(matrix_plan *plan)
{
    float world_view[16], projection[16];
    load_matrix(world_view, D3D8_DEVICE_BASE + MATRIX_WORLD_VIEW);
    load_matrix(projection, D3D8_DEVICE_BASE + MATRIX_PROJECTION);
    load_matrix(plan->viewport, D3D8_DEVICE_BASE + MATRIX_VIEWPORT);
    plan->lit = d3d8_device_load32(0x950u) != 0u || d3d8_guest_load32(GLOBAL_LIGHTING_STATE) != 0u;
    plan->blend = d3d8_guest_load32(GLOBAL_BLENDING) != 0u;
    matrix_multiply(plan->first, world_view, projection);
    matrix_multiply(plan->second, plan->first, plan->viewport);
    if (plan->lit) lit_words(plan->first, plan->lit_first);
    if (!plan->blend) return;
    for (uint32_t index = 0u; index < BLEND_MATRICES; index++) {
        float blend[16];
        load_matrix(blend, D3D8_DEVICE_BASE + MATRIX_BLEND + index * 0x40u);
        matrix_multiply(plan->blended[index], blend, projection);
        if (plan->lit) lit_words(plan->blended[index], plan->lit_blended[index]);
    }
}

static uint32_t store_lit_packet(uint32_t cursor, uint32_t header, const uint32_t words[16])
{
    d3d8_guest_store32(cursor, header);
    for (uint32_t index = 0u; index < 12u; index++) d3d8_guest_store32(cursor + 4u + index * 4u, words[index]);
    return cursor + LIT_PACKET_BYTES;
}

static uint32_t store_packet_at(uint32_t cursor, uint32_t header, const float matrix[16])
{
    store_matrix_packet(cursor, header, matrix);
    return cursor + MATRIX_PACKET_BYTES;
}

static void plan_matrix_sites(const matrix_plan *plan, d3d8_pushbuffer_sim *sim)
{
    const uint32_t lit_bytes = plan->lit ? LIT_PACKET_BYTES : 0u;
    d3d8_pushbuffer_sim_site(sim, 0x003DEB80u, 2u * MATRIX_PACKET_BYTES + lit_bytes);
    if (plan->blend) {
        d3d8_pushbuffer_sim_site(sim, 0x003DEB80u, BLEND_MATRICES * (MATRIX_PACKET_BYTES + lit_bytes));
    }
}

/* 0x003DEB80. One reservation preamble at the entry, and all of a plain call (the product with the world-view,
 * the lit packet, the product with the viewport) lands behind it. Vertex blending (0x003E3EE4) writes the viewport
 * matrix itself instead of the product, publishes the cursor, makes a second preamble and loops over the three
 * blend matrices, each a 68 byte packet at 0x4004C0 + 0x40 * index and, when lit, a 52 byte one. */
void d3d8_emit_fixed_function_matrices(uint32_t dirty)
{
    if (!matrices_run(dirty)) return;
    matrix_plan plan;
    prepare_matrices(&plan);
    /* Both preambles of the blended path are planned first: a roll-over device flag 4 cannot take refuses before the
     * first packet, not between the two. */
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    plan_matrix_sites(&plan, &sim);
    uint32_t cursor = d3d8_pushbuffer_begin();
    cursor = store_packet_at(cursor, PACKET_PROJECTION, plan.first);
    if (plan.lit) cursor = store_lit_packet(cursor, PACKET_LIT, plan.lit_first);
    if (!plan.blend) {
        d3d8_pushbuffer_end(store_packet_at(cursor, PACKET_VIEWPORT, plan.second));
        return;
    }
    d3d8_pushbuffer_end(store_packet_at(cursor, PACKET_VIEWPORT, plan.viewport));
    cursor = d3d8_pushbuffer_begin();
    for (uint32_t index = 0u; index < BLEND_MATRICES; index++) {
        cursor = store_packet_at(cursor, PACKET_BLEND + index * 0x40u, plan.blended[index]);
        if (plan.lit) cursor = store_lit_packet(cursor, PACKET_LIT_BLEND + index * 0x40u, plan.lit_blended[index]);
    }
    d3d8_pushbuffer_end(cursor);
}

void d3d8_plan_fixed_function_matrices(uint32_t dirty, d3d8_pushbuffer_sim *sim)
{
    if (!matrices_run(dirty)) return;
    matrix_plan plan;
    prepare_matrices(&plan);
    plan_matrix_sites(&plan, sim);
}

void d3d8_plan_default_lighting(uint32_t dirty, d3d8_pushbuffer_sim *sim)
{
    d3d8_validate_default_lighting(dirty);
    d3d8_pushbuffer_sim_site(sim, 0x003DE830u,
                             ((dirty & 0x1000u) != 0u ? 24u : 0u) +
                             ((dirty & 0xFF8000u) != 0u ? 8u : 0u));
}
