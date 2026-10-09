/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_state.h. Every function carries the address of the original it ports.
 */

#include "d3d8_state.h"

#include <stdbool.h>
#include <string.h>

#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "d3d8_scaled_viewport.h"
#include "d3d8_shader.h"
#include "d3d8_texture_dirty.h"
#include "d3d8_viewport_matrix.h"
#include "kernel_call.h"

/* Device fields. */
#define DEV_CONTROL_WORD_A 0x2428u
#define DEV_CONTROL_WORD_B 0x242Cu
#define DEV_CONSTANT_MODE 0x1928u
#define DEV_RENDER_TARGET 0x1A04u
#define DEV_BACK_BUFFER_0 0x1A14u

/* Globals read by the control-word update. */
#define GLOBAL_CONTROL_SOURCE_A 0x003E3F54u

/* Render states 0x4D to 0x51, set through the out-of-line SetRenderState by the depth-bias
 * helper. All below 0x5C, so each is an immediate-class state: the original emits it and then
 * stores the shadow, and the shadow is what is reproduced. */
#define RS_DEPTH_BIAS_FIRST 0x4Du

/* Float constants in the title's `.rdata`, read rather than copied: 2^32 and 0.25. */
#define CONSTANT_TWO_POW_32 0x00475CCCu
#define CONSTANT_QUARTER 0x00475D4Cu
#define CONSTANT_EIGHT 0x0047D1E4u
#define CONSTANT_HALF 0x00475CD4u
#define GLOBAL_FORMAT_INFO 0x003E1828u

static void store_state(uint32_t global, uint32_t value)
{
    d3d8_guest_store32(global, value);
}

void d3d8_state_set_93(uint32_t value)
{
    store_state(D3D8_STATE_93, value);
}

void d3d8_state_set_90(uint32_t value)
{
    store_state(D3D8_STATE_90, value);
}

void d3d8_state_set_91(uint32_t value)
{
    store_state(D3D8_STATE_91, value);
}

void d3d8_state_set_94(uint32_t value)
{
    store_state(D3D8_STATE_94, value);
}

void d3d8_state_set_a1(uint32_t value)
{
    store_state(D3D8_STATE_A1, value);
}

void d3d8_state_update_control_words(void)
{
    uint32_t word_a = d3d8_device_load32(DEV_CONTROL_WORD_A) & 0xFFFFFFF7u;
    if (d3d8_guest_load32(GLOBAL_CONTROL_SOURCE_A) != 0u) {
        word_a |= 0x8u;
    }
    d3d8_device_store32(DEV_CONTROL_WORD_A, word_a);

    uint32_t word_b = d3d8_device_load32(DEV_CONTROL_WORD_B) & 0xE7EFFFFFu;
    if (d3d8_guest_load32(D3D8_STATE_A3) != 0u) {
        word_b |= 0x00100000u;
    }
    if (d3d8_guest_load32(D3D8_STATE_A4) != 0u) {
        word_b |= 0x08000000u;
    }
    d3d8_device_store32(DEV_CONTROL_WORD_B, word_b);
}

void d3d8_state_set_a3(uint32_t value)
{
    store_state(D3D8_STATE_A3, value);
    d3d8_state_update_control_words();
}

void d3d8_state_set_a4(uint32_t value)
{
    store_state(D3D8_STATE_A4, value);
    d3d8_state_update_control_words();
}

void d3d8_state_set_8f(uint32_t value)
{
    const uint32_t previous = d3d8_guest_load32(D3D8_STATE_8F);
    store_state(D3D8_STATE_8F, value);
    if (previous == 2u || value == 2u) {
        d3d8_hle_note_unmodelled(0x003D7EE0u,
                                 "state 0x8F entered or left value 2: the original flushes "
                                 "deferred state and rebuilds the viewport (0x003D6E50, "
                                 "0x003D5C50, 0x003D7A50, 0x003D7860)");
    }
}

/* 0x003D81F0, the title's own call (T598): the same store and 0x003D7B80 plus SetViewport(NULL) at the first back
 * buffer as the library's own SetRenderState reaches (T533), one function, so the title's boot stream carries the
 * 0x40208, 0x41D7C and viewport packets the original writes. */
void d3d8_state_set_9a(uint32_t value)
{
    d3d8_state_library_set_9a(value);
}

/* --- the library's own helpers (T443) ---------------------------------------------------- */

#define DEV_DEPTH_SURFACE 0x1A08u
#define PUSH_FILL_MODE 0x0008038Cu
#define PUSH_LOGIC_OP_OFF 0x000417BCu
#define PUSH_LOGIC_OP_ON 0x000817BCu
#define PUSH_EDGE_ANTI_ALIAS 0x00080320u
#define PUSH_MULTISAMPLE_MASK 0x00041D7Cu
#define PUSH_SURFACE_CONTROL 0x00040290u

void d3d8_state_set_88(uint32_t value)
{
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    d3d8_guest_store32(device + 0x790u, value);
    d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK, d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | 0x4000u);
    store_state(D3D8_STATE_88, value);
}

/* 0x003D7380 and 0x003D73D0 emit the same fill mode packet. The back fill mode is used for the
 * second word only when two sided lighting (state 0x8D) is on. */
static void emit_fill_mode(uint32_t front)
{
    uint32_t cursor = d3d8_pushbuffer_begin();
    const uint32_t two_sided = d3d8_guest_load32(D3D8_STATE_8D);
    const uint32_t back = d3d8_guest_load32(D3D8_STATE_8C);
    const uint32_t second = two_sided != 0u ? back : front;
    d3d8_guest_store32(cursor, PUSH_FILL_MODE);
    d3d8_guest_store32(cursor + 4u, front);
    d3d8_guest_store32(cursor + 8u, second);
    d3d8_pushbuffer_end(cursor + 12u);
}

void d3d8_state_set_8b(uint32_t value)
{
    /* Both shadows are read after the original refill boundary, in instruction order. */
    uint32_t cursor = d3d8_pushbuffer_begin();
    const uint32_t two_sided = d3d8_guest_load32(D3D8_STATE_8D);
    const uint32_t back = d3d8_guest_load32(D3D8_STATE_8C);
    const uint32_t second = two_sided != 0u ? back : value;
    d3d8_guest_store32(cursor, PUSH_FILL_MODE);
    d3d8_guest_store32(cursor + 4u, value);
    d3d8_guest_store32(cursor + 8u, second);
    d3d8_pushbuffer_end(cursor + 12u);
    store_state(D3D8_STATE_8B, value);
}

void d3d8_state_set_8c(uint32_t value)
{
    /* Stores the new back fill mode first, then reads the CURRENT fill mode (0x003D73DB, 0x003D73E6)
     * and emits it with the new back mode when two sided lighting is on. */
    store_state(D3D8_STATE_8C, value);
    const uint32_t front = d3d8_guest_load32(D3D8_STATE_8B);
    emit_fill_mode(front);
    store_state(D3D8_STATE_8B, front);
}

void d3d8_state_set_96(uint32_t value)
{
    uint32_t cursor = d3d8_pushbuffer_begin();
    if (value == 0u) {
        d3d8_guest_store32(cursor, PUSH_LOGIC_OP_OFF);
        d3d8_guest_store32(cursor + 4u, value);
        d3d8_pushbuffer_end(cursor + 8u);
    } else {
        d3d8_guest_store32(cursor, PUSH_LOGIC_OP_ON);
        d3d8_guest_store32(cursor + 4u, 1u);
        d3d8_guest_store32(cursor + 8u, value);
        d3d8_pushbuffer_end(cursor + 12u);
    }
    store_state(D3D8_STATE_96, value);
}

void d3d8_state_set_97(uint32_t value)
{
    uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, PUSH_EDGE_ANTI_ALIAS);
    d3d8_guest_store32(cursor + 4u, value);
    d3d8_guest_store32(cursor + 8u, value);
    d3d8_pushbuffer_end(cursor + 12u);
    store_state(D3D8_STATE_97, value);
}

void d3d8_state_set_99(uint32_t value)
{
    /* The mask is stored first and the word is formed BEFORE the reservation. */
    store_state(D3D8_STATE_99, value);
    uint32_t word = (value << 16) | d3d8_guest_load32(D3D8_STATE_9E);
    if ((d3d8_device_load32(D3D8_DEV_FLAGS) & 0x8000u) != 0u &&
        d3d8_guest_load32(D3D8_STATE_98) != 0u) {
        word |= 1u;
    }
    const uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, PUSH_MULTISAMPLE_MASK);
    d3d8_guest_store32(cursor + 4u, word);
    d3d8_pushbuffer_end(cursor + 8u);
}

void d3d8_state_set_9b(uint32_t value)
{
    /* 0x003D8220: the store, then 0x003D7B80 unless the render target is the first back buffer. */
    if (d3d8_device_load32(DEV_RENDER_TARGET) != d3d8_device_load32(DEV_BACK_BUFFER_0)) {
        d3d8_scaled_viewport_run(value, D3D8_STATE_9B);
        return;
    }
    store_state(D3D8_STATE_9B, value);
}

static uint32_t emit_surface_control_at(uint32_t device, uint32_t cursor)
{
    uint32_t word = 0x00100001u;
    if (d3d8_guest_load32(D3D8_STATE_A0) != 0u) {
        word = 0x10100001u;
    }
    if (d3d8_guest_load32(D3D8_STATE_8F) == 2u) {
        word |= 0x10000u;
    }
    const uint32_t depth = d3d8_guest_load32(device + DEV_DEPTH_SURFACE);
    if (depth != 0u) {
        const uint8_t format = d3d8_guest_load8(depth + 0xDu);
        if (format == 0x2Du || format == 0x2Bu || format == 0x31u || format == 0x2Fu) {
            word |= 0x1000u;
        }
    }
    d3d8_guest_store32(cursor, PUSH_SURFACE_CONTROL);
    d3d8_guest_store32(cursor + 4u, word);
    return cursor + 8u;
}

uint32_t d3d8_state_emit_surface_control(uint32_t cursor)
{
    return emit_surface_control_at(D3D8_DEVICE_BASE, cursor);
}

void d3d8_state_set_a0(uint32_t value)
{
    store_state(D3D8_STATE_A0, value);
    const uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_pushbuffer_end(d3d8_state_emit_surface_control(cursor));
}

void d3d8_state_library_set_93(uint32_t value)
{
    uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, 0x00040308u);
    if (value == 0u) {
        d3d8_guest_store32(cursor + 4u, 0u);
        d3d8_pushbuffer_end(cursor + 8u);
    } else {
        d3d8_guest_store32(cursor + 4u, 1u);
        d3d8_guest_store32(cursor + 8u, 0x0004039Cu);
        d3d8_guest_store32(cursor + 12u,
                           0x404u + (value != d3d8_guest_load32(D3D8_STATE_92) ? 1u : 0u));
        d3d8_pushbuffer_end(cursor + 16u);
    }
    store_state(D3D8_STATE_93, value);
}

void d3d8_state_library_set_90(uint32_t value)
{
    /* The stencil enable is stored first, before the reservation (0x003D7F7C). */
    store_state(D3D8_STATE_90, value);
    const uint32_t cursor = d3d8_pushbuffer_begin();
    uint32_t cull = d3d8_guest_load32(D3D8_STATE_A2) != 0u ? 2u : 0u;
    if (d3d8_guest_load32(D3D8_STATE_A1) != 0u &&
        (d3d8_guest_load32(D3D8_STATE_90) == 0u || d3d8_guest_load32(D3D8_STATE_91) == 0x1E00u)) {
        cull |= 1u;
    }
    d3d8_guest_store32(cursor, 0x00041D84u);
    d3d8_guest_store32(cursor + 4u, cull);
    d3d8_guest_store32(cursor + 8u, 0x0004032Cu);
    d3d8_guest_store32(cursor + 12u,
                       value != 0u && d3d8_device_load32(DEV_DEPTH_SURFACE) != 0u ? 1u : 0u);
    d3d8_pushbuffer_end(cursor + 16u);
}

void d3d8_state_library_set_94(uint32_t value)
{
    if (d3d8_device_load32(0x784u) == 0u) {
        const uint32_t cursor = d3d8_pushbuffer_begin();
        d3d8_guest_store32(cursor, 0x00400A60u);
        for (uint32_t index = 0u; index < 16u; index++) {
            d3d8_guest_store32(cursor + 4u + index * 4u, value);
        }
        d3d8_pushbuffer_end(cursor + 68u);
    }
    store_state(D3D8_STATE_94, value);
}

static float float_of_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t bits_of_float(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void d3d8_state_set_95(uint32_t value)
{
    /* 0x003D72A4: `fild` of the argument, widened by 2^32 when it reads as negative (so the
     * argument is treated as unsigned), negated and stored as a float, then scaled by the
     * constant at 0x475D4C. x87 does this in extended precision and rounds to float on each
     * store, which long double reproduces. */
    long double widened = (long double)(int32_t)value;
    if ((int32_t)value < 0) {
        widened += (long double)float_of_bits(d3d8_guest_load32(CONSTANT_TWO_POW_32));
    }
    const long double negated = -widened;
    const float negated_float = (float)negated;
    const float scaled_float =
        (float)(negated * (long double)float_of_bits(d3d8_guest_load32(CONSTANT_QUARTER)));
    const uint32_t flag = value != 0u ? 1u : 0u;

    const uint32_t shadow_values[5] = {bits_of_float(scaled_float), bits_of_float(negated_float),
                                       flag, flag, flag};
    for (uint32_t index = 0u; index < 5u; index++) {
        d3d8_guest_store32(D3D8_GLOBAL_RS_SHADOW + (RS_DEPTH_BIAS_FIRST + index) * 4u,
                           shadow_values[index]);
    }
    store_state(D3D8_STATE_95, value);
}

/* 0x003D72A0 through the library's own dispatcher: the five states go through 0x003D6CC0, so each
 * emits its pair (the immediate class) and the shadow follows. d3d8_set_state.c owns that
 * dispatcher, this forward declaration keeps the include one way. */
void d3d8_set_render_state_notinline(int32_t state, uint32_t value);

void d3d8_state_library_set_95(uint32_t value)
{
    long double widened = (long double)(int32_t)value;
    if ((int32_t)value < 0) {
        widened += (long double)float_of_bits(d3d8_guest_load32(CONSTANT_TWO_POW_32));
    }
    const long double negated = -widened;
    const float negated_float = (float)negated;
    const float scaled_float =
        (float)(negated * (long double)float_of_bits(d3d8_guest_load32(CONSTANT_QUARTER)));
    const uint32_t flag = value != 0u ? 1u : 0u;
    const uint32_t states[5] = {bits_of_float(scaled_float), bits_of_float(negated_float), flag,
                                flag, flag};
    for (uint32_t index = 0u; index < 5u; index++) {
        d3d8_set_render_state_notinline((int32_t)(RS_DEPTH_BIAS_FIRST + index), states[index]);
    }
    store_state(D3D8_STATE_95, value);
}

/* --- T461: the emitting library variants of the helpers the title stores without emitting -- */

#define DEV_DECLARATION 0x0794u
#define PUSH_CULL 0x00041D84u
#define PUSH_CONTROL_RESET 0x00040110u
#define PUSH_CONTROL_SELECT 0x00081D8Cu
#define PUSH_CONTROL_STROBE 0x00040100u
#define GLOBAL_STATE_4A 0x003E3E08u

/* The 0x41D84 word of 0x003D7F70, 0x003D8010, 0x003D80B0 and 0x003D8120: bit 1 follows state
 * 0xA2, bit 0 is set when state 0xA1 is on and stencil (0x90) is off or state 0x91 is 0x1E00. */
static uint32_t cull_word(void)
{
    uint32_t word = d3d8_guest_load32(D3D8_STATE_A2) != 0u ? 2u : 0u;
    if (d3d8_guest_load32(D3D8_STATE_A1) != 0u &&
        (d3d8_guest_load32(D3D8_STATE_90) == 0u || d3d8_guest_load32(D3D8_STATE_91) == 0x1E00u)) {
        word |= 1u;
    }
    return word;
}

static void emit_cull_word(void)
{
    const uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, PUSH_CULL);
    d3d8_guest_store32(cursor + 4u, cull_word());
    d3d8_pushbuffer_end(cursor + 8u);
}

void d3d8_state_library_set_91(uint32_t value)
{
    store_state(D3D8_STATE_91, value);
    const uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, PUSH_CULL);
    d3d8_guest_store32(cursor + 4u, cull_word());
    d3d8_guest_store32(cursor + 8u, 0x00040370u);
    d3d8_guest_store32(cursor + 12u, value);
    d3d8_pushbuffer_end(cursor + 16u);
}

void d3d8_state_library_set_a1(uint32_t value)
{
    store_state(D3D8_STATE_A1, value);
    emit_cull_word();
}

/* 0x003D7AB0: the control words, then the 12 dword packet that selects and strobes both. */
static void emit_control_words(void)
{
    d3d8_state_update_control_words();
    const uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, PUSH_CONTROL_RESET);
    d3d8_guest_store32(cursor + 4u, 0u);
    d3d8_guest_store32(cursor + 8u, PUSH_CONTROL_SELECT);
    d3d8_guest_store32(cursor + 12u, 0x00400094u);
    d3d8_guest_store32(cursor + 16u, d3d8_device_load32(DEV_CONTROL_WORD_A));
    d3d8_guest_store32(cursor + 20u, PUSH_CONTROL_STROBE);
    d3d8_guest_store32(cursor + 24u, 9u);
    d3d8_guest_store32(cursor + 28u, PUSH_CONTROL_SELECT);
    d3d8_guest_store32(cursor + 32u, 0x00400B80u);
    d3d8_guest_store32(cursor + 36u, d3d8_device_load32(DEV_CONTROL_WORD_B));
    d3d8_guest_store32(cursor + 40u, PUSH_CONTROL_STROBE);
    d3d8_guest_store32(cursor + 44u, 9u);
    d3d8_pushbuffer_end(cursor + 48u);
}

void d3d8_state_library_set_a3(uint32_t value)
{
    store_state(D3D8_STATE_A3, value);
    emit_control_words();
}

void d3d8_state_library_set_a4(uint32_t value)
{
    store_state(D3D8_STATE_A4, value);
    emit_control_words();
}

/* 0x003D81F0: the store, then 0x003D7B80 (the scaled viewport and clip recomputation, then SetViewport(NULL), T533)
 * when the render target is the first back buffer. */
void d3d8_state_library_set_9a(uint32_t value)
{
    if (d3d8_device_load32(DEV_RENDER_TARGET) == d3d8_device_load32(DEV_BACK_BUFFER_0)) {
        d3d8_scaled_viewport_run(value, D3D8_STATE_9A);
        return;
    }
    store_state(D3D8_STATE_9A, value);
}

/* 0x003D7EE0. The packet first, then, when the state enters or leaves value 2, the viewport matrix
 * (0x003D6E50), the vertex program helper (0x003D5C50, a no-op unless the declaration is
 * programmable), the surface control packet (0x003D7A50) and the viewport packets (0x003D7860). The
 * whole sequence is planned first so every refusal precedes the first write. */
void d3d8_state_library_set_8f(uint32_t value)
{
    const bool transition = d3d8_guest_load32(D3D8_STATE_8F) == 2u || value == 2u;
    const uint32_t declaration_flags =
        d3d8_guest_load32(d3d8_device_load32(DEV_DECLARATION) + 4u);
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    d3d8_pushbuffer_sim_site(&sim, 0x003D7EE0u, 16u);
    if (transition) {
        d3d8_rebuild_viewport_matrix_check();
        d3d8_plan_vertex_program_helper(&sim);
        const bool programmable = (declaration_flags & 0x12u) != 0u;
        const uint32_t viewport =
            programmable ? ((d3d8_device_load32(D3D8_DEV_FLAGS) & 0x200u) != 0u ? 0u : 40u) : 20u;
        d3d8_pushbuffer_sim_site(&sim, 0x003D7EE0u, 8u + viewport + 12u);
    }
    uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, 0x0004030Cu);
    d3d8_guest_store32(cursor + 4u,
                       value != 0u && d3d8_device_load32(DEV_DEPTH_SURFACE) != 0u ? 1u : 0u);
    d3d8_guest_store32(cursor + 8u, 0x00041D78u);
    d3d8_guest_store32(cursor + 12u, d3d8_guest_load32(GLOBAL_STATE_4A));
    d3d8_pushbuffer_end(cursor + 16u);
    store_state(D3D8_STATE_8F, value);
    if (!transition) {
        return;
    }
    (void)d3d8_rebuild_viewport_matrix();
    d3d8_run_vertex_program_helper();
    cursor = d3d8_pushbuffer_begin();
    cursor = d3d8_state_emit_surface_control(cursor);
    cursor = d3d8_viewport_emit(cursor, declaration_flags);
    d3d8_pushbuffer_end(cursor);
}

/* --- T461: the remaining emitting library helpers, states 0x89 0x8A 0x8D 0x8E 0x92 0x98 0x9C 0x9D 0x9E
 * 0x9F 0xA2 and 0xA5. The title never calls them and nothing ported reached them. --------------- */

#define RS_SHADOW_OF(state) (D3D8_GLOBAL_RS_SHADOW + (state) * 4u)

/* One pair behind the plain `cursor >= limit` preamble. */
static void emit_pair_at_preamble(uint32_t header, uint32_t value)
{
    const uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, header);
    d3d8_guest_store32(cursor + 4u, value);
    d3d8_pushbuffer_end(cursor + 8u);
}

static void dirty_or(uint32_t bits)
{
    d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK, d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | bits);
}

/* The multisample mask word of 0x003D8320 and 0x003D8250: state 0x99 above, state 0x9E below, bit 0
 * when device flag 0x8000 and state 0x98 are both set. */
static uint32_t multisample_word(uint32_t sample_alpha, uint32_t antialias)
{
    uint32_t word = (d3d8_guest_load32(D3D8_STATE_99) << 16) | sample_alpha;
    if ((d3d8_device_load32(D3D8_DEV_FLAGS) & 0x8000u) != 0u && antialias != 0u) {
        word |= 1u;
    }
    return word;
}

void d3d8_state_library_set_89(uint32_t value)
{
    dirty_or(0x200u);
    emit_pair_at_preamble(0x00040328u, value);
    d3d8_guest_store32(RS_SHADOW_OF(0x89u), value);
}

void d3d8_state_library_set_8a(uint32_t value)
{
    const uint32_t swapped =
        ((value >> 16) & 0xFFu) | ((value & 0xFFu) << 16) | (value & 0xFF00FF00u);
    emit_pair_at_preamble(0x000402A8u, swapped);
    d3d8_guest_store32(RS_SHADOW_OF(0x8Au), value);
}

void d3d8_state_library_set_8d(uint32_t value)
{
    emit_pair_at_preamble(0x000417C4u, value);
    if (value != 0u) {
        dirty_or(0x9000u);
    }
    const uint32_t front = d3d8_guest_load32(D3D8_STATE_8B);
    d3d8_guest_store32(D3D8_STATE_8D, value);
    const uint32_t cursor = d3d8_pushbuffer_begin();
    const uint32_t two_sided = d3d8_guest_load32(D3D8_STATE_8D);
    const uint32_t back = d3d8_guest_load32(D3D8_STATE_8C);
    d3d8_guest_store32(cursor, PUSH_FILL_MODE);
    d3d8_guest_store32(cursor + 4u, front);
    d3d8_guest_store32(cursor + 8u, two_sided != 0u ? back : front);
    d3d8_pushbuffer_end(cursor + 12u);
    d3d8_guest_store32(D3D8_STATE_8B, front);
}

void d3d8_state_library_set_8e(uint32_t value)
{
    emit_pair_at_preamble(0x000403A4u, value);
    dirty_or(0x200u);
    d3d8_guest_store32(RS_SHADOW_OF(0x8Eu), value);
}

void d3d8_state_library_set_92(uint32_t value)
{
    emit_pair_at_preamble(0x000403A0u, value);
    const uint32_t current_93 = d3d8_guest_load32(D3D8_STATE_93);
    d3d8_guest_store32(D3D8_STATE_92, value);
    /* A tail call: 0x003D7060 with the CURRENT state 0x93, which stores it back. */
    d3d8_state_library_set_93(current_93);
}

/* 0x003D8250 runs the vertex program helper 0x003D5C50 (a no-op unless the declaration has flag 2),
 * then emits its word and the viewport packets at a second preamble. */
void d3d8_state_library_set_98(uint32_t value)
{
    const uint32_t declaration_flags =
        d3d8_guest_load32(d3d8_device_load32(DEV_DECLARATION) + 4u);
    const bool programmable = (declaration_flags & 0x12u) != 0u;
    const uint32_t viewport =
        programmable ? ((d3d8_device_load32(D3D8_DEV_FLAGS) & 0x200u) != 0u ? 0u : 40u) : 20u;
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    d3d8_plan_vertex_program_helper(&sim);
    d3d8_pushbuffer_sim_site(&sim, 0x003D8250u, 8u);
    d3d8_pushbuffer_sim_site(&sim, 0x003D8250u, viewport + 12u);
    d3d8_guest_store32(D3D8_STATE_98, value);
    d3d8_run_vertex_program_helper();
    uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, PUSH_MULTISAMPLE_MASK);
    d3d8_guest_store32(cursor + 4u, multisample_word(d3d8_guest_load32(D3D8_STATE_9E), value));
    d3d8_pushbuffer_end(cursor + 8u);
    cursor = d3d8_pushbuffer_begin();
    d3d8_pushbuffer_end(d3d8_viewport_emit(cursor, declaration_flags));
}

void d3d8_state_library_set_9c(uint32_t value)
{
    emit_pair_at_preamble(0x00041E6Cu, value - 0x200u);
    d3d8_guest_store32(RS_SHADOW_OF(0x9Cu), value);
}

/* cvttss2si: the integer indefinite for a NaN or an out of range value. */
static uint32_t truncate_to_integer(float value)
{
    if (value != value || value >= 2147483648.0f || value < -2147483648.0f) {
        return 0x80000000u;
    }
    return (uint32_t)(int32_t)value;
}

void d3d8_state_library_set_9d(uint32_t value)
{
    /* fld, fmul device+0x964, fmul 8.0, fadd 0.5 in extended precision, one float store. */
    const float scale = float_of_bits(d3d8_device_load32(0x964u));
    long double scaled = (long double)float_of_bits(value) * (long double)scale;
    scaled = scaled * (long double)float_of_bits(d3d8_guest_load32(CONSTANT_EIGHT));
    scaled = scaled + (long double)float_of_bits(d3d8_guest_load32(CONSTANT_HALF));
    uint32_t level = truncate_to_integer((float)scaled);
    if (level > 0x1FFu) {
        level = 0x1FFu;
    }
    emit_pair_at_preamble(0x00040380u, level);
    d3d8_guest_store32(RS_SHADOW_OF(0x9Du), value);
}

void d3d8_state_library_set_9e(uint32_t value)
{
    d3d8_guest_store32(D3D8_STATE_9E, value);
    const uint32_t word = multisample_word(value, d3d8_guest_load32(D3D8_STATE_98));
    emit_pair_at_preamble(PUSH_MULTISAMPLE_MASK, word);
}

/* 0x003D7220 as the library runs it, with the command the title's port (d3d8_set_render_target_flag)
 * elides: the supersample bit of the device flags follows the render target format, and a change
 * writes the 0x40110 and 0x40100 pair. */
void d3d8_state_library_set_9f(uint32_t value)
{
    const uint32_t target = d3d8_device_load32(DEV_RENDER_TARGET);
    const uint32_t format = d3d8_guest_load8(target + 0xDu);
    const uint32_t wanted =
        (d3d8_guest_load8(GLOBAL_FORMAT_INFO + format) & 0x3Cu) == 0x20u ? value : 0u;
    const uint32_t flags = d3d8_device_load32(D3D8_DEV_FLAGS);
    if (wanted != (flags & 1u)) {
        d3d8_device_store32(D3D8_DEV_FLAGS, flags ^ 1u);
        const uint32_t cursor = d3d8_pushbuffer_begin();
        d3d8_guest_store32(cursor, PUSH_CONTROL_RESET);
        d3d8_guest_store32(cursor + 4u, 0u);
        d3d8_guest_store32(cursor + 8u, PUSH_CONTROL_STROBE);
        d3d8_guest_store32(cursor + 12u, (wanted << 5) | 8u);
        d3d8_pushbuffer_end(cursor + 16u);
    }
    d3d8_guest_store32(RS_SHADOW_OF(0x9Fu), value);
}

void d3d8_state_library_set_a2(uint32_t value)
{
    d3d8_guest_store32(D3D8_STATE_A2, value);
    emit_cull_word();
}

void d3d8_state_library_set_a5(uint32_t value)
{
    d3d8_guest_store32(GLOBAL_CONTROL_SOURCE_A, value);
    emit_control_words();
}

static uint32_t constant_mode(uint32_t mode)
{
    /* Original 0x003D5AF7 captures EBX before any guest writes. */
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    const uint32_t old_flags = d3d8_guest_load32(device + D3D8_DEV_FLAGS);
    const uint32_t flags = (old_flags & 0xFFFFFDFFu) | ((mode & 0x10u) << 5u);
    const uint32_t masked = mode & 0xFFFFFFEFu;
    d3d8_guest_store32(device + D3D8_DEV_FLAGS, flags);
    d3d8_guest_store32(device + DEV_CONSTANT_MODE, masked);
    if (masked != 0u) {
        return masked;
    }
    d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK,
                       d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | 0x1600u);
    uint32_t cursor = d3d8_guest_load32(device + D3D8_DEV_CURSOR);
    if (cursor >= d3d8_guest_load32(device + D3D8_DEV_LIMIT)) {
        if (device != D3D8_DEVICE_BASE) {
            d3d8_hle_fatal(0x003D5AF0u,
                "constant-mode refill for a nondefault device is not modelled");
        }
        cursor = d3d8_pushbuffer_begin();
    }
    const uint32_t start = cursor;
    d3d8_guest_store32(cursor, 0x00041EA4u);
    d3d8_guest_store32(cursor + 4u, 0x3Cu);
    d3d8_guest_store32(cursor + 8u, 0x00300B80u);
    /* REP MOVSD and each 0x003D62F0 load/store are sequential, not snapshots. */
    for (uint32_t word = 0u; word < 12u; ++word) {
        const uint32_t value = d3d8_guest_load32(0x003E18D8u + word * 4u);
        d3d8_guest_store32(cursor + 12u + word * 4u, value);
    }
    cursor += 60u;
    for (uint32_t matrix = 0u; matrix < 4u; ++matrix) {
        d3d8_guest_store32(cursor, 0x00400840u + matrix * 0x40u);
        for (uint32_t column = 0u; column < 4u; ++column) {
            for (uint32_t row = 0u; row < 4u; ++row) {
                const uint32_t value = d3d8_guest_load32(0x003E1898u +
                    (row * 4u + column) * 4u);
                d3d8_guest_store32(cursor + 4u + (column * 4u + row) * 4u, value);
            }
        }
        cursor += 68u;
    }
    d3d8_guest_store32(cursor, 0x001009D0u);
    d3d8_guest_store32(cursor + 4u, 0u);
    d3d8_guest_store32(cursor + 8u, 0u);
    /* 0x003D5BC7 writes +16 before 0x003D5BD2 publishes +12. */
    d3d8_guest_store32(cursor + 16u, 0u);
    d3d8_guest_store32(cursor + 12u, 0x3F800000u);
    d3d8_guest_store32(cursor + 20u, 0x00100A50u);
    d3d8_guest_store32(cursor + 24u, 0u);
    d3d8_guest_store32(cursor + 28u, 0u);
    d3d8_guest_store32(cursor + 32u, 0u);
    d3d8_guest_store32(cursor + 36u, 0x3F800000u);
    d3d8_pushbuffer_end_at(device, start, cursor + 40u);
    return 0u;
}

void d3d8_state_set_constant_mode(uint32_t mode)
{
    (void)constant_mode(mode);
}

void d3d8_state_set_vertex_shader_constant(uint32_t index, uint32_t source)
{
    if (index >= D3D8_CONSTANT_REGISTERS) {
        d3d8_hle_fatal(0x003D5670u,
                       "constant register %u is past the 192-entry shadow (the original does "
                       "not bound it and would overwrite whatever follows)",
                       (unsigned)index);
    }
    const uint32_t shadow = D3D8_CONSTANT_SHADOW + (index << 4);
    for (uint32_t word = 0u; word < 4u; word++) {
        d3d8_guest_store32(shadow + word * 4u, d3d8_guest_load32(source + word * 4u));
    }
}

/* --- handlers -------------------------------------------------------------------------- */

static uint32_t argument(const void *context, unsigned index, uint32_t address)
{
    uint32_t value = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, index, &value)) {
        d3d8_hle_fatal(address, "argument %u of the call cannot be read", index);
    }
    return value;
}

#define STATE_HANDLER(name, address, call)                                                   \
    static uint32_t name(void *context)                                                      \
    {                                                                                        \
        call(argument(context, 0u, address));                                                \
        return 0u;                                                                           \
    }

static uint32_t state_packet_begin_at(uint32_t device, uint32_t address)
{
    uint32_t cursor = d3d8_guest_load32(device + D3D8_DEV_CURSOR);
    if (cursor >= d3d8_guest_load32(device + D3D8_DEV_LIMIT)) {
        if (device != D3D8_DEVICE_BASE)
            d3d8_hle_fatal(address, "state packet refill for a nondefault device is not modelled");
        cursor = d3d8_pushbuffer_begin();
    }
    return cursor;
}

static uint32_t handler_93(void *context)
{
    /* 0x003D7073 loads the argument only after the single entry reservation. */
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    uint32_t cursor = state_packet_begin_at(device, 0x003D7060u);
    const uint32_t begin = cursor;
    const uint32_t value = argument(context, 0u, 0x003D7060u);
    d3d8_guest_store32(cursor, 0x00040308u);
    if (value == 0u) {
        d3d8_guest_store32(cursor + 4u, value);
        cursor += 8u;
    } else {
        d3d8_guest_store32(cursor + 4u, 1u);
        /* The original reads state 0x92 at 0x003D709B before the third store. */
        const uint32_t face = d3d8_guest_load32(D3D8_STATE_92);
        d3d8_guest_store32(cursor + 8u, 0x0004039Cu);
        cursor += 16u;
        d3d8_guest_store32(cursor - 4u, 0x404u + (value != face ? 1u : 0u));
    }
    d3d8_pushbuffer_end_at(device, begin, cursor);
    d3d8_guest_store32(D3D8_STATE_93, value);
    return cursor;
}
static uint32_t handler_88(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D6C60u);
    d3d8_state_set_88(value);
    /* The retail leaf loads its argument into EAX and leaves it there through RET 4. */
    return value;
}

static uint32_t handler_8a(void *context)
{
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    uint32_t cursor = d3d8_guest_load32(device + D3D8_DEV_CURSOR);
    if (cursor >= d3d8_guest_load32(device + D3D8_DEV_LIMIT)) {
        if (device != D3D8_DEVICE_BASE) {
            d3d8_hle_fatal(0x003D7010u, "fog-color refill for a nondefault device is not modelled");
        }
        cursor = d3d8_pushbuffer_begin();
    }
    const uint32_t value = argument(context, 0u, 0x003D7010u);
    /* Original MOVZX reads this caller byte separately after the dword. */
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    const uint32_t blue = d3d8_guest_load8(frame->stack_ptr + 6u);
    const uint32_t packed = blue | ((value & 0xFFu) << 16) | (value & 0xFF00FF00u);
    const uint32_t begin = cursor;
    d3d8_guest_store32(cursor, 0x000402A8u);
    d3d8_guest_store32(cursor + 4u, packed);
    cursor += 8u;
    d3d8_pushbuffer_end_at(device, begin, cursor);
    d3d8_guest_store32(RS_SHADOW_OF(0x8Au), value);
    return cursor;
}

static uint32_t handler_97(void *context)
{
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    uint32_t cursor = d3d8_guest_load32(device + D3D8_DEV_CURSOR);
    if (cursor >= d3d8_guest_load32(device + D3D8_DEV_LIMIT)) {
        if (device != D3D8_DEVICE_BASE) {
            d3d8_hle_fatal(0x003D6F90u, "shade-state refill for a nondefault device is not modelled");
        }
        cursor = d3d8_pushbuffer_begin();
    }
    /* The original reads the caller slot after the refill dependency. */
    const uint32_t value = argument(context, 0u, 0x003D6F90u);
    const uint32_t begin = cursor;
    d3d8_guest_store32(cursor, 0x00080320u);
    d3d8_guest_store32(cursor + 4u, value);
    d3d8_guest_store32(cursor + 8u, value);
    cursor += 12u;
    d3d8_pushbuffer_end_at(device, begin, cursor);
    d3d8_guest_store32(RS_SHADOW_OF(0x97u), value);
    return cursor;
}

/* RET 4 leaves the final packet cursor in EAX for each raster helper. */
static uint32_t handler_8b(void *context)
{
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    const uint32_t cursor = state_packet_begin_at(device, 0x003D7380u);
    const uint32_t two_sided = d3d8_guest_load32(D3D8_STATE_8D);
    const uint32_t back = d3d8_guest_load32(D3D8_STATE_8C);
    const uint32_t value = argument(context, 0u, 0x003D7380u);
    const uint32_t second = two_sided != 0u ? back : value;
    d3d8_guest_store32(cursor, PUSH_FILL_MODE);
    d3d8_guest_store32(cursor + 4u, value);
    d3d8_guest_store32(cursor + 8u, second);
    d3d8_pushbuffer_end_at(device, cursor, cursor + 12u);
    store_state(D3D8_STATE_8B, value);
    return cursor + 12u;
}

static uint32_t handler_8c(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D73D0u);
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    store_state(D3D8_STATE_8C, value);
    uint32_t cursor = d3d8_guest_load32(device + D3D8_DEV_CURSOR);
    const uint32_t limit = d3d8_guest_load32(device + D3D8_DEV_LIMIT);
    const uint32_t front = d3d8_guest_load32(D3D8_STATE_8B);
    if (cursor >= limit) {
        if (device != D3D8_DEVICE_BASE)
            d3d8_hle_fatal(0x003D73D0u, "back fill refill for a nondefault device is not modelled");
        cursor = d3d8_pushbuffer_begin();
    }
    const uint32_t two_sided = d3d8_guest_load32(D3D8_STATE_8D);
    const uint32_t back = d3d8_guest_load32(D3D8_STATE_8C);
    const uint32_t second = two_sided != 0u ? back : front;
    d3d8_guest_store32(cursor, PUSH_FILL_MODE);
    d3d8_guest_store32(cursor + 4u, front);
    d3d8_guest_store32(cursor + 8u, second);
    d3d8_pushbuffer_end_at(device, cursor, cursor + 12u);
    store_state(D3D8_STATE_8B, front);
    return cursor + 12u;
}

static uint32_t handler_8d(void *context)
{
    uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    uint32_t cursor = state_packet_begin_at(device, 0x003D7430u);
    const uint32_t value = argument(context, 0u, 0x003D7430u);
    d3d8_guest_store32(cursor, 0x000417C4u);
    d3d8_guest_store32(cursor + 4u, value);
    d3d8_pushbuffer_end_at(device, cursor, cursor + 8u);
    if (value != 0u) {
        dirty_or(0x9000u);
    }
    device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    const uint32_t front = d3d8_guest_load32(D3D8_STATE_8B);
    d3d8_guest_store32(D3D8_STATE_8D, value);
    cursor = state_packet_begin_at(device, 0x003D7430u);
    const uint32_t two_sided = d3d8_guest_load32(D3D8_STATE_8D);
    const uint32_t back = d3d8_guest_load32(D3D8_STATE_8C);
    d3d8_guest_store32(cursor, PUSH_FILL_MODE);
    d3d8_guest_store32(cursor + 4u, front);
    d3d8_guest_store32(cursor + 8u, two_sided != 0u ? back : front);
    d3d8_pushbuffer_end_at(device, cursor, cursor + 12u);
    d3d8_guest_store32(D3D8_STATE_8B, front);
    return cursor + 12u;
}

static uint32_t handler_90(void *context)
{
    /* The original captures EDI and publishes its shadow before the reservation. */
    const uint32_t value = argument(context, 0u, 0x003D7F70u);
    d3d8_guest_store32(D3D8_STATE_90, value);
    const uint32_t cursor = d3d8_pushbuffer_begin();
    const uint32_t cull = cull_word();
    d3d8_guest_store32(cursor, PUSH_CULL);
    d3d8_guest_store32(cursor + 4u, cull);
    /* 0x003D7FCF tests the depth pointer before either stencil packet store. */
    const uint32_t enabled =
        value != 0u && d3d8_device_load32(DEV_DEPTH_SURFACE) != 0u ? 1u : 0u;
    d3d8_guest_store32(cursor + 8u, 0x0004032Cu);
    d3d8_guest_store32(cursor + 12u, enabled);
    d3d8_pushbuffer_end(cursor + 16u);
    return cursor + 16u;
}

static uint32_t handler_91(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D8010u);
    d3d8_guest_store32(D3D8_STATE_91, value);
    const uint32_t cursor = d3d8_pushbuffer_begin();
    /* All cull state reads precede the original first packet store at 0x003D8062. */
    const uint32_t cull = cull_word();
    d3d8_guest_store32(cursor, PUSH_CULL);
    d3d8_guest_store32(cursor + 4u, cull);
    d3d8_guest_store32(cursor + 8u, 0x00040370u);
    d3d8_guest_store32(cursor + 12u, value);
    d3d8_pushbuffer_end(cursor + 16u);
    return cursor + 16u;
}
static uint32_t handler_94(void *context)
{
    /* The original selects its shader branch before any entry reservation. */
    if (d3d8_device_load32(0x784u) != 0u) {
        const uint32_t value = argument(context, 0u, 0x003D7150u);
        d3d8_guest_store32(D3D8_STATE_94, value);
        return value;
    }
    uint32_t cursor = d3d8_pushbuffer_begin();
    /* 0x003D716D reloads the stack argument after the refill callback. */
    const uint32_t value = argument(context, 0u, 0x003D7150u);
    d3d8_guest_store32(cursor, 0x00400A60u);
    for (uint32_t word = 0u; word < 16u; ++word) {
        d3d8_guest_store32(cursor + 4u + word * 4u, value);
    }
    cursor += 68u;
    d3d8_pushbuffer_end(cursor);
    d3d8_guest_store32(D3D8_STATE_94, value);
    return cursor;
}
static uint32_t handler_a1(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D80B0u);
    d3d8_guest_store32(D3D8_STATE_A1, value);
    const uint32_t cursor = d3d8_pushbuffer_begin();
    /* Original cull reads precede the first packet store at 0x003D8100. */
    const uint32_t cull = cull_word();
    d3d8_guest_store32(cursor, PUSH_CULL);
    d3d8_guest_store32(cursor + 4u, cull);
    d3d8_pushbuffer_end(cursor + 8u);
    return cursor + 8u;
}

static uint32_t handler_a2(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D8120u);
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    d3d8_guest_store32(D3D8_STATE_A2, value);
    const uint32_t cursor = state_packet_begin_at(device, 0x003D8120u);
    /* Original cull reads precede the first packet store at 0x003D8170. */
    const uint32_t cull = cull_word();
    d3d8_guest_store32(cursor, PUSH_CULL);
    d3d8_guest_store32(cursor + 4u, cull);
    d3d8_pushbuffer_end_at(device, cursor, cursor + 8u);
    return cursor + 8u;
}
static uint32_t handler_emit_control_words(void)
{
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    uint32_t word_a = d3d8_guest_load32(device + DEV_CONTROL_WORD_A) & 0xFFFFFFF7u;
    /* 0x003D7AC0 publishes the masked word before reading its source. */
    d3d8_guest_store32(device + DEV_CONTROL_WORD_A, word_a);
    if (d3d8_guest_load32(GLOBAL_CONTROL_SOURCE_A) != 0u) {
        word_a |= 0x8u;
        d3d8_guest_store32(device + DEV_CONTROL_WORD_A, word_a);
    }
    uint32_t word_b = d3d8_guest_load32(device + DEV_CONTROL_WORD_B) & 0xE7EFFFFFu;
    d3d8_guest_store32(device + DEV_CONTROL_WORD_B, word_b);
    if (d3d8_guest_load32(D3D8_STATE_A3) != 0u) {
        word_b |= 0x00100000u;
        d3d8_guest_store32(device + DEV_CONTROL_WORD_B, word_b);
    }
    if (d3d8_guest_load32(D3D8_STATE_A4) != 0u) {
        word_b = d3d8_guest_load32(device + DEV_CONTROL_WORD_B) | 0x08000000u;
        d3d8_guest_store32(device + DEV_CONTROL_WORD_B, word_b);
    }
    const uint32_t cursor = state_packet_begin_at(device, 0x003D7AB0u);
    d3d8_guest_store32(cursor, PUSH_CONTROL_RESET);
    d3d8_guest_store32(cursor + 4u, 0u);
    /* Original 0x003D7B30 reads word A before the selectors can overwrite it. */
    const uint32_t payload_a = d3d8_guest_load32(device + DEV_CONTROL_WORD_A);
    d3d8_guest_store32(cursor + 8u, PUSH_CONTROL_SELECT);
    d3d8_guest_store32(cursor + 12u, 0x00400094u);
    d3d8_guest_store32(cursor + 16u, payload_a);
    d3d8_guest_store32(cursor + 20u, PUSH_CONTROL_STROBE);
    d3d8_guest_store32(cursor + 24u, 9u);
    /* Original 0x003D7B56 reads word B after seven writes, before its selector. */
    const uint32_t payload_b = d3d8_guest_load32(device + DEV_CONTROL_WORD_B);
    d3d8_guest_store32(cursor + 28u, PUSH_CONTROL_SELECT);
    d3d8_guest_store32(cursor + 32u, 0x00400B80u);
    d3d8_guest_store32(cursor + 36u, payload_b);
    d3d8_guest_store32(cursor + 40u, PUSH_CONTROL_STROBE);
    d3d8_guest_store32(cursor + 44u, 9u);
    d3d8_pushbuffer_end_at(device, cursor, cursor + 48u);
    return cursor + 48u;
}

static uint32_t handler_a3(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D8190u);
    d3d8_guest_store32(D3D8_STATE_A3, value);
    return handler_emit_control_words();
}

static uint32_t handler_a4(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D81B0u);
    d3d8_guest_store32(D3D8_STATE_A4, value);
    return handler_emit_control_words();
}
/* 0x003D82D0 and 0x003D8320 preserve the fully assembled mask across
 * MakeSpace. The supported alternate-device path needs no shared refill. */
static uint32_t handler_multisample_emit(uint32_t device, uint32_t word, uint32_t entry)
{
    uint32_t cursor = d3d8_guest_load32(device + D3D8_DEV_CURSOR);
    if (cursor >= d3d8_guest_load32(device + D3D8_DEV_LIMIT)) {
        if (device != D3D8_DEVICE_BASE) {
            d3d8_hle_fatal(entry, "multisample-mask refill for a nondefault device is not modelled");
        }
        cursor = d3d8_pushbuffer_begin();
    }
    const uint32_t begin = cursor;
    d3d8_guest_store32(cursor, PUSH_MULTISAMPLE_MASK);
    d3d8_guest_store32(cursor + 4u, word);
    d3d8_pushbuffer_end_at(device, begin, cursor + 8u);
    return cursor + 8u;
}

static uint32_t handler_99(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D82D0u);
    const uint32_t other = d3d8_guest_load32(D3D8_STATE_9E);
    d3d8_guest_store32(D3D8_STATE_99, value);
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    uint32_t word = (value << 16u) | other;
    if ((d3d8_guest_load32(device + D3D8_DEV_FLAGS) & 0x8000u) != 0u &&
        d3d8_guest_load32(D3D8_STATE_98) != 0u) {
        word |= 1u;
    }
    return handler_multisample_emit(device, word, 0x003D82D0u);
}

static uint32_t handler_9e(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D8320u);
    const uint32_t other = d3d8_guest_load32(D3D8_STATE_99);
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    uint32_t word = (other << 16u) | value;
    d3d8_guest_store32(D3D8_STATE_9E, value);
    if ((d3d8_guest_load32(device + D3D8_DEV_FLAGS) & 0x8000u) != 0u &&
        d3d8_guest_load32(D3D8_STATE_98) != 0u) {
        word |= 1u;
    }
    return handler_multisample_emit(device, word, 0x003D8320u);
}

/* 0x003D8080 stores its captured argument before reservation; 0x003D7A50
 * subsequently reads the live control fields and returns the post-packet cursor. */
static uint32_t handler_a0(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D8080u);
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    d3d8_guest_store32(D3D8_STATE_A0, value);
    const uint32_t begin = state_packet_begin_at(device, 0x003D8080u);
    const uint32_t cursor = emit_surface_control_at(device, begin);
    d3d8_pushbuffer_end_at(device, begin, cursor);
    return cursor;
}

/* The direct SDK body reads its stack argument only after the first reservation,
 * publishes the first packet/cursor before its shadow and returns the old shadow
 * unless entering/leaving mode 2 invokes the original modeled nested helpers. */
static uint32_t handler_8f(void *context)
{
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    uint32_t cursor = d3d8_pushbuffer_begin();
    const uint32_t value = argument(context, 0u, 0x003D7EE0u);
    const uint32_t enabled = value != 0u &&
        d3d8_guest_load32(device + DEV_DEPTH_SURFACE) != 0u ? 1u : 0u;
    d3d8_guest_store32(cursor, 0x0004030Cu);
    d3d8_guest_store32(cursor + 4u, enabled);
    const uint32_t mask = d3d8_guest_load32(GLOBAL_STATE_4A);
    d3d8_guest_store32(cursor + 8u, 0x00041D78u);
    d3d8_guest_store32(cursor + 12u, mask);
    d3d8_pushbuffer_end_at(device, cursor, cursor + 16u);
    const uint32_t previous = d3d8_guest_load32(D3D8_STATE_8F);
    d3d8_guest_store32(D3D8_STATE_8F, value);
    if (previous != 2u && value != 2u) {
        return previous;
    }
    (void)d3d8_rebuild_viewport_matrix();
    d3d8_run_vertex_program_helper();
    cursor = d3d8_pushbuffer_begin();
    const uint32_t second_begin = cursor;
    cursor = d3d8_state_emit_surface_control(cursor);
    const uint32_t declaration = d3d8_guest_load32(device + DEV_DECLARATION);
    const uint32_t declaration_flags = d3d8_guest_load32(declaration + 4u);
    cursor = d3d8_viewport_emit(cursor, declaration_flags);
    d3d8_pushbuffer_end_at(device, second_begin, cursor);
    return cursor;
}
static uint32_t direct_scaled_mode(void *context, uint32_t entry,
                                   uint32_t state_address, bool first_back)
{
    const uint32_t value = argument(context, 0u, entry);
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    /* 0x003D81FA/0x003D822A publish the shadow before target reads or
     * any refusal in the scaled-viewport helper. Legacy setters stay guarded. */
    d3d8_guest_store32(state_address, value);
    const uint32_t target = d3d8_guest_load32(device + DEV_RENDER_TARGET);
    const uint32_t back = d3d8_guest_load32(device + DEV_BACK_BUFFER_0);
    if ((target == back) != first_back) {
        return value;
    }
    /* Original SetViewport(NULL) leaves the final packet cursor in EAX. */
    return d3d8_scaled_viewport_run_direct(value);
}

static uint32_t handler_9a(void *context)
{
    return direct_scaled_mode(context, 0x003D81F0u, D3D8_STATE_9A, true);
}

static uint32_t handler_9b(void *context)
{
    return direct_scaled_mode(context, 0x003D8220u, D3D8_STATE_9B, false);
}
static void handler_95_store_argument(void *context, uint32_t value)
{
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    const uint64_t slot = (uint64_t)frame->stack_ptr + sizeof(uint32_t);
    if (slot > UINT32_MAX || !kernel_guest_write_u32((kernel_guest_ptr)slot, value)) {
        d3d8_hle_fatal(0x003D72A0u, "depth-bias caller argument slot cannot be written");
    }
}

static uint32_t handler_95(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D72A0u);
    const uint32_t flag = value != 0u ? 1u : 0u;
    /* Original 0x003D72B0 publishes the integer before FILD reads that slot. */
    handler_95_store_argument(context, value);
    long double widened = (long double)(int32_t)value;
    if ((int32_t)value < 0) {
        widened += (long double)float_of_bits(d3d8_guest_load32(CONSTANT_TWO_POW_32));
    }
    const long double negated = -widened;
    handler_95_store_argument(context, bits_of_float((float)negated));
    const uint32_t scaled = bits_of_float(
        (float)(negated * (long double)float_of_bits(d3d8_guest_load32(CONSTANT_QUARTER))));
    d3d8_set_render_state_notinline((int32_t)RS_DEPTH_BIAS_FIRST, scaled);
    /* The first generic call may refill before 0x003D72DE rereads the caller slot. */
    const uint32_t second = argument(context, 0u, 0x003D72A0u);
    handler_95_store_argument(context, second);
    d3d8_set_render_state_notinline((int32_t)(RS_DEPTH_BIAS_FIRST + 1u), second);
    for (uint32_t index = 2u; index < 5u; ++index) {
        d3d8_set_render_state_notinline((int32_t)(RS_DEPTH_BIAS_FIRST + index), flag);
    }
    const uint32_t cursor = d3d8_device_load32(D3D8_DEV_CURSOR);
    d3d8_guest_store32(D3D8_STATE_95, value);
    return cursor;
}
static uint32_t handler_constant_mode(void *context)
{
    return constant_mode(argument(context, 0u, 0x003D5AF0u));
}

static uint32_t handler_9c(void *context)
{
    /* Save ESI's original device before the method stores can alias the pointer slot. */
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    uint32_t cursor = d3d8_guest_load32(device + D3D8_DEV_CURSOR);
    if (cursor >= d3d8_guest_load32(device + D3D8_DEV_LIMIT)) {
        if (device != D3D8_DEVICE_BASE)
            d3d8_hle_fatal(0x003D6FD0u, "state9C refill for a nondefault device is not modelled");
        cursor = d3d8_pushbuffer_begin();
    }
    const uint32_t value = argument(context, 0u, 0x003D6FD0u);
    const uint32_t begin = cursor;
    d3d8_guest_store32(cursor, 0x00041E6Cu);
    d3d8_guest_store32(cursor + 4u, value - 0x200u);
    cursor += 8u;
    d3d8_pushbuffer_end_at(device, begin, cursor);
    d3d8_guest_store32(RS_SHADOW_OF(0x9Cu), value);
    return cursor;
}

static uint32_t handler_92(void *context)
{
    /* The entry refills before it reads [esp+4]. The ported helper is used by the
     * render-state dispatcher, but a direct XDK call must also retain its live stack slot. */
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    const uint32_t cursor = state_packet_begin_at(device, 0x003D70D0u);
    const uint32_t value = argument(context, 0u, 0x003D70D0u);
    d3d8_guest_store32(cursor, 0x000403A0u);
    d3d8_guest_store32(cursor + 4u, value);
    d3d8_pushbuffer_end_at(device, cursor, cursor + 8u);

    /* The retail tail call overwrites its argument slot with current state 0x93,
     * then the 0x7060 entry may refill before reading that slot. */
    const uint32_t current_93 = d3d8_guest_load32(D3D8_STATE_93);
    d3d8_guest_store32(D3D8_STATE_92, value);
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    const uint64_t tail_slot = (uint64_t)frame->stack_ptr + sizeof(uint32_t);
    if (tail_slot > UINT32_MAX ||
        !kernel_guest_write_u32((kernel_guest_ptr)tail_slot, current_93)) {
        d3d8_hle_fatal(0x003D70D0u, "tail-call state 0x93 argument slot cannot be written");
    }
    return handler_93(context);
}

static uint32_t handler_8e(void *context)
{
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    const uint32_t cursor = state_packet_begin_at(device, 0x003D7110u);
    const uint32_t value = argument(context, 0u, 0x003D7110u);
    d3d8_guest_store32(cursor, 0x000403A4u);
    d3d8_guest_store32(cursor + 4u, value);
    d3d8_pushbuffer_end_at(device, cursor, cursor + 8u);
    dirty_or(0x200u);
    d3d8_guest_store32(RS_SHADOW_OF(0x8Eu), value);
    return cursor + 8u;
}

static uint32_t handler_96(void *context)
{
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    uint32_t cursor = d3d8_guest_load32(device + D3D8_DEV_CURSOR);
    if (cursor >= d3d8_guest_load32(device + D3D8_DEV_LIMIT)) {
        if (device != D3D8_DEVICE_BASE) {
            d3d8_hle_fatal(0x003D7320u, "logic-operation refill for a nondefault device is not modelled");
        }
        cursor = d3d8_pushbuffer_begin();
    }
    /* Original 0x003D7333 reads the caller argument after MakeSpace returns. */
    const uint32_t value = argument(context, 0u, 0x003D7320u);
    const uint32_t begin = cursor;
    if (value == 0u) {
        d3d8_guest_store32(cursor, 0x000417BCu);
        d3d8_guest_store32(cursor + 4u, value);
        cursor += 8u;
    } else {
        d3d8_guest_store32(cursor, PUSH_LOGIC_OP_ON);
        d3d8_guest_store32(cursor + 4u, 1u);
        d3d8_guest_store32(cursor + 8u, value);
        cursor += 12u;
    }
    d3d8_pushbuffer_end_at(device, begin, cursor);
    d3d8_guest_store32(D3D8_STATE_96, value);
    return cursor;
}

static uint32_t handler_89(void *context)
{
    /* Original captures the dirty value before the device, then publishes it. */
    const uint32_t dirty = d3d8_guest_load32(0x003E3AB8u) | 0x200u;
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    d3d8_guest_store32(0x003E3AB8u, dirty);
    uint32_t cursor = d3d8_guest_load32(device + D3D8_DEV_CURSOR);
    if (cursor >= d3d8_guest_load32(device + D3D8_DEV_LIMIT)) {
        if (device != D3D8_DEVICE_BASE) {
            d3d8_hle_fatal(0x003D74B0u, "skin-mode refill for a nondefault device is not modelled");
        }
        cursor = d3d8_pushbuffer_begin();
    }
    /* 0x003D74D5 reads the caller argument after the refill callback. */
    const uint32_t value = argument(context, 0u, 0x003D74B0u);
    const uint32_t begin = cursor;
    d3d8_guest_store32(cursor, 0x00040328u);
    d3d8_guest_store32(cursor + 4u, value);
    cursor += 8u;
    d3d8_pushbuffer_end_at(device, begin, cursor);
    d3d8_guest_store32(RS_SHADOW_OF(0x89u), value);
    return cursor;
}

static uint32_t handler_9d(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D71B0u);
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    /* 0x003D71BC republishes EBX into the caller slot before FLD rereads it. */
    const kernel_call_frame *frame = (const kernel_call_frame *)context;
    const uint64_t caller_slot = (uint64_t)frame->stack_ptr + sizeof(uint32_t);
    uint32_t operand;
    if (caller_slot > UINT32_MAX ||
        !kernel_guest_write_u32((kernel_guest_ptr)caller_slot, value) ||
        !kernel_guest_read_u32((kernel_guest_ptr)caller_slot, &operand)) {
        d3d8_hle_fatal(0x003D71B0u, "point-size caller argument slot cannot be written and reread");
    }
    const float scale = float_of_bits(d3d8_guest_load32(device + 0x964u));
    /* The original stores once to float before CVTTSS2SI, before entry refill. */
    long double point_scaled = (long double)float_of_bits(operand) * (long double)scale;
    point_scaled *= (long double)float_of_bits(d3d8_guest_load32(CONSTANT_EIGHT));
    point_scaled += (long double)float_of_bits(d3d8_guest_load32(CONSTANT_HALF));
    uint32_t point_level = truncate_to_integer((float)point_scaled);
    if (point_level > 0x1FFu) {
        point_level = 0x1FFu;
    }
    uint32_t cursor = d3d8_guest_load32(device + D3D8_DEV_CURSOR);
    if (cursor >= d3d8_guest_load32(device + D3D8_DEV_LIMIT)) {
        if (device != D3D8_DEVICE_BASE) {
            d3d8_hle_fatal(0x003D71B0u, "point-size refill for a nondefault device is not modelled");
        }
        cursor = d3d8_pushbuffer_begin();
    }
    const uint32_t begin = cursor;
    d3d8_guest_store32(cursor, 0x00040380u);
    d3d8_guest_store32(cursor + 4u, point_level);
    cursor += 8u;
    d3d8_pushbuffer_end_at(device, begin, cursor);
    d3d8_guest_store32(RS_SHADOW_OF(0x9Du), value);
    return cursor;
}

static uint32_t handler_a5(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D81D0u);
    /* Original stores A5 before entering the same ordered direct core as A3/A4. */
    d3d8_guest_store32(GLOBAL_CONTROL_SOURCE_A, value);
    return handler_emit_control_words();
}

static uint32_t handler_98(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D8250u);
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    d3d8_guest_store32(D3D8_STATE_98, value);
    if (device != D3D8_DEVICE_BASE)
        d3d8_hle_fatal(0x003D8250u, "multisample nested helpers require the recovered device");
    d3d8_run_vertex_program_helper();
    const uint32_t high = d3d8_guest_load32(D3D8_STATE_99);
    const uint32_t low = d3d8_guest_load32(D3D8_STATE_9E);
    const uint32_t flags = d3d8_guest_load32(device + D3D8_DEV_FLAGS);
    uint32_t mask = (high << 16) | low;
    if ((flags & 0x8000u) != 0u && d3d8_guest_load32(D3D8_STATE_98) != 0u)
        mask |= 1u;
    uint32_t cursor = state_packet_begin_at(device, 0x003D8250u);
    d3d8_guest_store32(cursor, PUSH_MULTISAMPLE_MASK);
    d3d8_guest_store32(cursor + 4u, mask);
    d3d8_pushbuffer_end_at(device, cursor, cursor + 8u);
    cursor = state_packet_begin_at(device, 0x003D8250u);
    const uint32_t declaration = d3d8_guest_load32(device + DEV_DECLARATION);
    const uint32_t declaration_flags = d3d8_guest_load32(declaration + 4u);
    const uint32_t advanced = d3d8_viewport_emit(cursor, declaration_flags);
    d3d8_pushbuffer_end_at(device, cursor, advanced);
    return advanced;
}

/* Direct SDK 0x003D7220 preserves captured device and raw branch returns. */
static uint32_t handler_9f(void *context)
{
    const uint32_t value = argument(context, 0u, 0x003D7220u);
    const uint32_t device = d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT);
    const uint32_t target = d3d8_guest_load32(device + DEV_RENDER_TARGET);
    const uint32_t format = d3d8_guest_load8(target + 0xDu);
    const uint32_t classification = d3d8_guest_load8(GLOBAL_FORMAT_INFO + format);
    const uint32_t requested = (classification & 0x3Cu) == 0x20u ? value : 0u;
    const uint32_t flags = d3d8_guest_load32(device + D3D8_DEV_FLAGS);
    uint32_t result = flags;
    if (requested != (flags & 1u)) {
        const uint32_t limit = d3d8_guest_load32(device + D3D8_DEV_LIMIT);
        d3d8_guest_store32(device + D3D8_DEV_FLAGS, flags ^ 1u);
        uint32_t cursor = d3d8_guest_load32(device + D3D8_DEV_CURSOR);
        if (cursor >= limit) {
            if (device != D3D8_DEVICE_BASE) {
                d3d8_hle_fatal(0x003D7220u, "supersample refill for a nondefault device is not modelled");
            }
            cursor = d3d8_pushbuffer_begin();
        }
        const uint32_t begin = cursor;
        d3d8_guest_store32(cursor, PUSH_CONTROL_RESET);
        d3d8_guest_store32(cursor + 4u, 0u);
        d3d8_guest_store32(cursor + 8u, PUSH_CONTROL_STROBE);
        d3d8_guest_store32(cursor + 12u, (requested << 5) | 8u);
        result = cursor + 16u;
        d3d8_pushbuffer_end_at(device, begin, result);
    }
    d3d8_guest_store32(RS_SHADOW_OF(0x9Fu), value);
    return result;
}

static uint32_t handler_constant(void *context)
{
    uint32_t index = 0u;
    uint32_t source = 0u;
    if (!kernel_frame_reg_arg((const kernel_call_frame *)context, 0u, &index) ||
        !kernel_frame_reg_arg((const kernel_call_frame *)context, 1u, &source)) {
        d3d8_hle_fatal(0x003D5670u, "register arguments were not supplied");
    }
    d3d8_state_set_vertex_shader_constant(index, source);
    return 0u;
}

size_t d3d8_state_register(void)
{
    static const struct {
        uint32_t address;
        d3d8_fn handler;
    } handlers[] = {
        {0x003D6C60u, handler_88},
        {0x003D6F90u, handler_97},
        {0x003D7380u, handler_8b},
        {0x003D73D0u, handler_8c},
        {0x003D7430u, handler_8d},
        {0x003D7060u, handler_93},        {0x003D7010u, handler_8a},
        {0x003D7F70u, handler_90},
        {0x003D8010u, handler_91},        {0x003D7150u, handler_94},
        {0x003D8080u, handler_a0},
        {0x003D80B0u, handler_a1},        {0x003D8120u, handler_a2},        {0x003D8190u, handler_a3},
        {0x003D81B0u, handler_a4},        {0x003D7EE0u, handler_8f},
        {0x003D81F0u, handler_9a}, {0x003D8220u, handler_9b},        {0x003D72A0u, handler_95},
        {0x003D6FD0u, handler_9c},
        {0x003D70D0u, handler_92},
        {0x003D7110u, handler_8e},
        {0x003D7320u, handler_96},
        {0x003D74B0u, handler_89},
        {0x003D71B0u, handler_9d},
        {0x003D81D0u, handler_a5},
        {0x003D8250u, handler_98},
        {0x003D7220u, handler_9f},
        {0x003D82D0u, handler_99},
        {0x003D8320u, handler_9e},
        {0x003D5AF0u, handler_constant_mode}, {0x003D5670u, handler_constant},
    };
    size_t registered = 0u;
    for (size_t index = 0u; index < sizeof(handlers) / sizeof(handlers[0]); index++) {
        if (d3d8_hle_register(handlers[index].address, handlers[index].handler)) {
            registered++;
        }
    }
    return registered;
}
