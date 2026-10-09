# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the replay copy's planner rules (T832, HQ58): `gpu_pgraph_replay_copy_planner_rules`, the opt-in
GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES path that makes `gpu_pgraph_replay_copy` agree with `live_target_plan_blit`.

Grouped by what a survivor would let through:

    format     formats 7 and 6 force the alpha byte, the byte formats stay refused
    clamp      the row cut to the narrower pitch, its offset-0 rule and the bounds that remain
    pitch      the tightly packed image rule and the operation, carried over from the default copy
    copy       the inference, the flip_y row mirror, the rows and the byte arithmetic
    wiring     the delegation from `gpu_pgraph_replay_copy` and the host mask that adds the bit

All are killed by `test_live_target` (the parity test against the planner, no device) except the host mask, which
`test_d3d8_swap_replay` kills. Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

_RULES = "src/gpu/gpu_pgraph_replay_copy_rules.c"
_TEST = ["test_live_target"]


def _rules(mutation_id: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"t832-copy-rules-{mutation_id}",
        "file": _RULES,
        "old": old,
        "new": new,
        "targets": list(_TEST),
        "why": why,
        "needs_device": False,
    }


MUTATIONS: list[dict] = [
    # ================================================================== format
    _rules(
        "alpha-ff-not-recognised",
        "    const bool force_alpha = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ||",
        "    const bool force_alpha = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0 + 0x100u ||",
        "format 7 is refused or copied with the source alpha.",
    ),
    _rules(
        "alpha-00-not-recognised",
        "                             copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0;",
        "                             copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0 + 0x100u;",
        "format 6 is refused or copied with the source alpha.",
    ),
    _rules(
        "format-refusal",
        "    if (copy->color_format != GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8 && !force_alpha) {",
        "    if (copy->color_format != GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8 && !force_alpha && false) {",
        "the INFERRED Y8 and R5G6B5 byte copies run on A8R8G8B8 images as if they were pixels.",
    ),
    _rules(
        "alpha-value-swapped",
        "ALPHAFF ? 0xFFu : 0x00u;",
        "ALPHAFF ? 0x00u : 0xFFu;",
        "format 7 clears and format 6 sets the alpha byte.",
    ),
    _rules(
        "alpha-byte-index",
        "to[(size_t)pixel * 4u + 3u] = alpha;",
        "to[(size_t)pixel * 4u + 2u] = alpha;",
        "the red byte is forced instead of the alpha byte.",
    ),
    _rules(
        "alpha-last-pixel",
        "            for (uint32_t pixel = 0u; pixel < row_pixels; pixel++) {",
        "            for (uint32_t pixel = 0u; pixel + 1u < row_pixels; pixel++) {",
        "the last pixel of each row keeps the source alpha.",
    ),
    _rules(
        "alpha-skipped",
        "        if (force_alpha) {\n            const uint8_t alpha",
        "        if (force_alpha && false) {\n            const uint8_t alpha",
        "formats 7 and 6 copy the source alpha unchanged.",
    ),
    # ================================================================== clamp
    _rules(
        "narrow-is-wider",
        "    const uint32_t narrow_width = source->width < destination->width ? source->width : destination->width;",
        "    const uint32_t narrow_width = source->width > destination->width ? source->width : destination->width;",
        "the row is cut to the WIDER pitch.",
    ),
    _rules(
        "clamp-not-applied",
        "        row_pixels = narrow_width;",
        "        row_pixels = copy->width;",
        "a row wider than the narrower pitch is not cut, so the bounds check refuses what the planner clamps.",
    ),
    _rules(
        "clamp-in-offset",
        "        if (copy->in_x != 0u || copy->out_x != 0u) {",
        "        if (copy->out_x != 0u) {",
        "a clamped row with a source x offset (not measured) is cut anyway.",
    ),
    _rules(
        "clamp-out-offset",
        "        if (copy->in_x != 0u || copy->out_x != 0u) {",
        "        if (copy->in_x != 0u) {",
        "a clamped row with a destination x offset (not measured) is cut anyway.",
    ),
    _rules(
        "bounds-in-x",
        "    if ((uint64_t)copy->in_x + row_pixels > source->width ||",
        "    if (false ||",
        "a source rectangle past the source's right edge is read.",
    ),
    _rules(
        "bounds-in-x-edge",
        "    if ((uint64_t)copy->in_x + row_pixels > source->width ||",
        "    if ((uint64_t)copy->in_x + row_pixels >= source->width ||",
        "a rectangle that exactly reaches the source's right edge is refused.",
    ),
    _rules(
        "bounds-in-y",
        " || (uint64_t)copy->in_y + copy->height > source->height ||",
        " || false ||",
        "a source rectangle past the source's bottom edge is read.",
    ),
    _rules(
        "bounds-in-y-edge",
        " || (uint64_t)copy->in_y + copy->height > source->height ||",
        " || (uint64_t)copy->in_y + copy->height >= source->height ||",
        "a rectangle that exactly reaches the source's bottom edge is refused.",
    ),
    _rules(
        "bounds-out-x",
        "        (uint64_t)copy->out_x + row_pixels > destination->width ||",
        "        false ||",
        "a destination rectangle past the destination's right edge is written.",
    ),
    _rules(
        "bounds-out-x-edge",
        "        (uint64_t)copy->out_x + row_pixels > destination->width ||",
        "        (uint64_t)copy->out_x + row_pixels >= destination->width ||",
        "a rectangle that exactly reaches the destination's right edge is refused.",
    ),
    _rules(
        "bounds-out-y",
        "        (uint64_t)copy->out_y + copy->height > destination->height) {",
        "        false) {",
        "a destination rectangle past the destination's bottom edge is written.",
    ),
    _rules(
        "bounds-out-y-edge",
        "        (uint64_t)copy->out_y + copy->height > destination->height) {",
        "        (uint64_t)copy->out_y + copy->height >= destination->height) {",
        "a rectangle that exactly reaches the destination's bottom edge is refused.",
    ),
    # ================================================================== pitch
    _rules(
        "operation-refusal",
        "    if (copy->operation != GPU_PGRAPH_BLIT_OPERATION_SRCCOPY) {",
        "    if (copy->operation != GPU_PGRAPH_BLIT_OPERATION_SRCCOPY && false) {",
        "a blit operation other than SRCCOPY is copied.",
    ),
    _rules(
        "source-pitch",
        "    if (copy->source_pitch != source->width * 4u || copy->destination_pitch != destination->width * 4u ||",
        "    if (copy->destination_pitch != destination->width * 4u ||",
        "a source pitch that does not address the tightly packed image is accepted.",
    ),
    _rules(
        "destination-pitch",
        "    if (copy->source_pitch != source->width * 4u || copy->destination_pitch != destination->width * 4u ||",
        "    if (copy->source_pitch != source->width * 4u ||",
        "a destination pitch that does not address the tightly packed image is accepted.",
    ),
    _rules(
        "source-stride",
        "        source->stride_bytes != source->width * 4u || destination->stride_bytes != destination->width * 4u) {",
        "        destination->stride_bytes != destination->width * 4u) {",
        "a source image with padded rows is accepted although the copy addresses it tightly packed.",
    ),
    _rules(
        "destination-stride",
        "        source->stride_bytes != source->width * 4u || destination->stride_bytes != destination->width * 4u) {",
        "        source->stride_bytes != source->width * 4u) {",
        "a destination image with padded rows is accepted although the copy addresses it tightly packed.",
    ),
    # ================================================================== copy
    _rules(
        "inference-needed",
        "    if ((backend->allowed_inferences & GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL) == 0u) {",
        "    if ((backend->allowed_inferences & GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL) == 0u && false) {",
        "the planner rules copy without the blit model inference being allowed.",
    ),
    _rules(
        "inference-reported-model",
        "    *used_inferences |= GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL;",
        "    *used_inferences |= 0u;",
        "the blit model inference is used but not reported.",
    ),
    _rules(
        "inference-reported-rules",
        "    *used_inferences |= GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES;",
        "    *used_inferences |= 0u;",
        "the planner rules are used but the run does not report them, so the census cannot tell which rule set copied.",
    ),
    _rules(
        "flip-source-row",
        "        const uint32_t source_row = backend->flip_y ? source->height - 1u - (copy->in_y + row) : copy->in_y + row;",
        "        const uint32_t source_row = copy->in_y + row;",
        "the source rows are not mirrored under flip_y.",
    ),
    _rules(
        "flip-destination-row",
        "            backend->flip_y ? destination->height - 1u - (copy->out_y + row) : copy->out_y + row;",
        "            copy->out_y + row;",
        "the destination rows are not mirrored under flip_y.",
    ),
    _rules(
        "destination-column",
        "(size_t)copy->out_x * 4u;",
        "(size_t)copy->out_x * 2u;",
        "the destination column is counted at two bytes a pixel.",
    ),
    _rules(
        "source-column",
        "(size_t)copy->in_x * 4u,\n",
        "(size_t)copy->in_x * 2u,\n",
        "the source column is counted at two bytes a pixel.",
    ),
    _rules(
        "row-bytes",
        "                (size_t)row_pixels * 4u);",
        "                (size_t)row_pixels * 2u);",
        "only half of each row is copied.",
    ),
    _rules(
        "row-count",
        "    for (uint32_t row = 0u; row < copy->height; row++) {",
        "    for (uint32_t row = 0u; row + 1u < copy->height; row++) {",
        "the last row of the rectangle is not copied.",
    ),
    _rules(
        "row-order-descending",
        "    for (uint32_t row = 0u; row < copy->height; row++) {\n        const uint32_t source_row = backend->flip_y ? source->height - 1u - (copy->in_y + row) : copy->in_y + row;",
        "    for (uint32_t step = 0u; step < copy->height; step++) {\n        const uint32_t row = copy->height - 1u - step;\n        const uint32_t source_row = backend->flip_y ? source->height - 1u - (copy->in_y + row) : copy->in_y + row;",
        "the rows are copied descending, so an overlap smears the other way (xemu copies ascending).",
    ),
    # ================================================================== wiring
    {
        "id": "t832-copy-rules-delegation",
        "file": "src/gpu/gpu_pgraph_replay.c",
        "old": "    if ((backend->allowed_inferences & GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES) != 0u) {",
        "new": "    if ((backend->allowed_inferences & GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES) != 0u && false) {",
        "targets": list(_TEST),
        "why": "the gate never reaches the planner rules, the replay keeps refusing the clamp, the overlap and formats 7 and 6.",
        "needs_device": False,
    },
    {
        "id": "t832-copy-rules-delegation-inverted",
        "file": "src/gpu/gpu_pgraph_replay.c",
        "old": "    if ((backend->allowed_inferences & GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES) != 0u) {",
        "new": "    if ((backend->allowed_inferences & GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES) == 0u) {",
        "targets": list(_TEST),
        "why": "the default replay takes the planner rules and the gated one the default refusals.",
        "needs_device": False,
    },
    {
        "id": "t832-copy-rules-host-mask",
        "file": "src/gpu/d3d8_swap_replay.c",
        "old": "        allowed |= GPU_PGRAPH_INFER_OUTPUT_BLIT_PLANNER_RULES;",
        "new": "        allowed |= 0u;",
        "targets": ["test_d3d8_swap_replay"],
        "why": "`--gpu-replay-output-state` does not turn the planner rules on, the live renderer and the replay disagree again.",
        "needs_device": True,
    },
    {
        "id": "t832-copy-rules-default-mask",
        "file": "src/gpu/gpu_pgraph_replay.h",
        "old": "#define GPU_PGRAPH_INFER_OUTPUT_ALL 0xFFFE000u",
        "new": "#define GPU_PGRAPH_INFER_OUTPUT_ALL 0x2FFFE000u",
        "targets": list(_TEST),
        "why": "the planner rules join INFER_OUTPUT_ALL, so every caller that allows all output inferences loses the tested default refusals.",
        "needs_device": False,
    },
]
