/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See gpu_pgraph.h for what is decoded, from which emitter, and what is refused.
 */

#include "gpu_pgraph.h"

#include "gpu_pgraph_replay.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t method;
    uint64_t pairs;
} unhandled_entry;

struct gpu_pgraph {
    gpu_pgraph_state state;
    gpu_pgraph_array arrays[GPU_PGRAPH_ATTRIBUTES];
    bool strict;
    bool combiner;
    bool visibility;
    gpu_pgraph_report_identity_fn report_identity;
    void *report_identity_context;
    gpu_pgraph_query queries[GPU_PGRAPH_MAX_QUERY_EVENTS];
    size_t query_count;
    uint32_t output_groups; /* T267, GPU_PGRAPH_OUTPUT_* the model decodes */
    bool snapshot_dirty;
    bool in_bracket;
    uint32_t bracket_primitive;
    uint32_t bracket_command;
    uint32_t bracket_first_index;
    uint32_t program_cursor;  /* dwords into the program file */
    uint32_t constant_cursor; /* dwords into the constant file */
    uint32_t fence_tail;      /* T391: commands of a fence packet still expected after its software method */
    /* T462, the IMMEDIATE group: the latched SET_VERTEX_DATA2F_M values (they persist across brackets and frames like the
     * hardware's current attributes), and the vertices of the open bracket, 16 slots of two dwords per vertex. */
    uint32_t inline_value[GPU_PGRAPH_ATTRIBUTES][2];
    bool inline_written[GPU_PGRAPH_ATTRIBUTES];
    bool inline_x_pending[GPU_PGRAPH_ATTRIBUTES];
    bool bracket_inline;
    uint32_t bracket_inline_mask;
    uint32_t *inline_buffer;
    size_t inline_count;
    size_t inline_capacity;
    uint8_t *inline_pool;
    size_t inline_pool_held;
    size_t inline_pool_capacity;

    gpu_pgraph_clear clears[GPU_PGRAPH_MAX_CLEARS]; /* T267 */
    size_t clear_count;
    /* T578, the BLIT group: the 2D engine's words as written (they persist across frames like the hardware's registers) and
     * the blits of the frame. */
    struct {
        uint32_t source_offset;
        uint32_t destination_offset;
        uint32_t color_format;
        uint32_t pitch;
        uint32_t point_in;
        uint32_t point_out;
        bool source_written;
        bool destination_written;
        bool format_written;
        bool pitch_written;
        bool in_written;
        bool out_written;
    } blit;
    gpu_pgraph_copy copies[GPU_PGRAPH_MAX_COPIES];
    size_t copy_count;
    gpu_pgraph_draw *draws;
    size_t draw_count;
    size_t draw_capacity;
    uint32_t *indices;
    size_t index_count;
    size_t index_capacity;
    gpu_pgraph_state *snapshots;
    size_t snapshot_count;
    size_t snapshot_capacity;

    /* T262: the vertex snapshot pool of the frame, see gpu_pgraph_set_vertex_capture. */
    gpu_pgraph_read_fn capture_read;
    void *capture_context;
    size_t capture_budget;
    uint8_t *pool;
    size_t pool_held;
    size_t pool_capacity;

    gpu_pgraph_stats stats;
    unhandled_entry unhandled[GPU_PGRAPH_UNHANDLED_TABLE];
    size_t unhandled_used;
    uint64_t unhandled_overflow;
    char error[256];
};

const char *gpu_pgraph_result_string(gpu_pgraph_result result)
{
    switch (result) {
    case GPU_PGRAPH_OK: return "ok";
    case GPU_PGRAPH_ERR_ARGUMENT: return "bad argument";
    case GPU_PGRAPH_ERR_UNMEASURED: return "refused: not measured";
    case GPU_PGRAPH_ERR_MALFORMED: return "refused: malformed stream";
    case GPU_PGRAPH_ERR_FULL: return "a bound was reached";
    case GPU_PGRAPH_ERR_MEMORY: return "out of memory";
    case GPU_PGRAPH_ERR_DEVICE: return "device failure";
    }
    return "unknown";
}

gpu_pgraph_format gpu_pgraph_decode_format(uint32_t format_word)
{
    gpu_pgraph_format format;
    format.type = format_word & 0xFu;
    format.size = (format_word >> 4) & 0xFu;
    format.stride = format_word >> 8;
    return format;
}

gpu_pgraph *gpu_pgraph_create(void)
{
    gpu_pgraph *pgraph = calloc(1u, sizeof *pgraph);
    return pgraph;
}

void gpu_pgraph_destroy(gpu_pgraph *pgraph)
{
    if (pgraph == NULL) {
        return;
    }
    free(pgraph->draws);
    free(pgraph->indices);
    free(pgraph->snapshots);
    free(pgraph->pool);
    free(pgraph->inline_buffer);
    free(pgraph->inline_pool);
    free(pgraph);
}

void gpu_pgraph_reset(gpu_pgraph *pgraph)
{
    if (pgraph == NULL) {
        return;
    }
    memset(&pgraph->blit, 0, sizeof pgraph->blit);
    pgraph->copy_count = 0u;
    pgraph->fence_tail = 0u;
    memset(pgraph->inline_value, 0, sizeof pgraph->inline_value);
    memset(pgraph->inline_written, 0, sizeof pgraph->inline_written);
    memset(pgraph->inline_x_pending, 0, sizeof pgraph->inline_x_pending);
    pgraph->bracket_inline = false;
    pgraph->bracket_inline_mask = 0u;
    pgraph->inline_count = 0u;
    pgraph->inline_pool_held = 0u;
    memset(&pgraph->state, 0, sizeof pgraph->state);
    pgraph->state.combiner_captured = pgraph->combiner;
    pgraph->state.output_groups = pgraph->output_groups;
    memset(pgraph->arrays, 0, sizeof pgraph->arrays);
    pgraph->snapshot_dirty = false;
    pgraph->in_bracket = false;
    pgraph->bracket_primitive = 0u;
    pgraph->bracket_command = 0u;
    pgraph->bracket_first_index = 0u;
    pgraph->program_cursor = 0u;
    pgraph->constant_cursor = 0u;
    pgraph->query_count = 0u;
    pgraph->clear_count = 0u;
    pgraph->draw_count = 0u;
    pgraph->index_count = 0u;
    pgraph->snapshot_count = 0u;
    pgraph->pool_held = 0u;
    memset(&pgraph->stats, 0, sizeof pgraph->stats);
    memset(pgraph->unhandled, 0, sizeof pgraph->unhandled);
    pgraph->unhandled_used = 0u;
    pgraph->unhandled_overflow = 0u;
    pgraph->error[0] = '\0';
}

/* T1246: a self-contained copy of the frame the decoder holds, so the live renderer can draw it on another thread while the
 * decoder starts the next frame. The whole struct is copied (state, arrays, the fixed event lists), then the growable buffers
 * are replaced by `dst`'s own (kept across clones, grown on demand) holding the used bytes only. */
static bool clone_buffer(void **buffer, size_t *capacity, size_t need_bytes, size_t unit)
{
    if (need_bytes > *capacity * unit) {
        void *grown = realloc(*buffer, need_bytes);
        if (grown == NULL) {
            return false;
        }
        *buffer = grown;
        *capacity = need_bytes / unit;
    }
    return true;
}

bool gpu_pgraph_clone_frame(gpu_pgraph *dst, const gpu_pgraph *src)
{
    if (dst == NULL || src == NULL || dst == src) {
        return false;
    }
    const gpu_pgraph own = *dst; /* the buffers dst owns, restored after the struct copy */
    void *draws = own.draws, *indices = own.indices, *snapshots = own.snapshots, *pool = own.pool, *inline_pool = own.inline_pool,
         *inline_buffer = own.inline_buffer;
    size_t draw_capacity = own.draw_capacity, index_capacity = own.index_capacity, snapshot_capacity = own.snapshot_capacity,
           pool_capacity = own.pool_capacity, inline_pool_capacity = own.inline_pool_capacity, inline_capacity = own.inline_capacity;
    const size_t inline_held = src->inline_pool_held < src->inline_pool_capacity ? src->inline_pool_held : src->inline_pool_capacity;
    const size_t pool_held = src->pool_held < src->pool_capacity ? src->pool_held : src->pool_capacity;
    bool ok = clone_buffer(&draws, &draw_capacity, src->draw_count * sizeof *src->draws, sizeof *src->draws) &&
              clone_buffer(&indices, &index_capacity, src->index_count * sizeof *src->indices, sizeof *src->indices) &&
              clone_buffer(&snapshots, &snapshot_capacity, src->snapshot_count * sizeof *src->snapshots, sizeof *src->snapshots) &&
              clone_buffer(&pool, &pool_capacity, pool_held, 1u) &&
              clone_buffer(&inline_pool, &inline_pool_capacity, inline_held, 1u) &&
              clone_buffer(&inline_buffer, &inline_capacity, src->inline_count * (GPU_PGRAPH_ATTRIBUTES * 2u) * sizeof(uint32_t),
                           (GPU_PGRAPH_ATTRIBUTES * 2u) * sizeof(uint32_t));
    /* a failed grow keeps whatever realloc left valid in the locals, so store them back either way */
    *dst = *src;
    dst->draws = draws;
    dst->indices = indices;
    dst->snapshots = snapshots;
    dst->pool = pool;
    dst->inline_pool = inline_pool;
    dst->inline_buffer = inline_buffer;
    dst->draw_capacity = draw_capacity;
    dst->index_capacity = index_capacity;
    dst->snapshot_capacity = snapshot_capacity;
    dst->pool_capacity = pool_capacity;
    dst->inline_pool_capacity = inline_pool_capacity;
    dst->inline_capacity = inline_capacity;
    if (!ok) {
        dst->draw_count = dst->index_count = dst->snapshot_count = dst->pool_held = dst->inline_pool_held = dst->inline_count = 0u;
        return false;
    }
    if (src->draw_count != 0u) memcpy(dst->draws, src->draws, src->draw_count * sizeof *src->draws);
    if (src->index_count != 0u) memcpy(dst->indices, src->indices, src->index_count * sizeof *src->indices);
    if (src->snapshot_count != 0u) memcpy(dst->snapshots, src->snapshots, src->snapshot_count * sizeof *src->snapshots);
    if (pool_held != 0u) memcpy(dst->pool, src->pool, pool_held);
    if (inline_held != 0u) memcpy(dst->inline_pool, src->inline_pool, inline_held);
    if (src->inline_count != 0u) memcpy(dst->inline_buffer, src->inline_buffer, src->inline_count * (GPU_PGRAPH_ATTRIBUTES * 2u) * sizeof(uint32_t));
    dst->pool_held = pool_held;
    dst->inline_pool_held = inline_held;
    return true;
}

/* T1246: true when drawing the frame reads nothing from guest memory through the model: every draw's vertex bytes are in the
 * model's pools (`vertices_captured`), so a clone can be drawn after the guest changed that memory. */
bool gpu_pgraph_frame_self_contained(const gpu_pgraph *pgraph)
{
    if (pgraph == NULL) {
        return false;
    }
    for (size_t index = 0u; index < pgraph->draw_count; index++) {
        if (!pgraph->draws[index].vertices_captured) {
            return false;
        }
    }
    return true;
}

gpu_pgraph_result gpu_pgraph_begin_frame(gpu_pgraph *pgraph)
{
    if (pgraph == NULL) {
        return GPU_PGRAPH_ERR_ARGUMENT;
    }
    pgraph->copy_count = 0u; /* T578, before the bracket check: a refused begin_frame leaves a model that is reset anyway */
    if (pgraph->in_bracket) {
        return GPU_PGRAPH_ERR_MALFORMED;
    }
    pgraph->query_count = 0u;
    pgraph->clear_count = 0u;
    pgraph->draw_count = 0u;
    pgraph->index_count = 0u;
    pgraph->snapshot_count = 0u;
    pgraph->snapshot_dirty = false;
    pgraph->pool_held = 0u;
    return GPU_PGRAPH_OK;
}

uint32_t gpu_pgraph_element_bytes(uint32_t type, uint32_t size)
{
    if (type == GPU_PGRAPH_TYPE_F && size >= 1u && size <= 4u) {
        return size * 4u;
    }
    if (type == GPU_PGRAPH_TYPE_UB_D3D && size == 4u) {
        return 4u;
    }
    if (type == GPU_PGRAPH_TYPE_S32K && size == 2u) {
        return 4u; /* the one S32K shape the title's builder emits (v0, T84d) */
    }
    if (type == GPU_PGRAPH_TYPE_CMP && size == 1u) {
        return 4u; /* one packed 11-11-10 word (HQ28, T1204) */
    }
    return 0u;
}

void gpu_pgraph_set_vertex_capture(gpu_pgraph *pgraph, gpu_pgraph_read_fn read, void *context,
                                   size_t budget_bytes)
{
    if (pgraph == NULL) {
        return;
    }
    pgraph->capture_read = budget_bytes != 0u ? read : NULL;
    pgraph->capture_context = context;
    pgraph->capture_budget = read != NULL ? budget_bytes : 0u;
}

bool gpu_pgraph_vertex_capture_enabled(const gpu_pgraph *pgraph)
{
    return pgraph != NULL && pgraph->capture_read != NULL;
}

size_t gpu_pgraph_vertex_bytes_held(const gpu_pgraph *pgraph)
{
    return pgraph != NULL ? pgraph->pool_held : 0u;
}

bool gpu_pgraph_in_bracket(const gpu_pgraph *pgraph)
{
    return pgraph != NULL && pgraph->in_bracket;
}

const uint8_t *gpu_pgraph_draw_vertex_bytes(const gpu_pgraph *pgraph, size_t draw_index,
                                            uint32_t slot, uint32_t *address, uint32_t *length)
{
    if (pgraph == NULL || draw_index >= pgraph->draw_count || slot >= GPU_PGRAPH_ATTRIBUTES) {
        return NULL;
    }
    const gpu_pgraph_draw *draw = &pgraph->draws[draw_index];
    if (!draw->vertices_captured || !draw->vertices[slot].captured) {
        return NULL;
    }
    if (address != NULL) {
        *address = draw->vertices[slot].address;
    }
    if (length != NULL) {
        *length = draw->vertices[slot].bytes;
    }
    return (draw->inline_vertices ? pgraph->inline_pool : pgraph->pool) + draw->vertices[slot].offset;
}

void gpu_pgraph_set_strict(gpu_pgraph *pgraph, bool strict)
{
    if (pgraph != NULL) {
        pgraph->strict = strict;
    }
}

void gpu_pgraph_set_combiner(gpu_pgraph *pgraph, bool enabled)
{
    if (pgraph != NULL) {
        pgraph->combiner = enabled;
        pgraph->state.combiner_captured = enabled;
    }
}

void gpu_pgraph_set_output_groups(gpu_pgraph *pgraph, uint32_t groups)
{
    if (pgraph != NULL) {
        pgraph->output_groups = groups & GPU_PGRAPH_OUTPUT_ALL_MEASURED;
        pgraph->state.output_groups = pgraph->output_groups;
    }
}

/* The method of each output-state word and the group it belongs to, in gpu_pgraph_output_word
 * order. ONE table: the decode, the in-bracket refusal and gpu_pgraph_output_index all read it. */
static const struct {
    uint32_t method;
    uint32_t group;
} output_words[GPU_PGRAPH_OUT_COUNT] = {
    [GPU_PGRAPH_OUT_SURFACE_CLIP_HORIZONTAL] = {0x0200u, GPU_PGRAPH_OUTPUT_SCISSOR},
    [GPU_PGRAPH_OUT_SURFACE_CLIP_VERTICAL] = {0x0204u, GPU_PGRAPH_OUTPUT_SCISSOR},
    [GPU_PGRAPH_OUT_WINDOW_CLIP_TYPE] = {0x02B4u, GPU_PGRAPH_OUTPUT_SCISSOR},
    [GPU_PGRAPH_OUT_WINDOW_CLIP_HORIZONTAL] = {0x02C0u, GPU_PGRAPH_OUTPUT_SCISSOR},
    [GPU_PGRAPH_OUT_WINDOW_CLIP_VERTICAL] = {0x02E0u, GPU_PGRAPH_OUTPUT_SCISSOR},
    [GPU_PGRAPH_OUT_CULL_ENABLE] = {0x0308u, GPU_PGRAPH_OUTPUT_CULL},
    [GPU_PGRAPH_OUT_CULL_FACE] = {0x039Cu, GPU_PGRAPH_OUTPUT_CULL},
    [GPU_PGRAPH_OUT_FRONT_FACE] = {0x03A0u, GPU_PGRAPH_OUTPUT_CULL},
    [GPU_PGRAPH_OUT_BLEND_ENABLE] = {0x0304u, GPU_PGRAPH_OUTPUT_BLEND},
    [GPU_PGRAPH_OUT_BLEND_SFACTOR] = {0x0344u, GPU_PGRAPH_OUTPUT_BLEND},
    [GPU_PGRAPH_OUT_BLEND_DFACTOR] = {0x0348u, GPU_PGRAPH_OUTPUT_BLEND},
    [GPU_PGRAPH_OUT_BLEND_COLOR] = {0x034Cu, GPU_PGRAPH_OUTPUT_BLEND},
    [GPU_PGRAPH_OUT_BLEND_EQUATION] = {0x0350u, GPU_PGRAPH_OUTPUT_BLEND},
    [GPU_PGRAPH_OUT_COLOR_MASK] = {0x0358u, GPU_PGRAPH_OUTPUT_BLEND},
    [GPU_PGRAPH_OUT_ALPHA_TEST_ENABLE] = {0x0300u, GPU_PGRAPH_OUTPUT_ALPHA_TEST},
    [GPU_PGRAPH_OUT_ALPHA_FUNC] = {0x033Cu, GPU_PGRAPH_OUTPUT_ALPHA_TEST},
    [GPU_PGRAPH_OUT_ALPHA_REF] = {0x0340u, GPU_PGRAPH_OUTPUT_ALPHA_TEST},
    [GPU_PGRAPH_OUT_DEPTH_ENABLE] = {0x030Cu, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
    [GPU_PGRAPH_OUT_DEPTH_FUNC] = {0x0354u, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
    [GPU_PGRAPH_OUT_DEPTH_MASK] = {0x035Cu, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
    [GPU_PGRAPH_OUT_STENCIL_ENABLE] = {0x032Cu, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
    [GPU_PGRAPH_OUT_STENCIL_MASK] = {0x0360u, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
    [GPU_PGRAPH_OUT_STENCIL_FUNC] = {0x0364u, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
    [GPU_PGRAPH_OUT_STENCIL_REF] = {0x0368u, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
    [GPU_PGRAPH_OUT_STENCIL_FUNC_MASK] = {0x036Cu, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
    [GPU_PGRAPH_OUT_STENCIL_OP_FAIL] = {0x0370u, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
    [GPU_PGRAPH_OUT_STENCIL_OP_ZFAIL] = {0x0374u, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
    [GPU_PGRAPH_OUT_STENCIL_OP_ZPASS] = {0x0378u, GPU_PGRAPH_OUTPUT_DEPTH_STENCIL},
    [GPU_PGRAPH_OUT_CLEAR_ZSTENCIL] = {0x1D8Cu, GPU_PGRAPH_OUTPUT_CLEAR},
    [GPU_PGRAPH_OUT_CLEAR_COLOR] = {0x1D90u, GPU_PGRAPH_OUTPUT_CLEAR},
    [GPU_PGRAPH_OUT_CLEAR_RECT_HORIZONTAL] = {0x1D98u, GPU_PGRAPH_OUTPUT_CLEAR},
    [GPU_PGRAPH_OUT_CLEAR_RECT_VERTICAL] = {0x1D9Cu, GPU_PGRAPH_OUTPUT_CLEAR},
    [GPU_PGRAPH_OUT_POLY_OFFSET_POINT] = {0x0330u, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET},
    [GPU_PGRAPH_OUT_POLY_OFFSET_LINE] = {0x0334u, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET},
    [GPU_PGRAPH_OUT_POLY_OFFSET_FILL] = {0x0338u, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET},
    [GPU_PGRAPH_OUT_POLY_OFFSET_SCALE] = {0x0384u, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET},
    [GPU_PGRAPH_OUT_POLY_OFFSET_BIAS] = {0x0388u, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET},
    [GPU_PGRAPH_OUT_DITHER_ENABLE] = {0x0310u, GPU_PGRAPH_OUTPUT_IGNORED},
    [GPU_PGRAPH_OUT_SPECULAR_PARAMS] = {0x09F8u, GPU_PGRAPH_OUTPUT_IGNORED},
    [GPU_PGRAPH_OUT_SURFACE_FORMAT] = {0x0208u, GPU_PGRAPH_OUTPUT_SURFACE},
    [GPU_PGRAPH_OUT_SURFACE_PITCH] = {0x020Cu, GPU_PGRAPH_OUTPUT_SURFACE},
    [GPU_PGRAPH_OUT_SURFACE_COLOR_OFFSET] = {0x0210u, GPU_PGRAPH_OUTPUT_SURFACE},
    [GPU_PGRAPH_OUT_SURFACE_ZETA_OFFSET] = {0x0214u, GPU_PGRAPH_OUTPUT_SURFACE},
    [GPU_PGRAPH_OUT_CONTROL0] = {0x0290u, GPU_PGRAPH_OUTPUT_SURFACE},
    [GPU_PGRAPH_OUT_CLIP_MIN] = {0x0394u, GPU_PGRAPH_OUTPUT_SURFACE},
    [GPU_PGRAPH_OUT_CLIP_MAX] = {0x0398u, GPU_PGRAPH_OUTPUT_SURFACE},
    [GPU_PGRAPH_OUT_ANTI_ALIASING] = {0x1D7Cu, GPU_PGRAPH_OUTPUT_SURFACE},
    [GPU_PGRAPH_OUT_LIGHTING_ENABLE] = {0x0314u, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_LIGHT_CONTROL] = {0x0294u, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_LIGHT_ENABLE_MASK] = {0x03BCu, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_SPECULAR_ENABLE] = {0x03B8u, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_FOG_ENABLE] = {0x02A4u, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_FOG_COLOR] = {0x02A8u, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_FOG_MODE] = {0x029Cu, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_FOG_GEN_MODE] = {0x02A0u, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_FOG_PARAM0] = {0x09C0u, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_FOG_PARAM1] = {0x09C4u, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_FOG_PARAM2] = {0x09C8u, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_POINT_PARAMS_ENABLE] = {0x0318u, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_POINT_SMOOTH_ENABLE] = {0x031Cu, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_POINT_SIZE] = {0x043Cu, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_FRONT_POLYGON_MODE] = {0x038Cu, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_BACK_POLYGON_MODE] = {0x0390u, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_ZCULL_ENABLE] = {0x1D84u, GPU_PGRAPH_OUTPUT_FIXED},
    [GPU_PGRAPH_OUT_TEXTURE_ADDRESS + 0] = {0x1B08u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + 0] = {0x1B0Cu, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_FILTER + 0] = {0x1B14u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_ADDRESS + 1] = {0x1B48u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + 1] = {0x1B4Cu, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_FILTER + 1] = {0x1B54u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_ADDRESS + 2] = {0x1B88u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + 2] = {0x1B8Cu, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_FILTER + 2] = {0x1B94u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_ADDRESS + 3] = {0x1BC8u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_CONTROL0 + 3] = {0x1BCCu, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_FILTER + 3] = {0x1BD4u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 0] = {0x1B68u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 1] = {0x1B6Cu, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 2] = {0x1B70u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 3] = {0x1B74u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 4] = {0x1B78u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 5] = {0x1B7Cu, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 6] = {0x1BA8u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 7] = {0x1BACu, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 8] = {0x1BB0u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 9] = {0x1BB4u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 10] = {0x1BB8u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 11] = {0x1BBCu, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 12] = {0x1BE8u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 13] = {0x1BECu, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 14] = {0x1BF0u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 15] = {0x1BF4u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 16] = {0x1BF8u, GPU_PGRAPH_OUTPUT_TEXTURE},
    [GPU_PGRAPH_OUT_TEXTURE_BUMP + 17] = {0x1BFCu, GPU_PGRAPH_OUTPUT_TEXTURE},
};

/* T1289: every method the decoder sees asked this question and the answer was a linear scan of the 150 word table (6.5 percent of the guest
 * thread CPU, MEASURED with a -g build). A direct table over method / 4 gives the same answer: the lowest word whose method matches. */
#define OUTPUT_LOOKUP_METHODS 0x2000u
static int16_t output_lookup[OUTPUT_LOOKUP_METHODS / 4u];
static _Atomic bool output_lookup_ready;

static void output_lookup_build(void)
{
    for (size_t slot = 0u; slot < OUTPUT_LOOKUP_METHODS / 4u; slot++) {
        output_lookup[slot] = -1;
    }
    for (int word = (int)GPU_PGRAPH_OUT_COUNT - 1; word >= 0; word--) { /* backwards, so the lowest word of a repeated method wins */
        const uint32_t method = output_words[word].method;
        if ((method & 3u) == 0u && method < OUTPUT_LOOKUP_METHODS) {
            output_lookup[method / 4u] = (int16_t)word;
        }
    }
    atomic_store_explicit(&output_lookup_ready, true, memory_order_release);
}

int gpu_pgraph_output_index(uint32_t method, uint32_t *group)
{
    if ((method & 3u) == 0u && method < OUTPUT_LOOKUP_METHODS) {
        if (!atomic_load_explicit(&output_lookup_ready, memory_order_acquire)) {
            output_lookup_build(); /* idempotent: racing callers write the same values */
        }
        const int found = output_lookup[method / 4u];
        if (found >= 0 && group != NULL) {
            *group = output_words[found].group;
        }
        return found; /* a table word that is not aligned or not below the range cannot equal this method */
    }
    for (int word = 0; word < (int)GPU_PGRAPH_OUT_COUNT; word++) {
        if (output_words[word].method == method) {
            if (group != NULL) {
                *group = output_words[word].group;
            }
            return word;
        }
    }
    return -1;
}

uint32_t gpu_pgraph_output_group(uint32_t word)
{
    return word < GPU_PGRAPH_OUT_COUNT ? output_words[word].group : 0u;
}

int gpu_pgraph_combiner_index(uint32_t method)
{
    if ((method & 3u) != 0u) {
        return -1;
    }
    if (method >= 0x0260u && method <= 0x027Cu) {
        return (int)((method - 0x0260u) / 4u);
    }
    if (method == 0x0288u || method == 0x028Cu) {
        return 8 + (int)((method - 0x0288u) / 4u);
    }
    if (method >= 0x0A60u && method <= 0x0ADCu) {
        return 10 + (int)((method - 0x0A60u) / 4u);
    }
    if (method == 0x17F8u) {
        return 42;
    }
    if (method == 0x1E20u || method == 0x1E24u) {
        return 43 + (int)((method - 0x1E20u) / 4u);
    }
    if (method >= 0x1E40u && method <= 0x1E60u) {
        return 45 + (int)((method - 0x1E40u) / 4u);
    }
    if (method == 0x1E70u) {
        return 54;
    }
    if (method == 0x1E74u || method == 0x1E78u) {
        return 55 + (int)((method - 0x1E74u) / 4u);
    }
    return -1;
}

const char *gpu_pgraph_error(const gpu_pgraph *pgraph)
{
    return pgraph != NULL ? pgraph->error : "";
}

size_t gpu_pgraph_draw_count(const gpu_pgraph *pgraph)
{
    return pgraph != NULL ? pgraph->draw_count : 0u;
}

void gpu_pgraph_set_visibility(gpu_pgraph *pgraph, bool enabled)
{
    if (pgraph != NULL) pgraph->visibility = enabled;
}

void gpu_pgraph_set_report_identity(gpu_pgraph *pgraph, gpu_pgraph_report_identity_fn capture, void *context)
{
    if (pgraph != NULL) { pgraph->report_identity = capture; pgraph->report_identity_context = context; }
}

size_t gpu_pgraph_query_count(const gpu_pgraph *pgraph)
{
    return pgraph != NULL ? pgraph->query_count : 0u;
}

const gpu_pgraph_query *gpu_pgraph_query_at(const gpu_pgraph *pgraph, size_t index)
{
    return pgraph != NULL && index < pgraph->query_count ? &pgraph->queries[index] : NULL;
}

size_t gpu_pgraph_clear_count(const gpu_pgraph *pgraph)
{
    return pgraph != NULL ? pgraph->clear_count : 0u;
}

const gpu_pgraph_clear *gpu_pgraph_clear_at(const gpu_pgraph *pgraph, size_t index)
{
    return pgraph != NULL && index < pgraph->clear_count ? &pgraph->clears[index] : NULL;
}

size_t gpu_pgraph_copy_count(const gpu_pgraph *pgraph)
{
    return pgraph != NULL ? pgraph->copy_count : 0u;
}

const gpu_pgraph_copy *gpu_pgraph_copy_at(const gpu_pgraph *pgraph, size_t index)
{
    return pgraph != NULL && index < pgraph->copy_count ? &pgraph->copies[index] : NULL;
}

const gpu_pgraph_draw *gpu_pgraph_draw_at(const gpu_pgraph *pgraph, size_t index)
{
    return pgraph != NULL && index < pgraph->draw_count ? &pgraph->draws[index] : NULL;
}

const uint32_t *gpu_pgraph_indices(const gpu_pgraph *pgraph)
{
    return pgraph != NULL ? pgraph->indices : NULL;
}

size_t gpu_pgraph_snapshot_count(const gpu_pgraph *pgraph)
{
    return pgraph != NULL ? pgraph->snapshot_count : 0u;
}

const gpu_pgraph_state *gpu_pgraph_snapshot(const gpu_pgraph *pgraph, size_t index)
{
    return pgraph != NULL && index < pgraph->snapshot_count ? &pgraph->snapshots[index] : NULL;
}

const gpu_pgraph_state *gpu_pgraph_state_now(const gpu_pgraph *pgraph)
{
    return pgraph != NULL ? &pgraph->state : NULL;
}

gpu_pgraph_stats gpu_pgraph_get_stats(const gpu_pgraph *pgraph)
{
    gpu_pgraph_stats none;
    memset(&none, 0, sizeof none);
    return pgraph != NULL ? pgraph->stats : none;
}

size_t gpu_pgraph_unhandled_count(const gpu_pgraph *pgraph)
{
    return pgraph != NULL ? pgraph->unhandled_used : 0u;
}

void gpu_pgraph_unhandled_at(const gpu_pgraph *pgraph, size_t index, uint32_t *method,
                             uint64_t *pairs)
{
    if (pgraph == NULL || index >= pgraph->unhandled_used) {
        *method = 0u;
        *pairs = 0u;
        return;
    }
    *method = pgraph->unhandled[index].method;
    *pairs = pgraph->unhandled[index].pairs;
}

uint64_t gpu_pgraph_unhandled_overflow(const gpu_pgraph *pgraph)
{
    return pgraph != NULL ? pgraph->unhandled_overflow : 0u;
}

static gpu_pgraph_result refuse(gpu_pgraph *pgraph, gpu_pgraph_result result, uint64_t index,
                                uint32_t method, const char *message)
{
    snprintf(pgraph->error, sizeof pgraph->error, "pair %llu method 0x%04X: %s",
             (unsigned long long)index, (unsigned)method, message);
    return result;
}

/* T1176: retail software method9 copies ZSTENCIL_CLEAR_VALUE (address)
 * and COLOR_CLEAR_VALUE (payload) into MMIO. Project the two measured control
 * registers into equivalent method state; do not discard the write. */
static void software_output(gpu_pgraph *pgraph, gpu_pgraph_output_word word, uint32_t value)
{
    pgraph->state.output[word] = value;
    pgraph->state.output_written[word] = true;
}

static gpu_pgraph_result software_control(gpu_pgraph *pgraph, uint64_t index)
{
    const gpu_pgraph_state *state = &pgraph->state;
    if (!state->output_written[GPU_PGRAPH_OUT_CLEAR_ZSTENCIL] ||
        !state->output_written[GPU_PGRAPH_OUT_CLEAR_COLOR]) {
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, GPU_PGRAPH_NO_OPERATION,
                      "software method9 requires the original address/payload mailbox");
    }
    const uint32_t address = state->output[GPU_PGRAPH_OUT_CLEAR_ZSTENCIL];
    const uint32_t value = state->output[GPU_PGRAPH_OUT_CLEAR_COLOR];
    /* Preserve the other method bits only from established state. The software
     * handler does not initialise the other control register. */
    if (!state->output_written[GPU_PGRAPH_OUT_CONTROL0]) {
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, GPU_PGRAPH_NO_OPERATION,
                      "software method9 requires established CONTROL0 state");
    }
    const uint32_t required = GPU_PGRAPH_OUTPUT_SURFACE | GPU_PGRAPH_OUTPUT_CLEAR |
        GPU_PGRAPH_OUTPUT_ALPHA_TEST | GPU_PGRAPH_OUTPUT_DEPTH_STENCIL |
        GPU_PGRAPH_OUTPUT_CULL | GPU_PGRAPH_OUTPUT_FIXED |
        GPU_PGRAPH_OUTPUT_POLYGON_OFFSET | GPU_PGRAPH_OUTPUT_IGNORED;
    if ((pgraph->output_groups & required) != required) {
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, GPU_PGRAPH_NO_OPERATION,
                      "software method9 requires the corresponding output groups");
    }
    uint32_t control = state->output[GPU_PGRAPH_OUT_CONTROL0];
    if (address == 0x00400094u) {
        if (((value >> 8u) & 15u) > 7u || ((value >> 16u) & 15u) > 7u) {
            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, GPU_PGRAPH_NO_OPERATION,
                          "software CONTROL0 has an unsupported comparison function");
        }
        software_output(pgraph, GPU_PGRAPH_OUT_ALPHA_REF, value & 255u);
        software_output(pgraph, GPU_PGRAPH_OUT_ALPHA_FUNC, 0x200u | ((value >> 8u) & 7u));
        software_output(pgraph, GPU_PGRAPH_OUT_ALPHA_TEST_ENABLE, (value >> 12u) & 1u);
        software_output(pgraph, GPU_PGRAPH_OUT_DEPTH_ENABLE, (value >> 14u) & 1u);
        software_output(pgraph, GPU_PGRAPH_OUT_DEPTH_FUNC, 0x200u | ((value >> 16u) & 7u));
        software_output(pgraph, GPU_PGRAPH_OUT_DEPTH_MASK, (value >> 24u) & 1u);
        software_output(pgraph, GPU_PGRAPH_OUT_DITHER_ENABLE, (value >> 22u) & 1u);
        software_output(pgraph, GPU_PGRAPH_OUT_COLOR_MASK,
                        (((value >> 26u) & 1u) << 24u) | (((value >> 27u) & 1u) << 16u) |
                        (((value >> 28u) & 1u) << 8u) | ((value >> 29u) & 1u));
        control = (control & ~0x00010001u) | ((value >> 25u) & 1u) |
                  (((value >> 23u) & 1u) << 16u);
    } else if (address == 0x00400B80u) {
        const uint32_t front = value & 3u, back = (value >> 2u) & 3u;
        const uint32_t cull = (value >> 21u) & 3u;
        if ((value & 0xC00u) != 0u || front == 3u || back == 3u || (cull == 0u && (value & (1u << 28u)) != 0u)) {
            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, GPU_PGRAPH_NO_OPERATION,
                          "software SETUPRASTER has an unsupported polygon/cull encoding");
        }
        static const uint32_t modes[3] = {0x1B02u, 0x1B00u, 0x1B01u};
        static const uint32_t faces[4] = {0u, 0x404u, 0x405u, 0x408u};
        software_output(pgraph, GPU_PGRAPH_OUT_FRONT_POLYGON_MODE, modes[front]);
        software_output(pgraph, GPU_PGRAPH_OUT_BACK_POLYGON_MODE, modes[back]);
        software_output(pgraph, GPU_PGRAPH_OUT_POLY_OFFSET_POINT, (value >> 6u) & 1u);
        software_output(pgraph, GPU_PGRAPH_OUT_POLY_OFFSET_LINE, (value >> 7u) & 1u);
        software_output(pgraph, GPU_PGRAPH_OUT_POLY_OFFSET_FILL, (value >> 8u) & 1u);
        software_output(pgraph, GPU_PGRAPH_OUT_POINT_SMOOTH_ENABLE, (value >> 9u) & 1u);
        software_output(pgraph, GPU_PGRAPH_OUT_CULL_ENABLE, (value >> 28u) & 1u);
        if (cull != 0u) software_output(pgraph, GPU_PGRAPH_OUT_CULL_FACE, faces[cull]);
        software_output(pgraph, GPU_PGRAPH_OUT_FRONT_FACE, 0x900u | ((value >> 23u) & 1u));
        software_output(pgraph, GPU_PGRAPH_OUT_WINDOW_CLIP_TYPE, value >> 31u);
        control = (control & ~0x1000u) | (((value >> 29u) & 1u) << 12u);
    } else {
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, GPU_PGRAPH_NO_OPERATION,
                      "software method9 targets an unmodelled GPU register");
    }
    software_output(pgraph, GPU_PGRAPH_OUT_CONTROL0, control);
    pgraph->stats.software_methods++;
    return GPU_PGRAPH_OK;
}

static float float_from_bits(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static void note_unhandled(gpu_pgraph *pgraph, uint32_t method)
{
    for (size_t i = 0u; i < pgraph->unhandled_used; i++) {
        if (pgraph->unhandled[i].method == method) {
            pgraph->unhandled[i].pairs++;
            return;
        }
    }
    if (pgraph->unhandled_used >= GPU_PGRAPH_UNHANDLED_TABLE) {
        pgraph->unhandled_overflow++;
        return;
    }
    pgraph->unhandled[pgraph->unhandled_used].method = method;
    pgraph->unhandled[pgraph->unhandled_used].pairs = 1u;
    pgraph->unhandled_used++;
}

static gpu_pgraph_result push_index(gpu_pgraph *pgraph, uint32_t value)
{
    if (pgraph->index_count >= GPU_PGRAPH_MAX_INDICES) {
        return GPU_PGRAPH_ERR_FULL;
    }
    if (pgraph->index_count == pgraph->index_capacity) {
        const size_t capacity = pgraph->index_capacity == 0u ? 1024u : pgraph->index_capacity * 2u;
        uint32_t *grown = realloc(pgraph->indices, capacity * sizeof *grown);
        if (grown == NULL) {
            return GPU_PGRAPH_ERR_MEMORY;
        }
        pgraph->indices = grown;
        pgraph->index_capacity = capacity;
    }
    pgraph->indices[pgraph->index_count++] = value;
    if (pgraph->index_count > pgraph->stats.indices_peak) {
        pgraph->stats.indices_peak = pgraph->index_count;
    }
    return GPU_PGRAPH_OK;
}

static gpu_pgraph_result take_snapshot(gpu_pgraph *pgraph, uint32_t *out_index)
{
    if (pgraph->snapshot_count != 0u && !pgraph->snapshot_dirty) {
        *out_index = (uint32_t)(pgraph->snapshot_count - 1u);
        return GPU_PGRAPH_OK;
    }
    if (pgraph->snapshot_count >= GPU_PGRAPH_MAX_SNAPSHOTS) {
        return GPU_PGRAPH_ERR_FULL;
    }
    if (pgraph->snapshot_count == pgraph->snapshot_capacity) {
        const size_t capacity = pgraph->snapshot_capacity == 0u ? 8u : pgraph->snapshot_capacity * 2u;
        gpu_pgraph_state *grown = realloc(pgraph->snapshots, capacity * sizeof *grown);
        if (grown == NULL) {
            return GPU_PGRAPH_ERR_MEMORY;
        }
        pgraph->snapshots = grown;
        pgraph->snapshot_capacity = capacity;
    }
    pgraph->snapshots[pgraph->snapshot_count] = pgraph->state;
    *out_index = (uint32_t)pgraph->snapshot_count;
    pgraph->snapshot_count++;
    if (pgraph->snapshot_count > pgraph->stats.snapshots_peak) {
        pgraph->stats.snapshots_peak = pgraph->snapshot_count;
    }
    pgraph->snapshot_dirty = false;
    return GPU_PGRAPH_OK;
}

/* --- vertex snapshot (T262) ---------------------------------------------------------------- */

typedef struct {
    uint64_t start;  /* guest byte range [start, end) */
    uint64_t end;
    size_t offset;   /* where the span sits in the pool, once read */
} vertex_span;

/* Smallest and largest vertex the replay will fetch for the bracket, after the same primitive
 * expansion the replay does (a dropped remainder is not fetched, so it is not copied). False when the
 * replay would refuse the draw for its primitive or its size, or when no vertex survives. */
static gpu_pgraph_result vertex_extent(const gpu_pgraph *pgraph, const gpu_pgraph_draw *draw,
                                       bool *any, uint32_t *lowest, uint32_t *highest)
{
    const uint32_t *indices = pgraph->indices + draw->first_index;
    uint32_t capacity = draw->index_count * 6u;
    if (capacity > GPU_VSH_MAX_VERTICES) {
        capacity = GPU_VSH_MAX_VERTICES;
    }
    uint32_t *expanded = malloc((size_t)capacity * sizeof *expanded);
    if (expanded == NULL) {
        return GPU_PGRAPH_ERR_MEMORY;
    }
    uint32_t used;
    if (draw->primitive == GPU_PGRAPH_OP_POINTS) {
        used = draw->index_count <= capacity ? draw->index_count : UINT32_MAX;
        if (used != UINT32_MAX) {
            memcpy(expanded, indices, (size_t)used * sizeof *expanded);
        }
    } else if (draw->primitive == GPU_PGRAPH_OP_LINES || draw->primitive == GPU_PGRAPH_OP_LINE_STRIP) {
        used = gpu_pgraph_lineate(draw->primitive, indices, draw->index_count, expanded, capacity);
    } else {
        used = gpu_pgraph_triangulate(draw->primitive, indices, draw->index_count, expanded, capacity);
    }
    *any = used != UINT32_MAX && used != 0u;
    if (*any) {
        *lowest = expanded[0];
        *highest = expanded[0];
        for (uint32_t i = 1u; i < used; i++) {
            if (expanded[i] < *lowest) {
                *lowest = expanded[i];
            }
            if (expanded[i] > *highest) {
                *highest = expanded[i];
            }
        }
    }
    free(expanded);
    return GPU_PGRAPH_OK;
}

static gpu_pgraph_result grow_pool(gpu_pgraph *pgraph, size_t needed)
{
    if (needed <= pgraph->pool_capacity) {
        return GPU_PGRAPH_OK;
    }
    size_t capacity = pgraph->pool_capacity == 0u ? 4096u : pgraph->pool_capacity * 2u;
    while (capacity < needed) {
        capacity *= 2u;
    }
    if (capacity > pgraph->capture_budget) {
        capacity = pgraph->capture_budget;
    }
    uint8_t *grown = realloc(pgraph->pool, capacity);
    if (grown == NULL) {
        return GPU_PGRAPH_ERR_MEMORY;
    }
    pgraph->pool = grown;
    pgraph->pool_capacity = capacity;
    return GPU_PGRAPH_OK;
}

/* Copy the vertex bytes `draw` will read out of guest memory, now. */
static gpu_pgraph_result capture_vertices(gpu_pgraph *pgraph, uint64_t index, gpu_pgraph_draw *draw)
{
    if (pgraph->capture_read == NULL) {
        return GPU_PGRAPH_OK;
    }
    bool any = false;
    uint32_t lowest = 0u;
    uint32_t highest = 0u;
    gpu_pgraph_result result = vertex_extent(pgraph, draw, &any, &lowest, &highest);
    if (result != GPU_PGRAPH_OK) {
        return refuse(pgraph, result, index, GPU_PGRAPH_BEGIN_END,
                      "out of memory expanding a draw for the vertex snapshot");
    }
    if (!any) {
        return GPU_PGRAPH_OK; /* the replay refuses it, or draws nothing, before it reads a byte */
    }
    vertex_span spans[GPU_PGRAPH_ATTRIBUTES];
    uint32_t span_of[GPU_PGRAPH_ATTRIBUTES];
    uint64_t slot_start[GPU_PGRAPH_ATTRIBUTES];
    uint32_t span_count = 0u;
    for (uint32_t slot = 0u; slot < GPU_PGRAPH_ATTRIBUTES; slot++) {
        const gpu_pgraph_array *array = &draw->arrays[slot];
        span_of[slot] = UINT32_MAX;
        if (!array->format_set || !array->address_set) {
            continue;
        }
        const gpu_pgraph_format format = gpu_pgraph_decode_format(array->format);
        const uint32_t element = gpu_pgraph_element_bytes(format.type, format.size);
        if (element == 0u) {
            continue;
        }
        const uint64_t start = (uint64_t)array->address + (uint64_t)format.stride * lowest;
        const uint64_t end = (uint64_t)array->address + (uint64_t)format.stride * highest + element;
        if (end > UINT32_MAX) {
            return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, GPU_PGRAPH_BEGIN_END,
                          "a vertex array reaches past the 32-bit guest address space");
        }
        slot_start[slot] = start;
        spans[span_count].start = start;
        spans[span_count].end = end;
        span_of[slot] = span_count++;
    }
    /* Merge overlapping ranges until none overlaps (adjacent ones stay apart), so an interleaved
     * buffer read through several slots is copied once. */
    bool merged = true;
    while (merged) {
        merged = false;
        for (uint32_t a = 0u; a < span_count && !merged; a++) {
            for (uint32_t b = a + 1u; b < span_count && !merged; b++) {
                if (spans[a].start < spans[b].end && spans[b].start < spans[a].end) {
                    if (spans[b].start < spans[a].start) {
                        spans[a].start = spans[b].start;
                    }
                    if (spans[b].end > spans[a].end) {
                        spans[a].end = spans[b].end;
                    }
                    spans[b] = spans[span_count - 1u];
                    for (uint32_t slot = 0u; slot < GPU_PGRAPH_ATTRIBUTES; slot++) {
                        if (span_of[slot] == b) {
                            span_of[slot] = a;
                        } else if (span_of[slot] == span_count - 1u) {
                            span_of[slot] = b;
                        }
                    }
                    span_count--;
                    merged = true;
                }
            }
        }
    }
    size_t needed = 0u;
    for (uint32_t i = 0u; i < span_count; i++) {
        needed += (size_t)(spans[i].end - spans[i].start);
    }
    if (needed > pgraph->capture_budget - pgraph->pool_held) {
        pgraph->stats.vertex_budget_refusals++;
        char message[200];
        snprintf(message, sizeof message,
                 "the vertex snapshot budget of %zu bytes is exceeded: draw %zu needs %zu more bytes "
                 "and %zu are held (GPU_PGRAPH_ERR_FULL, nothing is truncated)",
                 pgraph->capture_budget, pgraph->draw_count, needed, pgraph->pool_held);
        return refuse(pgraph, GPU_PGRAPH_ERR_FULL, index, GPU_PGRAPH_BEGIN_END, message);
    }
    result = grow_pool(pgraph, pgraph->pool_held + needed);
    if (result != GPU_PGRAPH_OK) {
        return refuse(pgraph, result, index, GPU_PGRAPH_BEGIN_END, "out of memory for the vertex snapshot");
    }
    for (uint32_t i = 0u; i < span_count; i++) {
        const size_t length = (size_t)(spans[i].end - spans[i].start);
        spans[i].offset = pgraph->pool_held;
        if (!pgraph->capture_read(pgraph->capture_context, (uint32_t)spans[i].start,
                                  pgraph->pool + pgraph->pool_held, length)) {
            char message[160];
            snprintf(message, sizeof message,
                     "the vertex snapshot could not read %zu guest bytes at 0x%08llX (draw %zu)", length,
                     (unsigned long long)spans[i].start, pgraph->draw_count);
            return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, GPU_PGRAPH_BEGIN_END, message);
        }
        pgraph->pool_held += length;
        pgraph->stats.vertex_bytes_captured += length;
    }
    for (uint32_t slot = 0u; slot < GPU_PGRAPH_ATTRIBUTES; slot++) {
        if (span_of[slot] == UINT32_MAX) {
            continue;
        }
        const gpu_pgraph_format format = gpu_pgraph_decode_format(draw->arrays[slot].format);
        const uint32_t element = gpu_pgraph_element_bytes(format.type, format.size);
        const vertex_span *span = &spans[span_of[slot]];
        draw->vertices[slot].captured = true;
        draw->vertices[slot].address = (uint32_t)slot_start[slot];
        draw->vertices[slot].bytes = (uint32_t)((uint64_t)format.stride * (highest - lowest) + element);
        draw->vertices[slot].offset = span->offset + (size_t)(slot_start[slot] - span->start);
    }
    draw->vertices_captured = true;
    pgraph->stats.vertex_draws_captured++;
    return GPU_PGRAPH_OK;
}

/* --- immediate vertices (T462, the IMMEDIATE group) --------------------------------------- */

#define INLINE_FLOATS_PER_VERTEX (GPU_PGRAPH_ATTRIBUTES * 2u)
#define INLINE_ARRAY_FORMAT ((8u << 8) | (2u << 4) | GPU_PGRAPH_TYPE_F) /* stride 8, 2 components, float32 */

/* The write of a slot's y word completes an attribute (2f sets x, y, then z = 0 and w = 1, which the replay's component
 * defaults give). Writing attribute 0, the position, EMITS the vertex: the order the title's own SetVertexData2f
 * emitter writes, texture coordinate first and position last (the T443 composition, tests/test_d3d8_immediate_oracle.py).
 * The emit-on-position rule is the NV2A convention (INFERRED, the nxdk and xemu headers name no such rule). */
static gpu_pgraph_result inline_vertex_word(gpu_pgraph *pgraph, uint64_t index, uint32_t method, uint32_t data)
{
    if (!pgraph->in_bracket) {
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,
                      "vertex data written outside a BEGIN_END bracket, which no measured emitter does");
    }
    const uint32_t slot = (method - GPU_PGRAPH_VERTEX_DATA2F) / 8u;
    if (((method - GPU_PGRAPH_VERTEX_DATA2F) & 4u) == 0u) {
        pgraph->inline_value[slot][0] = data;
        pgraph->inline_x_pending[slot] = true;
        return GPU_PGRAPH_OK;
    }
    if (!pgraph->inline_x_pending[slot]) {
        return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,
                      "the y word of a vertex attribute with no x word written before it");
    }
    pgraph->inline_x_pending[slot] = false;
    pgraph->inline_value[slot][1] = data;
    pgraph->inline_written[slot] = true;
    if (slot != 0u) {
        return GPU_PGRAPH_OK;
    }
    if (!pgraph->bracket_inline && pgraph->index_count != pgraph->bracket_first_index) {
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,
                      "an immediate vertex in a bracket that already holds array vertices, which no measured emitter does");
    }
    uint32_t mask = 0u;
    for (uint32_t i = 0u; i < GPU_PGRAPH_ATTRIBUTES; i++) {
        if (pgraph->inline_written[i]) {
            mask |= 1u << i;
        }
    }
    if (pgraph->inline_count == 0u) {
        pgraph->bracket_inline_mask = mask;
    } else if (mask != pgraph->bracket_inline_mask) {
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,
                      "an attribute first written after the first vertex of the bracket, which no measured emitter does");
    }
    if (pgraph->inline_count >= GPU_PGRAPH_MAX_INLINE_VERTICES) {
        return refuse(pgraph, GPU_PGRAPH_ERR_FULL, index, method, "the bracket holds GPU_PGRAPH_MAX_INLINE_VERTICES vertices");
    }
    if (pgraph->inline_count == pgraph->inline_capacity) {
        const size_t capacity = pgraph->inline_capacity == 0u ? 16u : pgraph->inline_capacity * 2u;
        uint32_t *grown = realloc(pgraph->inline_buffer, capacity * INLINE_FLOATS_PER_VERTEX * sizeof *grown);
        if (grown == NULL) {
            return refuse(pgraph, GPU_PGRAPH_ERR_MEMORY, index, method, "out of memory for the immediate vertices");
        }
        pgraph->inline_buffer = grown;
        pgraph->inline_capacity = capacity;
    }
    memcpy(pgraph->inline_buffer + pgraph->inline_count * INLINE_FLOATS_PER_VERTEX, pgraph->inline_value,
           sizeof pgraph->inline_value);
    const gpu_pgraph_result pushed = push_index(pgraph, (uint32_t)pgraph->inline_count);
    if (pushed != GPU_PGRAPH_OK) {
        return refuse(pgraph, pushed, index, method, pushed == GPU_PGRAPH_ERR_FULL ? "the index list is full" : "out of memory");
    }
    pgraph->bracket_inline = true;
    pgraph->inline_count++;
    pgraph->stats.inline_vertices++;
    return GPU_PGRAPH_OK;
}

/* Lay the bracket's vertices out per slot in the inline pool and describe them as arrays, so the replay assembles them
 * through the path every other draw takes. A slot nothing wrote is a disabled array (the component defaults). */
static gpu_pgraph_result build_inline_draw(gpu_pgraph *pgraph, uint64_t index, gpu_pgraph_draw *draw)
{
    const size_t count = pgraph->inline_count;
    if (pgraph->draw_count == 0u) {
        pgraph->inline_pool_held = 0u; /* the first draw of a frame: the frame's earlier bytes are gone */
    }
    size_t slots = 0u;
    for (uint32_t slot = 0u; slot < GPU_PGRAPH_ATTRIBUTES; slot++) {
        slots += (pgraph->bracket_inline_mask >> slot) & 1u;
    }
    const size_t needed = pgraph->inline_pool_held + slots * count * 8u;
    if (needed > pgraph->inline_pool_capacity) {
        uint8_t *grown = realloc(pgraph->inline_pool, needed * 2u);
        if (grown == NULL) {
            return refuse(pgraph, GPU_PGRAPH_ERR_MEMORY, index, GPU_PGRAPH_BEGIN_END, "out of memory for the immediate vertices");
        }
        pgraph->inline_pool = grown;
        pgraph->inline_pool_capacity = needed * 2u;
    }
    for (uint32_t slot = 0u; slot < GPU_PGRAPH_ATTRIBUTES; slot++) {
        draw->arrays[slot] = (gpu_pgraph_array){.format_set = true}; /* format 0: size 0, disabled */
        if (((pgraph->bracket_inline_mask >> slot) & 1u) == 0u) {
            continue;
        }
        draw->arrays[slot] = (gpu_pgraph_array){.address_set = true, .format_set = true, .address = 0u,
                                                .format = INLINE_ARRAY_FORMAT};
        uint8_t *at = pgraph->inline_pool + pgraph->inline_pool_held;
        for (size_t vertex = 0u; vertex < count; vertex++) {
            memcpy(at + vertex * 8u, pgraph->inline_buffer + vertex * INLINE_FLOATS_PER_VERTEX + slot * 2u, 8u);
        }
        draw->vertices[slot].captured = true;
        draw->vertices[slot].address = 0u;
        draw->vertices[slot].bytes = (uint32_t)(count * 8u);
        draw->vertices[slot].offset = pgraph->inline_pool_held;
        pgraph->inline_pool_held += count * 8u;
    }
    draw->inline_vertices = true;
    draw->vertices_captured = true;
    return GPU_PGRAPH_OK;
}

static gpu_pgraph_result end_bracket(gpu_pgraph *pgraph, uint64_t index)
{
    const uint32_t first = pgraph->bracket_first_index;
    const uint32_t count = (uint32_t)pgraph->index_count - first;
    pgraph->in_bracket = false;
    if (count == 0u) {
        pgraph->stats.empty_brackets++;
        return GPU_PGRAPH_OK;
    }
    if (pgraph->draw_count >= GPU_PGRAPH_MAX_DRAWS) {
        return refuse(pgraph, GPU_PGRAPH_ERR_FULL, index, GPU_PGRAPH_BEGIN_END,
                      "the draw list is full (GPU_PGRAPH_MAX_DRAWS)");
    }
    uint32_t snapshot = 0u;
    const gpu_pgraph_result taken = take_snapshot(pgraph, &snapshot);
    if (taken != GPU_PGRAPH_OK) {
        return refuse(pgraph, taken, index, GPU_PGRAPH_BEGIN_END, "no room for a state snapshot");
    }
    if (pgraph->draw_count == pgraph->draw_capacity) {
        const size_t capacity = pgraph->draw_capacity == 0u ? 16u : pgraph->draw_capacity * 2u;
        gpu_pgraph_draw *grown = realloc(pgraph->draws, capacity * sizeof *grown);
        if (grown == NULL) {
            return GPU_PGRAPH_ERR_MEMORY;
        }
        pgraph->draws = grown;
        pgraph->draw_capacity = capacity;
    }
    gpu_pgraph_draw built;
    memset(&built, 0, sizeof built);
    built.primitive = pgraph->bracket_primitive;
    built.first_command = pgraph->bracket_command;
    built.first_index = first;
    built.index_count = count;
    built.snapshot = snapshot;
    memcpy(built.arrays, pgraph->arrays, sizeof built.arrays);
    /* The bytes are read NOW, at the END of the bracket: this is the moment of the draw. Immediate vertices are
     * laid out from the bracket's own buffer instead (T462). */
    const gpu_pgraph_result captured = pgraph->bracket_inline ? build_inline_draw(pgraph, index, &built)
                                                              : capture_vertices(pgraph, index, &built);
    if (captured != GPU_PGRAPH_OK) {
        return captured;
    }
    pgraph->stats.inline_draws += pgraph->bracket_inline ? 1u : 0u;
    pgraph->draws[pgraph->draw_count++] = built;
    if (pgraph->draw_count > pgraph->stats.draws_peak) {
        pgraph->stats.draws_peak = pgraph->draw_count;
    }
    pgraph->stats.draws++;
    return GPU_PGRAPH_OK;
}

static bool is_vertex_state_method(uint32_t method)
{
    return (method >= GPU_PGRAPH_VIEWPORT_OFFSET && method < GPU_PGRAPH_VIEWPORT_OFFSET + 16u) ||
           (method >= GPU_PGRAPH_VIEWPORT_SCALE && method < GPU_PGRAPH_VIEWPORT_SCALE + 16u) ||
           (method >= GPU_PGRAPH_PROGRAM_DATA && method < GPU_PGRAPH_CONSTANT_DATA_END) ||
           (method >= GPU_PGRAPH_ARRAY_OFFSET &&
            method < GPU_PGRAPH_ARRAY_FORMAT + 4u * GPU_PGRAPH_ATTRIBUTES) ||
           method == GPU_PGRAPH_CXT_WRITE_EN ||
           method == GPU_PGRAPH_EXECUTION_MODE || method == GPU_PGRAPH_PROGRAM_LOAD ||
           method == GPU_PGRAPH_PROGRAM_START || method == GPU_PGRAPH_CONSTANT_LOAD;
}

static gpu_pgraph_result handle(gpu_pgraph *pgraph, uint64_t index, uint32_t method, uint32_t data,
                                bool *handled)
{
    *handled = true;
    gpu_pgraph_state *state = &pgraph->state;

    const int combiner_word = pgraph->combiner ? gpu_pgraph_combiner_index(method) : -1;
    if (pgraph->in_bracket && (is_vertex_state_method(method) || combiner_word >= 0)) {
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,
                      "vertex-stage or combiner state changes inside a BEGIN_END bracket, which no "
                      "measured emitter does");
    }
    if (combiner_word >= 0) {
        state->combiner[combiner_word] = data;
        state->combiner_written[combiner_word] = true;
        pgraph->snapshot_dirty = true;
        return GPU_PGRAPH_OK;
    }
    if (pgraph->visibility && (method == 0x17C8u || method == 0x17CCu || method == 0x17D0u)) {
        if (pgraph->in_bracket)
            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method, "visibility method inside BEGIN_END");
        if ((method == 0x17C8u && data != 1u) || (method == 0x17CCu && data > 1u) ||
            (method == 0x17D0u && ((data >> 24u) != 1u || (data & 15u) != 0u)))
            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method, "unsupported visibility method value");
        if (pgraph->query_count == GPU_PGRAPH_MAX_QUERY_EVENTS)
            return refuse(pgraph, GPU_PGRAPH_ERR_FULL, index, method, "visibility event budget exhausted");
        gpu_pgraph_query *query = &pgraph->queries[pgraph->query_count++];
        *query = (gpu_pgraph_query){.before_draw=(uint32_t)pgraph->draw_count, .command=index, .method=method, .data=data};
        if (method == 0x17D0u && pgraph->report_identity != NULL)
            pgraph->report_identity(pgraph->report_identity_context,query);
        return GPU_PGRAPH_OK;
    }
    if (method == GPU_PGRAPH_SEMAPHORE_RELEASE) {
        if (pgraph->in_bracket) {
            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,
                          "a semaphore release inside a BEGIN_END bracket, which no measured emitter does");
        }
        pgraph->stats.semaphore_releases++;
        return GPU_PGRAPH_OK;
    }
    if (method == GPU_PGRAPH_CLEAR_SURFACE && (pgraph->output_groups & GPU_PGRAPH_OUTPUT_CLEAR) != 0u) {
        if (pgraph->in_bracket) {
            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,
                          "a clear inside a BEGIN_END bracket, which no measured emitter does");
        }
        if (pgraph->clear_count >= GPU_PGRAPH_MAX_CLEARS) {
            return refuse(pgraph, GPU_PGRAPH_ERR_FULL, index, method,
                          "the clear list is full (GPU_PGRAPH_MAX_CLEARS)");
        }
        gpu_pgraph_clear *clear = &pgraph->clears[pgraph->clear_count++];
        clear->before_draw = (uint32_t)pgraph->draw_count;
        clear->command = (uint32_t)index;
        clear->flags = data;
        clear->zstencil = state->output[GPU_PGRAPH_OUT_CLEAR_ZSTENCIL];
        clear->color = state->output[GPU_PGRAPH_OUT_CLEAR_COLOR];
        clear->rect_horizontal = state->output[GPU_PGRAPH_OUT_CLEAR_RECT_HORIZONTAL];
        clear->rect_vertical = state->output[GPU_PGRAPH_OUT_CLEAR_RECT_VERTICAL];
        clear->zstencil_written = state->output_written[GPU_PGRAPH_OUT_CLEAR_ZSTENCIL];
        clear->color_written = state->output_written[GPU_PGRAPH_OUT_CLEAR_COLOR];
        clear->rect_horizontal_written = state->output_written[GPU_PGRAPH_OUT_CLEAR_RECT_HORIZONTAL];
        clear->rect_vertical_written = state->output_written[GPU_PGRAPH_OUT_CLEAR_RECT_VERTICAL];
        clear->surface_format = state->output[GPU_PGRAPH_OUT_SURFACE_FORMAT];
        clear->surface_pitch = state->output[GPU_PGRAPH_OUT_SURFACE_PITCH];
        clear->surface_color_offset = state->output[GPU_PGRAPH_OUT_SURFACE_COLOR_OFFSET];
        clear->surface_written = state->output_written[GPU_PGRAPH_OUT_SURFACE_FORMAT] &&
                                 state->output_written[GPU_PGRAPH_OUT_SURFACE_PITCH] &&
                                 state->output_written[GPU_PGRAPH_OUT_SURFACE_COLOR_OFFSET];
        return GPU_PGRAPH_OK;
    }
    if ((pgraph->output_groups & GPU_PGRAPH_OUTPUT_SURFACE) != 0u &&
        (method == GPU_PGRAPH_NO_OPERATION || method == GPU_PGRAPH_WAIT_FOR_IDLE)) {
        /* T462, T541. The bind packet puts a NO_OPERATION or a WAIT_FOR_IDLE with data 0 between its register writes
         * (MEASURED: the original SetRenderTarget, docs/t541-bind-packet-proof.md). The replay executes the stream in
         * order and instantaneously. Zero is a sync marker; T1176 software type9 writes control state. */
        if (pgraph->in_bracket) {
            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,
                          "a NO_OPERATION or WAIT_FOR_IDLE inside a BEGIN_END bracket, which no measured emitter does");
        }
        if (method == GPU_PGRAPH_NO_OPERATION && data == 9u) {
            return software_control(pgraph, index);
        }
        if (data != 0u) {
            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,
                          "NO_OPERATION or WAIT_FOR_IDLE data word other than 0 or supported software NO_OPERATION(9)");
        }
        pgraph->stats.sync_pairs++;
        return GPU_PGRAPH_OK;
    }
    if ((pgraph->output_groups & GPU_PGRAPH_OUTPUT_IMMEDIATE) != 0u && method >= GPU_PGRAPH_VERTEX_DATA2F &&
        method < GPU_PGRAPH_VERTEX_DATA2F_END) {
        return inline_vertex_word(pgraph, index, method, data);
    }
    uint32_t output_group = 0u;
    const int output_word = gpu_pgraph_output_index(method, &output_group);
    if (output_word >= 0 && (pgraph->output_groups & output_group) != 0u) {
        if (pgraph->in_bracket) {
            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,
                          "output state changes inside a BEGIN_END bracket, which no measured emitter "
                          "does");
        }
        state->output[output_word] = data;
        state->output_written[output_word] = true;
        if (output_group == GPU_PGRAPH_OUTPUT_IGNORED) {
            pgraph->stats.pairs_ignored++; /* skipped on purpose: no draw depends on it, no new snapshot */
            return GPU_PGRAPH_OK;
        }
        pgraph->snapshot_dirty = true;
        return GPU_PGRAPH_OK;
    }
    if (method >= GPU_PGRAPH_VIEWPORT_OFFSET && method < GPU_PGRAPH_VIEWPORT_OFFSET + 16u) {
        state->viewport_offset[(method - GPU_PGRAPH_VIEWPORT_OFFSET) / 4u] = float_from_bits(data);
        state->viewport_offset_set = true;
        pgraph->snapshot_dirty = true;
        return GPU_PGRAPH_OK;
    }
    if (method >= GPU_PGRAPH_VIEWPORT_SCALE && method < GPU_PGRAPH_VIEWPORT_SCALE + 16u) {
        state->viewport_scale[(method - GPU_PGRAPH_VIEWPORT_SCALE) / 4u] = float_from_bits(data);
        state->viewport_scale_set = true;
        pgraph->snapshot_dirty = true;
        return GPU_PGRAPH_OK;
    }
    if (method >= GPU_PGRAPH_PROGRAM_DATA && method < GPU_PGRAPH_PROGRAM_DATA_END) {
        if (pgraph->program_cursor >= GPU_PGRAPH_PROGRAM_SLOTS * 4u) {
            return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,
                          "program data past the 136-slot program file");
        }
        const uint32_t cursor = pgraph->program_cursor++;
        if (cursor % 4u == 0u) {
            state->slot_written[cursor / 4u] = false;
        }
        state->program[cursor] = data;
        if (cursor % 4u == 3u) {
            state->slot_written[cursor / 4u] = true;
        }
        pgraph->stats.program_dwords++;
        pgraph->snapshot_dirty = true;
        return GPU_PGRAPH_OK;
    }
    if (method >= GPU_PGRAPH_CONSTANT_DATA && method < GPU_PGRAPH_CONSTANT_DATA_END) {
        if (pgraph->constant_cursor >= GPU_PGRAPH_CONSTANT_ROWS * 4u) {
            return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,
                          "constant data past the 192-row constant file");
        }
        const uint32_t cursor = pgraph->constant_cursor++;
        if (cursor % 4u == 0u) {
            state->constant_written[cursor / 4u] = false;
        }
        state->constants[cursor] = float_from_bits(data);
        if (cursor % 4u == 3u) {
            state->constant_written[cursor / 4u] = true;
        }
        pgraph->stats.constant_dwords++;
        pgraph->snapshot_dirty = true;
        return GPU_PGRAPH_OK;
    }
    if (method >= GPU_PGRAPH_ARRAY_OFFSET &&
        method < GPU_PGRAPH_ARRAY_OFFSET + 4u * GPU_PGRAPH_ATTRIBUTES) {
        gpu_pgraph_array *array = &pgraph->arrays[(method - GPU_PGRAPH_ARRAY_OFFSET) / 4u];
        array->address = data;
        array->address_set = true;
        return GPU_PGRAPH_OK;
    }
    if (method >= GPU_PGRAPH_ARRAY_FORMAT &&
        method < GPU_PGRAPH_ARRAY_FORMAT + 4u * GPU_PGRAPH_ATTRIBUTES) {
        gpu_pgraph_array *array = &pgraph->arrays[(method - GPU_PGRAPH_ARRAY_FORMAT) / 4u];
        array->format = data;
        array->format_set = true;
        return GPU_PGRAPH_OK;
    }
    switch (method) {
    case GPU_PGRAPH_EXECUTION_MODE:
        state->execution_mode = data;
        state->execution_mode_set = true;
        pgraph->snapshot_dirty = true;
        return GPU_PGRAPH_OK;
    case GPU_PGRAPH_CXT_WRITE_EN:
        /* T521. NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, the second dword of the 0x00081E94 packet. MEASURED: the
         * title's four count-2 emitters write 0 or flags & 1, and the real stream carries 0. Only 0 is decoded:
         * a program context-write enable of 1 would let a vertex program write constants, which the replay does
         * not model (INFERRED from the nxdk/xemu name). A kept, counted no-op: no snapshot, no pixel. */
        if (data != 0u) {
            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,
                          "program context write enable is not 0, which the real stream never writes and the "
                          "replay does not model");
        }
        state->cxt_write_en = data;
        state->cxt_write_en_set = true;
        pgraph->stats.cxt_write_en_pairs++;
        return GPU_PGRAPH_OK;
    case GPU_PGRAPH_PROGRAM_LOAD:
        if (data >= GPU_PGRAPH_PROGRAM_SLOTS) {
            return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,
                          "program load slot is outside the 136-slot program file");
        }
        pgraph->program_cursor = data * 4u;
        return GPU_PGRAPH_OK;
    case GPU_PGRAPH_PROGRAM_START:
        if (data >= GPU_PGRAPH_PROGRAM_SLOTS) {
            return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,
                          "program start slot is outside the 136-slot program file");
        }
        state->program_start = data;
        state->program_start_set = true;
        pgraph->snapshot_dirty = true;
        return GPU_PGRAPH_OK;
    case GPU_PGRAPH_CONSTANT_LOAD:
        if (data >= GPU_PGRAPH_CONSTANT_ROWS) {
            return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,
                          "constant load row is outside the 192-row constant file");
        }
        pgraph->constant_cursor = data * 4u;
        return GPU_PGRAPH_OK;
    case GPU_PGRAPH_BEGIN_END:
        if (data == GPU_PGRAPH_OP_END) {
            if (!pgraph->in_bracket) {
                return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,
                              "BEGIN_END(0) with no bracket open");
            }
            return end_bracket(pgraph, index);
        }
        if (pgraph->in_bracket) {
            return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,
                          "BEGIN_END opened inside an open bracket");
        }
        if (data > GPU_PGRAPH_OP_POLYGON) {
            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,
                          "primitive operation above 10 is not a known operation");
        }
        pgraph->in_bracket = true;
        pgraph->bracket_inline = false;
        pgraph->inline_count = 0u;
        pgraph->bracket_primitive = data;
        pgraph->bracket_command = (uint32_t)index;
        pgraph->bracket_first_index = (uint32_t)pgraph->index_count;
        return GPU_PGRAPH_OK;
    case GPU_PGRAPH_DRAW_ARRAYS:
    case GPU_PGRAPH_ARRAY_ELEMENT16:
    case GPU_PGRAPH_ARRAY_ELEMENT32: {
        if (!pgraph->in_bracket) {
            return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,
                          "vertices submitted outside a BEGIN_END bracket");
        }
        if (pgraph->bracket_inline) {
            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,
                          "array vertices in a bracket that already holds immediate vertices, which no measured "
                          "emitter does");
        }
        gpu_pgraph_result pushed = GPU_PGRAPH_OK;
        if (method == GPU_PGRAPH_DRAW_ARRAYS) {
            const uint32_t count = (data >> 24) + 1u;
            const uint32_t start = data & 0x00FFFFFFu;
            for (uint32_t i = 0u; i < count && pushed == GPU_PGRAPH_OK; i++) {
                pushed = push_index(pgraph, start + i);
            }
        } else if (method == GPU_PGRAPH_ARRAY_ELEMENT16) {
            pushed = push_index(pgraph, data & 0xFFFFu);
            if (pushed == GPU_PGRAPH_OK) {
                pushed = push_index(pgraph, data >> 16);
            }
        } else {
            pushed = push_index(pgraph, data);
        }
        if (pushed != GPU_PGRAPH_OK) {
            return refuse(pgraph, pushed, index, method,
                          pushed == GPU_PGRAPH_ERR_FULL ? "the index list is full" : "out of memory");
        }
        return GPU_PGRAPH_OK;
    }
    default:
        *handled = false;
        return GPU_PGRAPH_OK;
    }
}

/* The three 3D commands that follow the fence's software method (0x003D67B0): SEMAPHORE_RELEASE, then the colour
 * clear value 0 twice. */
#define FENCE_TAIL_COMMANDS 3u
static const uint32_t fence_tail_methods[FENCE_TAIL_COMMANDS] = {GPU_PGRAPH_SEMAPHORE_RELEASE, 0x1D90u, 0x1D90u};

/* T578. Bytes per pixel of the colour formats the title's CopyRects writes (the table at 0x003E1828 gives 1, 4 or 0xA, and the
 * byte path 1), 0 for every other format word. */
static uint32_t blit_bytes_per_pixel(uint32_t format)
{
    switch (format) {
    case GPU_PGRAPH_BLIT_FORMAT_Y8: return 1u;
    case GPU_PGRAPH_BLIT_FORMAT_R5G6B5: return 2u;
    case GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8: return 4u;
    default: return 0u;
    }
}

static gpu_pgraph_result accept_blit_word(gpu_pgraph *pgraph, uint64_t pair, uint32_t method, uint32_t data,
                                          uint32_t measured, const char *what)
{
    if (data != measured) {
        char message[200];
        snprintf(message, sizeof message,
                 "%s written with 0x%X, the original CreateDevice init writes 0x%X (0x003DA407..0x003DA4EE)", what,
                 (unsigned)data, (unsigned)measured);
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, method, message);
    }
    return GPU_PGRAPH_OK;
}

static gpu_pgraph_result run_blit(gpu_pgraph *pgraph, uint64_t pair, uint32_t data)
{
    const uint32_t width = data & 0xFFFFu;
    const uint32_t height = data >> 16;
    if (!pgraph->blit.source_written || !pgraph->blit.destination_written || !pgraph->blit.format_written ||
        !pgraph->blit.pitch_written || !pgraph->blit.in_written || !pgraph->blit.out_written) {
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, GPU_PGRAPH_BLIT_SIZE,
                      "an image blit before the surface offsets, colour format, pitches and both points were written: "
                      "their initial values are not measured");
    }
    const uint32_t bytes = blit_bytes_per_pixel(pgraph->blit.color_format);
    if (bytes == 0u) {
        char message[160];
        snprintf(message, sizeof message,
                 "an image blit with colour format 0x%X: only 1 (Y8), 4 (R5G6B5) and 0xA (A8R8G8B8), the formats "
                 "CopyRects writes, are decoded", (unsigned)pgraph->blit.color_format);
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, GPU_PGRAPH_BLIT_SIZE, message);
    }
    const uint32_t source_pitch = pgraph->blit.pitch & 0xFFFFu;
    const uint32_t destination_pitch = pgraph->blit.pitch >> 16;
    if (source_pitch == 0u || destination_pitch == 0u) {
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, GPU_PGRAPH_BLIT_SIZE,
                      "an image blit with a zero pitch, which no measured emitter writes");
    }
    if ((pgraph->blit.point_in & 0xFFFFu) + width > 0xFFFFu || (pgraph->blit.point_out & 0xFFFFu) + width > 0xFFFFu ||
        (pgraph->blit.point_in >> 16) + height > 0xFFFFu || (pgraph->blit.point_out >> 16) + height > 0xFFFFu) {
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, GPU_PGRAPH_BLIT_SIZE,
                      "an image blit rectangle that passes 65535 in x or y, past the 16 bit point fields");
    }
    if ((uint64_t)width * bytes > source_pitch || (uint64_t)width * bytes > destination_pitch) {
        char message[200];
        snprintf(message, sizeof message,
                 "an image blit row of %u x %u bytes wider than a pitch (source %u, destination %u): the hardware "
                 "clamping is not measured", (unsigned)width, (unsigned)bytes, (unsigned)source_pitch,
                 (unsigned)destination_pitch);
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, GPU_PGRAPH_BLIT_SIZE, message);
    }
    if (pgraph->copy_count >= GPU_PGRAPH_MAX_COPIES) {
        return refuse(pgraph, GPU_PGRAPH_ERR_FULL, pair, GPU_PGRAPH_BLIT_SIZE,
                      "the blit list is full (GPU_PGRAPH_MAX_COPIES)");
    }
    gpu_pgraph_copy *copy = &pgraph->copies[pgraph->copy_count++];
    copy->before_draw = (uint32_t)pgraph->draw_count;
    copy->before_clear = (uint32_t)pgraph->clear_count;
    copy->command = (uint32_t)pair;
    copy->source_offset = pgraph->blit.source_offset;
    copy->destination_offset = pgraph->blit.destination_offset;
    copy->color_format = pgraph->blit.color_format;
    copy->source_pitch = source_pitch;
    copy->destination_pitch = destination_pitch;
    copy->in_x = pgraph->blit.point_in & 0xFFFFu;
    copy->in_y = pgraph->blit.point_in >> 16;
    copy->out_x = pgraph->blit.point_out & 0xFFFFu;
    copy->out_y = pgraph->blit.point_out >> 16;
    copy->width = width;
    copy->height = height;
    copy->operation = GPU_PGRAPH_BLIT_OPERATION_SRCCOPY;
    pgraph->stats.copies++;
    if (width == 0u || height == 0u) {
        pgraph->stats.copies_empty++;
    }
    return GPU_PGRAPH_OK;
}

/* T578, the BLIT group: one command on subchannel 2 (image blit, class 0x9F) or 3 (context surfaces 2D, class 0x62). The
 * bindings and the context words are the original CreateDevice init's, accepted only at the measured values (the ported
 * CreateDevice does not write them, so a recorded stream never holds them, an original ring does). */
static gpu_pgraph_result decode_blit(gpu_pgraph *pgraph, uint32_t subchannel, uint32_t method, uint32_t data)
{
    const uint64_t pair = pgraph->stats.pairs;
    if (pgraph->in_bracket) {
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, method,
                      "a 2D engine command inside a BEGIN_END bracket, which no measured emitter does");
    }
    gpu_pgraph_result result = GPU_PGRAPH_OK;
    if (subchannel == GPU_PGRAPH_SUBCHANNEL_SURFACES_2D) {
        switch (method) {
        case GPU_PGRAPH_SET_OBJECT:
            result = accept_blit_word(pgraph, pair, method, data, GPU_PGRAPH_SURFACES_2D_HANDLE,
                                      "SET_OBJECT on subchannel 3 (context surfaces 2D)");
            break;
        case GPU_PGRAPH_S2D_DMA_SOURCE:
            result = accept_blit_word(pgraph, pair, method, data, 3u, "the source DMA context");
            break;
        case GPU_PGRAPH_S2D_DMA_DESTINATION:
            result = accept_blit_word(pgraph, pair, method, data, 0xBu, "the destination DMA context");
            break;
        case GPU_PGRAPH_S2D_COLOR_FORMAT:
            pgraph->blit.color_format = data;
            pgraph->blit.format_written = true;
            break;
        case GPU_PGRAPH_S2D_PITCH:
            pgraph->blit.pitch = data;
            pgraph->blit.pitch_written = true;
            break;
        case GPU_PGRAPH_S2D_OFFSET_SOURCE:
        case GPU_PGRAPH_S2D_OFFSET_DESTINATION:
            if ((data & 0xF0000000u) != 0u) {
                return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, method,
                              "a surface offset past 28 bits: the Data word of a surface header is masked to 28 bits");
            }
            if (method == GPU_PGRAPH_S2D_OFFSET_SOURCE) {
                pgraph->blit.source_offset = data;
                pgraph->blit.source_written = true;
            } else {
                pgraph->blit.destination_offset = data;
                pgraph->blit.destination_written = true;
            }
            break;
        default:
            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, method,
                          "an unmeasured method on subchannel 3 (context surfaces 2D, class 0x62): the library writes "
                          "SET_OBJECT, the two DMA contexts, the colour format, the pitches and the two offsets");
        }
    } else if (method == GPU_PGRAPH_SET_OBJECT) {
        result = accept_blit_word(pgraph, pair, method, data, GPU_PGRAPH_BLIT_HANDLE,
                                  "SET_OBJECT on subchannel 2 (image blit)");
    } else if (method >= GPU_PGRAPH_BLIT_CONTEXT_FIRST && method < GPU_PGRAPH_BLIT_CONTEXT_SURFACES) {
        result = accept_blit_word(pgraph, pair, method, data, 0x19u, "a blit context (colour key, clip, pattern, rop, beta)");
    } else if (method == GPU_PGRAPH_BLIT_CONTEXT_SURFACES) {
        result = accept_blit_word(pgraph, pair, method, data, GPU_PGRAPH_SURFACES_2D_HANDLE,
                                  "the blit's surfaces context");
    } else if (method == GPU_PGRAPH_BLIT_OPERATION) {
        result = accept_blit_word(pgraph, pair, method, data, GPU_PGRAPH_BLIT_OPERATION_SRCCOPY,
                                  "the blit operation (only SRCCOPY, 3, is measured)");
    } else if (method == GPU_PGRAPH_BLIT_POINT_IN) {
        pgraph->blit.point_in = data;
        pgraph->blit.in_written = true;
    } else if (method == GPU_PGRAPH_BLIT_POINT_OUT) {
        pgraph->blit.point_out = data;
        pgraph->blit.out_written = true;
    } else if (method == GPU_PGRAPH_BLIT_SIZE) {
        result = run_blit(pgraph, pair, data);
    } else {
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, method,
                      "an unmeasured method on subchannel 2 (image blit, class 0x9F): the library writes SET_OBJECT, the "
                      "contexts, the operation, the two points and the size");
    }
    if (result != GPU_PGRAPH_OK) {
        return result;
    }
    pgraph->stats.blit_state_pairs++;
    pgraph->stats.pairs++;
    pgraph->stats.pairs_handled++;
    return GPU_PGRAPH_OK;
}

gpu_pgraph_result gpu_pgraph_decode_other_subchannel(gpu_pgraph *pgraph, uint32_t subchannel,
                                                     uint32_t method, uint32_t data)
{
    (void)data;
    if (pgraph == NULL || subchannel == GPU_PGRAPH_SUBCHANNEL_3D) {
        return GPU_PGRAPH_ERR_ARGUMENT;
    }
    if ((pgraph->output_groups & GPU_PGRAPH_OUTPUT_BLIT) != 0u &&
        (subchannel == GPU_PGRAPH_SUBCHANNEL_IMAGE_BLIT || subchannel == GPU_PGRAPH_SUBCHANNEL_SURFACES_2D)) {
        return decode_blit(pgraph, subchannel, method, data);
    }
    const uint64_t pair = pgraph->stats.pairs;
    const bool fence_notify =
        subchannel == GPU_PGRAPH_SUBCHANNEL_SOFTWARE && method == GPU_PGRAPH_SOFTWARE_FENCE_NOTIFY;
    if (!fence_notify || pgraph->in_bracket) {
        char message[160];
        snprintf(message, sizeof message,
                 "a command on subchannel %u, %s: only the 3D subchannel 0 is decoded and only the "
                 "fence's subchannel-5 0x310 is known",
                 (unsigned)subchannel, pgraph->in_bracket ? "inside a BEGIN_END bracket" : "an unmeasured method");
        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, method, message);
    }
    pgraph->stats.software_methods++;
    pgraph->fence_tail = FENCE_TAIL_COMMANDS;
    pgraph->stats.pairs++;
    pgraph->stats.pairs_handled++;
    return GPU_PGRAPH_OK;
}

gpu_pgraph_result gpu_pgraph_decode(gpu_pgraph *pgraph, const gpu_pgraph_command *commands,
                                    size_t count)
{
    if (pgraph == NULL || (commands == NULL && count != 0u)) {
        return GPU_PGRAPH_ERR_ARGUMENT;
    }
    for (size_t i = 0u; i < count; i++) {
        const uint64_t index = pgraph->stats.pairs;
        if (pgraph->fence_tail != 0u) {
            const uint32_t step = FENCE_TAIL_COMMANDS - pgraph->fence_tail;
            if (commands[i].method == fence_tail_methods[step] && (step == 0u || commands[i].data == 0u)) {
                pgraph->fence_tail--;
                if (step != 0u && (pgraph->output_groups & GPU_PGRAPH_OUTPUT_CLEAR) == 0u) {
                    pgraph->stats.fence_clear_values++;
                    pgraph->stats.pairs++;
                    pgraph->stats.pairs_handled += 1u;
                    continue;
                }
            } else {
                pgraph->fence_tail = 0u;
            }
        }
        bool handled = false;
        const gpu_pgraph_result result = handle(pgraph, index, commands[i].method, commands[i].data,
                                                &handled);
        if (result != GPU_PGRAPH_OK) {
            return result;
        }
        if (!handled && pgraph->strict) {
            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, commands[i].method,
                          "method is not in the measured list (strict mode)");
        }
        pgraph->stats.pairs++;
        if (handled) {
            pgraph->stats.pairs_handled++;
            continue;
        }
        note_unhandled(pgraph, commands[i].method);
    }
    return GPU_PGRAPH_OK;
}
