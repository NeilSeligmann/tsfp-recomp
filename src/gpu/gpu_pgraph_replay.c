/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See gpu_pgraph_replay.h for what each piece of a draw is built from and which inferences the
 * replay refuses unless a caller allows them.
 */

#include "gpu_pgraph_replay.h"
#include "gpu_fog.h"

#include "gpu_sha256.h"
#include "gpu_vsh_draw.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FINAL_DWORD 3u
#define FINAL_BIT 1u

static gpu_pgraph_result fail(gpu_pgraph_report *report, gpu_pgraph_result result, const char *format,
                              ...) __attribute__((format(printf, 3, 4)));

static gpu_pgraph_result fail(gpu_pgraph_report *report, gpu_pgraph_result result, const char *format,
                              ...)
{
    if (report != NULL) {
        va_list arguments;
        va_start(arguments, format);
        vsnprintf(report->error, sizeof report->error, format, arguments);
        va_end(arguments);
    }
    return result;
}

/* --- topology ---------------------------------------------------------------------------- */

uint32_t gpu_pgraph_triangulate(uint32_t primitive, const uint32_t *indices, uint32_t count,
                                uint32_t *out, uint32_t capacity)
{
    uint32_t used = 0u;
#define EMIT(a, b, c)                                                                         \
    do {                                                                                      \
        if (used + 3u > capacity) {                                                           \
            return UINT32_MAX;                                                                \
        }                                                                                     \
        out[used++] = (a);                                                                    \
        out[used++] = (b);                                                                    \
        out[used++] = (c);                                                                    \
    } while (0)
    switch (primitive) {
    case GPU_PGRAPH_OP_TRIANGLES:
        for (uint32_t i = 0u; i + 3u <= count; i += 3u) {
            EMIT(indices[i], indices[i + 1u], indices[i + 2u]);
        }
        return used;
    case GPU_PGRAPH_OP_TRIANGLE_STRIP:
        for (uint32_t i = 0u; i + 3u <= count; i++) {
            if ((i & 1u) == 0u) {
                EMIT(indices[i], indices[i + 1u], indices[i + 2u]);
            } else {
                EMIT(indices[i + 1u], indices[i], indices[i + 2u]);
            }
        }
        return used;
    case GPU_PGRAPH_OP_TRIANGLE_FAN:
        for (uint32_t i = 1u; i + 2u <= count; i++) {
            EMIT(indices[0], indices[i], indices[i + 1u]);
        }
        return used;
    case GPU_PGRAPH_OP_QUADS:
        for (uint32_t i = 0u; i + 4u <= count; i += 4u) {
            EMIT(indices[i], indices[i + 1u], indices[i + 2u]);
            EMIT(indices[i], indices[i + 2u], indices[i + 3u]);
        }
        return used;
    default:
        return UINT32_MAX;
    }
#undef EMIT
}

uint32_t gpu_pgraph_lineate(uint32_t primitive, const uint32_t *indices, uint32_t count,
                            uint32_t *out, uint32_t capacity)
{
    uint32_t used = 0u;
    uint32_t step;
    switch (primitive) {
    case GPU_PGRAPH_OP_LINES:
        step = 2u;
        break;
    case GPU_PGRAPH_OP_LINE_STRIP:
        step = 1u;
        break;
    default:
        return UINT32_MAX;
    }
    for (uint32_t i = 0u; i + 2u <= count; i += step) {
        if (used + 2u > capacity) {
            return UINT32_MAX;
        }
        out[used++] = indices[i];
        out[used++] = indices[i + 1u];
    }
    return used;
}

/* --- program ----------------------------------------------------------------------------- */

static void put_le16(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

gpu_pgraph_result gpu_pgraph_resolve_program(const gpu_pgraph_state *state,
                                             const gpu_pgraph_backend *backend,
                                             gpu_pgraph_program *out, gpu_pgraph_report *report)
{
    if (state == NULL || backend == NULL || backend->table == NULL || out == NULL) {
        return fail(report, GPU_PGRAPH_ERR_ARGUMENT, "resolve_program: missing argument");
    }
    if ((backend->allowed_inferences & GPU_PGRAPH_INFER_PROGRAM_HEADER) == 0u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "the program header version (0x%04X) is INFERRED and not allowed",
                    GPU_PGRAPH_PROGRAM_HEADER_VERSION);
    }
    out->mode_inferred = false;
    if (!state->execution_mode_set && state->program_start_set &&
        (backend->allowed_inferences & GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN) != 0u) {
        out->mode_inferred = true;
    } else if (!state->execution_mode_set ||
               (state->execution_mode & 3u) != GPU_PGRAPH_EXECUTION_MODE_PROGRAM) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "transform execution mode is %s: only the program mode (low bits 2) is replayed, "
                    "a fixed-function draw is refused",
                    state->execution_mode_set ? "not the program mode" : "never set");
    }
    if (!state->program_start_set) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "no program start slot (0x1EA0) was written");
    }
    const uint32_t start = state->program_start;
    uint32_t count = 0u;
    bool final_found = false;
    for (uint32_t slot = start; slot < GPU_PGRAPH_PROGRAM_SLOTS; slot++) {
        if (!state->slot_written[slot]) {
            return fail(report, GPU_PGRAPH_ERR_MALFORMED,
                        "program slot %u was never completely written (start slot %u)",
                        (unsigned)slot, (unsigned)start);
        }
        count++;
        if ((state->program[slot * 4u + FINAL_DWORD] & FINAL_BIT) != 0u) {
            final_found = true;
            break;
        }
    }
    if (!final_found) {
        return fail(report, GPU_PGRAPH_ERR_MALFORMED,
                    "no instruction from slot %u on has the FINAL bit", (unsigned)start);
    }
    uint8_t bytes[4u + GPU_PGRAPH_PROGRAM_SLOTS * 16u];
    put_le16(bytes, GPU_PGRAPH_PROGRAM_HEADER_VERSION);
    put_le16(bytes + 2u, count);
    for (uint32_t i = 0u; i < count * 4u; i++) {
        put_le32(bytes + 4u + i * 4u, state->program[start * 4u + i]);
    }
    uint8_t digest[32];
    gpu_sha256(bytes, 4u + count * 16u, digest);
    gpu_sha256_hex(digest, out->digest);
    char name[80];
    snprintf(name, sizeof name, "static_%s", out->digest);
    out->is_static = true;
    if (!gpu_vsh_lookup_name(backend->table, name, &out->module)) {
        snprintf(name, sizeof name, "generated_%s", out->digest);
        out->is_static = false;
        char made_error[160] = "";
        const bool found = gpu_vsh_lookup_name(backend->table, name, &out->module);
        if (!found && backend->make_module != NULL &&
            backend->make_module(backend->make_module_context, false, name, bytes, 4u + count * 16u, made_error, sizeof made_error) &&
            gpu_vsh_lookup_name(backend->table, name, &out->module)) {
            made_error[0] = '\0';
        } else if (!found) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "the bound program (%u instructions from slot %u, sha256 %s) is in neither "
                        "the static nor the generated table%s%s",
                        (unsigned)count, (unsigned)start, out->digest, made_error[0] != '\0' ? ", and translating it failed: " : "",
                        made_error);
        }
    }
    out->instructions = count;
    return GPU_PGRAPH_OK;
}

gpu_pgraph_result gpu_pgraph_resolve_fragment(const gpu_pgraph_state *state,
                                              const gpu_pgraph_backend *backend,
                                              gpu_pgraph_fragment *out, gpu_pgraph_report *report)
{
    if (state == NULL || backend == NULL || out == NULL || !backend->combiner ||
        backend->fragment_table == NULL || backend->load_fragment_module == NULL) {
        return fail(report, GPU_PGRAPH_ERR_ARGUMENT,
                    "resolve_fragment: missing argument, or the combiner stage is not enabled with a "
                    "fragment table and loader");
    }
    memset(out, 0, sizeof *out);
    char error[sizeof report->error];
    const gpu_pgraph_result planned =
        gpu_combiner_plan_build_profile(state,
                                (backend->allowed_inferences & GPU_COMBINER_INFER_ALL) |
                                    (backend->live_texture_modes ? GPU_COMBINER_INFER_TEXTURE_MODES : 0u),
                                backend->test_textures, backend->live_fog_modules, &out->plan, error, sizeof error);
    if (planned != GPU_PGRAPH_OK) {
        return fail(report, planned, "combiner: %s", error);
    }
    /* T860: an enabled alpha test (function 0x200 NEVER to 0x206 NOTEQUAL... GEQUAL, not ALWAYS) runs in the combiner module, as in
     * xemu (psh.c: the byte-rounded final alpha against the alpha reference, discard on failure). The module is the `_alpha` variant
     * of the configuration's module, reading the reference and the function from the block's alpha_ref slot (vec4 19: x ref, y
     * function 0..7). Invalid or half-written alpha words are left to resolve_alpha_test, which refuses them by name. */
    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_ALPHA_TEST) != 0u &&
        state->output_written[GPU_PGRAPH_OUT_ALPHA_TEST_ENABLE] && state->output[GPU_PGRAPH_OUT_ALPHA_TEST_ENABLE] == 1u &&
        state->output_written[GPU_PGRAPH_OUT_ALPHA_FUNC] && state->output_written[GPU_PGRAPH_OUT_ALPHA_REF]) {
        const uint32_t function = state->output[GPU_PGRAPH_OUT_ALPHA_FUNC];
        const uint32_t reference = state->output[GPU_PGRAPH_OUT_ALPHA_REF];
        if (function >= 0x0200u && function < 0x0207u && reference <= 0xFFu) {
            const size_t length = strlen(out->plan.name);
            if (length + 7u > sizeof out->plan.name) {
                return fail(report, GPU_PGRAPH_ERR_ARGUMENT, "resolve_fragment: the combiner module name has no room for the alpha suffix");
            }
            memcpy(out->plan.name + length, "_alpha", 7u);
            out->plan.constants[19u * 4u] = (float)reference;
            out->plan.constants[19u * 4u + 1u] = (float)(function - 0x0200u);
            out->alpha_in_module = true;
        }
    }
    if (!gpu_vsh_lookup_name(backend->fragment_table, out->plan.name, &out->module)) {
        char made_error[160] = "";
        bool made = false;
        if (backend->make_module != NULL && state->combiner_captured) {
            uint8_t block[GPU_PGRAPH_COMBINER_BLOCK_BYTES] = {0};
            for (uint32_t word = 0u; word < GPU_PGRAPH_COMBINER_WORDS; word++) {
                put_le32(block + word * 4u, state->combiner[word]);
            }
            made = backend->make_module(backend->make_module_context, true, out->plan.name, block, sizeof block, made_error,
                                        sizeof made_error) &&
                   gpu_vsh_lookup_name(backend->fragment_table, out->plan.name, &out->module);
        }
        if (!made) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "the combiner configuration (%u stages, %s) has no translated module in the "
                        "fragment table%s%s",
                        (unsigned)out->plan.stage_count, out->plan.name,
                        made_error[0] != '\0' ? ", and translating it failed: " : "", made_error);
        }
    }
    return GPU_PGRAPH_OK;
}

/* --- vertices ---------------------------------------------------------------------------- */

static const char *type_name(uint32_t type)
{
    switch (type) {
    case GPU_PGRAPH_TYPE_UB_D3D: return "UB_D3D";
    case GPU_PGRAPH_TYPE_S1: return "S1";
    case GPU_PGRAPH_TYPE_F: return "F";
    case GPU_PGRAPH_TYPE_UB_OGL: return "UB_OGL";
    case GPU_PGRAPH_TYPE_S32K: return "S32K";
    case GPU_PGRAPH_TYPE_CMP: return "CMP";
    default: return "unknown";
    }
}

/* What a refused type still needs (T84d), for the refusal text. */
static const char *type_missing(uint32_t type)
{
    switch (type) {
    case GPU_PGRAPH_TYPE_CMP:
        return "only the one word 11-11-10 shape (size 1) is converted, and the layout is INFERRED from xemu (HQ28)";
    case GPU_PGRAPH_TYPE_S32K: return "only size 2 is emitted by the title";
    case GPU_PGRAPH_TYPE_S1:
    case GPU_PGRAPH_TYPE_UB_OGL: return "no emitter in the image writes this type";
    default: return "the only converted shapes are F sizes 1 to 4, UB_D3D 4 and S32K 2";
    }
}

static gpu_pgraph_result need_inference(const gpu_pgraph_backend *backend, uint32_t bit,
                                        const char *what, uint32_t *used, gpu_pgraph_report *report)
{
    if ((backend->allowed_inferences & bit) == 0u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "%s is INFERRED and not allowed", what);
    }
    *used |= bit;
    return GPU_PGRAPH_OK;
}

/* Where an attribute's bytes come from: the draw's vertex snapshot (T262, `bytes` non-NULL, guest
 * [address, address + length)) or the backend's guest reader, at the replay (`bytes` NULL). */
typedef struct {
    const uint8_t *bytes;
    uint32_t address;
    uint32_t length;
} vertex_source;

static gpu_pgraph_result fetch_attribute(const gpu_pgraph_backend *backend, const gpu_pgraph_array *array,
                                         const vertex_source *source,
                                         uint32_t slot, uint32_t vertex, float *out,
                                         uint32_t *used, gpu_pgraph_report *report)
{
    static const float defaults[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    memcpy(out, defaults, sizeof defaults);
    gpu_pgraph_format format = {0u, 0u, 0u};
    if (array->format_set) {
        format = gpu_pgraph_decode_format(array->format);
    }
    if (format.size == 0u) {
        return need_inference(backend, GPU_PGRAPH_INFER_COMPONENT_DEFAULTS,
                              "a disabled vertex slot reading as (0, 0, 0, 1)", used, report);
    }
    if (!array->address_set) {
        return fail(report, GPU_PGRAPH_ERR_MALFORMED,
                    "vertex slot %u has a format but no address was ever written", (unsigned)slot);
    }
    const uint64_t at = (uint64_t)array->address + (uint64_t)format.stride * vertex;
    uint8_t raw[16];
    const size_t bytes = gpu_pgraph_element_bytes(format.type, format.size);
    if (bytes == 0u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "vertex slot %u: type %s (%u) with %u components has no measured conversion "
                    "(%s)",
                    (unsigned)slot, type_name(format.type), (unsigned)format.type,
                    (unsigned)format.size, type_missing(format.type));
    }
    if (source->bytes != NULL) {
        if (at < source->address || at + bytes > (uint64_t)source->address + source->length) {
            return fail(report, GPU_PGRAPH_ERR_MALFORMED,
                        "vertex slot %u: the element at 0x%llX is outside the vertex snapshot [0x%X, "
                        "+%u)", (unsigned)slot, (unsigned long long)at, (unsigned)source->address,
                        (unsigned)source->length);
        }
        memcpy(raw, source->bytes + (at - source->address), bytes);
    } else if (at + bytes > UINT32_MAX || backend->read_guest == NULL ||
               !backend->read_guest(backend->context, (uint32_t)at, raw, bytes)) {
        return fail(report, GPU_PGRAPH_ERR_MALFORMED,
                    "vertex slot %u: guest read of %zu bytes at 0x%llX failed", (unsigned)slot, bytes,
                    (unsigned long long)at);
    }
    if (format.type == GPU_PGRAPH_TYPE_UB_D3D) {
        const gpu_pgraph_result allowed =
            need_inference(backend, GPU_PGRAPH_INFER_D3DCOLOR_ORDER,
                           "reading a UB_D3D attribute as D3DCOLOR (R, G, B, A) / 255", used, report);
        if (allowed != GPU_PGRAPH_OK) {
            return allowed;
        }
        out[0] = (float)raw[2] / 255.0f;
        out[1] = (float)raw[1] / 255.0f;
        out[2] = (float)raw[0] / 255.0f;
        out[3] = (float)raw[3] / 255.0f;
        return GPU_PGRAPH_OK;
    }
    if (format.type == GPU_PGRAPH_TYPE_CMP) {
        const gpu_pgraph_result allowed =
            need_inference(backend, GPU_PGRAPH_INFER_CMP_PACKED,
                           "reading a CMP attribute as one 11-11-10 signed normalised word (xemu layout)", used, report);
        if (allowed != GPU_PGRAPH_OK) {
            return allowed;
        }
        const uint32_t packed = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) |
                                ((uint32_t)raw[3] << 24);
        const int32_t x = (int32_t)(packed << 21) >> 21;
        const int32_t y = (int32_t)(packed << 10) >> 21;
        const int32_t z = (int32_t)packed >> 22;
        /* xemu glsl/vsh.c decompress_11_11_10 (signed bitfieldExtract, no floor): -1024 reads -1.001, -512 reads -1.002.
         * Only xemu's immediate-mode path (pgraph/vertex.c) floors at -1, this is the array path (T1206, HQ28). */
        out[0] = (float)x / 1023.0f;
        out[1] = (float)y / 1023.0f;
        out[2] = (float)z / 511.0f;
        return need_inference(backend, GPU_PGRAPH_INFER_COMPONENT_DEFAULTS,
                              "components a vertex array does not supply reading as (0, 0, 0, 1)", used, report);
    }
    if (format.type == GPU_PGRAPH_TYPE_S32K) {
        const gpu_pgraph_result allowed =
            need_inference(backend, GPU_PGRAPH_INFER_S32K_UNNORMALISED,
                           "reading an S32K attribute as signed 16-bit integers, unscaled", used,
                           report);
        if (allowed != GPU_PGRAPH_OK) {
            return allowed;
        }
        for (uint32_t lane = 0u; lane < format.size; lane++) {
            const uint32_t half = (uint32_t)raw[lane * 2u] | ((uint32_t)raw[lane * 2u + 1u] << 8);
            out[lane] = (float)(int16_t)half;
        }
        return need_inference(backend, GPU_PGRAPH_INFER_COMPONENT_DEFAULTS,
                              "components a vertex array does not supply reading as (0, 0, 0, 1)",
                              used, report);
    }
    for (uint32_t lane = 0u; lane < format.size; lane++) {
        uint32_t word = (uint32_t)raw[lane * 4u] | ((uint32_t)raw[lane * 4u + 1u] << 8) |
                        ((uint32_t)raw[lane * 4u + 2u] << 16) | ((uint32_t)raw[lane * 4u + 3u] << 24);
        memcpy(&out[lane], &word, sizeof word);
    }
    if (format.size < 4u) {
        return need_inference(backend, GPU_PGRAPH_INFER_COMPONENT_DEFAULTS,
                              "components a vertex array does not supply reading as (0, 0, 0, 1)",
                              used, report);
    }
    return GPU_PGRAPH_OK;
}

void gpu_pgraph_assembled_free(gpu_pgraph_assembled *assembled)
{
    if (assembled != NULL) {
        free(assembled->attributes);
        memset(assembled, 0, sizeof *assembled);
    }
}

/* The state a point or line draw rasterizes with, none of it measured (see POINT_SIZE and LINE_WIDTH). */
uint32_t gpu_pgraph_topology_of(uint32_t primitive)
{
    if (primitive == GPU_PGRAPH_OP_POINTS) {
        return GPU_VSH_TOPOLOGY_POINT_LIST;
    }
    if (primitive == GPU_PGRAPH_OP_LINES || primitive == GPU_PGRAPH_OP_LINE_STRIP) {
        return GPU_VSH_TOPOLOGY_LINE_LIST;
    }
    return GPU_VSH_TOPOLOGY_TRIANGLE_LIST;
}

gpu_pgraph_result gpu_pgraph_resolve_primitive(const gpu_pgraph_backend *backend, uint32_t topology,
                                               uint32_t *used, gpu_pgraph_report *report)
{
    if (topology == GPU_VSH_TOPOLOGY_POINT_LIST) {
        return need_inference(backend, GPU_PGRAPH_INFER_POINT_SIZE,
                              "drawing a point at the size the program writes to oPts.x", used, report);
    }
    if (topology != GPU_VSH_TOPOLOGY_LINE_LIST) {
        return GPU_PGRAPH_OK;
    }
    if (backend->line_width == 0.0f) {
        return need_inference(backend, GPU_PGRAPH_INFER_LINE_WIDTH,
                              "drawing a line one pixel wide, the backend stated no line_width", used,
                              report);
    }
    if (backend->line_width != 1.0f) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "line width %g is not drawn: the device has no wideLines, only 1.0 is defined",
                    (double)backend->line_width);
    }
    return GPU_PGRAPH_OK;
}

/* T1255: the vertex independent part of fetch_attribute for a snapshot source, built after vertex 0 passed it. */
typedef enum { FAST_SLOW = 0, FAST_DISABLED, FAST_D3D, FAST_CMP, FAST_S32K, FAST_FLOAT } fast_mode;
typedef struct {
    fast_mode mode;
    const uint8_t *bytes;
    uint64_t snapshot_address, snapshot_end, array_address;
    uint32_t stride, lanes, element;
} fast_slot;

static fast_slot fast_slot_prepare(const gpu_pgraph_array *array, const vertex_source *source)
{
    fast_slot slot = {FAST_SLOW, NULL, 0u, 0u, 0u, 0u, 0u, 0u};
    gpu_pgraph_format format = {0u, 0u, 0u};
    if (array->format_set) {
        format = gpu_pgraph_decode_format(array->format);
    }
    if (format.size == 0u) {
        slot.mode = FAST_DISABLED;
        return slot;
    }
    const size_t element = gpu_pgraph_element_bytes(format.type, format.size);
    if (source->bytes == NULL || !array->address_set || element == 0u) {
        return slot;
    }
    slot.bytes = source->bytes;
    slot.snapshot_address = source->address;
    slot.snapshot_end = (uint64_t)source->address + source->length;
    slot.array_address = array->address;
    slot.stride = format.stride;
    slot.lanes = format.size;
    slot.element = (uint32_t)element;
    slot.mode = format.type == GPU_PGRAPH_TYPE_UB_D3D ? FAST_D3D
                : format.type == GPU_PGRAPH_TYPE_CMP  ? FAST_CMP
                : format.type == GPU_PGRAPH_TYPE_S32K ? FAST_S32K
                                                      : FAST_FLOAT;
    return slot;
}

/* False: take the slow path (it reports the error). Same arithmetic as fetch_attribute (inference bits were set by vertex 0). */
static bool fast_fetch(const fast_slot *slot, uint32_t vertex, float *out)
{
    static const float defaults[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    if (slot->mode == FAST_SLOW) {
        return false;
    }
    memcpy(out, defaults, sizeof defaults);
    if (slot->mode == FAST_DISABLED) {
        return true;
    }
    const uint64_t at = slot->array_address + (uint64_t)slot->stride * vertex;
    if (at < slot->snapshot_address || at + slot->element > slot->snapshot_end) {
        return false;
    }
    const uint8_t *raw = slot->bytes + (at - slot->snapshot_address);
    if (slot->mode == FAST_D3D) {
        out[0] = (float)raw[2] / 255.0f;
        out[1] = (float)raw[1] / 255.0f;
        out[2] = (float)raw[0] / 255.0f;
        out[3] = (float)raw[3] / 255.0f;
    } else if (slot->mode == FAST_CMP) {
        const uint32_t packed = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
        out[0] = (float)((int32_t)(packed << 21) >> 21) / 1023.0f;
        out[1] = (float)((int32_t)(packed << 10) >> 21) / 1023.0f;
        out[2] = (float)((int32_t)packed >> 22) / 511.0f;
    } else if (slot->mode == FAST_S32K) {
        for (uint32_t lane = 0u; lane < slot->lanes; lane++) {
            out[lane] = (float)(int16_t)((uint32_t)raw[lane * 2u] | ((uint32_t)raw[lane * 2u + 1u] << 8));
        }
    } else {
        memcpy(out, raw, (size_t)slot->lanes * 4u); /* the host is little endian, as the word assembly in fetch_attribute */
    }
    return true;
}

gpu_pgraph_result gpu_pgraph_assemble_draw(const gpu_pgraph *pgraph, size_t draw_index,
                                           const gpu_pgraph_backend *backend,
                                           gpu_pgraph_assembled *out, gpu_pgraph_report *report)
{
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(pgraph, draw_index);
    if (draw == NULL || backend == NULL || out == NULL) {
        return fail(report, GPU_PGRAPH_ERR_ARGUMENT, "assemble_draw: no such draw or missing argument");
    }
    const gpu_pgraph_state *state = gpu_pgraph_snapshot(pgraph, draw->snapshot);
    if (draw->inline_vertices) {
        if ((backend->output_groups & GPU_PGRAPH_OUTPUT_IMMEDIATE) == 0u) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "the draw's vertices were written as immediate vertex data (0x1880) but the replay's output_groups "
                        "does not enable the immediate group");
        }
        const gpu_pgraph_result allowed = need_inference(
            backend, GPU_PGRAPH_INFER_OUTPUT_IMMEDIATE_VERTEX,
            "emitting an immediate vertex when attribute 0 is written, each 2f attribute as (x, y, 0, 1)", &out->used_inferences,
            report);
        if (allowed != GPU_PGRAPH_OK) {
            return allowed;
        }
    }
    const uint32_t *indices = gpu_pgraph_indices(pgraph) + draw->first_index;
    const uint32_t capacity = draw->index_count * 6u;
    uint32_t *triangles = malloc((size_t)capacity * sizeof *triangles);
    if (triangles == NULL) {
        return fail(report, GPU_PGRAPH_ERR_MEMORY, "out of memory expanding the primitive");
    }
    uint32_t expanded;
    out->topology = gpu_pgraph_topology_of(draw->primitive);
    if (draw->primitive == GPU_PGRAPH_OP_POINTS) {
        memcpy(triangles, indices, (size_t)draw->index_count * sizeof *triangles);
        expanded = draw->index_count;
    } else if (draw->primitive == GPU_PGRAPH_OP_LINES || draw->primitive == GPU_PGRAPH_OP_LINE_STRIP) {
        expanded = gpu_pgraph_lineate(draw->primitive, indices, draw->index_count, triangles, capacity);
    } else {
        expanded = gpu_pgraph_triangulate(draw->primitive, indices, draw->index_count, triangles,
                                          capacity);
    }
    gpu_pgraph_result result = GPU_PGRAPH_OK;
    if (expanded == UINT32_MAX) {
        result = fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                      "primitive operation %u is not replayed: gpu_vsh_render draws triangle, point "
                      "and line lists only (LINE_LOOP, QUAD_STRIP and POLYGON are never emitted by "
                      "the title)",
                      (unsigned)draw->primitive);
    } else if (expanded > GPU_VSH_MAX_VERTICES) {
        result = fail(report, GPU_PGRAPH_ERR_FULL, "draw expands to %u vertices, the draw limit is %u",
                      (unsigned)expanded, (unsigned)GPU_VSH_MAX_VERTICES);
    }
    if (result == GPU_PGRAPH_OK) {
        result = gpu_pgraph_resolve_primitive(backend, out->topology, &out->used_inferences, report);
    }
    if (result == GPU_PGRAPH_OK && expanded != 0u) {
        out->attributes = calloc((size_t)expanded * GPU_VSH_ATTRIBUTE_FLOATS, sizeof(float));
        if (out->attributes == NULL) {
            result = fail(report, GPU_PGRAPH_ERR_MEMORY, "out of memory assembling vertices");
        }
    }
    /* T1255: vertex 0 goes through fetch_attribute (it owns every error and inference check, and the first error stays the
     * same), the other vertices use the per slot decode prepared once (the format, the source and the checks do not depend on
     * the vertex, only the range check does and a miss falls back to fetch_attribute for the identical error). */
    fast_slot fast[GPU_PGRAPH_ATTRIBUTES];
    for (uint32_t vertex = 0u; result == GPU_PGRAPH_OK && vertex < expanded; vertex++) {
        for (uint32_t slot = 0u; result == GPU_PGRAPH_OK && slot < GPU_PGRAPH_ATTRIBUTES; slot++) {
            float *const destination = out->attributes + (size_t)vertex * GPU_VSH_ATTRIBUTE_FLOATS + slot * 4u;
            if (vertex == 0u) {
                vertex_source source = {NULL, 0u, 0u};
                source.bytes = gpu_pgraph_draw_vertex_bytes(pgraph, draw_index, slot, &source.address, &source.length);
                result = fetch_attribute(backend, &draw->arrays[slot], &source, slot, triangles[vertex], destination,
                                         &out->used_inferences, report);
                fast[slot] = fast_slot_prepare(&draw->arrays[slot], &source);
            } else if (!fast_fetch(&fast[slot], triangles[vertex], destination)) {
                vertex_source source = {NULL, 0u, 0u};
                source.bytes = gpu_pgraph_draw_vertex_bytes(pgraph, draw_index, slot, &source.address, &source.length);
                result = fetch_attribute(backend, &draw->arrays[slot], &source, slot, triangles[vertex], destination,
                                         &out->used_inferences, report);
            }
        }
    }
    free(triangles);
    if (result == GPU_PGRAPH_OK) {
        out->vertex_count = expanded;
        memcpy(out->constants, state->constants, sizeof out->constants);
        const bool scale_in_stream = state->constant_written[58];
        const bool offset_in_stream = state->constant_written[59];
        if ((state->viewport_scale_set && !scale_in_stream) ||
            (state->viewport_offset_set && !offset_in_stream)) {
            result = need_inference(backend, GPU_PGRAPH_INFER_VIEWPORT_CONSTANTS,
                                    "feeding the viewport registers 0x0AF0 and 0x0A20 into "
                                    "constants 58 and 59",
                                    &out->used_inferences, report);
            if (result == GPU_PGRAPH_OK && state->viewport_scale_set && !scale_in_stream) {
                memcpy(out->constants + 58u * 4u, state->viewport_scale, 16u);
            }
            if (result == GPU_PGRAPH_OK && state->viewport_offset_set && !offset_in_stream) {
                memcpy(out->constants + 59u * 4u, state->viewport_offset, 16u);
            }
        }
        if (result == GPU_PGRAPH_OK && backend->viewport_from_target &&
            !state->viewport_scale_set && !state->viewport_offset_set &&
            (!state->constant_written[58] || !state->constant_written[59])) {
            result = need_inference(backend, GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET,
                                    "deriving whole-target viewport constants 58/59 from the render-target size",
                                    &out->used_inferences, report);
            if (result == GPU_PGRAPH_OK) {
                const float half_width = (float)backend->viewport_width * 0.5f;
                const float half_height = (float)backend->viewport_height * 0.5f;
                const float scale[4] = {half_width, -half_height, 1.0f, 0.0f};
                const float offset[4] = {half_width, half_height, 0.0f, 0.0f};
                if (!state->constant_written[58]) {
                    memcpy(out->constants + 58u * 4u, scale, sizeof scale);
                }
                if (!state->constant_written[59]) {
                    memcpy(out->constants + 59u * 4u, offset, sizeof offset);
                }
            }
        }
    }
    if (result == GPU_PGRAPH_OK && backend->window_clip_modules && backend->viewport_from_target &&
        !state->constant_written[58] && !state->viewport_scale_set) {
        /* T560. The movie loop's first two frames write 0x0A20 = (0.53125, 0.53125, 0, 0) and no 0x0AF0 (MEASURED),
         * so the register feed above leaves c58 zero and a lone offset in c59: a half written viewport is not one.
         * The window modules take the whole-target map as a PAIR (T477's numbers), the lone offset is replaced. */
        result = need_inference(backend, GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET,
                                "deriving whole-target viewport constants 58/59 from the render-target size "
                                "for the window-to-clip modules",
                                &out->used_inferences, report);
        if (result == GPU_PGRAPH_OK) {
            const float half_width = (float)backend->viewport_width * 0.5f;
            const float half_height = (float)backend->viewport_height * 0.5f;
            const float scale[4] = {half_width, -half_height, 1.0f, 0.0f};
            const float offset[4] = {half_width, half_height, 0.0f, 0.0f};
            memcpy(out->constants + 58u * 4u, scale, sizeof scale);
            if (!state->constant_written[59]) {
                memcpy(out->constants + 59u * 4u, offset, sizeof offset);
            }
        }
    }
    if (result == GPU_PGRAPH_OK && backend->window_clip_modules &&
        out->constants[58u * 4u] == 0.0f && out->constants[58u * 4u + 1u] == 0.0f) {
        result = fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                      "window-to-clip modules convert x and y with constants 58 and 59, and this draw has "
                      "no viewport scale (no 0x0AF0 words, no row 58 write, no viewport-from-target): "
                      "refused, not drawn raw");
    }
    if (result != GPU_PGRAPH_OK) {
        gpu_pgraph_assembled_free(out);
    }
    return result;
}

/* --- replay ------------------------------------------------------------------------------ */

static uint8_t clear_byte(float value)
{
    const float scaled = value * 255.0f + 0.5f;
    return scaled <= 0.0f ? 0u : scaled >= 255.0f ? 255u : (uint8_t)scaled;
}

static gpu_pgraph_result render_draw(gpu_device *device, const gpu_pgraph_backend *backend,
                                     const gpu_pgraph_program *program,
                                     const gpu_pgraph_fragment *combiner,
                                     const gpu_pgraph_assembled *assembled,
                                     const gpu_vsh_output *output, uint32_t width, uint32_t height,
                                     const float clear[4], gpu_image *out, gpu_pgraph_report *report)
{
    const uint32_t *words = NULL;
    size_t word_count = 0u;
    if (backend->load_module == NULL ||
        !backend->load_module(backend->context, program->module, &words, &word_count)) {
        return fail(report, GPU_PGRAPH_ERR_MALFORMED, "the module of program %s could not be loaded",
                    program->digest);
    }
    gpu_vsh_fragment fragment;
    memset(&fragment, 0, sizeof fragment);
    uint32_t *defaulted = NULL; /* T462: the fragment module rewritten for unwritten varyings */
    uint32_t *texeled = NULL;   /* T510: the fragment module rewritten to take texel coordinates */
    if (combiner != NULL) {
        size_t fragment_count = 0u;
        if (!backend->load_fragment_module(backend->context, combiner->module, &fragment.words,
                                           &fragment_count)) {
            return fail(report, GPU_PGRAPH_ERR_MALFORMED,
                        "the module %s of the combiner stage could not be loaded",
                        combiner->plan.name);
        }
        fragment.word_count = fragment_count;
        uint32_t needed = 0u;
        uint32_t provided = 0u;
        if (!gpu_spirv_interface_locations(fragment.words, fragment.word_count, 1u, &needed) ||
            !gpu_spirv_interface_locations(words, word_count, 3u, &provided)) {
            return fail(report, GPU_PGRAPH_ERR_MALFORMED,
                        "the interface of the vertex module or of %s could not be read",
                        combiner->plan.name);
        }
        if ((needed & ~provided) != 0u) {
            size_t defaulted_count = 0u;
            if ((backend->allowed_inferences & GPU_PGRAPH_INFER_OUTPUT_UNWRITTEN_VARYING) != 0u &&
                gpu_spirv_default_inputs(fragment.words, fragment.word_count, needed & ~provided, &defaulted,
                                         &defaulted_count)) {
                report->used_inferences |= GPU_PGRAPH_INFER_OUTPUT_UNWRITTEN_VARYING;
                fragment.words = defaulted;
                fragment.word_count = defaulted_count;
            } else {
                return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                            "the combiner reads varyings 0x%X but the program %s writes only 0x%X (one "
                            "bit per location, oD0 is 0, oD1 1, oFog 2, oT0 to oT3 3 to 6)%s",
                            (unsigned)needed, program->digest, (unsigned)provided,
                            (backend->allowed_inferences & GPU_PGRAPH_INFER_OUTPUT_UNWRITTEN_VARYING) != 0u
                                ? ", and the fragment module's input could not be defaulted"
                                : "");
            }
        }
        fragment.constants = combiner->plan.constants;
        uint32_t texel_stages = 0u;
        float texel_size[GPU_COMBINER_TEXTURE_STAGES][2] = {{0.0f}};
        for (uint32_t stage = 0u; stage < GPU_COMBINER_TEXTURE_STAGES; stage++) {
            if ((combiner->plan.texture_stages >> stage) & 1u) {
                fragment.textures[stage].rgba = backend->test_textures[stage].rgba;
                fragment.textures[stage].width = backend->test_textures[stage].width;
                fragment.textures[stage].height = backend->test_textures[stage].height;
                fragment.textures[stage].linear = backend->test_textures[stage].linear;
                fragment.textures[stage].repeat = backend->test_textures[stage].repeat;
                if (backend->test_textures[stage].unnormalised) {
                    texel_stages |= 1u << stage;
                    texel_size[stage][0] = (float)backend->test_textures[stage].width;
                    texel_size[stage][1] = (float)backend->test_textures[stage].height;
                }
            }
        }
        if (texel_stages != 0u) {
            size_t texeled_count = 0u;
            if (!gpu_spirv_texel_coordinates(fragment.words, fragment.word_count, texel_stages, texel_size, &texeled,
                                             &texeled_count)) {
                free(defaulted);
                return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                            "the combiner module %s cannot take texel coordinates for texture stages 0x%X (its sample is not "
                            "one OpImageSampleImplicitLod of a vec2 straight from the sampler)",
                            combiner->plan.name, (unsigned)texel_stages);
            }
            fragment.words = texeled;
            fragment.word_count = texeled_count;
        }
    }
    const gpu_vsh_draw draw = {
        .words = words,
        .word_count = word_count,
        .vertex_count = assembled->vertex_count,
        .attributes = assembled->attributes,
        .constants = assembled->constants,
        .topology = assembled->topology,
        .line_width = 1.0f, /* gpu_pgraph_resolve_primitive accepted only 1.0, stated or inferred */
        .fragment = combiner != NULL ? &fragment : NULL,
        .output = output,
    };
    const gpu_result rendered = gpu_vsh_render(device, width, height, clear, &draw, out);
    free(defaulted);
    free(texeled);
    if (rendered != GPU_OK) {
        return fail(report, GPU_PGRAPH_ERR_DEVICE, "gpu_vsh_render: %s", gpu_result_string(rendered));
    }
    return GPU_PGRAPH_OK;
}

/* T552: a drawn draw whose resolved output carries a polygon offset bias. gpu_vsh_draw.c puts the bias on the pipeline
 * of triangle lists only (a line or point list is never biased: RADV biases them and llvmpipe does not, HQ55), so
 * only a triangle draw is APPLIED. The same offset on a line or point draw cannot show, which is what
 * `offset_unobserved` counts, whatever the reason (no depth test, or a topology that is never biased). */
static void count_polygon_offset(const gpu_pgraph_output *output, uint32_t topology, gpu_pgraph_report *report)
{
    const bool biased = output->active && output->output.depth_bias;
    const bool triangles = topology != GPU_VSH_TOPOLOGY_POINT_LIST && topology != GPU_VSH_TOPOLOGY_LINE_LIST;
    report->offset_applied += biased && triangles ? 1u : 0u;
    report->offset_unobserved += output->offset_unobserved || (biased && !triangles) ? 1u : 0u;
}

/* T724: one JSON line per draw for tools.nv2a.standin_units (observation only, a failed write is silent because it changes no frame). */
static void dump_floats(FILE *file, const float *values, size_t count)
{
    fputc('[', file);
    for (size_t i = 0u; i < count; i++) {
        const double value = values[i];
        if (isnan(value)) {
            fputs(i ? ",NaN" : "NaN", file);
        } else if (isinf(value)) {
            fputs(value < 0 ? (i ? ",-Infinity" : "-Infinity") : (i ? ",Infinity" : "Infinity"), file);
        } else {
            fprintf(file, i ? ",%.9g" : "%.9g", value);
        }
    }
    fputc(']', file);
}

static void dump_draw(const char *path, size_t index, const char *digest, bool samples_t0, const gpu_pgraph_assembled *assembled)
{
    FILE *file = fopen(path, "a");
    if (file == NULL) {
        return;
    }
    fprintf(file, "{\"draw\":%zu,\"digest\":\"%s\",\"samples_t0\":%s,\"vertices\":%u,\"attributes\":", index, digest,
            samples_t0 ? "true" : "false", (unsigned)assembled->vertex_count);
    dump_floats(file, assembled->attributes, (size_t)assembled->vertex_count * GPU_VSH_ATTRIBUTE_FLOATS);
    fputs(",\"constants\":", file);
    dump_floats(file, assembled->constants, sizeof assembled->constants / sizeof assembled->constants[0]);
    fputs("}\n", file);
    fclose(file);
}

static gpu_pgraph_result composite(gpu_device *device, const gpu_pgraph_backend *backend,
                                   const gpu_pgraph_program *program,
                                   const gpu_pgraph_fragment *combiner,
                                   const gpu_pgraph_assembled *assembled,
                                   const gpu_pgraph_output *resolved, float *depth, uint8_t *stencil,
                                   gpu_image *frame, gpu_pgraph_report *report)
{
    static const float clear_low[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    static const float clear_high[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const gpu_vsh_output *output = resolved->active ? &resolved->output : NULL;
    if (resolved->needs_destination) {
        /* A draw that depends on what is already in the frame (a blend, a partial colour write) cannot be
         * told apart by its coverage: it is rendered once onto the frame so far and replaces it. */
        gpu_vsh_output onto_frame = resolved->output;
        onto_frame.destination = frame->pixels;
        if (resolved->needs_depth) {
            onto_frame.depth = depth;
            onto_frame.stencil = stencil;
        }
        gpu_image next = {0};
        const gpu_pgraph_result drawn = render_draw(device, backend, program, combiner, assembled,
                                                    &onto_frame, frame->width, frame->height, clear_low,
                                                    &next, report);
        if (drawn == GPU_PGRAPH_OK) {
            memcpy(frame->pixels, next.pixels, (size_t)frame->stride_bytes * frame->height);
        }
        gpu_image_free(&next);
        return drawn;
    }
    gpu_image low = {0};
    gpu_image high = {0};
    gpu_pgraph_result result = render_draw(device, backend, program, combiner, assembled, output,
                                           frame->width, frame->height, clear_low, &low, report);
    if (result == GPU_PGRAPH_OK) {
        result = render_draw(device, backend, program, combiner, assembled, output, frame->width,
                             frame->height, clear_high, &high, report);
    }
    if (result == GPU_PGRAPH_OK) {
        for (uint32_t y = 0u; y < frame->height; y++) {
            for (uint32_t x = 0u; x < frame->width; x++) {
                const uint8_t *a = low.pixels + gpu_image_offset(&low, x, y);
                const uint8_t *b = high.pixels + gpu_image_offset(&high, x, y);
                const bool low_clear = a[0] == 0u && a[1] == 0u && a[2] == 0u && a[3] == 0u;
                const bool high_clear = b[0] == 255u && b[1] == 255u && b[2] == 255u && b[3] == 255u;
                if (!low_clear || !high_clear) {
                    memcpy(frame->pixels + gpu_image_offset(frame, x, y), a, 4u);
                }
            }
        }
    }
    gpu_image_free(&low);
    gpu_image_free(&high);
    return result;
}

static void flip_rows(gpu_image *frame)
{
    uint8_t *row = malloc(frame->stride_bytes);
    if (row == NULL) {
        return;
    }
    for (uint32_t y = 0u; y < frame->height / 2u; y++) {
        uint8_t *top = frame->pixels + (size_t)y * frame->stride_bytes;
        uint8_t *bottom = frame->pixels + (size_t)(frame->height - 1u - y) * frame->stride_bytes;
        memcpy(row, top, frame->stride_bytes);
        memcpy(top, bottom, frame->stride_bytes);
        memcpy(bottom, row, frame->stride_bytes);
    }
    free(row);
}

static gpu_pgraph_result apply_clears_until(const gpu_pgraph *pgraph, const gpu_pgraph_backend *backend,
                                            size_t *next, size_t end, size_t until, gpu_image *frame,
                                            float *depth, uint8_t *stencil, gpu_pgraph_report *report);

gpu_pgraph_result gpu_pgraph_replay(const gpu_pgraph *pgraph, gpu_device *device,
                                    const gpu_pgraph_backend *backend, uint32_t width,
                                    uint32_t height, const float clear[4], gpu_image *out,
                                    gpu_pgraph_report *report)
{
    return gpu_pgraph_replay_range(pgraph, device, backend, width, height, clear, 0u,
                                   gpu_pgraph_draw_count(pgraph), out, report);
}

gpu_pgraph_result gpu_pgraph_replay_range(const gpu_pgraph *pgraph, gpu_device *device,
                                          const gpu_pgraph_backend *backend, uint32_t width,
                                          uint32_t height, const float clear[4], size_t first_draw,
                                          size_t draw_count, gpu_image *out,
                                          gpu_pgraph_report *report)
{
    /* The clears of a range by draw counts alone: those that precede its first draw belong to the passes before it, those
     * that precede one of its draws to it, and those after the frame's last draw to it only when it reaches the end. */
    size_t first_clear = 0u;
    size_t end_clear = 0u;
    if (pgraph != NULL && first_draw <= gpu_pgraph_draw_count(pgraph) &&
        draw_count <= gpu_pgraph_draw_count(pgraph) - first_draw) {
        const size_t clears = gpu_pgraph_clear_count(pgraph);
        while (first_clear < clears && gpu_pgraph_clear_at(pgraph, first_clear)->before_draw < first_draw) {
            first_clear++;
        }
        const bool reaches_the_end = first_draw + draw_count == gpu_pgraph_draw_count(pgraph);
        end_clear = first_clear;
        while (end_clear < clears &&
               (reaches_the_end || gpu_pgraph_clear_at(pgraph, end_clear)->before_draw < first_draw + draw_count)) {
            end_clear++;
        }
    }
    return gpu_pgraph_replay_pass(pgraph, device, backend, width, height, clear, first_draw, draw_count,
                                  first_clear, end_clear - first_clear, out, report);
}

gpu_pgraph_result gpu_pgraph_replay_pass(const gpu_pgraph *pgraph, gpu_device *device,
                                         const gpu_pgraph_backend *backend, uint32_t width,
                                         uint32_t height, const float clear[4], size_t first_draw,
                                         size_t draw_count, size_t first_clear, size_t clear_count,
                                         gpu_image *out, gpu_pgraph_report *report)
{
    gpu_pgraph_report scratch;
    if (report == NULL) {
        report = &scratch;
    }
    memset(report, 0, sizeof *report);
    if (out != NULL) {
        memset(out, 0, sizeof *out);
    }
    if (pgraph == NULL || device == NULL || backend == NULL || clear == NULL || out == NULL ||
        width == 0u || height == 0u || width > 8192u || height > 8192u ||
        first_draw > gpu_pgraph_draw_count(pgraph) ||
        draw_count > gpu_pgraph_draw_count(pgraph) - first_draw ||
        first_clear > gpu_pgraph_clear_count(pgraph) ||
        clear_count > gpu_pgraph_clear_count(pgraph) - first_clear) {
        return fail(report, GPU_PGRAPH_ERR_ARGUMENT,
                    "replay: missing argument, bad target size or a draw or clear range outside the list");
    }
    if (backend->initial_pixels != NULL && backend->flip_y) {
        return fail(report, GPU_PGRAPH_ERR_ARGUMENT,
                    "replay: a kept initial image cannot be combined with flip_y, it would be mirrored a second time");
    }
    report->draws = (uint32_t)draw_count;
    gpu_image frame = {0};
    frame.width = width;
    frame.height = height;
    frame.stride_bytes = width * 4u;
    frame.pixels = malloc((size_t)frame.stride_bytes * height);
    if (frame.pixels == NULL) {
        return fail(report, GPU_PGRAPH_ERR_MEMORY, "out of memory for the frame");
    }
    if (backend->initial_pixels != NULL) {
        memcpy(frame.pixels, backend->initial_pixels, (size_t)frame.stride_bytes * height);
    } else {
        for (uint32_t i = 0u; i < width * height; i++) {
            for (uint32_t lane = 0u; lane < 4u; lane++) {
                frame.pixels[i * 4u + lane] = clear_byte(clear[lane]);
            }
        }
    }
    /* T267: the depth and stencil of this pass. The title's Clear is not in the stream, so they start at 1.0
     * and 0 (the INFERRED depth and stencil models). */
    float *depth = NULL;
    uint8_t *stencil = NULL;
    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_DEPTH_STENCIL) != 0u) {
        depth = malloc((size_t)width * height * sizeof *depth);
        stencil = calloc((size_t)width * height, 1u);
        if (depth == NULL || stencil == NULL) {
            free(depth);
            free(stencil);
            free(frame.pixels);
            return fail(report, GPU_PGRAPH_ERR_MEMORY, "out of memory for the depth and stencil buffers");
        }
        for (size_t i = 0u; i < (size_t)width * height; i++) {
            depth[i] = 1.0f;
        }
    }
    gpu_pgraph_result result = GPU_PGRAPH_OK;
    /* T267: this pass's clear events, applied before the first draw at or after the point they were written */
    size_t next_clear = first_clear;
    const size_t end_of_clears = first_clear + clear_count;
    for (size_t index = first_draw; result == GPU_PGRAPH_OK && index < first_draw + draw_count; index++) {
        report->failed_draw = index;
        result = apply_clears_until(pgraph, backend, &next_clear, end_of_clears, index, &frame, depth, stencil,
                                    report);
        if (result != GPU_PGRAPH_OK) {
            break;
        }
        gpu_pgraph_program program;
        gpu_pgraph_assembled assembled;
        const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(pgraph, index);
        result = gpu_pgraph_resolve_program(gpu_pgraph_snapshot(pgraph, draw->snapshot), backend,
                                            &program, report);
        if (result != GPU_PGRAPH_OK) {
            break;
        }
        gpu_pgraph_fragment combiner;
        memset(&combiner, 0, sizeof combiner);
        gpu_pgraph_backend draw_backend = *backend;
        if (backend->resolve_texture != NULL) {
            for (uint32_t stage = 0u; stage < GPU_COMBINER_TEXTURE_STAGES; stage++) {
                memset(&draw_backend.test_textures[stage], 0, sizeof draw_backend.test_textures[stage]);
                backend->resolve_texture(backend->texture_context, index, gpu_pgraph_snapshot(pgraph, draw->snapshot), stage,
                                         &draw_backend.test_textures[stage]);
            }
        }
        bool unit_unclassified = false;
        if (backend->standin_unit_rule_count != 0u) {
            bool texel = false;
            unit_unclassified = !gpu_standin_unit_lookup(backend->standin_unit_rules, backend->standin_unit_rule_count,
                                                         program.digest, &texel);
            draw_backend.test_textures[0].unnormalised = !unit_unclassified && texel;
        }
        if (backend->combiner) {
            result = gpu_pgraph_resolve_fragment(gpu_pgraph_snapshot(pgraph, draw->snapshot), &draw_backend,
                                                 &combiner, report);
            if (result != GPU_PGRAPH_OK) {
                break;
            }
            if (unit_unclassified && (combiner.plan.texture_stages & 1u) != 0u) {
                result = fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                              "the combiner samples the stand-in at stage 0 but the vertex program %s is in neither the texel nor the "
                              "normalised list (T719): its oT0 unit is unmeasured",
                              program.digest);
                break;
            }
        }
        gpu_pgraph_output output;
        result = gpu_pgraph_resolve_output(gpu_pgraph_snapshot(pgraph, draw->snapshot), backend, width,
                                           height, &output, report);
        if (result != GPU_PGRAPH_OK) {
            break;
        }
        result = gpu_pgraph_assemble_draw(pgraph, index, backend, &assembled, report);
        if (result != GPU_PGRAPH_OK) {
            break;
        }
        if (backend->draw_dump_path != NULL) {
            dump_draw(backend->draw_dump_path, index, program.digest, backend->combiner && (combiner.plan.texture_stages & 1u) != 0u,
                      &assembled);
        }
        if (assembled.vertex_count == 0u) {
            report->degenerate++;
        } else {
            result = composite(device, &draw_backend, &program, backend->combiner ? &combiner : NULL,
                               &assembled, &output, depth, stencil, &frame, report);
            if (result == GPU_PGRAPH_OK) {
                report->drawn++;
                count_polygon_offset(&output, assembled.topology, report);
                report->vertices += assembled.vertex_count;
                report->textured_draws += backend->combiner && combiner.plan.texture_stages != 0u;
                for (uint32_t stage = 0u; backend->combiner && backend->texture_sampled != NULL &&
                                          stage < GPU_COMBINER_TEXTURE_STAGES; stage++) {
                    if ((combiner.plan.texture_stages >> stage) & 1u) {
                        backend->texture_sampled(backend->texture_context, index, stage);
                    }
                }
                report->used_inferences |= assembled.used_inferences | GPU_PGRAPH_INFER_PROGRAM_HEADER |
                                           (program.mode_inferred ? GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN : 0u) |
                                           (backend->combiner ? combiner.plan.used_inferences : 0u) |
                                           output.used_inferences;
            }
        }
        gpu_pgraph_assembled_free(&assembled);
    }
    if (result == GPU_PGRAPH_OK) {
        report->failed_draw = first_draw + draw_count;
        result = apply_clears_until(pgraph, backend, &next_clear, end_of_clears, SIZE_MAX, &frame, depth,
                                    stencil, report);
    }
    free(depth);
    free(stencil);
    if (result != GPU_PGRAPH_OK) {
        free(frame.pixels);
        return result;
    }
    if (backend->flip_y) {
        flip_rows(&frame);
    }
    *out = frame;
    return GPU_PGRAPH_OK;
}

/* --- output state (T267) ------------------------------------------------------------------- */

/* Is any word of `group` written in the snapshot? The decoder wrote only the groups it was told to. */
static bool output_group_written(const gpu_pgraph_state *state, uint32_t group)
{
    for (uint32_t word = 0u; word < GPU_PGRAPH_OUT_COUNT; word++) {
        if (gpu_pgraph_output_group(word) == group && state->output_written[word]) {
            return true;
        }
    }
    return false;
}

static gpu_pgraph_result resolve_scissor(const gpu_pgraph_state *state,
                                         const gpu_pgraph_backend *backend, uint32_t width,
                                         uint32_t height, gpu_pgraph_output *out,
                                         gpu_pgraph_report *report)
{
    const bool horizontal = state->output_written[GPU_PGRAPH_OUT_SURFACE_CLIP_HORIZONTAL];
    const bool vertical = state->output_written[GPU_PGRAPH_OUT_SURFACE_CLIP_VERTICAL];
    if (horizontal != vertical) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "only one of the surface clip words 0x0200 and 0x0204 was written: the "
                    "scissor emitter writes both together");
    }
    if (state->output_written[GPU_PGRAPH_OUT_WINDOW_CLIP_TYPE] &&
        state->output[GPU_PGRAPH_OUT_WINDOW_CLIP_TYPE] != 0u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "window clip type 0x%X is not measured: the scissor emitter writes 0 only and "
                    "refuses an exclusive request",
                    (unsigned)state->output[GPU_PGRAPH_OUT_WINDOW_CLIP_TYPE]);
    }
    if (state->output_written[GPU_PGRAPH_OUT_WINDOW_CLIP_HORIZONTAL]) {
        const uint32_t word = state->output[GPU_PGRAPH_OUT_WINDOW_CLIP_HORIZONTAL];
        if ((word & 0xFFFFu) != 0u || (word >> 16) < width) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "window clip horizontal 0x%08X does not cover the %u pixel target from column 0: "
                        "what a clipping window clip does is not measured",
                        (unsigned)word, (unsigned)width);
        }
    }
    if (state->output_written[GPU_PGRAPH_OUT_WINDOW_CLIP_VERTICAL]) {
        const uint32_t word = state->output[GPU_PGRAPH_OUT_WINDOW_CLIP_VERTICAL];
        if ((word & 0xFFFFu) != 0u || (word >> 16) < height) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "window clip vertical 0x%08X does not cover the %u pixel target from row 0: "
                        "what a clipping window clip does is not measured",
                        (unsigned)word, (unsigned)height);
        }
    }
    if (!horizontal) {
        return GPU_PGRAPH_OK;
    }
    const uint32_t horizontal_word = state->output[GPU_PGRAPH_OUT_SURFACE_CLIP_HORIZONTAL];
    const uint32_t vertical_word = state->output[GPU_PGRAPH_OUT_SURFACE_CLIP_VERTICAL];
    const uint32_t x = horizontal_word & 0xFFFFu;
    const uint32_t clip_width = horizontal_word >> 16;
    const uint32_t y = vertical_word & 0xFFFFu;
    const uint32_t clip_height = vertical_word >> 16;
    if (x + clip_width > width || y + clip_height > height) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "surface clip %ux%u at (%u, %u) lies outside the %ux%u target: clamping it is not "
                    "measured",
                    (unsigned)clip_width, (unsigned)clip_height, (unsigned)x, (unsigned)y,
                    (unsigned)width, (unsigned)height);
    }
    if (x == 0u && y == 0u && clip_width == width && clip_height == height) {
        return GPU_PGRAPH_OK; /* the whole target: nothing depends on how the hardware clips */
    }
    const gpu_pgraph_result allowed = need_inference(
        backend, GPU_PGRAPH_INFER_OUTPUT_SCISSOR,
        "clipping the pixels written to the surface clip rectangle 0x0200 / 0x0204", &out->used_inferences,
        report);
    if (allowed != GPU_PGRAPH_OK) {
        return allowed;
    }
    out->active = true;
    out->output.scissor = true;
    out->output.scissor_x = x;
    out->output.scissor_width = clip_width;
    out->output.scissor_height = clip_height;
    /* flip_y reverses the rows of the finished frame, and the rectangle is the rows of that image. */
    out->output.scissor_y = backend->flip_y ? height - (y + clip_height) : y;
    return GPU_PGRAPH_OK;
}

static gpu_pgraph_result resolve_cull(const gpu_pgraph_state *state,
                                      const gpu_pgraph_backend *backend, gpu_pgraph_output *out,
                                      gpu_pgraph_report *report)
{
    const bool enable_written = state->output_written[GPU_PGRAPH_OUT_CULL_ENABLE];
    const bool face_written = state->output_written[GPU_PGRAPH_OUT_CULL_FACE];
    const bool front_written = state->output_written[GPU_PGRAPH_OUT_FRONT_FACE];
    if (!enable_written) {
        if (face_written) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "a cull face (0x039C) was written but never the enable (0x0308): whether culling is on "
                        "is not measured");
        }
        return GPU_PGRAPH_OK;
    }
    const uint32_t enable = state->output[GPU_PGRAPH_OUT_CULL_ENABLE];
    if (enable > 1u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "cull enable 0x%X is neither 0 nor 1, the only values the library's cull helper writes",
                    (unsigned)enable);
    }
    if (enable == 0u) {
        return GPU_PGRAPH_OK; /* culling off: nothing depends on the face or the winding */
    }
    if (!face_written || !front_written) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "culling is enabled but %s was never written: the initial value of the stored front face "
                    "and of the cull face is not measured",
                    !face_written ? "the cull face (0x039C)" : "the front face (0x03A0)");
    }
    const uint32_t face = state->output[GPU_PGRAPH_OUT_CULL_FACE];
    const uint32_t front = state->output[GPU_PGRAPH_OUT_FRONT_FACE];
    if (face != 0x404u && face != 0x405u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "cull face 0x%X is not 0x404 (front) or 0x405 (back), the only values the library's cull "
                    "helper writes",
                    (unsigned)face);
    }
    if (front != 0x900u && front != 0x901u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "front face 0x%X is not 0x900 (CW) or 0x901 (CCW), the two values of the register headers",
                    (unsigned)front);
    }
    const gpu_pgraph_result allowed = need_inference(
        backend, GPU_PGRAPH_INFER_OUTPUT_CULL_WINDING,
        "judging CW and CCW on the finished image with row 0 at the top for the cull and front face "
        "words 0x039C / 0x03A0",
        &out->used_inferences, report);
    if (allowed != GPU_PGRAPH_OK) {
        return allowed;
    }
    out->active = true;
    out->output.cull_mode = face == 0x404u ? GPU_VSH_CULL_FRONT : GPU_VSH_CULL_BACK;
    /* T1227 (xemu-level, MEASURED against the Story level, HQ49): the title's 0x900 (CW) is NOT clockwise on the finished
     * image (row 0 at the top). With cull back and front 0x900 the first level's cave walls were all culled (only edge-on
     * "ribbons" of the wrong parity survived) and the characters showed their inner faces; the same frames with the winding
     * swapped, or with culling off, match xemu's cave, walls and faces. So the NV2A winding is judged in the y-up window the
     * title's own negative viewport scale (c58.y = -h/2) implies, and on the y-down finished image CW is Vulkan's
     * COUNTER_CLOCKWISE. flip_y (rows reversed) mirrors the image back to that y-up orientation, so the two cancel. */
    out->output.front_clockwise = (front == 0x900u) == backend->flip_y;
    return GPU_PGRAPH_OK;
}

/* The blend factor of an NV2A value (the two register headers agree on all of these), or false. */
static bool blend_factor_of(uint32_t value, uint32_t *factor, bool *constant)
{
    *constant = false;
    switch (value) {
    case 0x0000u: *factor = GPU_VSH_BLEND_ZERO; return true;
    case 0x0001u: *factor = GPU_VSH_BLEND_ONE; return true;
    case 0x0300u: *factor = GPU_VSH_BLEND_SRC_COLOR; return true;
    case 0x0301u: *factor = GPU_VSH_BLEND_ONE_MINUS_SRC_COLOR; return true;
    case 0x0302u: *factor = GPU_VSH_BLEND_SRC_ALPHA; return true;
    case 0x0303u: *factor = GPU_VSH_BLEND_ONE_MINUS_SRC_ALPHA; return true;
    case 0x0304u: *factor = GPU_VSH_BLEND_DST_ALPHA; return true;
    case 0x0305u: *factor = GPU_VSH_BLEND_ONE_MINUS_DST_ALPHA; return true;
    case 0x0306u: *factor = GPU_VSH_BLEND_DST_COLOR; return true;
    case 0x0307u: *factor = GPU_VSH_BLEND_ONE_MINUS_DST_COLOR; return true;
    case 0x0308u: *factor = GPU_VSH_BLEND_SRC_ALPHA_SATURATE; return true;
    case 0x8001u: *factor = GPU_VSH_BLEND_CONSTANT_COLOR; *constant = true; return true;
    case 0x8002u: *factor = GPU_VSH_BLEND_ONE_MINUS_CONSTANT_COLOR; *constant = true; return true;
    case 0x8003u: *factor = GPU_VSH_BLEND_CONSTANT_ALPHA; *constant = true; return true;
    case 0x8004u: *factor = GPU_VSH_BLEND_ONE_MINUS_CONSTANT_ALPHA; *constant = true; return true;
    default: return false;
    }
}

static bool blend_equation_of(uint32_t value, uint32_t *equation)
{
    switch (value) {
    case 0x8006u: *equation = GPU_VSH_BLEND_OP_ADD; return true;
    case 0x800Au: *equation = GPU_VSH_BLEND_OP_SUBTRACT; return true;
    case 0x800Bu: *equation = GPU_VSH_BLEND_OP_REVERSE_SUBTRACT; return true;
    case 0x8007u: *equation = GPU_VSH_BLEND_OP_MIN; return true;
    case 0x8008u: *equation = GPU_VSH_BLEND_OP_MAX; return true;
    default: return false;
    }
}

#define COLOR_MASK_ALL 0x01010101u

static gpu_pgraph_result resolve_blend(const gpu_pgraph_state *state,
                                       const gpu_pgraph_backend *backend, gpu_pgraph_output *out,
                                       gpu_pgraph_report *report)
{
    static const char *const inference_text =
        "the NV2A blend and colour write of the fragment against the frame so far being Vulkan's on RGBA8";
    if (state->output_written[GPU_PGRAPH_OUT_COLOR_MASK]) {
        const uint32_t mask = state->output[GPU_PGRAPH_OUT_COLOR_MASK];
        if ((mask & ~COLOR_MASK_ALL) != 0u) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "colour mask 0x%08X has bits beyond the four channel enables 0x01010101",
                        (unsigned)mask);
        }
        if (mask != COLOR_MASK_ALL) {
            const gpu_pgraph_result allowed = need_inference(
                backend, GPU_PGRAPH_INFER_OUTPUT_BLEND_MODEL, inference_text, &out->used_inferences, report);
            if (allowed != GPU_PGRAPH_OK) {
                return allowed;
            }
            out->active = true;
            out->needs_destination = true;
            out->output.color_write_disable =
                ((mask & 0x00010000u) == 0u ? GPU_VSH_CHANNEL_R : 0u) |
                ((mask & 0x00000100u) == 0u ? GPU_VSH_CHANNEL_G : 0u) |
                ((mask & 0x00000001u) == 0u ? GPU_VSH_CHANNEL_B : 0u) |
                ((mask & 0x01000000u) == 0u ? GPU_VSH_CHANNEL_A : 0u);
        }
    }
    if (!state->output_written[GPU_PGRAPH_OUT_BLEND_ENABLE]) {
        if (state->output_written[GPU_PGRAPH_OUT_BLEND_SFACTOR] ||
            state->output_written[GPU_PGRAPH_OUT_BLEND_DFACTOR] ||
            state->output_written[GPU_PGRAPH_OUT_BLEND_EQUATION] ||
            state->output_written[GPU_PGRAPH_OUT_BLEND_COLOR]) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "blend factors, colour or equation were written but never the blend enable "
                        "(0x0304): whether blending is on is not measured");
        }
        return GPU_PGRAPH_OK;
    }
    const uint32_t enable = state->output[GPU_PGRAPH_OUT_BLEND_ENABLE];
    if (enable > 1u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "blend enable 0x%X is neither 0 nor 1", (unsigned)enable);
    }
    if (enable == 0u) {
        return GPU_PGRAPH_OK;
    }
    if (!state->output_written[GPU_PGRAPH_OUT_BLEND_SFACTOR] ||
        !state->output_written[GPU_PGRAPH_OUT_BLEND_DFACTOR] ||
        !state->output_written[GPU_PGRAPH_OUT_BLEND_EQUATION]) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "blending is enabled but the %s was never written: its initial value is not measured",
                    !state->output_written[GPU_PGRAPH_OUT_BLEND_SFACTOR]   ? "source factor (0x0344)"
                    : !state->output_written[GPU_PGRAPH_OUT_BLEND_DFACTOR] ? "destination factor (0x0348)"
                                                                           : "equation (0x0350)");
    }
    uint32_t source = 0u;
    uint32_t destination = 0u;
    uint32_t equation = 0u;
    bool source_constant = false;
    bool destination_constant = false;
    if (!blend_factor_of(state->output[GPU_PGRAPH_OUT_BLEND_SFACTOR], &source, &source_constant)) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "blend source factor 0x%X is not one of the 15 values of the register headers",
                    (unsigned)state->output[GPU_PGRAPH_OUT_BLEND_SFACTOR]);
    }
    if (!blend_factor_of(state->output[GPU_PGRAPH_OUT_BLEND_DFACTOR], &destination,
                         &destination_constant)) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "blend destination factor 0x%X is not one of the 15 values of the register headers",
                    (unsigned)state->output[GPU_PGRAPH_OUT_BLEND_DFACTOR]);
    }
    if (!blend_equation_of(state->output[GPU_PGRAPH_OUT_BLEND_EQUATION], &equation)) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "blend equation 0x%X is not ADD, SUBTRACT, REVERSE_SUBTRACT, MIN or MAX",
                    (unsigned)state->output[GPU_PGRAPH_OUT_BLEND_EQUATION]);
    }
    if ((source_constant || destination_constant) && !state->output_written[GPU_PGRAPH_OUT_BLEND_COLOR]) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "a constant blend factor is used but the blend colour (0x034C) was never written");
    }
    const gpu_pgraph_result allowed = need_inference(backend, GPU_PGRAPH_INFER_OUTPUT_BLEND_MODEL,
                                                     inference_text, &out->used_inferences, report);
    if (allowed != GPU_PGRAPH_OK) {
        return allowed;
    }
    out->active = true;
    out->needs_destination = true;
    out->output.blend = true;
    out->output.blend_source = source;
    out->output.blend_destination = destination;
    out->output.blend_equation = equation;
    if (state->output_written[GPU_PGRAPH_OUT_BLEND_COLOR]) {
        const uint32_t color = state->output[GPU_PGRAPH_OUT_BLEND_COLOR]; /* ARGB, INFERRED */
        out->output.blend_constant[0] = (float)((color >> 16) & 0xFFu) / 255.0f;
        out->output.blend_constant[1] = (float)((color >> 8) & 0xFFu) / 255.0f;
        out->output.blend_constant[2] = (float)(color & 0xFFu) / 255.0f;
        out->output.blend_constant[3] = (float)(color >> 24) / 255.0f;
    }
    return GPU_PGRAPH_OK;
}

static gpu_pgraph_result resolve_alpha_test(const gpu_pgraph_state *state,
                                            const gpu_pgraph_backend *backend, gpu_pgraph_output *out,
                                            gpu_pgraph_report *report)
{
    if (!state->output_written[GPU_PGRAPH_OUT_ALPHA_TEST_ENABLE]) {
        if (state->output_written[GPU_PGRAPH_OUT_ALPHA_FUNC] ||
            state->output_written[GPU_PGRAPH_OUT_ALPHA_REF]) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "an alpha function or reference was written but never the alpha test enable "
                        "(0x0300): whether the test is on is not measured");
        }
        return GPU_PGRAPH_OK;
    }
    const uint32_t enable = state->output[GPU_PGRAPH_OUT_ALPHA_TEST_ENABLE];
    if (enable > 1u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "alpha test enable 0x%X is neither 0 nor 1",
                    (unsigned)enable);
    }
    if (enable == 0u) {
        return GPU_PGRAPH_OK;
    }
    if (!state->output_written[GPU_PGRAPH_OUT_ALPHA_FUNC] ||
        !state->output_written[GPU_PGRAPH_OUT_ALPHA_REF]) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "the alpha test is enabled but the %s was never written: its initial value is not "
                    "measured",
                    !state->output_written[GPU_PGRAPH_OUT_ALPHA_FUNC] ? "function (0x033C)"
                                                                      : "reference (0x0340)");
    }
    const uint32_t function = state->output[GPU_PGRAPH_OUT_ALPHA_FUNC];
    const uint32_t reference = state->output[GPU_PGRAPH_OUT_ALPHA_REF];
    if (function < 0x0200u || function > 0x0207u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "alpha function 0x%X is not one of 0x200 (NEVER) to 0x207 (ALWAYS)", (unsigned)function);
    }
    if (reference > 0xFFu) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "alpha reference 0x%X is above 255, which no byte comparison has", (unsigned)reference);
    }
    if (function == 0x0207u) {
        return GPU_PGRAPH_OK; /* ALWAYS passes every fragment: nothing depends on the reference */
    }
    const gpu_pgraph_result allowed = need_inference(
        backend, GPU_PGRAPH_INFER_OUTPUT_ALPHA_TEST_MODEL,
        "comparing the fragment alpha, clamped and rounded to a byte, with the alpha reference byte",
        &out->used_inferences, report);
    if (allowed != GPU_PGRAPH_OK) {
        return allowed;
    }
    if (backend->combiner) {
        return GPU_PGRAPH_OK; /* T860: the combiner module runs the test (resolve_fragment), the fixed fragment stage is not used */
    }
    out->active = true;
    out->output.alpha_test = true;
    out->output.alpha_func = function - 0x0200u;
    out->output.alpha_ref = reference;
    return GPU_PGRAPH_OK;
}

/* A comparison of the headers' 0x200 + n enumeration as a GPU_VSH_COMPARE_*, or false. */
static bool compare_of(uint32_t value, uint32_t *compare)
{
    if (value < 0x0200u || value > 0x0207u) {
        return false;
    }
    *compare = value - 0x0200u;
    return true;
}

static bool stencil_op_of(uint32_t value, uint32_t *operation)
{
    switch (value) {
    case 0x1E00u: *operation = GPU_VSH_STENCIL_OP_KEEP; return true;
    case 0x0000u: *operation = GPU_VSH_STENCIL_OP_ZERO; return true;
    case 0x1E01u: *operation = GPU_VSH_STENCIL_OP_REPLACE; return true;
    case 0x1E02u: *operation = GPU_VSH_STENCIL_OP_INCREMENT_CLAMP; return true;
    case 0x1E03u: *operation = GPU_VSH_STENCIL_OP_DECREMENT_CLAMP; return true;
    case 0x150Au: *operation = GPU_VSH_STENCIL_OP_INVERT; return true;
    case 0x8507u: *operation = GPU_VSH_STENCIL_OP_INCREMENT_WRAP; return true;
    case 0x8508u: *operation = GPU_VSH_STENCIL_OP_DECREMENT_WRAP; return true;
    default: return false;
    }
}

static gpu_pgraph_result resolve_depth(const gpu_pgraph_state *state,
                                       const gpu_pgraph_backend *backend, gpu_pgraph_output *out,
                                       gpu_pgraph_report *report)
{
    const bool enable_written = state->output_written[GPU_PGRAPH_OUT_DEPTH_ENABLE];
    const bool function_written = state->output_written[GPU_PGRAPH_OUT_DEPTH_FUNC];
    const bool mask_written = state->output_written[GPU_PGRAPH_OUT_DEPTH_MASK];
    if (enable_written) {
        const uint32_t enable = state->output[GPU_PGRAPH_OUT_DEPTH_ENABLE];
        if (enable > 1u) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "depth test enable 0x%X is neither 0 nor 1",
                        (unsigned)enable);
        }
        if (enable == 0u) {
            return GPU_PGRAPH_OK;
        }
    } else if (!function_written && !mask_written) {
        return GPU_PGRAPH_OK;
    }
    if (!function_written || !mask_written) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "the depth test is enabled but the %s was never written: its initial value is not "
                    "measured",
                    !function_written ? "function (0x0354)" : "mask (0x035C)");
    }
    uint32_t function = 0u;
    const uint32_t mask = state->output[GPU_PGRAPH_OUT_DEPTH_MASK];
    if (!compare_of(state->output[GPU_PGRAPH_OUT_DEPTH_FUNC], &function)) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "depth function 0x%X is not one of 0x200 (NEVER) to 0x207 (ALWAYS)",
                    (unsigned)state->output[GPU_PGRAPH_OUT_DEPTH_FUNC]);
    }
    if (mask > 1u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "depth mask 0x%X is neither 0 nor 1", (unsigned)mask);
    }
    if (function == GPU_VSH_COMPARE_ALWAYS && mask == 0u) {
        return GPU_PGRAPH_OK; /* every fragment passes and nothing is written: no effect on any draw */
    }
    gpu_pgraph_result allowed = need_inference(
        backend, GPU_PGRAPH_INFER_OUTPUT_DEPTH_MODEL,
        "comparing and writing the vertex program's z / w as a float32 depth that starts at 1.0 for the pass",
        &out->used_inferences, report);
    if (allowed == GPU_PGRAPH_OK && !enable_written) {
        allowed = need_inference(
            backend, GPU_PGRAPH_INFER_OUTPUT_DEPTH_STENCIL_ENABLE,
            "taking the depth test as enabled because its enable word 0x030C was never written (its emitter "
            "is elided)",
            &out->used_inferences, report);
    }
    if (allowed != GPU_PGRAPH_OK) {
        return allowed;
    }
    out->active = true;
    out->needs_destination = true;
    out->needs_depth = true;
    out->output.depth_test = true;
    out->output.depth_func = function;
    out->output.depth_write = mask == 1u;
    return GPU_PGRAPH_OK;
}

static gpu_pgraph_result resolve_stencil(const gpu_pgraph_state *state,
                                         const gpu_pgraph_backend *backend, gpu_pgraph_output *out,
                                         gpu_pgraph_report *report)
{
    static const struct {
        int word;
        const char *name;
        uint32_t fallback;
    } words[] = {
        {GPU_PGRAPH_OUT_STENCIL_FUNC, "function (0x0364)", 0x0207u},
        {GPU_PGRAPH_OUT_STENCIL_REF, "reference (0x0368)", 0u},
        {GPU_PGRAPH_OUT_STENCIL_FUNC_MASK, "function mask (0x036C)", 0xFFu},
        {GPU_PGRAPH_OUT_STENCIL_MASK, "write mask (0x0360)", 0xFFu},
        {GPU_PGRAPH_OUT_STENCIL_OP_FAIL, "op on fail (0x0370)", 0x1E00u},
        {GPU_PGRAPH_OUT_STENCIL_OP_ZFAIL, "op on depth fail (0x0374)", 0x1E00u},
        {GPU_PGRAPH_OUT_STENCIL_OP_ZPASS, "op on pass (0x0378)", 0x1E00u},
    };
    bool any_written = false;
    for (size_t i = 0u; i < sizeof words / sizeof words[0]; i++) {
        any_written = any_written || state->output_written[words[i].word];
    }
    const bool enable_written = state->output_written[GPU_PGRAPH_OUT_STENCIL_ENABLE];
    if (enable_written) {
        const uint32_t enable = state->output[GPU_PGRAPH_OUT_STENCIL_ENABLE];
        if (enable > 1u) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "stencil test enable 0x%X is neither 0 nor 1",
                        (unsigned)enable);
        }
        if (enable == 0u) {
            return GPU_PGRAPH_OK;
        }
    } else if (!any_written) {
        return GPU_PGRAPH_OK;
    }
    uint32_t value[7];
    bool defaulted = false;
    for (size_t i = 0u; i < sizeof words / sizeof words[0]; i++) {
        const bool written = state->output_written[words[i].word];
        value[i] = written ? state->output[words[i].word] : words[i].fallback;
        defaulted = defaulted || !written;
    }
    uint32_t function = 0u;
    uint32_t operations[3] = {0u, 0u, 0u};
    if (!compare_of(value[0], &function)) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "stencil function 0x%X is not one of 0x200 (NEVER) to 0x207 (ALWAYS)", (unsigned)value[0]);
    }
    if (value[1] > 0xFFu || value[2] > 0xFFu || value[3] > 0xFFu) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "a stencil %s 0x%X is above 255, which an 8 bit stencil does not have",
                    value[1] > 0xFFu ? "reference" : value[2] > 0xFFu ? "function mask" : "write mask",
                    (unsigned)(value[1] > 0xFFu ? value[1] : value[2] > 0xFFu ? value[2] : value[3]));
    }
    for (size_t i = 0u; i < 3u; i++) {
        if (!stencil_op_of(value[4u + i], &operations[i])) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "stencil %s 0x%X is not one of the eight operations of the headers",
                        words[4u + i].name, (unsigned)value[4u + i]);
        }
    }
    if (function == GPU_VSH_COMPARE_ALWAYS && operations[0] == GPU_VSH_STENCIL_OP_KEEP &&
        operations[1] == GPU_VSH_STENCIL_OP_KEEP && operations[2] == GPU_VSH_STENCIL_OP_KEEP) {
        return GPU_PGRAPH_OK; /* every fragment passes and the stencil is never changed: no effect */
    }
    gpu_pgraph_result allowed = need_inference(
        backend, GPU_PGRAPH_INFER_OUTPUT_STENCIL_MODEL,
        "an 8 bit stencil that starts at 0 for the pass and compares and updates like Vulkan's",
        &out->used_inferences, report);
    if (allowed == GPU_PGRAPH_OK && !enable_written) {
        allowed = need_inference(
            backend, GPU_PGRAPH_INFER_OUTPUT_DEPTH_STENCIL_ENABLE,
            "taking the stencil test as enabled because its enable word 0x032C was never written (its emitter "
            "is elided)",
            &out->used_inferences, report);
    }
    if (allowed == GPU_PGRAPH_OK && defaulted) {
        allowed = need_inference(
            backend, GPU_PGRAPH_INFER_OUTPUT_STENCIL_DEFAULTS,
            "taking the stencil words never written as ALWAYS, reference 0, masks 0xFF and every op KEEP",
            &out->used_inferences, report);
    }
    if (allowed != GPU_PGRAPH_OK) {
        return allowed;
    }
    out->active = true;
    out->needs_destination = true;
    out->needs_depth = true;
    out->output.stencil_test = true;
    out->output.stencil_func = function;
    out->output.stencil_ref = value[1];
    out->output.stencil_compare_mask = value[2];
    out->output.stencil_write_mask = value[3];
    out->output.stencil_fail_op = operations[0];
    out->output.stencil_zfail_op = operations[1];
    out->output.stencil_zpass_op = operations[2];
    return GPU_PGRAPH_OK;
}

static float float_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

/* T860, the front and back polygon mode words (0x038C, 0x0390): NV097_SET_FRONT_POLYGON_MODE and the back one take POINT 0x1B00,
 * LINE 0x1B01 or FILL 0x1B02. XEMU REFERENCE (v0.8.136, the hw/xbox/nv2a/pgraph source and an nxdk probe run in it, HQ61): the three
 * values draw filled triangles, only the 3 vertices as 1 pixel dots, and the 3 edges as 1 pixel lines, and any other value, 0
 * included, aborts xemu (`kelvin_map_polygon_mode` asserts, so xemu never rendered a 0). It also asserts that the front and the back
 * mode are equal ("missing support for 2-sided-poly mode"), so a difference is refused here. A mode applies to triangle primitives
 * only (a point or line list ignores it, as Vulkan's polygonMode does).
 * A written zero is not one of the three supported NV097 polygon-mode encodings and is refused. T885 measured that the original
 * CreateDevice default-state table initializes both D3D8 shadows to FILL (0x1B02); the host port now reproduces those stores.
 * An unwritten word retains the device reset default of FILL.
 * `*mode` is GPU_VSH_POLYGON_*. */
static gpu_pgraph_result resolve_polygon_mode(const gpu_pgraph_state *state, const gpu_pgraph_backend *backend,
                                              gpu_pgraph_output *out, gpu_pgraph_report *report, uint32_t *mode)
{
    (void)backend;
    static const struct {
        gpu_pgraph_output_word word;
        const char *name;
    } words[2] = {
        {GPU_PGRAPH_OUT_FRONT_POLYGON_MODE, "front polygon mode (0x038C)"},
        {GPU_PGRAPH_OUT_BACK_POLYGON_MODE, "back polygon mode (0x0390)"},
    };
    uint32_t modes[2] = {GPU_VSH_POLYGON_FILL, GPU_VSH_POLYGON_FILL};
    *mode = GPU_VSH_POLYGON_FILL;
    for (size_t i = 0u; i < 2u; i++) {
        if (!state->output_written[words[i].word]) {
            continue;
        }
        const uint32_t value = state->output[words[i].word];
        switch (value) {
        case 0x1B00u: modes[i] = GPU_VSH_POLYGON_POINT; break;
        case 0x1B01u: modes[i] = GPU_VSH_POLYGON_LINE; break;
        case 0x1B02u: modes[i] = GPU_VSH_POLYGON_FILL; break;
        default:
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "%s is 0x%08X, none of POINT 0x1B00, LINE 0x1B01 and FILL 0x1B02 (xemu aborts on any other value)",
                        words[i].name, (unsigned)value);
        }
    }
    if (modes[0] != modes[1]) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "the front polygon mode 0x%08X (0x038C) and the back polygon mode 0x%08X (0x0390) differ: Vulkan has one polygon mode "
                    "per pipeline and xemu asserts that they are equal",
                    (unsigned)state->output[GPU_PGRAPH_OUT_FRONT_POLYGON_MODE],
                    (unsigned)state->output[GPU_PGRAPH_OUT_BACK_POLYGON_MODE]);
    }
    *mode = modes[0];
    if (*mode != GPU_VSH_POLYGON_FILL) {
        out->active = true;
        out->output.polygon_mode = *mode;
    }
    return GPU_PGRAPH_OK;
}

/* T502, polygon offset. An enable word must be 0 or 1, the two float words finite. Only the FILL enable can show: the
 * point and line enables belong to polygons in point or line fill mode, which the replay never rasterizes. A zero
 * offset (-0.0 too) is nothing. A non-zero one becomes the Vulkan depth bias of a draw that tests depth, and is
 * counted as unobserved when nothing tests depth. */
static gpu_pgraph_result resolve_polygon_offset(const gpu_pgraph_state *state,
                                                const gpu_pgraph_backend *backend,
                                                gpu_pgraph_output *out, gpu_pgraph_report *report, uint32_t polygon_mode)
{
    static const struct {
        gpu_pgraph_output_word word;
        const char *name;
    } enables[] = {
        {GPU_PGRAPH_OUT_POLY_OFFSET_POINT, "point (0x0330)"},
        {GPU_PGRAPH_OUT_POLY_OFFSET_LINE, "line (0x0334)"},
        {GPU_PGRAPH_OUT_POLY_OFFSET_FILL, "fill (0x0338)"},
    };
    bool enabled[3] = {false, false, false};
    for (size_t i = 0u; i < sizeof enables / sizeof enables[0]; i++) {
        if (!state->output_written[enables[i].word]) {
            continue;
        }
        const uint32_t value = state->output[enables[i].word];
        if (value > 1u) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "polygon offset %s enable 0x%X is neither 0 nor 1",
                        enables[i].name, (unsigned)value);
        }
        enabled[i] = value == 1u;
    }
    const bool scale_written = state->output_written[GPU_PGRAPH_OUT_POLY_OFFSET_SCALE];
    const bool bias_written = state->output_written[GPU_PGRAPH_OUT_POLY_OFFSET_BIAS];
    float scale = 0.0f;
    float bias = 0.0f;
    if (scale_written) {
        scale = float_from_bits(state->output[GPU_PGRAPH_OUT_POLY_OFFSET_SCALE]);
        if (!isfinite(scale)) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "polygon offset scale factor 0x%08X is not finite",
                        (unsigned)state->output[GPU_PGRAPH_OUT_POLY_OFFSET_SCALE]);
        }
    }
    if (bias_written) {
        bias = float_from_bits(state->output[GPU_PGRAPH_OUT_POLY_OFFSET_BIAS]);
        if (!isfinite(bias)) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "polygon offset bias 0x%08X is not finite",
                        (unsigned)state->output[GPU_PGRAPH_OUT_POLY_OFFSET_BIAS]);
        }
    }
    /* T860: xemu (pgraph/glsl/psh.c) applies the offset only when the enable of the FRONT POLYGON MODE is on (fill 0x0338, line
     * 0x0334, point 0x0330): in LINE or POINT mode that enable decides and the others are ignored. */
    if (polygon_mode != GPU_VSH_POLYGON_FILL) {
        enabled[2] = polygon_mode == GPU_VSH_POLYGON_LINE ? enabled[1] : enabled[0];
        enabled[0] = false;
        enabled[1] = false;
    }
    if (enabled[0] || enabled[1]) {
        const gpu_pgraph_result allowed = need_inference(
            backend, GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_FILL_ONLY,
            "ignoring the point and line polygon offset enables, which apply to polygons drawn in point or line "
            "mode, a mode the replay never rasterizes",
            &out->used_inferences, report);
        if (allowed != GPU_PGRAPH_OK) {
            return allowed;
        }
    }
    if (!enabled[2]) {
        return GPU_PGRAPH_OK;
    }
    if (!scale_written || !bias_written) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "the polygon offset fill enable is on but the %s was never written: its initial value is not "
                    "measured",
                    scale_written ? "bias (0x0388)" : "scale factor (0x0384)");
    }
    if (scale == 0.0f && bias == 0.0f) {
        return GPU_PGRAPH_OK; /* an enabled offset of nothing (the ZBIAS handler's -0.0 too) moves no depth */
    }
    const gpu_pgraph_result allowed = need_inference(
        backend, GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_MODEL,
        "offsetting the depth of filled triangles by Vulkan's depth bias, constant factor = the NV2A bias (0x0388) "
        "and slope factor = its scale factor (0x0384), as glPolygonOffset(scale, bias) on a float32 depth buffer",
        &out->used_inferences, report);
    if (allowed != GPU_PGRAPH_OK) {
        return allowed;
    }
    if (!out->output.depth_test) {
        out->offset_unobserved = true; /* nothing compares or writes the depth: the offset cannot show */
        return GPU_PGRAPH_OK;
    }
    out->output.depth_bias = true;
    out->output.depth_bias_constant = bias;
    out->output.depth_bias_slope = scale;
    return GPU_PGRAPH_OK;
}

/* --- T462: surface, fixed-function and texture stage words, each pinned to the value measured ------------------------ */

static const char *const pinned_state_note =
    "treating the surface, fixed-function and texture stage words as describing the target and pipeline the replay "
    "already models, each at the value measured in the recorded stream";

/* Is the word written, and when it is, is it exactly `expected`? Refuses with the register and what it is for. */
static gpu_pgraph_result pin_word(const gpu_pgraph_state *state, gpu_pgraph_output_word word, uint32_t expected,
                                  const char *what, bool *any, gpu_pgraph_report *report)
{
    if (!state->output_written[word]) {
        return GPU_PGRAPH_OK;
    }
    *any = true;
    if (state->output[word] != expected) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "%s is 0x%08X, the replay decodes only the measured 0x%08X", what,
                    (unsigned)state->output[word], (unsigned)expected);
    }
    return GPU_PGRAPH_OK;
}

static gpu_pgraph_result resolve_surface(const gpu_pgraph_state *state, const gpu_pgraph_backend *backend,
                                         gpu_pgraph_output *out, gpu_pgraph_report *report)
{
    bool any = false;
    for (gpu_pgraph_output_word word = GPU_PGRAPH_OUT_SURFACE_FORMAT; word <= GPU_PGRAPH_OUT_ANTI_ALIASING; word++) {
        any = any || state->output_written[word];
    }
    if (state->output_written[GPU_PGRAPH_OUT_SURFACE_FORMAT]) {
        const uint32_t format = state->output[GPU_PGRAPH_OUT_SURFACE_FORMAT];
        /* T1489: with live_swizzled_targets the swizzled type 2 (log2 width bits 16..23, log2 height 24..31) is a target of that size drawn in
         * picture order, no antialiasing. Without it (the default and the CPU replay) only the measured pitch layout passes. */
        const bool swizzled = backend->live_swizzled_targets && ((format >> 8) & 0xFu) == 2u && ((format >> 16) & 0xFFu) <= 15u &&
                              ((format >> 24) & 0xFFu) <= 15u;
        if ((format & 0xFu) != 8u || ((format >> 4) & 0xFu) != 2u || (((format >> 8) & 0xFu) != 1u && !swizzled) ||
            ((format >> 12) & 0xFu) != 0u || (!swizzled && (format >> 16) != 0u)) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "surface format 0x%08X (0x0208) is not A8R8G8B8 (colour 8) with Z24S8 (zeta 2) in the pitch layout "
                        "(type 1) without antialiasing: the replay renders A8R8G8B8 targets only",
                        (unsigned)format);
        }
    }
    gpu_pgraph_result result = pin_word(state, GPU_PGRAPH_OUT_CONTROL0, 0x00100001u,
                                        "CONTROL0 (0x0290)", &any, report);
    if (result == GPU_PGRAPH_OK) {
        result = pin_word(state, GPU_PGRAPH_OUT_CLIP_MIN, 0x00000000u, "depth clip minimum (0x0394)", &any, report);
    }
    if (result == GPU_PGRAPH_OK) {
        result = pin_word(state, GPU_PGRAPH_OUT_CLIP_MAX, 0x4B7FFFFFu, "depth clip maximum (0x0398)", &any, report);
    }
    if (result == GPU_PGRAPH_OK && state->output_written[GPU_PGRAPH_OUT_ANTI_ALIASING]) {
        /* Anti-aliasing off (bits 0 to 15 zero) with every sample (0xFFFF, the retail movie loop) or none (0, the library's
         * own un-initialised shadow, the disc-less steady loop, T572): that a zero mask hides no pixel is NOT measured. */
        const uint32_t control = state->output[GPU_PGRAPH_OUT_ANTI_ALIASING];
        any = true;
        if ((control & 0x0000FFFFu) != 0u || ((control >> 16) != 0u && (control >> 16) != 0xFFFFu)) {
            result = fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                          "anti-aliasing control (0x1D7C) is 0x%08X, the replay decodes only the measured 0xFFFF0000 and 0x00000000",
                          (unsigned)control);
        }
    }
    if (result != GPU_PGRAPH_OK || !any) {
        return result;
    }
    return need_inference(backend, GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE, pinned_state_note, &out->used_inferences, report);
}

static gpu_pgraph_result resolve_fixed(const gpu_pgraph_state *state, const gpu_pgraph_backend *backend,
                                       gpu_pgraph_output *out, gpu_pgraph_report *report)
{
    static const struct {
        gpu_pgraph_output_word word;
        uint32_t value;
        const char *what;
    } pinned[] = {
        {GPU_PGRAPH_OUT_LIGHTING_ENABLE, 0u, "lighting enable (0x0314)"},
        {GPU_PGRAPH_OUT_SPECULAR_ENABLE, 0u, "specular enable (0x03B8)"},
        {GPU_PGRAPH_OUT_LIGHT_ENABLE_MASK, 0u, "light enable mask (0x03BC)"},
        {GPU_PGRAPH_OUT_FOG_ENABLE, 0u, "fog enable (0x02A4)"},
        {GPU_PGRAPH_OUT_POINT_PARAMS_ENABLE, 0u, "point parameters enable (0x0318)"},
        {GPU_PGRAPH_OUT_POINT_SMOOTH_ENABLE, 0u, "point smooth enable (0x031C)"},
    };
    /* T860: the two polygon mode words are decoded by resolve_polygon_mode (before the polygon offset, which depends on the
     * mode), here they only make the group "used" so the pinned-state inference is still needed. */
    bool any = state->output_written[GPU_PGRAPH_OUT_POINT_SIZE] || state->output_written[GPU_PGRAPH_OUT_FRONT_POLYGON_MODE] ||
               state->output_written[GPU_PGRAPH_OUT_BACK_POLYGON_MODE];
    for (size_t i = 0u; i < sizeof pinned / sizeof pinned[0]; i++) {
        if (pinned[i].word == GPU_PGRAPH_OUT_FOG_ENABLE && backend->live_fog_modules &&
            state->output_written[GPU_PGRAPH_OUT_FOG_ENABLE]) {
            gpu_fog_control fog;
            char reason[160];
            if (!gpu_fog_decode(state, &fog, reason, sizeof reason))
                return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "%s", reason);
            any = true;
            continue;
        }
        const gpu_pgraph_result result = pin_word(state, pinned[i].word, pinned[i].value, pinned[i].what, &any, report);
        if (result != GPU_PGRAPH_OK) {
            return result;
        }
    }
    if (state->output_written[GPU_PGRAPH_OUT_LIGHT_CONTROL]) {
        any = true;
        const uint32_t control = state->output[GPU_PGRAPH_OUT_LIGHT_CONTROL];
        if ((control & ~0x00030001u) != 0u) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "light control 0x%08X (0x0294) sets a bit beyond separate specular, local eye and alpha from material "
                        "specular",
                        (unsigned)control);
        }
    }
    if (state->output_written[GPU_PGRAPH_OUT_ZCULL_ENABLE]) {
        any = true;
        if (state->output[GPU_PGRAPH_OUT_ZCULL_ENABLE] > 3u) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "the z-cull enable 0x%X (0x1D84) is above 3, the library computes 0 to 3",
                        (unsigned)state->output[GPU_PGRAPH_OUT_ZCULL_ENABLE]);
        }
    }
    return any ? need_inference(backend, GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE, pinned_state_note, &out->used_inferences, report)
               : GPU_PGRAPH_OK;
}

static gpu_pgraph_result resolve_texture(const gpu_pgraph_state *state, const gpu_pgraph_backend *backend,
                                         gpu_pgraph_output *out, gpu_pgraph_report *report)
{
    bool any = false;
    for (uint32_t stage = 0u; stage < 4u; stage++) {
        const uint32_t address = state->output[GPU_PGRAPH_OUT_TEXTURE_ADDRESS + stage];
        if (state->output_written[GPU_PGRAPH_OUT_TEXTURE_ADDRESS + stage]) {
            any = true;
            /* T1488: the mixed wrap/clamp pairs 0x00000301 (U wrap, V clamp) and 0x00000103 (U clamp, V wrap) are the same sampler fields as the measured pairs (U bits 0..3, V bits 8..11, 1 wrap, 3 clamp to edge,
             * xemu pgraph), INFERRED until a retail frame is compared. */
            const bool mixed = address == 0x00000301u || address == 0x00000103u;
            /* T1490: a cube map stage (program mode 3) carries the P field as well (xemu ADDRP, bits 16..19) and a cube map ignores the wrap
             * modes (seamless), so any U, V and P modes xemu's table has (1..5) are the same sampler for it. INFERRED, live_texture_modes only. */
            const bool cube_stage = backend->live_texture_modes && state->combiner_written[54] && ((state->combiner[54] >> (5u * stage)) & 0x1Fu) == 3u &&
                                    (address & ~0x000F0F0Fu) == 0u && (address & 0xFu) >= 1u && (address & 0xFu) <= 5u &&
                                    ((address >> 8) & 0xFu) >= 1u && ((address >> 8) & 0xFu) <= 5u && ((address >> 16) & 0xFu) <= 5u;
            if (address != 0u && address != 0x00000101u && address != 0x00000303u && !mixed && !cube_stage) {
                return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                            "texture stage %u address 0x%08X (0x%04X) is none of the measured 0 (unbound), 0x00000101 "
                            "(U and V wrap) and 0x00000303 (U and V clamp to edge), or the inferred 0x00000301 and 0x00000103",
                            (unsigned)stage, (unsigned)address, (unsigned)(0x1B08u + 0x40u * stage));
            }
            if (mixed) {
                const gpu_pgraph_result inferred = need_inference(
                    backend, GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING,
                    "texture address 0x00000301/0x00000103 (one axis wrap, one clamp to edge) is INFERRED from xemu's register map",
                    &out->used_inferences, report);
                if (inferred != GPU_PGRAPH_OK) return inferred;
            }
        }
        const uint32_t control = state->output[GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + stage];
        if (state->output_written[GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + stage]) {
            any = true;
            if ((control & ~0x40000030u) != 0x0003FFC0u) {
                return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                            "texture stage %u control 0x%08X (0x%04X) differs from the measured 0x0003FFC0 beyond the enable "
                            "bit or anisotropy (alpha kill, colour key or a LOD clamp)",
                            (unsigned)stage, (unsigned)control, (unsigned)(0x1B0Cu + 0x40u * stage));
            }
        }
        if (state->output_written[GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + stage] && (control & 0x30u) != 0u) {
            const gpu_pgraph_result inferred = need_inference(
                backend, GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING,
                "texture Control0 anisotropy is INFERRED from xemu (1 << field; 2/4/8); alpha kill and colour key unsupported",
                &out->used_inferences, report);
            if (inferred != GPU_PGRAPH_OK) return inferred;
        }
        if (state->output_written[GPU_PGRAPH_OUT_TEXTURE_FILTER + stage]) {
            any = true;
            if (state->output[GPU_PGRAPH_OUT_TEXTURE_FILTER + stage] != 0x02022000u &&
                state->output[GPU_PGRAPH_OUT_TEXTURE_FILTER + stage] != 0x02062000u) {
                return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                            "texture stage %u filter 0x%08X (0x%04X) is neither measured 0x02062000 nor inferred base-level linear 0x02022000", (unsigned)stage,
                            (unsigned)state->output[GPU_PGRAPH_OUT_TEXTURE_FILTER + stage],
                            (unsigned)(0x1B14u + 0x40u * stage));
            }
            if (state->output[GPU_PGRAPH_OUT_TEXTURE_FILTER + stage] == 0x02022000u) {
                const gpu_pgraph_result inferred = need_inference(
                    backend, GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING,
                    "texture filter 0x02022000 is base-level linear minification/magnification (INFERRED from xemu; "
                    "no mip selection, bias, convolution or signed channels)", &out->used_inferences, report);
                if (inferred != GPU_PGRAPH_OK) return inferred;
            }
        }
    }
    for (uint32_t index = 0u; index < 18u; index++) {
        if (state->output_written[GPU_PGRAPH_OUT_TEXTURE_BUMP + index]) {
            any = true;
            if (state->output[GPU_PGRAPH_OUT_TEXTURE_BUMP + index] != 0u) {
                return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                            "the bump environment word 0x%04X of texture stage %u is 0x%08X, the replay decodes only 0",
                            (unsigned)(0x1B28u + 0x40u * (index / 6u + 1u) + 4u * (index % 6u)),
                            (unsigned)(index / 6u + 1u), (unsigned)state->output[GPU_PGRAPH_OUT_TEXTURE_BUMP + index]);
            }
        }
    }
    return any ? need_inference(backend, GPU_PGRAPH_INFER_OUTPUT_PINNED_STATE, pinned_state_note, &out->used_inferences, report)
               : GPU_PGRAPH_OK;
}

gpu_pgraph_result gpu_pgraph_resolve_output(const gpu_pgraph_state *state,
                                            const gpu_pgraph_backend *backend, uint32_t width,
                                            uint32_t height, gpu_pgraph_output *out,
                                            gpu_pgraph_report *report)
{
    memset(out, 0, sizeof *out);
    static const struct {
        uint32_t group;
        const char *name;
    } groups[] = {
        {GPU_PGRAPH_OUTPUT_SCISSOR, "scissor"},
        {GPU_PGRAPH_OUTPUT_CULL, "cull"},
        {GPU_PGRAPH_OUTPUT_BLEND, "blend"},
        {GPU_PGRAPH_OUTPUT_ALPHA_TEST, "alpha test"},
        {GPU_PGRAPH_OUTPUT_DEPTH_STENCIL, "depth and stencil"},
        {GPU_PGRAPH_OUTPUT_POLYGON_OFFSET, "polygon offset"},
        {GPU_PGRAPH_OUTPUT_IGNORED, "ignored (dither, specular parameters)"},
        {GPU_PGRAPH_OUTPUT_SURFACE, "surface (render target packet)"},
        {GPU_PGRAPH_OUTPUT_FIXED, "fixed-function"},
        {GPU_PGRAPH_OUTPUT_TEXTURE, "texture stage"},
    };
    for (size_t i = 0u; i < sizeof groups / sizeof groups[0]; i++) {
        if (output_group_written(state, groups[i].group) &&
            (backend->output_groups & groups[i].group) == 0u) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "the stream set %s state but the replay's output_groups does not enable it: "
                        "applying nothing would silently ignore state the title set",
                        groups[i].name);
        }
    }
    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_SCISSOR) != 0u) {
        const gpu_pgraph_result scissor = resolve_scissor(state, backend, width, height, out, report);
        if (scissor != GPU_PGRAPH_OK) {
            return scissor;
        }
    }
    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_CULL) != 0u) {
        const gpu_pgraph_result cull = resolve_cull(state, backend, out, report);
        if (cull != GPU_PGRAPH_OK) {
            return cull;
        }
    }
    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_BLEND) != 0u) {
        const gpu_pgraph_result blend = resolve_blend(state, backend, out, report);
        if (blend != GPU_PGRAPH_OK) {
            return blend;
        }
    }
    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_ALPHA_TEST) != 0u) {
        const gpu_pgraph_result alpha = resolve_alpha_test(state, backend, out, report);
        if (alpha != GPU_PGRAPH_OK) {
            return alpha;
        }
    }
    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_DEPTH_STENCIL) != 0u) {
        const gpu_pgraph_result depth = resolve_depth(state, backend, out, report);
        if (depth != GPU_PGRAPH_OK) {
            return depth;
        }
        const gpu_pgraph_result stencil = resolve_stencil(state, backend, out, report);
        if (stencil != GPU_PGRAPH_OK) {
            return stencil;
        }
    }
    /* T860: the polygon mode (the FIXED group's 0x038C and 0x0390) first, the polygon offset reads it. */
    uint32_t polygon_mode = GPU_VSH_POLYGON_FILL;
    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_FIXED) != 0u) {
        const gpu_pgraph_result mode = resolve_polygon_mode(state, backend, out, report, &polygon_mode);
        if (mode != GPU_PGRAPH_OK) {
            memset(out, 0, sizeof *out);
            return mode;
        }
    }
    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_POLYGON_OFFSET) != 0u) {
        const gpu_pgraph_result offset = resolve_polygon_offset(state, backend, out, report, polygon_mode);
        if (offset != GPU_PGRAPH_OK) {
            memset(out, 0, sizeof *out); /* a refusal leaves no half-resolved output behind */
            return offset;
        }
    }
    /* T462: words pinned to the value measured, they move no modelled pixel. */
    gpu_pgraph_result pinned = GPU_PGRAPH_OK;
    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_SURFACE) != 0u) {
        pinned = resolve_surface(state, backend, out, report);
    }
    if (pinned == GPU_PGRAPH_OK && (backend->output_groups & GPU_PGRAPH_OUTPUT_FIXED) != 0u) {
        pinned = resolve_fixed(state, backend, out, report);
    }
    if (pinned == GPU_PGRAPH_OK && (backend->output_groups & GPU_PGRAPH_OUTPUT_TEXTURE) != 0u) {
        pinned = resolve_texture(state, backend, out, report);
    }
    if (pinned != GPU_PGRAPH_OK) {
        memset(out, 0, sizeof *out);
    }
    return pinned;
}

/* --- clears (T267) ------------------------------------------------------------------------- */

#define CLEAR_FLAG_Z 0x01u
#define CLEAR_FLAG_STENCIL 0x02u
#define CLEAR_FLAG_COLOUR 0xF0u

gpu_pgraph_result gpu_pgraph_resolve_clear(const gpu_pgraph_clear *clear,
                                           const gpu_pgraph_backend *backend, uint32_t width,
                                           uint32_t height, gpu_pgraph_clear_resolved *out,
                                           gpu_pgraph_report *report)
{
    memset(out, 0, sizeof *out);
    const uint32_t flags = clear->flags;
    if ((flags & ~(CLEAR_FLAG_Z | CLEAR_FLAG_STENCIL | CLEAR_FLAG_COLOUR)) != 0u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "clear flags 0x%X have bits beyond depth (1), stencil (2) and the four colour channels "
                    "(0xF0)",
                    (unsigned)flags);
    }
    if (flags == 0u) {
        return GPU_PGRAPH_OK; /* nothing to clear */
    }
    if (!clear->rect_horizontal_written || !clear->rect_vertical_written) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "a clear with no %s written: the library writes both rectangle words with every clear",
                    !clear->rect_horizontal_written ? "horizontal rectangle (0x1D98)"
                                                    : "vertical rectangle (0x1D9C)");
    }
    const uint32_t x_min = clear->rect_horizontal & 0xFFFFu;
    const uint32_t x_max = clear->rect_horizontal >> 16;
    const uint32_t y_min = clear->rect_vertical & 0xFFFFu;
    const uint32_t y_max = clear->rect_vertical >> 16;
    if (x_min > x_max || y_min > y_max || x_max >= width || y_max >= height) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "clear rectangle x %u..%u, y %u..%u (inclusive) is empty or reaches past the %ux%u "
                    "target: clamping it is not measured",
                    (unsigned)x_min, (unsigned)x_max, (unsigned)y_min, (unsigned)y_max, (unsigned)width,
                    (unsigned)height);
    }
    if ((flags & CLEAR_FLAG_COLOUR) != 0u && !clear->color_written) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "a colour clear with no clear colour (0x1D90) written: its initial value is not measured");
    }
    if ((flags & (CLEAR_FLAG_Z | CLEAR_FLAG_STENCIL)) != 0u) {
        if (!clear->zstencil_written) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "a depth or stencil clear with no clear value (0x1D8C) written: its initial value is "
                        "not measured");
        }
        if ((backend->output_groups & GPU_PGRAPH_OUTPUT_DEPTH_STENCIL) == 0u) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "a depth or stencil clear needs the depth and stencil group in the backend's "
                        "output_groups: without it the replay holds no depth or stencil buffer to clear");
        }
    }
    const gpu_pgraph_result allowed = need_inference(
        backend, GPU_PGRAPH_INFER_OUTPUT_CLEAR_MODEL,
        "clearing an A8R8G8B8 target with the colour 0xAARRGGBB and a D24S8 depth and stencil by the flagged "
        "channels, whatever the write masks hold",
        &out->used_inferences, report);
    if (allowed != GPU_PGRAPH_OK) {
        return allowed;
    }
    out->x_min = x_min;
    out->x_max = x_max;
    /* flip_y reverses the rows of the finished frame, and the rectangle is the rows of that image */
    out->y_min = backend->flip_y ? height - 1u - y_max : y_min;
    out->y_max = backend->flip_y ? height - 1u - y_min : y_max;
    if ((flags & CLEAR_FLAG_COLOUR) != 0u) {
        out->colour = true;
        out->channels = ((flags & 0x10u) != 0u ? GPU_VSH_CHANNEL_R : 0u) |
                        ((flags & 0x20u) != 0u ? GPU_VSH_CHANNEL_G : 0u) |
                        ((flags & 0x40u) != 0u ? GPU_VSH_CHANNEL_B : 0u) |
                        ((flags & 0x80u) != 0u ? GPU_VSH_CHANNEL_A : 0u);
        out->rgba[0] = (uint8_t)(clear->color >> 16);
        out->rgba[1] = (uint8_t)(clear->color >> 8);
        out->rgba[2] = (uint8_t)clear->color;
        out->rgba[3] = (uint8_t)(clear->color >> 24);
    }
    if ((flags & CLEAR_FLAG_Z) != 0u) {
        out->depth = true;
        out->z = (float)(clear->zstencil >> 8) / 16777215.0f;
    }
    if ((flags & CLEAR_FLAG_STENCIL) != 0u) {
        out->stencil = true;
        out->stencil_value = (uint8_t)clear->zstencil;
    }
    return GPU_PGRAPH_OK;
}

static void apply_clear(const gpu_pgraph_clear_resolved *clear, gpu_image *frame, float *depth,
                        uint8_t *stencil)
{
    for (uint32_t y = clear->y_min; y <= clear->y_max; y++) {
        for (uint32_t x = clear->x_min; x <= clear->x_max; x++) {
            uint8_t *pixel = frame->pixels + gpu_image_offset(frame, x, y);
            if (clear->colour) {
                for (uint32_t lane = 0u; lane < 4u; lane++) {
                    if ((clear->channels & (1u << lane)) != 0u) {
                        pixel[lane] = clear->rgba[lane];
                    }
                }
            }
            if (clear->depth && depth != NULL) {
                depth[(size_t)y * frame->width + x] = clear->z;
            }
            if (clear->stencil && stencil != NULL) {
                stencil[(size_t)y * frame->width + x] = clear->stencil_value;
            }
        }
    }
}

/* The clears of the pass, from *next up to `end`, that precede draw `until` (all of them for SIZE_MAX): applied in order. */
static gpu_pgraph_result apply_clears_until(const gpu_pgraph *pgraph, const gpu_pgraph_backend *backend,
                                            size_t *next, size_t end, size_t until, gpu_image *frame,
                                            float *depth, uint8_t *stencil, gpu_pgraph_report *report)
{
    while (*next < end && gpu_pgraph_clear_at(pgraph, *next)->before_draw <= until) {
        const gpu_pgraph_clear *clear = gpu_pgraph_clear_at(pgraph, *next);
        (*next)++;
        if ((backend->output_groups & GPU_PGRAPH_OUTPUT_CLEAR) == 0u) {
            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                        "the stream cleared (0x1D94) but the replay's output_groups does not enable the clear "
                        "group: applying nothing would silently ignore state the title set");
        }
        gpu_pgraph_clear_resolved resolved;
        const gpu_pgraph_result result =
            gpu_pgraph_resolve_clear(clear, backend, frame->width, frame->height, &resolved, report);
        if (result != GPU_PGRAPH_OK) {
            return result;
        }
        report->used_inferences |= resolved.used_inferences;
        report->clears_applied++;
        apply_clear(&resolved, frame, depth, stencil);
    }
    return GPU_PGRAPH_OK;
}

/* --- T578, the BLIT group: an image blit over two surface images ------------------------------------------------------- */

static const char blit_model_note[] =
    "copying the rectangle of one replayed A8R8G8B8 surface image into another as the 2D engine's SRCCOPY image blit "
    "(xemu's byte copy), taking the replayed image for what the hardware surface holds at that moment";

gpu_pgraph_result gpu_pgraph_replay_copy(const gpu_pgraph_copy *copy, const gpu_pgraph_backend *backend,
                                         const gpu_image *source, gpu_image *destination,
                                         uint32_t *used_inferences, gpu_pgraph_report *report)
{
    if (copy == NULL || backend == NULL || used_inferences == NULL) {
        return GPU_PGRAPH_ERR_ARGUMENT;
    }
    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_BLIT) == 0u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "the stream blitted (2D engine, subchannels 2 and 3) but the replay's output_groups does not enable "
                    "the blit group: applying nothing would silently ignore state the title set");
    }
    if (copy->width == 0u || copy->height == 0u) {
        return GPU_PGRAPH_OK;
    }
    if ((backend->allowed_inferences & GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES) != 0u) {
        return gpu_pgraph_replay_copy_planner_rules(copy, backend, source, destination, used_inferences, report);
    }
    if (copy->color_format != GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "a blit of colour format 0x%X: the replay's surfaces are A8R8G8B8 images, so only format 0xA has a "
                    "pixel meaning (the Y8 and R5G6B5 blits of CopyRects' byte path are refused)",
                    (unsigned)copy->color_format);
    }
    if (source == NULL || destination == NULL || source->pixels == NULL || destination->pixels == NULL) {
        return GPU_PGRAPH_ERR_ARGUMENT;
    }
    if (copy->operation != GPU_PGRAPH_BLIT_OPERATION_SRCCOPY) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "a blit operation %u: only SRCCOPY (3) is measured",
                    (unsigned)copy->operation);
    }
    if (copy->source_pitch != source->width * 4u || copy->destination_pitch != destination->width * 4u ||
        source->stride_bytes != source->width * 4u || destination->stride_bytes != destination->width * 4u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "a blit with source pitch %u and destination pitch %u over images %ux%u and %ux%u: the replay's "
                    "images are tightly packed, so only a pitch of width * 4 addresses their rows",
                    (unsigned)copy->source_pitch, (unsigned)copy->destination_pitch, (unsigned)source->width,
                    (unsigned)source->height, (unsigned)destination->width, (unsigned)destination->height);
    }
    if ((uint64_t)copy->in_x + copy->width > source->width || (uint64_t)copy->in_y + copy->height > source->height ||
        (uint64_t)copy->out_x + copy->width > destination->width ||
        (uint64_t)copy->out_y + copy->height > destination->height) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "a blit of %ux%u from (%u, %u) of a %ux%u image to (%u, %u) of a %ux%u image reaches past an "
                    "image: clamping is not measured",
                    (unsigned)copy->width, (unsigned)copy->height, (unsigned)copy->in_x, (unsigned)copy->in_y,
                    (unsigned)source->width, (unsigned)source->height, (unsigned)copy->out_x, (unsigned)copy->out_y,
                    (unsigned)destination->width, (unsigned)destination->height);
    }
    if (source == destination && copy->in_x < copy->out_x + copy->width && copy->out_x < copy->in_x + copy->width &&
        copy->in_y < copy->out_y + copy->height && copy->out_y < copy->in_y + copy->height) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "a blit whose source and destination rectangles overlap inside one surface: the copy order of the "
                    "hardware is not measured");
    }
    const gpu_pgraph_result allowed =
        need_inference(backend, GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL, blit_model_note, used_inferences, report);
    if (allowed != GPU_PGRAPH_OK) {
        return allowed;
    }
    for (uint32_t row = 0u; row < copy->height; row++) {
        const uint32_t source_row = backend->flip_y ? source->height - 1u - (copy->in_y + row) : copy->in_y + row;
        const uint32_t destination_row =
            backend->flip_y ? destination->height - 1u - (copy->out_y + row) : copy->out_y + row;
        memmove(destination->pixels + (size_t)destination_row * destination->stride_bytes + (size_t)copy->out_x * 4u,
                source->pixels + (size_t)source_row * source->stride_bytes + (size_t)copy->in_x * 4u,
                (size_t)copy->width * 4u);
    }
    return GPU_PGRAPH_OK;
}

/* --- T596, the BLIT group's byte path over surfaces held as guest bytes ---------------------------------------------------- */

uint32_t gpu_pgraph_blit_bytes_per_pixel(uint32_t colour_format)
{
    switch (colour_format) {
    case GPU_PGRAPH_BLIT_FORMAT_Y8: return 1u;
    case GPU_PGRAPH_BLIT_FORMAT_R5G6B5: return 2u;
    case GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8:
    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF:
    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0: return 4u;
    default: return 0u;
    }
}

uint32_t gpu_pgraph_blit_row_pixels(const gpu_pgraph_copy *copy)
{
    const uint32_t bytes_per_pixel = gpu_pgraph_blit_bytes_per_pixel(copy->color_format);
    if (bytes_per_pixel == 0u) {
        return 0u;
    }
    uint32_t pixels = copy->width;
    if (copy->source_pitch / bytes_per_pixel < pixels) {
        pixels = copy->source_pitch / bytes_per_pixel;
    }
    if (copy->destination_pitch / bytes_per_pixel < pixels) {
        pixels = copy->destination_pitch / bytes_per_pixel;
    }
    return pixels;
}

size_t gpu_pgraph_blit_extent_bytes(uint32_t colour_format, uint32_t pitch, uint32_t x, uint32_t y, uint32_t row_pixels, uint32_t height)
{
    const uint32_t bytes_per_pixel = gpu_pgraph_blit_bytes_per_pixel(colour_format);
    if (bytes_per_pixel == 0u || pitch == 0u || row_pixels == 0u || height == 0u) {
        return 0u;
    }
    return (size_t)(y + height - 1u) * pitch + (size_t)x * bytes_per_pixel + (size_t)row_pixels * bytes_per_pixel;
}

gpu_pgraph_result gpu_pgraph_replay_byte_copy(const gpu_pgraph_copy *copy, const gpu_pgraph_backend *backend,
                                              const uint8_t *source, size_t source_length, uint8_t *destination,
                                              size_t destination_length, uint32_t *used_inferences,
                                              gpu_pgraph_report *report)
{
    static const char note[] =
        "copying the rectangle between two surfaces held as guest bytes as the 2D engine's SRCCOPY byte copy (xemu, T769)";
    if (copy == NULL || backend == NULL || used_inferences == NULL) {
        return GPU_PGRAPH_ERR_ARGUMENT;
    }
    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_BLIT) == 0u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "the stream blitted bytes (2D engine, subchannels 2 and 3) but the replay's output_groups does not enable "
                    "the blit group: applying nothing would silently ignore state the title set");
    }
    if (copy->height == 0u || copy->width == 0u) {
        return GPU_PGRAPH_OK;
    }
    const uint32_t bytes_per_pixel = gpu_pgraph_blit_bytes_per_pixel(copy->color_format);
    if (bytes_per_pixel == 0u) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "a byte blit of colour format 0x%X: only Y8 (1), R5G6B5 (4), A8R8G8B8 (0xA) and the X8R8G8B8 variants 7 and 6 are measured",
                    (unsigned)copy->color_format);
    }
    if (source == NULL || destination == NULL) {
        return GPU_PGRAPH_ERR_ARGUMENT;
    }
    if (GPU_PGRAPH_BLIT_OPERATION_SRCCOPY != copy->operation) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "a blit operation %u: only SRCCOPY (3) is measured", (unsigned)copy->operation);
    }
    /* T769, MEASURED in xemu: min(width, source_pitch / bpp, destination_pitch / bpp) PIXELS a row, every format and either pitch. */
    const uint32_t row_pixels = gpu_pgraph_blit_row_pixels(copy);
    if (row_pixels == 0u) {
        return GPU_PGRAPH_OK;
    }
    const size_t row_bytes = (size_t)row_pixels * bytes_per_pixel;
    const size_t source_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->source_pitch, copy->in_x, copy->in_y, row_pixels, copy->height);
    const size_t destination_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->destination_pitch, copy->out_x, copy->out_y, row_pixels, copy->height);
    if (source_need > source_length || destination_need > destination_length) {
        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,
                    "a byte blit of %ux%u reaches %zu source bytes and %zu destination bytes of surfaces holding %zu and %zu: a rectangle past the held bytes is not modelled",
                    (unsigned)copy->width, (unsigned)copy->height, source_need, destination_need, source_length, destination_length);
    }
    const gpu_pgraph_result allowed = need_inference(backend, GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL, note, used_inferences, report);
    if (allowed != GPU_PGRAPH_OK) {
        return allowed;
    }
    uint8_t *row = malloc(row_bytes);
    if (row == NULL) {
        return fail(report, GPU_PGRAPH_ERR_MEMORY, "out of memory for the %zu byte row buffer of a byte blit", row_bytes);
    }
    /* Rows ascend, each is read whole before it is written (flat addresses: a row that passes its pitch runs on into the next row's bytes). */
    for (uint32_t line = 0u; line < copy->height; line++) {
        uint8_t *to = destination + (size_t)(copy->out_y + line) * copy->destination_pitch + (size_t)copy->out_x * bytes_per_pixel;
        memcpy(row, source + (size_t)(copy->in_y + line) * copy->source_pitch + (size_t)copy->in_x * bytes_per_pixel, row_bytes);
        memcpy(to, row, row_bytes);
        if (copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF || copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0) {
            for (uint32_t pixel = 0u; pixel < row_pixels; pixel++) {
                to[(size_t)pixel * 4u + 3u] = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ? 0xFFu : 0x00u;
            }
        }
    }
    free(row);
    return GPU_PGRAPH_OK;
}
