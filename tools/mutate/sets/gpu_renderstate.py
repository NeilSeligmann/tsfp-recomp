"""Mutations for the D3D8 render-state table and the dispatch that walks it.

WHAT THIS SET IS REALLY FOR. `src/gpu/d3d8_render_state_table.c` is 166 rows recovered from
a real image, and `docs/d3d8-usage.md` §2.1 records the failure mode a table of that shape
has: a previous NV2A method table was wrong in 46 of 102 checkable entries, OFF BY ONE SLOT,
so every entry held a real method name sitting on the wrong number and nothing about it
looked wrong. A suite can assert 92 rows and still be blind to that. So the first four
mutations below are the four ways the table can be wrong without looking wrong:

    gpu-rs-table-off-by-one-slot      the exact defect that got 46 of 102 wrong
    gpu-rs-method-byte-swapped        a transcription error with the right digits
    gpu-rs-dirty-bit-wrong-state      one bit moved between two states, both still plausible
    gpu-rs-immediate-bound-off-by-one the loop bound at the 0x5C class edge

The rest cover the behaviours the table alone cannot encode: the header bit-packing, the
signedness of the guest's range check, and the MEASURED divergence between D3D8's
out-of-line SetRenderState and the copy MSVC inlined into the game (one stores the shadow on
the immediate path, the other does not).

WHY `name_sources` IS MUTATED. The brief this work answers requires that anything resting on
a single derivation route be marked in the table rather than silently included. Promoting a
single-source name to doubly derived is therefore a real defect, not a cosmetic one, and it
is the one defect that would make the table's own honesty claim false.
"""

MUTATIONS: list[dict] = [
    {
        "id": "gpu-rs-table-off-by-one-slot",
        "file": "src/gpu/d3d8_render_state_table.c",
        "old": '    [0x00] = { 0x0260, 0, D3D8_RS_IMMEDIATE, 2, 0, 0, "SET_COMBINER_ALPHA_ICW" },',
        "new": '    [0x00] = { 0x0264, 0, D3D8_RS_IMMEDIATE, 2, 1, 0, "SET_COMBINER_ALPHA_ICW" },',
        "targets": ["test_d3d8_render_state"],
        "why": "the whole table is one slot off and every row still names a real NV2A "
        "register. If this survives, nothing in the suite pins the slot alignment and the "
        "exact defect that got 46 of 102 entries wrong elsewhere is undetectable here.",
    },
    {
        "id": "gpu-rs-method-byte-swapped",
        "file": "src/gpu/d3d8_render_state_table.c",
        "old": "    [0x08] = { 0x0288, 0, D3D8_RS_IMMEDIATE, 2, 0, 0, "
        '"SET_COMBINER_SPECULAR_FOG_CW0" },',
        "new": "    [0x08] = { 0x8802, 0, D3D8_RS_IMMEDIATE, 2, 0, 0, "
        '"SET_COMBINER_SPECULAR_FOG_CW0" },',
        "targets": ["test_d3d8_render_state"],
        "why": "a method transcribed with its bytes swapped keeps all the right digits. "
        "Surviving means nothing checks that a method is 4-aligned and inside pgraph's "
        "0x2000 window, so any corrupted number would be emitted to the hardware.",
    },
    {
        "id": "gpu-rs-dirty-bit-wrong-state",
        "file": "src/gpu/d3d8_render_state_table.c",
        "old": "    [0x5c] = { 0, 0x2000, D3D8_RS_DEFERRED, 0, 0, 0, NULL },",
        "new": "    [0x5c] = { 0, 0x1000, D3D8_RS_DEFERRED, 0, 0, 0, NULL },",
        "targets": ["test_d3d8_render_state"],
        "why": "both masks occur elsewhere in the table, so this state and every other "
        "state individually still hold a plausible bit. Surviving means the deferred flush "
        "would mark the wrong hardware group dirty and the suite could not tell.",
    },
    {
        "id": "gpu-rs-immediate-bound-off-by-one",
        "file": "src/gpu/d3d8_render_state_table.c",
        "old": "const uint32_t d3d8_rs_immediate_bound = 0x5c;",
        "new": "const uint32_t d3d8_rs_immediate_bound = 0x5b;",
        "targets": ["test_d3d8_render_state"],
        "why": "state 0x5B would be deferred instead of emitted. This is the 0x5C class edge "
        "the guest's own `cmp index, 0x5C` draws, and it is the single number the whole "
        "data-driven design rests on.",
    },
    {
        "id": "gpu-rs-classify-bound-inclusive",
        "file": "src/gpu/d3d8_render_state.c",
        "old": "    if (index < d3d8_rs_immediate_bound) {",
        "new": "    if (index <= d3d8_rs_immediate_bound) {",
        "targets": ["test_d3d8_render_state"],
        "why": "the same off-by-one from the other side: the bound is right and the "
        "comparison is wrong, so state 0x5C emits a header read from a deferred row whose "
        "method is zero. Surviving means the class edge is only checked from one side.",
    },
    {
        "id": "gpu-rs-handler-bound-shrunk",
        "file": "src/gpu/d3d8_render_state_table.c",
        "old": "const uint32_t d3d8_rs_handler_bound   = 0xa6;",
        "new": "const uint32_t d3d8_rs_handler_bound   = 0xa5;",
        "targets": ["test_d3d8_render_state"],
        "why": "the last handler state becomes out of range. That bound is the one not read "
        "from a compare in the guest -- neither compiled copy bounds the handler class -- so "
        "it rests entirely on where the pointer table stops holding function entries.",
    },
    {
        "id": "gpu-rs-row-bound-inclusive",
        "file": "src/gpu/d3d8_render_state.c",
        "old": "    if (state < 0 || (uint32_t)state >= d3d8_rs_handler_bound) {",
        "new": "    if (state < 0 || (uint32_t)state > d3d8_rs_handler_bound) {",
        "targets": ["test_d3d8_render_state"],
        "why": "d3d8_rs_row would hand out a pointer one past the end of the table. "
        "Surviving means the accessor's range check is untested and a caller walking it "
        "would read whatever the linker placed next.",
    },
    {
        "id": "gpu-rs-header-count-field-moved",
        "file": "src/gpu/d3d8_render_state.h",
        "old": "#define D3D8_NV2A_HEADER(method) (((uint32_t)(method) & 0x1FFFu) | (1u << 18))",
        "new": "#define D3D8_NV2A_HEADER(method) (((uint32_t)(method) & 0x1FFFu) | (1u << 19))",
        "targets": ["test_d3d8_render_state"],
        "why": "the parameter count lands in bit 19, so every header claims two parameters "
        "instead of one and the hardware would consume the next dword as a parameter. The "
        "table holds bare methods precisely so this encoding lives in one place; if a "
        "mutation of that one place survives, the choice bought nothing.",
    },
    {
        "id": "gpu-rs-negative-state-not-reported",
        "file": "src/gpu/d3d8_render_state.c",
        "old": "    if (state < 0) {\n        return D3D8_RS_NEGATIVE;",
        "new": "    if (state < -1) {\n        return D3D8_RS_NEGATIVE;",
        "targets": ["test_d3d8_render_state"],
        "why": "state -1 would report OUT_OF_RANGE instead of NEGATIVE. The guest's compare "
        "is SIGNED, so -1 really does reach its immediate class and index the header table "
        "backwards; collapsing the two statuses erases the one place where this "
        "implementation knowingly diverges from the original.",
    },
    {
        "id": "gpu-rs-inlined-copy-stores-shadow",
        "file": "src/gpu/d3d8_render_state.c",
        "old": "    return apply(rs, state, value, 0);",
        "new": "    return apply(rs, state, value, 1);",
        "targets": ["test_d3d8_render_state"],
        "why": "the inlined copy would store the shadow, which MEASURED it does not. A later "
        "GetRenderState would then return the right value where the original returned a "
        "stale one, and that is a divergence no later test could attribute to this.",
    },
    {
        "id": "gpu-rs-zero-dirty-mask-forced-nonzero",
        "file": "src/gpu/d3d8_render_state.c",
        "old": "        rs->dirty |= row->dirty_bit;",
        "new": "        rs->dirty |= row->dirty_bit ? row->dirty_bit : 1u;",
        "targets": ["test_d3d8_render_state"],
        "why": "12 of the 44 deferred states carry a zero mask, which is MEASURED and not a "
        "gap in the table. Treating zero as 'must mean something' would mark a group dirty "
        "that the original never marks, flushing state the guest did not ask to flush.",
    },
    {
        "id": "gpu-rs-single-source-name-promoted",
        "file": "src/gpu/d3d8_render_state_table.c",
        "old": '    [0x53] = { 0x147C, 0, D3D8_RS_IMMEDIATE, 1, 0, 0, "SET_STIPPLE_ENABLE" },',
        "new": '    [0x53] = { 0x147C, 0, D3D8_RS_IMMEDIATE, 2, 0, 0, "SET_STIPPLE_ENABLE" },',
        "targets": ["test_d3d8_render_state"],
        "why": "a name only ONE NV2A reference carries would claim to be doubly derived. "
        "This is the mutation that matters most for the table's honesty: if it survives, "
        "`name_sources` is decoration and a reader cannot tell a corroborated name from a "
        "single-sourced one.",
    },
]
