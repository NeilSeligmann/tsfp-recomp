/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SetRenderState as the guest actually implements it: one table, one loop.
 *
 * WHY A TABLE AND NOT 92 CASES. On Xbox there is no user-mode graphics driver -- D3D8 is
 * linked into the title and *is* the driver -- and its SetRenderState does not switch on
 * the state. It reads the NV2A command header out of an array indexed by the state number
 * and emits it. So the whole render-state-to-hardware mapping is a data table, it was read
 * out of a real image by `tools/d3dscan/rstable.py`, and reimplementing it means walking
 * that table rather than writing out the cases. `src/gpu/d3d8_render_state.c` is that
 * table, generated and committed; this header is the hand-written part.
 *
 * THREE CLASSES, PARTITIONING THE INDEX SPACE. MEASURED from two separately compiled
 * copies of the same dispatch (the game's inlined one and D3D8's own out-of-line one),
 * which agree on every boundary:
 *
 *   [0, d3d8_rs_immediate_bound)                  emit one header plus the value, now
 *   [immediate_bound, d3d8_rs_deferred_bound)     OR a dirty bit, store the shadow, defer
 *   [deferred_bound, d3d8_rs_handler_bound)       a per-state helper function
 *   anything else                                 out of range
 *
 * THE GUEST'S COMPARE IS SIGNED, WHICH IS NOT A DETAIL. It is `cmp index, bound` followed
 * by `jge`, so a NEGATIVE state also enters the first class and indexes the table
 * backwards off its front, and nothing in the dispatch rejects it. `d3d8_rs_classify`
 * returns D3D8_RS_NEGATIVE for that case rather than silently reproducing an out-of-bounds
 * read, and the suite pins it, because an unsigned reimplementation would diverge here
 * without any symptom until something indexed with a sign-extended value.
 *
 * TWO ENTRY POINTS, BECAUSE THE TWO COMPILED COPIES DIFFER. MEASURED: D3D8's out-of-line
 * SetRenderState stores the shadow on the immediate path as well as emitting, while the
 * copy MSVC inlined into the game's own code emits and does NOT store the shadow. That is
 * a real divergence in observable state, not a compiler artefact, so both are modelled:
 * `d3d8_set_render_state` is the library entry point and `d3d8_set_render_state_inlined`
 * is what the game's own code does. Collapsing them would make a later GetRenderState
 * read right where the original read stale.
 */

#ifndef TSFP_GPU_D3D8_RENDER_STATE_H
#define TSFP_GPU_D3D8_RENDER_STATE_H

#include <stddef.h>
#include <stdint.h>

/* Rows for every index in all three classes. Sized by the generated table so a
 * regeneration against a different build cannot leave the array and the bounds
 * disagreeing silently. */
#define D3D8_RS_TABLE_ROWS 0xA6u

/* Which class an index falls in. The generated table carries this per row so a row and its
 * class can never drift apart. */
typedef enum {
    D3D8_RS_IMMEDIATE = 1,
    D3D8_RS_DEFERRED = 2,
    D3D8_RS_HANDLER = 3
} d3d8_rs_kind;

/* One render state, exactly as the guest's tables describe it.
 *
 * `name_sources` IS PART OF THE DATA, NOT A COMMENT. It is how many independent NV2A
 * register references named `base_name`: 2 means the name is doubly derived, 1 means it
 * rests on a single route and must be read that way, 0 means no local reference named it
 * and `base_name` is NULL. `name_element` is how many dwords past `base_name` the method
 * sits, so a name that was REACHED rather than read says so -- an earlier analysis reached
 * 4 dwords past SET_SHADER_STAGE_PROGRAM and reported a name that is really
 * SET_DOT_RGBMAPPING. The method NUMBER is doubly derived for every row regardless; only
 * the name's evidence varies. */
typedef struct {
    uint16_t method;      /* NV2A pgraph method, 0 unless kind == D3D8_RS_IMMEDIATE */
    uint16_t dirty_bit;   /* OR'd into the dirty word, 0 unless kind == D3D8_RS_DEFERRED */
    uint8_t kind;         /* d3d8_rs_kind */
    uint8_t name_sources; /* independent references naming base_name: 0, 1 or 2 */
    uint8_t name_element; /* dwords from base_name to method; 0 means read, not reached */
    uint8_t name_disputed; /* the naming references disagreed on the spelling */
    const char *base_name;
} d3d8_render_state_row;

/* Generated in d3d8_render_state.c. */
extern const d3d8_render_state_row d3d8_render_state_rows[D3D8_RS_TABLE_ROWS];
extern const uint32_t d3d8_rs_immediate_bound;
extern const uint32_t d3d8_rs_deferred_bound;
extern const uint32_t d3d8_rs_handler_bound;

/* A one-parameter, subchannel-0, auto-incrementing pushbuffer command header.
 *
 * bits 0..12 method, 13..15 subchannel, 18..28 parameter count, bit 30 non-incrementing.
 * Every one of the immediate rows was MEASURED to be exactly this shape, so the raw dwords
 * are not stored: the encoding lives here once and the table holds bare methods. That also
 * keeps the generated file free of anything resembling an address dump. */
#define D3D8_NV2A_HEADER(method) (((uint32_t)(method) & 0x1FFFu) | (1u << 18))

/* What classify/set decided. Never "it worked" by omission: the handler and out-of-range
 * cases are reported so a caller cannot mistake an unimplemented state for a no-op. */
typedef enum {
    D3D8_RS_OK_IMMEDIATE = 0,
    D3D8_RS_OK_DEFERRED = 1,
    D3D8_RS_UNIMPLEMENTED_HANDLER = 2,
    D3D8_RS_OUT_OF_RANGE = 3,
    /* The guest's signed compare would accept this and read off the front of the table. */
    D3D8_RS_NEGATIVE = 4
} d3d8_rs_status;

/* Where an immediate state's header and value go. Modelling the sink as a callback keeps
 * this module independent of the pushbuffer, which is what lets the suite run with no GPU
 * and no guest memory. */
typedef void (*d3d8_rs_emit_fn)(void *context, uint32_t header, uint32_t value);

/* The live state the dispatch reads and writes. In the guest these are D3D8 globals at
 * fixed addresses that GAME CODE INDEXES DIRECTLY, so an HLE has to keep them somewhere
 * the guest can still see; that is a memory-model constraint recorded in
 * docs/d3d8-usage.md, not something this struct can settle. */
typedef struct {
    uint32_t shadow[D3D8_RS_TABLE_ROWS];
    uint32_t dirty;
    d3d8_rs_emit_fn emit;
    void *emit_context;
    /* Counted, not logged: a test can assert a handler was reached without a log sink. */
    uint32_t unimplemented_handlers;
    uint32_t rejected;
} d3d8_render_state;

/* The row for `state`, or NULL when it is outside the table. */
const d3d8_render_state_row *d3d8_rs_row(int32_t state);

/* Which class `state` is in, with no side effects. */
d3d8_rs_status d3d8_rs_classify(int32_t state);

void d3d8_render_state_init(d3d8_render_state *rs, d3d8_rs_emit_fn emit, void *context);

/* D3D8's out-of-line SetRenderState: emits on the immediate path AND stores the shadow. */
d3d8_rs_status d3d8_set_render_state(d3d8_render_state *rs, int32_t state, uint32_t value);

/* The copy MSVC inlined into the game's own code: emits WITHOUT storing the shadow. */
d3d8_rs_status d3d8_set_render_state_inlined(d3d8_render_state *rs, int32_t state,
                                             uint32_t value);

/* Apply a (state, value) run the way the game's own state blocks are flushed: the game
 * walks a table of pairs in its own data and calls the dispatch once per pair. `stride` is
 * in dwords between successive state fields, because the two call sites MEASURED use
 * different record sizes over the same dispatch. Returns how many pairs were rejected. */
size_t d3d8_apply_render_state_block(d3d8_render_state *rs, const uint32_t *pairs,
                                     size_t count, size_t stride);

#endif /* TSFP_GPU_D3D8_RENDER_STATE_H */
