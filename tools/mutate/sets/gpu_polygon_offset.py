# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the POLYGON OFFSET and IGNORED output groups (T502): the decoder words
(`src/gpu/gpu_pgraph.c`, `src/gpu/gpu_pgraph.h`), their resolution into a Vulkan depth bias
(`src/gpu/gpu_pgraph_replay.c`) and the pipeline state (`src/gpu/gpu_vsh_draw.c`).

    table      a method number or a group moved: the failure `docs/d3d8-usage.md` 2.1 records
    ignored    the named no-op group: its counter, its snapshot rule, its membership of the measured set
    resolve    every refusal, every inference and every number handed to Vulkan
    vulkan     the bias actually reaching the pipeline (device-only kills are `needs_device`)

Kills come from `test_gpu_pgraph_state` (decode and resolve device free, pixels and bounds on every device),
`test_d3d8_gpu_state` (the title's whole block) and `test_d3d8_swap_replay`.

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

_PGRAPH = "src/gpu/gpu_pgraph.c"
_HEADER = "src/gpu/gpu_pgraph.h"
_REPLAY = "src/gpu/gpu_pgraph_replay.c"
_DRAW = "src/gpu/gpu_vsh_draw.c"

_STATE = ["test_gpu_pgraph_state", "test_d3d8_gpu_state", "test_d3d8_swap_replay"]
_DECODE = [*_STATE, "test_gpu_pgraph", "test_gpu_pgraph_edges", "test_d3d8_gpu_pgraph"]


def _row(file: str, mutation_id: str, old: str, new: str, why: str, targets: list[str]) -> dict:
    return {
        "id": f"gpu-polyoff-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(targets),
        "why": why,
    }


def _pg(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_PGRAPH, mutation_id, old, new, why, _DECODE)


def _rp(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_REPLAY, mutation_id, old, new, why, _STATE)


def _dr(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_DRAW, mutation_id, old, new, why, _STATE)


_WORDS = [
    ("point-method", "POLY_OFFSET_POINT", "0x0330u", "0x0430u", "POLYGON_OFFSET"),
    ("line-method", "POLY_OFFSET_LINE", "0x0334u", "0x0434u", "POLYGON_OFFSET"),
    ("fill-method", "POLY_OFFSET_FILL", "0x0338u", "0x0438u", "POLYGON_OFFSET"),
    ("scale-method", "POLY_OFFSET_SCALE", "0x0384u", "0x0484u", "POLYGON_OFFSET"),
    ("bias-method", "POLY_OFFSET_BIAS", "0x0388u", "0x0488u", "POLYGON_OFFSET"),
    ("dither-method", "DITHER_ENABLE", "0x0310u", "0x0410u", "IGNORED"),
    ("specular-method", "SPECULAR_PARAMS", "0x09F8u", "0x0AF8u", "IGNORED"),
]

MUTATIONS: list[dict] = [
    *[
        _pg(
            mutation_id,
            f"[GPU_PGRAPH_OUT_{word}] = {{{method}, GPU_PGRAPH_OUTPUT_{group}}},",
            f"[GPU_PGRAPH_OUT_{word}] = {{{moved}, GPU_PGRAPH_OUTPUT_{group}}},",
            f"the {word} word is decoded from {moved}: the title's real {method[:-1]} stays unhandled (strict mode "
            "refuses a stream it should accept) and a method nobody writes is taken for it.",
        )
        for mutation_id, word, method, moved, group in _WORDS
    ],
    _pg(
        "fill-group",
        "[GPU_PGRAPH_OUT_POLY_OFFSET_FILL] = {0x0338u, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET},",
        "[GPU_PGRAPH_OUT_POLY_OFFSET_FILL] = {0x0338u, GPU_PGRAPH_OUTPUT_IGNORED},",
        "the fill enable is filed under the IGNORED group: it is counted as skipped and no offset is ever applied.",
    ),
    _pg(
        "scale-group-none",
        "[GPU_PGRAPH_OUT_POLY_OFFSET_SCALE] = {0x0384u, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET},",
        "[GPU_PGRAPH_OUT_POLY_OFFSET_SCALE] = {0x0384u, 0u},",
        "one word of the group belongs to no group and is never decoded.",
    ),
    _pg(
        "dither-group",
        "[GPU_PGRAPH_OUT_DITHER_ENABLE] = {0x0310u, GPU_PGRAPH_OUTPUT_IGNORED},",
        "[GPU_PGRAPH_OUT_DITHER_ENABLE] = {0x0310u, GPU_PGRAPH_OUTPUT_POLYGON_OFFSET},",
        "dither is filed as polygon offset: it is no longer a named no-op and is counted as handled state.",
    ),
    _pg(
        "ignored-not-counted",
        "            pgraph->stats.pairs_ignored++; /* skipped on purpose: no draw depends on it, no new snapshot */",
        "            /* skipped on purpose: no draw depends on it, no new snapshot */",
        "the ignored pairs are no longer counted: the skip becomes silent, which is what this group exists to prevent.",
    ),
    _pg(
        "ignored-forces-snapshot",
        "        if (output_group == GPU_PGRAPH_OUTPUT_IGNORED) {",
        "        if (output_group == 0xFFFFFFFFu) {",
        "an ignored word forces a new state snapshot, so a draw sequence splits its state for a word nothing reads.",
    ),
    _row(
        _HEADER,
        "measured-set-misses-offset",
        "     GPU_PGRAPH_OUTPUT_POLYGON_OFFSET | GPU_PGRAPH_OUTPUT_IGNORED)",
        "     GPU_PGRAPH_OUTPUT_IGNORED)",
        "polygon offset leaves the measured set: the host's every-group flag and the model's mask drop it.",
        _DECODE,
    ),
    _row(
        _HEADER,
        "measured-set-misses-ignored",
        "     GPU_PGRAPH_OUTPUT_POLYGON_OFFSET | GPU_PGRAPH_OUTPUT_IGNORED)",
        "     GPU_PGRAPH_OUTPUT_POLYGON_OFFSET)",
        "the IGNORED group leaves the measured set: the title's 0x09F8 is refused again in strict mode with every group on.",
        _DECODE,
    ),
    # ------------------------------------------------------------------ resolve
    _rp(
        "group-check-offset",
        '        {GPU_PGRAPH_OUTPUT_POLYGON_OFFSET, "polygon offset"},',
        '        {0u, "polygon offset"},',
        "a stream that wrote polygon offset state with the group off in the backend is applied as nothing, silently.",
    ),
    _rp(
        "group-check-ignored",
        '        {GPU_PGRAPH_OUTPUT_IGNORED, "ignored (dither, specular parameters)"},',
        '        {0u, "ignored (dither, specular parameters)"},',
        "ignored words with the group off in the backend pass without being named.",
    ),
    _rp(
        "offset-not-resolved",
        "    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_POLYGON_OFFSET) != 0u) {\n        const gpu_pgraph_result offset",
        "    if (false) {\n        const gpu_pgraph_result offset",
        "the polygon offset is never resolved: no bias reaches Vulkan whatever the stream wrote.",
    ),
    _rp(
        "refusal-leaves-output",
        "            memset(out, 0, sizeof *out); /* a refusal leaves no half-resolved output behind */",
        "            /* a refusal leaves no half-resolved output behind */",
        "a refused offset leaves the depth state it had half resolved, marked active.",
    ),
    _rp(
        "stencil-refusal-dropped",
        "        if (stencil != GPU_PGRAPH_OK) {\n            return stencil;\n        }",
        "        if (false) {\n            return stencil;\n        }",
        "a refused stencil state is ignored once the offset resolver runs after it.",
    ),
    _rp(
        "enable-bound",
        '        if (value > 1u) {\n            return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "polygon offset %s enable',
        '        if (value > 0x100u) {\n            return fail(report, GPU_PGRAPH_ERR_UNMEASURED, "polygon offset %s enable',
        "an enable of 2 to 0x100 is accepted.",
    ),
    _rp(
        "enable-value",
        "        enabled[i] = value == 1u;",
        "        enabled[i] = value == 0u;",
        "the enable sense is inverted.",
    ),
    _rp(
        "scale-finite",
        "        if (!isfinite(scale)) {",
        "        if (false) {",
        "an infinite or NaN slope factor reaches Vulkan.",
    ),
    _rp(
        "bias-finite",
        "        if (!isfinite(bias)) {",
        "        if (false) {",
        "an infinite or NaN constant bias reaches Vulkan.",
    ),
    _rp(
        "point-line-only-point",
        "    if (enabled[0] || enabled[1]) {",
        "    if (enabled[0]) {",
        "a line enable alone is ignored without the FILL_ONLY inference being named.",
    ),
    _rp(
        "point-line-only-line",
        "    if (enabled[0] || enabled[1]) {",
        "    if (enabled[1]) {",
        "a point enable alone is ignored without the FILL_ONLY inference being named.",
    ),
    _rp(
        "fill-off-applies",
        "    if (!enabled[2]) {\n        return GPU_PGRAPH_OK;\n    }",
        "    if (false) {\n        return GPU_PGRAPH_OK;\n    }",
        "a disabled fill offset still applies its values (or refuses for words the title never wrote).",
    ),
    _rp(
        "unwritten-both",
        "    if (!scale_written || !bias_written) {",
        "    if (!scale_written && !bias_written) {",
        "an enabled offset with only one of its two floats written is applied with the other as 0.",
    ),
    _rp(
        "zero-needs-both",
        "    if (scale == 0.0f && bias == 0.0f) {",
        "    if (scale == 0.0f || bias == 0.0f) {",
        "an offset with ONE zero factor (a pure bias or a pure slope) is dropped as nothing.",
    ),
    _rp(
        "zero-never",
        "    if (scale == 0.0f && bias == 0.0f) {",
        "    if (false) {",
        "an enabled offset of nothing (the ZBIAS handler's -0.0) needs the inference and applies a no-op bias.",
    ),
    _rp(
        "model-inference-swapped",
        "        backend, GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_MODEL,",
        "        backend, GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_FILL_ONLY,",
        "the offset model is gated by (and reported as) the point and line inference.",
    ),
    _rp(
        "fill-only-inference-swapped",
        "            backend, GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_FILL_ONLY,",
        "            backend, GPU_PGRAPH_INFER_OUTPUT_POLYGON_OFFSET_MODEL,",
        "ignoring the point and line enables is gated by (and reported as) the offset model inference.",
    ),
    _rp(
        "unobserved-applies",
        "    if (!out->output.depth_test) {",
        "    if (false) {",
        "an offset with no depth test is handed to the draw layer as a bias (refused there), instead of being counted.",
    ),
    _rp(
        "bias-to-slope",
        "    out->output.depth_bias_constant = bias;\n    out->output.depth_bias_slope = scale;",
        "    out->output.depth_bias_constant = scale;\n    out->output.depth_bias_slope = bias;",
        "the NV2A bias and scale factor swap roles in Vulkan's constant and slope factors.",
    ),
    _rp(
        "bias-sign",
        "    out->output.depth_bias_constant = bias;",
        "    out->output.depth_bias_constant = -bias;",
        "the constant bias moves depth the wrong way.",
    ),
    _rp(
        "offset-not-enabled",
        "    out->output.depth_bias = true;",
        "    out->output.depth_bias = false;",
        "the resolved offset is never switched on.",
    ),
    _rp(
        "applied-count",
        "    report->offset_applied += biased && triangles ? 1u : 0u;",
        "    report->offset_applied += 0u;",
        "the applied-offset draws are not counted.",
    ),
    _rp(
        "unobserved-count",
        "    report->offset_unobserved += output->offset_unobserved || (biased && !triangles) ? 1u : 0u;",
        "    report->offset_unobserved += 0u;",
        "the unobserved offsets are not counted: the no-op becomes silent.",
    ),
    _rp(
        "unobserved-no-depth-lost",
        "    report->offset_unobserved += output->offset_unobserved || (biased && !triangles) ? 1u : 0u;",
        "    report->offset_unobserved += biased && !triangles ? 1u : 0u;",
        "an offset with no depth test is no longer counted as unobserved, only the line and point draws are.",
    ),
    _rp(
        "unobserved-primitives-lost",
        "    report->offset_unobserved += output->offset_unobserved || (biased && !triangles) ? 1u : 0u;",
        "    report->offset_unobserved += output->offset_unobserved ? 1u : 0u;",
        "a line or point draw with an offset on is counted nowhere: the offset it never received is silent (T552).",
    ),
    _rp(
        "applied-lines",
        "    const bool triangles = topology != GPU_VSH_TOPOLOGY_POINT_LIST && topology != GPU_VSH_TOPOLOGY_LINE_LIST;",
        "    const bool triangles = topology != GPU_VSH_TOPOLOGY_POINT_LIST;",
        "a line draw counts as applied while the pipeline ran with no bias (the T552 finding).",
    ),
    _rp(
        "applied-points",
        "    const bool triangles = topology != GPU_VSH_TOPOLOGY_POINT_LIST && topology != GPU_VSH_TOPOLOGY_LINE_LIST;",
        "    const bool triangles = topology != GPU_VSH_TOPOLOGY_LINE_LIST;",
        "a point draw counts as applied while the pipeline ran with no bias (the T552 finding).",
    ),
    _rp(
        "applied-and-unobserved",
        "    report->offset_applied += biased && triangles ? 1u : 0u;",
        "    report->offset_applied += biased ? 1u : 0u;",
        "a line or point draw is counted as applied AND unobserved: the two counters overlap.",
    ),
    _rp(
        "count-topology-wrong",
        "        count_polygon_offset(&output, assembled.topology, report);",
        "        count_polygon_offset(&output, GPU_VSH_TOPOLOGY_TRIANGLE_LIST, report);",
        "every draw is counted as a triangle draw whatever its topology.",
    ),
    # ------------------------------------------------------------------ vulkan
    _dr(
        "bound-needs-depth-test",
        "    if (output->depth_bias && (!output->depth_test || !isfinite(output->depth_bias_constant) ||",
        "    if (output->depth_bias && (false || !isfinite(output->depth_bias_constant) ||",
        "a bias with no depth test is accepted by the draw layer.",
    ),
    _dr(
        "bound-constant-finite",
        "    if (output->depth_bias && (!output->depth_test || !isfinite(output->depth_bias_constant) ||",
        "    if (output->depth_bias && (!output->depth_test || false ||",
        "a non-finite constant factor reaches the pipeline.",
    ),
    _dr(
        "bound-slope-finite",
        "                               !isfinite(output->depth_bias_slope))) {",
        "                               false)) {",
        "a non-finite slope factor reaches the pipeline.",
    ),
    _dr(
        "pipeline-bias-off",
        "        .depthBiasEnable = biased ? VK_TRUE : VK_FALSE,",
        "        .depthBiasEnable = VK_FALSE,",
        "the pipeline never biases: the resolved offset has no effect on a pixel.",
    ),
    _dr(
        "pipeline-constant-zero",
        "        .depthBiasConstantFactor = biased ? draw->output->depth_bias_constant : 0.0f,",
        "        .depthBiasConstantFactor = 0.0f,",
        "the constant bias never reaches the pipeline.",
    ),
    _dr(
        "pipeline-slope-zero",
        "        .depthBiasSlopeFactor = biased ? draw->output->depth_bias_slope : 0.0f,",
        "        .depthBiasSlopeFactor = 0.0f,",
        "the slope factor never reaches the pipeline.",
    ),
    _dr(
        "pipeline-factors-swapped",
        "        .depthBiasConstantFactor = biased ? draw->output->depth_bias_constant : 0.0f,",
        "        .depthBiasConstantFactor = biased ? draw->output->depth_bias_slope : 0.0f,",
        "the constant factor is fed the slope value.",
    ),
    _dr(
        "pipeline-points-biased",
        "                        draw->topology != GPU_VSH_TOPOLOGY_POINT_LIST &&",
        "                        true &&",
        "points are biased when the pipeline carries a bias: RADV then lifts them and llvmpipe does not (T552).",
    ),
    _dr(
        "pipeline-lines-biased",
        "                        draw->topology != GPU_VSH_TOPOLOGY_LINE_LIST;",
        "                        true;",
        "lines are biased when the pipeline carries a bias: their result then depends on the implementation's rule.",
    ),
]

_NEEDS_DEVICE = frozenset(
    {
        "gpu-polyoff-applied-count",
        "gpu-polyoff-unobserved-count",
        "gpu-polyoff-unobserved-no-depth-lost",
        "gpu-polyoff-unobserved-primitives-lost",
        "gpu-polyoff-applied-lines",
        "gpu-polyoff-applied-points",
        "gpu-polyoff-applied-and-unobserved",
        "gpu-polyoff-count-topology-wrong",
        "gpu-polyoff-bound-needs-depth-test",
        "gpu-polyoff-bound-constant-finite",
        "gpu-polyoff-bound-slope-finite",
        "gpu-polyoff-pipeline-bias-off",
        "gpu-polyoff-pipeline-constant-zero",
        "gpu-polyoff-pipeline-slope-zero",
        "gpu-polyoff-pipeline-factors-swapped",
        "gpu-polyoff-pipeline-lines-biased",
        "gpu-polyoff-pipeline-points-biased",
    }
)
for _mutation in MUTATIONS:
    _mutation["needs_device"] = _mutation["id"] in _NEEDS_DEVICE
