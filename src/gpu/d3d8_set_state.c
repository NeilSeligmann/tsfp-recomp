/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_set_state.h. Every function carries the address of the original it ports.
 */

#include "d3d8_set_state.h"

#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "d3d8_state.h"

#define DEV_TEXCOORD_WRAP_MASK 0x950u

static uint32_t guest_dirty_load(void)
{
    return d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK);
}

static void dirty_or(uint32_t bits)
{
    d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK, guest_dirty_load() | bits);
}

/* The helper table maps a state to the original address the guest table holds. The port only calls
 * its own code, so the guest entry is checked against the address the port knows, and a state
 * with no ported helper stops. */
static void call_helper(int32_t state, uint32_t value)
{
    static const struct {
        uint32_t state;
        uint32_t address;
        void (*helper)(uint32_t);
    } helpers[] = {
        {0x88u, 0x003D6C60u, d3d8_state_set_88},
        {0x89u, 0x003D74B0u, d3d8_state_library_set_89},
        {0x8Au, 0x003D7010u, d3d8_state_library_set_8a},
        {0x8Bu, 0x003D7380u, d3d8_state_set_8b},
        {0x8Cu, 0x003D73D0u, d3d8_state_set_8c},
        {0x8Du, 0x003D7430u, d3d8_state_library_set_8d},
        {0x8Eu, 0x003D7110u, d3d8_state_library_set_8e},
        {0x8Fu, 0x003D7EE0u, d3d8_state_library_set_8f},
        {0x90u, 0x003D7F70u, d3d8_state_library_set_90},
        {0x91u, 0x003D8010u, d3d8_state_library_set_91},
        {0x92u, 0x003D70D0u, d3d8_state_library_set_92},
        {0x93u, 0x003D7060u, d3d8_state_library_set_93},
        {0x94u, 0x003D7150u, d3d8_state_library_set_94},
        {0x95u, 0x003D72A0u, d3d8_state_library_set_95},
        {0x96u, 0x003D7320u, d3d8_state_set_96},
        {0x97u, 0x003D6F90u, d3d8_state_set_97},
        {0x98u, 0x003D8250u, d3d8_state_library_set_98},
        {0x99u, 0x003D82D0u, d3d8_state_set_99},
        {0x9Au, 0x003D81F0u, d3d8_state_library_set_9a},
        {0x9Bu, 0x003D8220u, d3d8_state_set_9b},
        {0x9Cu, 0x003D6FD0u, d3d8_state_library_set_9c},
        {0x9Du, 0x003D71B0u, d3d8_state_library_set_9d},
        {0x9Eu, 0x003D8320u, d3d8_state_library_set_9e},
        {0x9Fu, 0x003D7220u, d3d8_state_library_set_9f},
        {0xA0u, 0x003D8080u, d3d8_state_set_a0},
        {0xA1u, 0x003D80B0u, d3d8_state_library_set_a1},
        {0xA2u, 0x003D8120u, d3d8_state_library_set_a2},
        {0xA3u, 0x003D8190u, d3d8_state_library_set_a3},
        {0xA4u, 0x003D81B0u, d3d8_state_library_set_a4},
        {0xA5u, 0x003D81D0u, d3d8_state_library_set_a5},
    };
    const uint32_t entry = d3d8_guest_load32(D3D8_RS_HELPER_TABLE + (uint32_t)state * 4u);
    for (size_t index = 0u; index < sizeof(helpers) / sizeof(helpers[0]); index++) {
        if (helpers[index].state == (uint32_t)state) {
            if (entry != helpers[index].address) {
                d3d8_hle_fatal(0x003D6CC0u,
                               "render state %#x dispatches to %#x, the port ported %#x",
                               (unsigned)state, (unsigned)entry, (unsigned)helpers[index].address);
            }
            helpers[index].helper(value);
            return;
        }
    }
    d3d8_hle_fatal(0x003D6CC0u,
                   "render state %#x calls the helper at %#x, which has no emitting port",
                   (unsigned)state, (unsigned)entry);
}

void d3d8_set_render_state_notinline(int32_t state, uint32_t value)
{
    if (state < 0) {
        d3d8_hle_fatal(0x003D6CC0u,
                       "negative render state %d: the signed compare would index off the tables",
                       (int)state);
    }
    const uint32_t shadow = D3D8_GLOBAL_RS_SHADOW + (uint32_t)state * 4u;
    if (state < 0x5C) {
        d3d8_pushbuffer_emit_pair(d3d8_guest_load32(D3D8_RS_HEADER_TABLE + (uint32_t)state * 4u),
                                  value);
        d3d8_guest_store32(shadow, value);
        return;
    }
    if (state < 0x88) {
        dirty_or(d3d8_guest_load32(D3D8_RS_DIRTY_TABLE + (uint32_t)state * 4u));
        d3d8_guest_store32(shadow, value);
        return;
    }
    if (state >= 0xA6) {
        d3d8_hle_fatal(0x003D6CC0u, "render state %#x is past the helper table", (unsigned)state);
    }
    call_helper(state, value);
}

void d3d8_set_texcoord_index(uint32_t stage, uint32_t value)
{
    if (stage >= D3D8_TS_STAGE_COUNT) {
        d3d8_hle_fatal(0x003D7500u, "texture stage %u is past the four stage arrays",
                       (unsigned)stage);
    }
    d3d8_guest_store32(D3D8_TS_SHADOW + (stage * D3D8_TS_STAGE_DWORDS + D3D8_TS_TEXCOORD_INDEX) * 4u,
                       value);
    uint32_t cursor = d3d8_pushbuffer_begin();
    uint32_t code = 0u;
    uint32_t wrap = 0u;
    uint32_t kept = value;
    const uint32_t mode = value & 0xFFFF0000u;
    if (mode != 0u) {
        d3d8_guest_store32(cursor, stage * 4u + 0x00041964u);
        d3d8_guest_store32(cursor + 4u, 0xFF000000u);
        cursor += 8u;
        kept = stage;
        if (mode > 0x30000u) {
            code = 0x2401u;
        } else if (mode == 0x30000u) {
            code = 0x8512u;
            wrap = 1u;
        } else if (mode == 0x10000u) {
            code = 0x8511u;
            wrap = 1u;
        } else {
            code = 0x2400u;
        }
    }
    d3d8_guest_store8(D3D8_TEXCOORD_MAP + stage, (uint8_t)(kept + 9u));
    d3d8_guest_store32(cursor, (stage + 0xC03Cu) << 4);
    d3d8_guest_store32(cursor + 4u, code);
    d3d8_guest_store32(cursor + 8u, code);
    d3d8_guest_store32(cursor + 12u, code);
    d3d8_pushbuffer_end(cursor + 16u);
    const uint32_t mask = d3d8_device_load32(DEV_TEXCOORD_WRAP_MASK);
    if (mask == 0u && wrap != 0u) {
        dirty_or(0x200u);
    }
    d3d8_device_store32(DEV_TEXCOORD_WRAP_MASK, (mask & ~(1u << stage)) | (wrap << stage));
    dirty_or(0x47Fu);
}

/* The 0x003D7610 family writes one pair behind the plain `cursor >= limit` preamble, not the
 * `cursor + 8 >= limit` of 0x003D6C90. */
static void emit_pair_at_preamble(uint32_t header, uint32_t value)
{
    const uint32_t cursor = d3d8_pushbuffer_begin();
    d3d8_guest_store32(cursor, header);
    d3d8_guest_store32(cursor + 4u, value);
    d3d8_pushbuffer_end(cursor + 8u);
}

void d3d8_set_texture_stage_state_notinline(uint32_t stage, int32_t state, uint32_t value)
{
    if (stage >= D3D8_TS_STAGE_COUNT) {
        d3d8_hle_fatal(0x003D7700u, "texture stage %u is past the four stage arrays",
                       (unsigned)stage);
    }
    if (state < 0) {
        d3d8_hle_fatal(0x003D7700u, "negative texture stage state %d", (int)state);
    }
    const uint32_t slot =
        D3D8_TS_SHADOW + (stage * D3D8_TS_STAGE_DWORDS + (uint32_t)state) * 4u;
    if (state < (int32_t)D3D8_TS_COLOR_OP) {
        dirty_or(1u << stage);
        d3d8_guest_store32(slot, value);
        return;
    }
    if (state == (int32_t)D3D8_TS_COLOR_OP) {
        /* cmp eax, 0x19 / sbb / and 0xFFFFBFF1 / add 0x480F: 0x800 below 0x19, else 0x480F. */
        dirty_or(value < 0x19u ? 0x800u : 0x480Fu);
        d3d8_guest_store32(slot, value);
        return;
    }
    if (state < 0x16) {
        dirty_or(d3d8_guest_load32(D3D8_TS_DIRTY_TABLE + (uint32_t)state * 4u));
        d3d8_guest_store32(slot, value);
        return;
    }
    if (state == (int32_t)D3D8_TS_TEXCOORD_INDEX) {
        d3d8_set_texcoord_index(stage, value);
        return;
    }
    if (state > 0x1E) {
        return;
    }
    if (state == 0x1D || state == 0x1E) {
        /* 0x003D7680 ColorKeyColor and 0x003D76C0 BorderColor: one header and value, then the shadow. */
        const uint32_t header = state == 0x1D ? 0x00041B24u + (stage << 6) : 0x00040AE0u + stage * 4u;
        emit_pair_at_preamble(header, value);
        d3d8_guest_store32(slot, value);
        return;
    }
    /* 0x003D7610 BumpEnv (states 0x16 to 0x1B). The pair goes out only while the stage number the
     * hardware sees (stage + 1 with no pixel shader bound) is not a multiple of 4. */
    const uint32_t hardware_stage = d3d8_device_load32(0x784u) == 0u ? stage + 1u : stage;
    if ((hardware_stage & 3u) != 0u) {
        const uint32_t header = 0x00041AD0u + ((hardware_stage << 4) + (uint32_t)state) * 4u;
        emit_pair_at_preamble(header, value);
    }
    d3d8_guest_store32(slot, value);
}
