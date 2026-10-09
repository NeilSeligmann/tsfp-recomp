/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The render-state dispatch, walking the generated table in d3d8_render_state_table.c.
 *
 * HAND-WRITTEN, UNLIKE THE TABLE. The split is deliberate: regenerating the table against
 * a different build of the game rewrites data and cannot rewrite behaviour. See
 * d3d8_render_state.h for what was measured and where, including why there are two entry
 * points and why the range check is signed.
 *
 * EVERY BOUND COMES FROM THE TABLE, NOT FROM A LITERAL HERE. `d3d8_rs_immediate_bound` and
 * friends are generated alongside the rows, so a table regenerated from an image with
 * different bounds stays self-consistent. The one place a literal would be tempting is the
 * loop that classifies, and it is exactly the place an off-by-one at the 0x5C edge would
 * hide, which is why `d3d8_rs_classify` is a separate function the suite can drive at the
 * boundary indices directly.
 */

#include "d3d8_render_state.h"

#include <string.h>

const d3d8_render_state_row *d3d8_rs_row(int32_t state)
{
    if (state < 0 || (uint32_t)state >= d3d8_rs_handler_bound) {
        return NULL;
    }
    return &d3d8_render_state_rows[(uint32_t)state];
}

d3d8_rs_status d3d8_rs_classify(int32_t state)
{
    /* Signed first, and reported rather than folded into OUT_OF_RANGE: the guest's `jge`
     * accepts a negative state and reads off the front of its table. A reimplementation
     * that cannot tell the two apart cannot be compared against the original. */
    if (state < 0) {
        return D3D8_RS_NEGATIVE;
    }
    uint32_t index = (uint32_t)state;
    if (index < d3d8_rs_immediate_bound) {
        return D3D8_RS_OK_IMMEDIATE;
    }
    if (index < d3d8_rs_deferred_bound) {
        return D3D8_RS_OK_DEFERRED;
    }
    if (index < d3d8_rs_handler_bound) {
        return D3D8_RS_UNIMPLEMENTED_HANDLER;
    }
    return D3D8_RS_OUT_OF_RANGE;
}

void d3d8_render_state_init(d3d8_render_state *rs, d3d8_rs_emit_fn emit, void *context)
{
    if (rs == NULL) {
        return;
    }
    memset(rs, 0, sizeof(*rs));
    rs->emit = emit;
    rs->emit_context = context;
}

/* The common body. `store_shadow` is what separates the two measured copies of the
 * dispatch on the immediate path; everything else is identical between them. */
static d3d8_rs_status apply(d3d8_render_state *rs, int32_t state, uint32_t value,
                            int store_shadow)
{
    if (rs == NULL) {
        return D3D8_RS_OUT_OF_RANGE;
    }

    d3d8_rs_status status = d3d8_rs_classify(state);
    if (status == D3D8_RS_NEGATIVE || status == D3D8_RS_OUT_OF_RANGE) {
        rs->rejected++;
        return status;
    }

    const d3d8_render_state_row *row = &d3d8_render_state_rows[(uint32_t)state];

    if (status == D3D8_RS_OK_IMMEDIATE) {
        if (rs->emit != NULL) {
            rs->emit(rs->emit_context, D3D8_NV2A_HEADER(row->method), value);
        }
        if (store_shadow) {
            rs->shadow[(uint32_t)state] = value;
        }
        return status;
    }

    if (status == D3D8_RS_OK_DEFERRED) {
        /* MEASURED: 12 of the 44 deferred states carry a zero dirty mask, so they store the
         * shadow and mark nothing. OR-ing zero is the right behaviour and is not a missing
         * table entry; the suite asserts the count so a regeneration that lost the masks
         * cannot pass. */
        rs->dirty |= row->dirty_bit;
        rs->shadow[(uint32_t)state] = value;
        return status;
    }

    /* A handler state. The guest calls a per-state helper here; none is implemented yet, so
     * it is counted and reported instead of being silently dropped. */
    rs->shadow[(uint32_t)state] = value;
    rs->unimplemented_handlers++;
    return D3D8_RS_UNIMPLEMENTED_HANDLER;
}

d3d8_rs_status d3d8_set_render_state(d3d8_render_state *rs, int32_t state, uint32_t value)
{
    return apply(rs, state, value, 1);
}

d3d8_rs_status d3d8_set_render_state_inlined(d3d8_render_state *rs, int32_t state,
                                            uint32_t value)
{
    return apply(rs, state, value, 0);
}

size_t d3d8_apply_render_state_block(d3d8_render_state *rs, const uint32_t *pairs,
                                     size_t count, size_t stride)
{
    if (rs == NULL || pairs == NULL || stride < 2) {
        return count;
    }
    size_t rejected = 0;
    for (size_t index = 0; index < count; index++) {
        const uint32_t *record = pairs + index * stride;
        d3d8_rs_status status =
            d3d8_set_render_state_inlined(rs, (int32_t)record[0], record[1]);
        if (status == D3D8_RS_NEGATIVE || status == D3D8_RS_OUT_OF_RANGE) {
            rejected++;
        }
    }
    return rejected;
}
