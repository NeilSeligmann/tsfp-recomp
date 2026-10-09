# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the Swap copy composition decode (T462, T541).

The SURFACE, FIXED, TEXTURE and IMMEDIATE output groups of `gpu_pgraph` (the SetRenderTarget packet, the fixed-function
words pinned to the measured values, the texture stage words, immediate vertices), their resolve functions in
`gpu_pgraph_replay.c` and the SPIR-V rewrite that gives an unwritten varying its initial value. Grouped by what a
survivor would let through:

    sync       NO_OPERATION and WAIT_FOR_IDLE: value 0 only, never in a bracket, counted
    table      the method of every decoded word and the group it belongs to
    inline     the immediate vertex path: the emit rule, its refusals, the bytes, the disabled slots
    surface    the surface format fields and the pinned CONTROL0, clip range and anti-aliasing words
    fixed      the pinned enables, the polygon modes, the light control mask and the z-cull range
    texture    the measured address, control and filter words and the zero bump environment
    wiring     the groups' calls from `gpu_pgraph_resolve_output`, the group names, the inference the host allows
    spirv      the Input to Private rewrite of a fragment module

The kills are by `test_gpu_pgraph_composition` (no device), `test_d3d8_swap_replay` for the host inference mask.

EQUIVALENT, not mutated: `bracket_inline` and `inline_count` are reset at BEGIN_END only (nothing reads them outside a bracket), the 24 word allowance per defaulted input of the rewrite (an allocation size, a smaller one would be caught
by a sanitizer, not by a value), and the frame profile's `gpu_pgraph_set_output_groups(pgraph, IMMEDIATE)` (a ctest
binary does not boot the movie loop, `tests/test_xmv_replay.py` does and asserts its census).

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

_PGRAPH = "src/gpu/gpu_pgraph.c"
_PGRAPH_H = "src/gpu/gpu_pgraph.h"
_REPLAY = "src/gpu/gpu_pgraph_replay.c"
_REPLAY_H = "src/gpu/gpu_pgraph_replay.h"
_COMBINER = "src/gpu/gpu_combiner.c"
_TEST = ["test_gpu_pgraph_composition"]
_SWAP_TEST = ["test_d3d8_swap_replay"]


def _row(
    file: str, mutation_id: str, old: str, new: str, why: str, targets: list[str] = _TEST
) -> dict:
    return {
        "id": f"gpu-composition-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(targets),
        "why": why,
    }


def _pg(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_PGRAPH, mutation_id, old, new, why)


def _rp(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_REPLAY, mutation_id, old, new, why)


def _sp(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_COMBINER, mutation_id, old, new, why)


MUTATIONS: list[dict] = [
    # ================================================================== sync
    _pg(
        "sync-nonzero-accepted",
        '        if (data != 0u) {\n            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,\n                          "NO_OPERATION or WAIT_FOR_IDLE data word other than 0',
        '        if (false) {\n            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,\n                          "NO_OPERATION or WAIT_FOR_IDLE data word other than 0',
        "an unsupported nonzero sync payload is decoded as a no-op instead of refused.",
    ),
    _pg(
        "sync-in-bracket-accepted",
        '        if (pgraph->in_bracket) {\n            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,\n                          "a NO_OPERATION or WAIT_FOR_IDLE inside',
        '        if (false) {\n            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,\n                          "a NO_OPERATION or WAIT_FOR_IDLE inside',
        "a wait for idle inside a bracket is accepted.",
    ),
    _pg(
        "sync-not-counted",
        "        pgraph->stats.sync_pairs++;\n",
        "",
        "the decoded no-ops are not counted, so a run cannot show how many it skipped.",
    ),
    _pg(
        "sync-wait-for-idle-dropped",
        "(method == GPU_PGRAPH_NO_OPERATION || method == GPU_PGRAPH_WAIT_FOR_IDLE)) {",
        "(method == GPU_PGRAPH_NO_OPERATION)) {",
        "WAIT_FOR_IDLE stays unhandled and a strict replay refuses the bind packet again.",
    ),
    _pg(
        "sync-group-ignored",
        "    if ((pgraph->output_groups & GPU_PGRAPH_OUTPUT_SURFACE) != 0u &&\n        (method == GPU_PGRAPH_NO_OPERATION",
        "    if (true &&\n        (method == GPU_PGRAPH_NO_OPERATION",
        "the sync pairs are decoded with the group off.",
    ),
    # ================================================================== table
    _pg(
        "table-format",
        "[GPU_PGRAPH_OUT_SURFACE_FORMAT] = {0x0208u,",
        "[GPU_PGRAPH_OUT_SURFACE_FORMAT] = {0x0228u,",
        "the surface format method is wrong.",
    ),
    _pg(
        "table-pitch",
        "[GPU_PGRAPH_OUT_SURFACE_PITCH] = {0x020Cu,",
        "[GPU_PGRAPH_OUT_SURFACE_PITCH] = {0x021Cu,",
        "the surface pitch method is wrong.",
    ),
    _pg(
        "table-control0",
        "[GPU_PGRAPH_OUT_CONTROL0] = {0x0290u,",
        "[GPU_PGRAPH_OUT_CONTROL0] = {0x0298u,",
        "CONTROL0 is at the wrong method.",
    ),
    _pg(
        "table-clip-max",
        "[GPU_PGRAPH_OUT_CLIP_MAX] = {0x0398u,",
        "[GPU_PGRAPH_OUT_CLIP_MAX] = {0x0399u,",
        "the clip maximum method is wrong.",
    ),
    _pg(
        "table-anti-aliasing",
        "[GPU_PGRAPH_OUT_ANTI_ALIASING] = {0x1D7Cu,",
        "[GPU_PGRAPH_OUT_ANTI_ALIASING] = {0x1D78u,",
        "the anti-aliasing control takes the z clamp control's method.",
    ),
    _pg(
        "table-zcull",
        "[GPU_PGRAPH_OUT_ZCULL_ENABLE] = {0x1D84u,",
        "[GPU_PGRAPH_OUT_ZCULL_ENABLE] = {0x1D80u,",
        "the z-cull enable takes the z buffer compression's method.",
    ),
    _pg(
        "table-fog",
        "[GPU_PGRAPH_OUT_FOG_ENABLE] = {0x02A4u, GPU_PGRAPH_OUTPUT_FIXED}",
        "[GPU_PGRAPH_OUT_FOG_ENABLE] = {0x02A4u, GPU_PGRAPH_OUTPUT_SURFACE}",
        "the fog enable sits in the wrong group.",
    ),
    _pg(
        "table-point-size",
        "[GPU_PGRAPH_OUT_POINT_SIZE] = {0x043Cu,",
        "[GPU_PGRAPH_OUT_POINT_SIZE] = {0x0438u,",
        "the point size method is wrong.",
    ),
    _pg(
        "table-texture-filter-stride",
        "[GPU_PGRAPH_OUT_TEXTURE_FILTER + 1] = {0x1B54u,",
        "[GPU_PGRAPH_OUT_TEXTURE_FILTER + 1] = {0x1B34u,",
        "the filter word of stages 1 to 3 is at the wrong method.",
    ),
    _pg(
        "table-texture-address-base",
        "[GPU_PGRAPH_OUT_TEXTURE_ADDRESS + 0] = {0x1B08u,",
        "[GPU_PGRAPH_OUT_TEXTURE_ADDRESS + 0] = {0x1B04u,",
        "the address word takes the format word's method.",
    ),
    _pg(
        "table-bump-base",
        "[GPU_PGRAPH_OUT_TEXTURE_BUMP + 0] = {0x1B68u,",
        "[GPU_PGRAPH_OUT_TEXTURE_BUMP + 0] = {0x1B64u,",
        "the first bump matrix word takes the border colour's method.",
    ),
    _pg(
        "table-bump-offset",
        "[GPU_PGRAPH_OUT_TEXTURE_BUMP + 17] = {0x1BFCu,",
        "[GPU_PGRAPH_OUT_TEXTURE_BUMP + 17] = {0x1C00u,",
        "the bump offset word of every stage is at the next stage's first word.",
    ),
    _pg(
        "table-output-mask-drops-group",
        "(pgraph->output_groups & output_group) != 0u) {\n        if (pgraph->in_bracket) {",
        "((pgraph->output_groups & output_group) != 0u || output_word >= (int)GPU_PGRAPH_OUT_LIGHTING_ENABLE)) {\n        if (pgraph->in_bracket) {",
        "the FIXED words are decoded with the group off.",
    ),
    # ================================================================== inline
    _pg(
        "inline-group-ignored",
        "    if ((pgraph->output_groups & GPU_PGRAPH_OUTPUT_IMMEDIATE) != 0u && method >=",
        "    if (true && method >=",
        "the immediate vertex methods are decoded with the group off.",
    ),
    _pg(
        "inline-outside-bracket",
        '    if (!pgraph->in_bracket) {\n        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,\n                      "vertex data written outside',
        '    if (false) {\n        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,\n                      "vertex data written outside',
        "vertex data outside a bracket is latched.",
    ),
    _pg(
        "inline-x-not-pending",
        "        pgraph->inline_x_pending[slot] = true;\n",
        "",
        "a y word is never matched to an x word before it.",
    ),
    _pg(
        "inline-y-without-x",
        "    if (!pgraph->inline_x_pending[slot]) {",
        "    if (false) {",
        "a y word with no x word before it completes an attribute.",
    ),
    _pg(
        "inline-emit-on-wrong-slot",
        "    if (slot != 0u) {\n        return GPU_PGRAPH_OK;\n    }\n    if (!pgraph->bracket_inline",
        "    if (slot != 9u) {\n        return GPU_PGRAPH_OK;\n    }\n    if (!pgraph->bracket_inline",
        "the texture coordinate's write emits the vertex instead of the position's.",
    ),
    _pg(
        "inline-emit-on-every-slot",
        "    if (slot != 0u) {\n        return GPU_PGRAPH_OK;\n    }\n    if (!pgraph->bracket_inline",
        "    if (false) {\n        return GPU_PGRAPH_OK;\n    }\n    if (!pgraph->bracket_inline",
        "every attribute write emits a vertex.",
    ),
    _pg(
        "inline-mixed-with-arrays",
        "    if (!pgraph->bracket_inline && pgraph->index_count != pgraph->bracket_first_index) {",
        "    if (false) {",
        "an immediate vertex joins a bracket that already holds array vertices.",
    ),
    _pg(
        "inline-slot-set-may-grow",
        "    } else if (mask != pgraph->bracket_inline_mask) {",
        "    } else if (false) {",
        "an attribute first written after the first vertex is accepted, the earlier vertices read it as 0.",
    ),
    _pg(
        "inline-vertex-bound-off-by-one",
        "    if (pgraph->inline_count >= GPU_PGRAPH_MAX_INLINE_VERTICES) {",
        "    if (pgraph->inline_count > GPU_PGRAPH_MAX_INLINE_VERTICES) {",
        "the bracket may hold one vertex more than the bound.",
    ),
    _pg(
        "inline-arrays-after-immediate",
        '        if (pgraph->bracket_inline) {\n            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,\n                          "array vertices in a bracket that already holds immediate',
        '        if (false) {\n            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,\n                          "array vertices in a bracket that already holds immediate',
        "array vertices follow immediate vertices in one bracket.",
    ),
    _pg(
        "inline-not-marked",
        "    draw->inline_vertices = true;\n    draw->vertices_captured = true;",
        "    draw->vertices_captured = true;",
        "the draw does not say its vertices are immediate, so the replay would read the guest pool.",
    ),
    _pg(
        "inline-slot-bytes",
        "pgraph->inline_buffer + vertex * INLINE_FLOATS_PER_VERTEX + slot * 2u, 8u);",
        "pgraph->inline_buffer + vertex * INLINE_FLOATS_PER_VERTEX + slot * 2u, 4u);",
        "only the x component of each vertex is copied.",
    ),
    _pg(
        "inline-slot-index",
        "pgraph->inline_buffer + vertex * INLINE_FLOATS_PER_VERTEX + slot * 2u, 8u);",
        "pgraph->inline_buffer + vertex * INLINE_FLOATS_PER_VERTEX + slot, 8u);",
        "the per slot copy overlaps its neighbour.",
    ),
    _pg(
        "inline-range-bytes",
        "draw->vertices[slot].bytes = (uint32_t)(count * 8u);",
        "draw->vertices[slot].bytes = (uint32_t)(count * 4u);",
        "the vertex range is half what was copied, the replay refuses the last vertices.",
    ),
    _pg(
        "inline-format-size",
        "#define INLINE_ARRAY_FORMAT ((8u << 8) | (2u << 4) | GPU_PGRAPH_TYPE_F)",
        "#define INLINE_ARRAY_FORMAT ((8u << 8) | (3u << 4) | GPU_PGRAPH_TYPE_F)",
        "the vertices are described as three components.",
    ),
    _pg(
        "inline-format-stride",
        "#define INLINE_ARRAY_FORMAT ((8u << 8) | (2u << 4) | GPU_PGRAPH_TYPE_F)",
        "#define INLINE_ARRAY_FORMAT ((16u << 8) | (2u << 4) | GPU_PGRAPH_TYPE_F)",
        "the stride is twice the packed vertex.",
    ),
    _pg(
        "inline-disabled-slot-format",
        "        draw->arrays[slot] = (gpu_pgraph_array){.format_set = true}; /* format 0: size 0, disabled */",
        "        draw->arrays[slot] = (gpu_pgraph_array){0}; /* format 0: size 0, disabled */",
        "a slot nothing wrote carries no format at all instead of a disabled one.",
    ),
    _pg(
        "inline-persist-reset",
        "    memset(pgraph->inline_written, 0, sizeof pgraph->inline_written);\n    memset(pgraph->inline_x_pending",
        "    memset(pgraph->inline_x_pending",
        "the latched attributes survive gpu_pgraph_reset.",
    ),
    _pg(
        "inline-draw-uncounted",
        "    pgraph->stats.inline_draws += pgraph->bracket_inline ? 1u : 0u;\n",
        "",
        "the draws made of immediate vertices are not counted.",
    ),
    _pg(
        "inline-pool-by-guest-pool",
        "return (draw->inline_vertices ? pgraph->inline_pool : pgraph->pool) + draw->vertices[slot].offset;",
        "return pgraph->pool + draw->vertices[slot].offset;",
        "an immediate draw's bytes are read from the guest snapshot pool.",
    ),
    # ================================================================== surface
    _rp(
        "surface-format-colour",
        "if ((format & 0xFu) != 8u ||",
        "if ((format & 0xFu) != 7u ||",
        "an A8R8G8B8 target is refused and X8R8G8B8 accepted.",
    ),
    _rp(
        "surface-format-zeta",
        "((format >> 4) & 0xFu) != 2u ||",
        "((format >> 4) & 0xFu) != 1u ||",
        "a Z16 target is accepted.",
    ),
    _rp(
        "surface-format-type",
        "(((format >> 8) & 0xFu) != 1u && !swizzled)",
        "false",
        "a swizzled target is accepted.",
    ),
    _rp(
        "surface-format-antialiasing",
        "((format >> 12) & 0xFu) != 0u ||",
        "false ||",
        "an antialiased target is accepted.",
    ),
    _rp(
        "surface-format-size",
        "(!swizzled && (format >> 16) != 0u)",
        "false",
        "a swizzle size field is accepted in the pitch layout.",
    ),
    _rp(
        "surface-control0",
        "GPU_PGRAPH_OUT_CONTROL0, 0x00100001u,",
        "GPU_PGRAPH_OUT_CONTROL0, 0x00100000u,",
        "the measured CONTROL0 is refused.",
    ),
    _rp(
        "surface-clip-min",
        "GPU_PGRAPH_OUT_CLIP_MIN, 0x00000000u,",
        "GPU_PGRAPH_OUT_CLIP_MIN, 0x00000001u,",
        "the measured clip minimum is refused.",
    ),
    _rp(
        "surface-clip-max",
        "GPU_PGRAPH_OUT_CLIP_MAX, 0x4B7FFFFFu,",
        "GPU_PGRAPH_OUT_CLIP_MAX, 0x4B800000u,",
        "the measured clip maximum is refused.",
    ),
    _rp(
        "surface-anti-aliasing-mask",
        "(control >> 16) != 0xFFFFu)) {",
        "(control >> 16) != 0xFFFEu)) {",
        "the measured full sample mask is refused.",
    ),
    _rp(
        "surface-anti-aliasing-zero",
        "((control >> 16) != 0u && (control >> 16) != 0xFFFFu)",
        "((control >> 16) != 0xFFFFu)",
        "the measured zero sample mask is refused.",
    ),
    _rp(
        "surface-anti-aliasing-low-bits",
        "if ((control & 0x0000FFFFu) != 0u ||",
        "if (false ||",
        "an anti-aliasing enable bit is accepted.",
    ),
    _rp(
        "surface-needs-no-inference",
        "    if (result != GPU_PGRAPH_OK || !any) {\n        return result;\n    }\n    return need_inference(",
        "    if (result != GPU_PGRAPH_OK) {\n        return result;\n    }\n    return need_inference(",
        "a draw with none of the surface words written still needs the inference.",
    ),
    _rp(
        "surface-any-ignores-format",
        "        any = any || state->output_written[word];\n    }\n    if (state->output_written[GPU_PGRAPH_OUT_SURFACE_FORMAT]) {",
        "        any = any && state->output_written[word];\n    }\n    if (state->output_written[GPU_PGRAPH_OUT_SURFACE_FORMAT]) {",
        "a stream that wrote only some surface words uses no inference.",
    ),
    # ================================================================== fixed
    _rp(
        "fixed-lighting",
        "{GPU_PGRAPH_OUT_LIGHTING_ENABLE, 0u,",
        "{GPU_PGRAPH_OUT_LIGHTING_ENABLE, 1u,",
        "a lit stream is accepted and the unlit one refused.",
    ),
    _rp(
        "fixed-fog",
        "{GPU_PGRAPH_OUT_FOG_ENABLE, 0u,",
        "{GPU_PGRAPH_OUT_FOG_ENABLE, 1u,",
        "a fogged draw is accepted.",
    ),
    _rp(
        "fixed-point-smooth",
        "{GPU_PGRAPH_OUT_POINT_SMOOTH_ENABLE, 0u,",
        "{GPU_PGRAPH_OUT_POINT_SMOOTH_ENABLE, 1u,",
        "smooth points are accepted.",
    ),
    # T860 replaced the two pinned-FILL rows (`fixed-front-polygon-mode`, `fixed-back-polygon-mode`: LINE and POINT were refused) by
    # `resolve_polygon_mode`, which accepts POINT, LINE and FILL (xemu draws all three), so those two mutants had no target left.
    # The three rows below mutate the new rule and are killed by its test in `test_gpu_pgraph_state` (T860 polygon mode cases).
    _row(
        _REPLAY,
        "polygon-mode-line-read-as-fill",
        "        case 0x1B01u: modes[i] = GPU_VSH_POLYGON_LINE; break;",
        "        case 0x1B01u: modes[i] = GPU_VSH_POLYGON_FILL; break;",
        "a LINE polygon mode draws filled.",
        ["test_gpu_pgraph_state"],
    ),
    _row(
        _REPLAY,
        "polygon-mode-back-word-unread",
        "    for (size_t i = 0u; i < 2u; i++) {\n        if (!state->output_written[words[i].word]) {",
        "    for (size_t i = 0u; i < 1u; i++) {\n        if (!state->output_written[words[i].word]) {",
        "the back polygon mode word is never read, so an equal LINE pair is refused as differing.",
        ["test_gpu_pgraph_state"],
    ),
    _row(
        _REPLAY,
        "polygon-mode-differ-accepted",
        "    if (modes[0] != modes[1]) {",
        "    if (modes[0] != modes[1] && 0) {",
        "a front and back polygon mode that differ (xemu asserts equal) are accepted.",
        ["test_gpu_pgraph_state"],
    ),
    _rp(
        "fixed-specular",
        "{GPU_PGRAPH_OUT_SPECULAR_ENABLE, 0u,",
        "{GPU_PGRAPH_OUT_SPECULAR_ENABLE, 1u,",
        "specular lighting is accepted.",
    ),
    _rp(
        "fixed-light-mask",
        "{GPU_PGRAPH_OUT_LIGHT_ENABLE_MASK, 0u,",
        "{GPU_PGRAPH_OUT_LIGHT_ENABLE_MASK, 1u,",
        "an enabled light is accepted.",
    ),
    _rp(
        "fixed-point-params",
        "{GPU_PGRAPH_OUT_POINT_PARAMS_ENABLE, 0u,",
        "{GPU_PGRAPH_OUT_POINT_PARAMS_ENABLE, 1u,",
        "point parameters are accepted.",
    ),
    _rp(
        "fixed-light-control-mask",
        "if ((control & ~0x00030001u) != 0u) {",
        "if ((control & ~0x00070001u) != 0u) {",
        "a light control bit beyond the three named ones is accepted.",
    ),
    _rp(
        "fixed-light-control-bits",
        "if ((control & ~0x00030001u) != 0u) {",
        "if ((control & ~0x00020001u) != 0u) {",
        "the local eye bit is refused.",
    ),
    _rp(
        "fixed-zcull-range",
        "if (state->output[GPU_PGRAPH_OUT_ZCULL_ENABLE] > 3u) {",
        "if (state->output[GPU_PGRAPH_OUT_ZCULL_ENABLE] > 4u) {",
        "a z-cull enable of 4 is accepted.",
    ),
    _rp(
        "fixed-zcull-upper",
        "if (state->output[GPU_PGRAPH_OUT_ZCULL_ENABLE] > 3u) {",
        "if (state->output[GPU_PGRAPH_OUT_ZCULL_ENABLE] > 2u) {",
        "the measured z-cull value 3 is refused.",
    ),
    _rp(
        "fixed-point-size-not-any",
        "    bool any = state->output_written[GPU_PGRAPH_OUT_POINT_SIZE] || state->output_written[GPU_PGRAPH_OUT_FRONT_POLYGON_MODE] ||",
        "    bool any = state->output_written[GPU_PGRAPH_OUT_FRONT_POLYGON_MODE] ||",
        "a stream that wrote only the point size needs no inference.",
    ),
    # ================================================================== texture
    _rp(
        "texture-address-wrap",
        "address != 0x00000101u && address != 0x00000303u",
        "address != 0x00000303u",
        "the measured wrap address is refused.",
    ),
    _rp(
        "texture-address-clamp",
        "address != 0x00000101u && address != 0x00000303u",
        "address != 0x00000101u",
        "the measured clamp address is refused.",
    ),
    _rp(
        "texture-address-any",
        "if (address != 0u && address != 0x00000101u",
        "if (address > 0x00000303u && address != 0x00000101u",
        "an address mode between the measured ones is accepted.",
    ),
    _rp(
        "texture-control-enable-bit",
        "if ((control & ~0x40000030u) != 0x0003FFC0u) {",
        "if (control != 0x0003FFC0u) {",
        "the enabled stage's control word is refused.",
    ),
    _rp(
        "texture-control-any-bit",
        "if ((control & ~0x40000030u) != 0x0003FFC0u) {",
        "if ((control & ~0x40000034u) != 0x0003FFC0u) {",
        "alpha kill is accepted.",
    ),
    _rp(
        "texture-control-lod",
        "if ((control & ~0x40000030u) != 0x0003FFC0u) {",
        "if ((control & ~0x4003FFF0u) != 0u) {",
        "any LOD clamp is accepted.",
    ),
    _rp(
        "texture-filter",
        "!= 0x02062000u) {",
        "!= 0x02062001u) {",
        "the measured filter is refused.",
    ),
    _rp(
        "texture-bump-nonzero",
        "if (state->output[GPU_PGRAPH_OUT_TEXTURE_BUMP + index] != 0u) {",
        "if (state->output[GPU_PGRAPH_OUT_TEXTURE_BUMP + index] > 1u) {",
        "a bump environment word of 1 is accepted.",
    ),
    _rp(
        "texture-bump-last-word",
        "    for (uint32_t index = 0u; index < 18u; index++) {",
        "    for (uint32_t index = 0u; index < 17u; index++) {",
        "the last bump word of stage 3 is not checked.",
    ),
    _rp(
        "texture-stages",
        "    for (uint32_t stage = 0u; stage < 4u; stage++) {\n        const uint32_t address = state->output[GPU_PGRAPH_OUT_TEXTURE_ADDRESS + stage];",
        "    for (uint32_t stage = 0u; stage < 3u; stage++) {\n        const uint32_t address = state->output[GPU_PGRAPH_OUT_TEXTURE_ADDRESS + stage];",
        "stage 3's words are not checked.",
    ),
    # ================================================================== wiring
    _rp(
        "wiring-surface-skipped",
        "if ((backend->output_groups & GPU_PGRAPH_OUTPUT_SURFACE) != 0u) {\n        pinned =",
        "if (false) {\n        pinned =",
        "the surface words are never resolved.",
    ),
    _rp(
        "wiring-fixed-skipped",
        "pinned == GPU_PGRAPH_OK && (backend->output_groups & GPU_PGRAPH_OUTPUT_FIXED) != 0u) {",
        "pinned == GPU_PGRAPH_OK && false) {",
        "the fixed-function words are never resolved.",
    ),
    _rp(
        "wiring-texture-skipped",
        "pinned == GPU_PGRAPH_OK && (backend->output_groups & GPU_PGRAPH_OUTPUT_TEXTURE) != 0u) {",
        "pinned == GPU_PGRAPH_OK && false) {",
        "the texture words are never resolved.",
    ),
    _rp(
        "wiring-surface-name",
        '{GPU_PGRAPH_OUTPUT_SURFACE, "surface (render target packet)"},',
        '{0u, "surface (render target packet)"},',
        "a stream that wrote surface words with the group off in the backend is accepted.",
    ),
    _rp(
        "wiring-fixed-name",
        '{GPU_PGRAPH_OUTPUT_FIXED, "fixed-function"},',
        '{0u, "fixed-function"},',
        "fixed-function words are applied with the group off.",
    ),
    _rp(
        "wiring-texture-name",
        '{GPU_PGRAPH_OUTPUT_TEXTURE, "texture stage"},',
        '{0u, "texture stage"},',
        "texture words are applied with the group off.",
    ),
    _rp(
        "wiring-refusal-leaves-output",
        "    if (pinned != GPU_PGRAPH_OK) {\n        memset(out, 0, sizeof *out);\n    }",
        "    if (pinned != GPU_PGRAPH_OK) {\n        out->used_inferences |= 1u;\n    }",
        "a refusal leaves a half resolved output behind.",
    ),
    _rp(
        "wiring-immediate-group",
        "        if ((backend->output_groups & GPU_PGRAPH_OUTPUT_IMMEDIATE) == 0u) {\n            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,\n                        \"the draw's vertices were written as immediate",
        "        if (false) {\n            return fail(report, GPU_PGRAPH_ERR_UNMEASURED,\n                        \"the draw's vertices were written as immediate",
        "an immediate draw replays with the group off in the backend.",
    ),
    _rp(
        "wiring-immediate-inference",
        "            backend, GPU_PGRAPH_INFER_OUTPUT_IMMEDIATE_VERTEX,\n",
        "            backend, GPU_PGRAPH_INFER_PROGRAM_HEADER,\n",
        "the immediate vertex rule is not announced: a replay that allows only the header inference draws it.",
    ),
    _row(
        _REPLAY_H,
        "wiring-host-drops-varying",
        "#define GPU_PGRAPH_INFER_OUTPUT_ALL 0xFFFE000u",
        "#define GPU_PGRAPH_INFER_OUTPUT_ALL 0xBFFE000u",
        "`--gpu-replay-output-state` does not allow the unwritten varying default, the composition is refused again.",
        _SWAP_TEST,
    ),
    _row(
        _PGRAPH_H,
        "wiring-all-measured-drops-immediate",
        "    (GPU_PGRAPH_OUTPUT_SURFACE | GPU_PGRAPH_OUTPUT_FIXED | GPU_PGRAPH_OUTPUT_TEXTURE | GPU_PGRAPH_OUTPUT_IMMEDIATE)",
        "    (GPU_PGRAPH_OUTPUT_SURFACE | GPU_PGRAPH_OUTPUT_FIXED | GPU_PGRAPH_OUTPUT_TEXTURE)",
        "`--gpu-replay-output-state` does not decode the immediate vertices.",
    ),
    # ================================================================== spirv
    _sp(
        "spirv-output-rewritten",
        "            if (variable[3] != SPV_STORAGE_INPUT) {\n                at += length; /* an Output may share the Location number */",
        "            if (false) {\n                at += length; /* an Output may share the Location number */",
        "an Output at the same Location is rewritten too.",
    ),
    _sp(
        "spirv-one-is-half",
        "result[used++] = 0x3F800000u; /* 1.0f */",
        "result[used++] = 0x3F000000u; /* 1.0f */",
        "the default w is 0.5.",
    ),
    _sp(
        "spirv-zero-is-one",
        "result[used++] = 0x00000000u; /* 0.0f */",
        "result[used++] = 0x3F800000u; /* 0.0f */",
        "the default xyz is 1.0.",
    ),
    _sp(
        "spirv-entry-keeps-variable",
        "if (opcode == SPV_OP_ENTRY_POINT && words[1] < SPV_VERSION_1_4) {",
        "if (opcode == SPV_OP_ENTRY_POINT && words[1] < 0u) {",
        "the entry point still lists the variable that is no longer an Input.",
    ),
    _sp(
        "spirv-decoration-kept",
        "        if (opcode == SPV_OP_DECORATE) {\n            for (size_t i = 0u; i < input_count; i++) {",
        "        if (opcode == SPV_OP_DECORATE && false) {\n            for (size_t i = 0u; i < input_count; i++) {",
        "the Location decoration stays on a Private variable.",
    ),
    _sp(
        "spirv-access-chain-allowed",
        "                if (words[at + 3u] == inputs[i].id) {\n                    return false;",
        "                if (words[at + 3u] == inputs[i].id + 100u) {\n                    return false;",
        "an input reached through an access chain is rewritten, leaving an Input pointer on a Private variable.",
    ),
    _sp(
        "spirv-bound-not-grown",
        "        }\n        at += length;\n    }\n    result[3] = bound;\n",
        "        }\n        at += length;\n    }\n",
        "the module's id bound stays below the ids added.",
    ),
    _sp(
        "spirv-private-class",
        "                result[used++] = SPV_STORAGE_PRIVATE;\n                result[used++] = input->pointee;",
        "                result[used++] = SPV_STORAGE_INPUT;\n                result[used++] = input->pointee;",
        "the new pointer type is still an Input pointer.",
    ),
    _sp(
        "spirv-no-initialiser",
        "                result[used++] = (5u << 16) | SPV_OP_VARIABLE;",
        "                result[used++] = (4u << 16) | SPV_OP_VARIABLE;",
        "the Private variable is cut short of its initialiser (its last word becomes the next instruction).",
    ),
    _sp(
        "spirv-no-locations-filter",
        "words[at + 3u] < 32u && ((locations >> words[at + 3u]) & 1u) != 0u) {",
        "words[at + 3u] < 32u && (locations & 0xFFu) != 0u) {",
        "every Input is rewritten whatever its Location, not only the unwritten ones.",
    ),
]

for _mutation in MUTATIONS:
    _mutation["needs_device"] = False
