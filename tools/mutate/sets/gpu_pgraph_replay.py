# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for `src/gpu/gpu_pgraph_replay.c`: topology expansion, program and fragment
resolution, vertex fetch, the INFERRED-bit gates, and the compositing of a replayed frame
(T84, T84b, T84d, T84a3, T75 fragment half).

T259. The T84b commit records "12 new mutants all killed" and T84d "mutants killed: normalised,
unsigned, lanes swapped, wrong gate bit, any S32K size" as counts only. The ones that already live
in `tests/test_gpu_pgraph_primitives.py`, `tests/test_gpu_pgraph_vtxtypes.py` and
`tests/test_gpu_pgraph.py` are NOT repeated (different anchors, killed by a runner rebuilt from
copies). This set adds the rest, grouped by what a survivor would let through:

    topology    triangulate and lineate: off-by-one loop bounds drop the last primitive, strip
                parity and fan/quad pivots flip winding, a capacity check at the exact fit
    program     digest inputs (final bit, header count, program start slot), range of the scan
    inference   every `need_inference` site and the gates behind it. An INFERRED rule that no
                longer needs its bit is the failure that matters most here: the replay would
                present a guess as a measurement with no flag on it
    fetch       the vertex fetch: defaults, address overflow, per-type byte counts and lane
                assembly, D3DCOLOR channel order
    viewport    which constants a viewport register may overwrite
    composite   the two-clear exact-coverage compositing, flip_y, counters (device-dependent)

T259b. T262 moved the per-type element sizes out of `fetch_attribute` into `gpu_pgraph_element_bytes`
(`gpu_pgraph.c`), so the five `fetch-float-size-bound`, `-float-bytes`, `-ub-size-bound`, `-ub-bytes` and
`-s32k-bytes` mutants are now `gpu-pg-element-*` in `gpu_pgraph.py` (same defects, the file that holds the
rule). `replay-width-bound` only needed its anchor extended (the line gained a trailing `||`).

DEVICE-DEPENDENT kills (exit 77 without a Vulkan device) are reported SKIPPED by the harness, never
killed. `_NEEDS_DEVICE` at the bottom records which ones (15 of 79).
"""

_FILE = "src/gpu/gpu_pgraph_replay.c"
# The textured combiner draws through the replay run in pytest (a runner built from the MUTATED C
# source, compared with the Python reference on a device), not in a ctest binary.
_TEXTURE_DEVICE = 'pytest:tests/test_gpu_combiner_replay.py -k "a_test_texture_is_sampled or random_configurations"'
_WIDE = [
    "test_gpu_pgraph_replay",
    "test_gpu_pgraph_vtxtypes",
    "test_gpu_combiner",
    "test_d3d8_swap_replay",
    "test_gpu_pgraph",
    "test_gpu_pgraph_edges",
]


def _row(mutation_id: str, old: str, new: str, why: str, targets: list[str] | None = None) -> dict:
    return {
        "id": f"gpu-pgr-{mutation_id}",
        "file": _FILE,
        "old": old,
        "new": new,
        "targets": list(targets or _WIDE),
        "why": why,
    }


MUTATIONS: list[dict] = [
    # ------------------------------------------------------------------ triangulate
    _row(
        "tri-list-last-triangle",
        "for (uint32_t i = 0u; i + 3u <= count; i += 3u) {",
        "for (uint32_t i = 0u; i + 3u < count; i += 3u) {",
        "the last triangle of every triangle list is dropped when it ends exactly at the "
        "last index, which is the normal case.",
    ),
    _row(
        "tri-strip-last-triangle",
        "for (uint32_t i = 0u; i + 3u <= count; i++) {\n            if ((i & 1u) == 0u) {",
        "for (uint32_t i = 0u; i + 3u < count; i++) {\n            if ((i & 1u) == 0u) {",
        "the last triangle of every strip is dropped.",
    ),
    _row(
        "tri-strip-parity",
        "if ((i & 1u) == 0u) {",
        "if ((i & 1u) == 1u) {",
        "every strip triangle is wound the other way, so a strip draws back to front.",
    ),
    _row(
        "tri-strip-odd-winding",
        "EMIT(indices[i + 1u], indices[i], indices[i + 2u]);",
        "EMIT(indices[i], indices[i + 1u], indices[i + 2u]);",
        "odd strip triangles keep the winding of even ones, so a strip folds in half.",
    ),
    _row(
        "tri-fan-last-triangle",
        "for (uint32_t i = 1u; i + 2u <= count; i++) {",
        "for (uint32_t i = 1u; i + 2u < count; i++) {",
        "the last triangle of every fan is dropped.",
    ),
    _row(
        "tri-fan-pivot",
        "EMIT(indices[0], indices[i], indices[i + 1u]);",
        "EMIT(indices[1], indices[i], indices[i + 1u]);",
        "a fan pivots on its second vertex instead of its first.",
    ),
    _row(
        "tri-quad-last",
        "for (uint32_t i = 0u; i + 4u <= count; i += 4u) {",
        "for (uint32_t i = 0u; i + 4u < count; i += 4u) {",
        "the last quad of every quad list is dropped.",
    ),
    _row(
        "tri-quad-diagonal",
        "EMIT(indices[i], indices[i + 2u], indices[i + 3u]);",
        "EMIT(indices[i + 1u], indices[i + 2u], indices[i + 3u]);",
        "the second triangle of a quad is cut along the other diagonal, which is wrong for any "
        "non-planar or non-convex quad.",
    ),
    _row(
        "tri-capacity-exact-fit",
        "if (used + 3u > capacity) {",
        "if (used + 3u >= capacity) {",
        "an output buffer that is exactly big enough is refused, which the assembler's "
        "6x worst case never hits but a caller sizing it exactly does.",
    ),
    _row(
        "tri-unknown-primitive",
        "    default:\n        return UINT32_MAX;\n    }\n#undef EMIT",
        "    default:\n        return 0u;\n    }\n#undef EMIT",
        "an unreplayable primitive (LINE_LOOP, QUAD_STRIP, POLYGON) expands to nothing instead "
        "of being refused, so a whole draw vanishes with no message.",
    ),
    # ------------------------------------------------------------------ lineate
    _row(
        "line-list-step",
        "    case GPU_PGRAPH_OP_LINES:\n        step = 2u;",
        "    case GPU_PGRAPH_OP_LINES:\n        step = 1u;",
        "a line list is read as a strip, so every vertex pair shares a vertex.",
    ),
    _row(
        "line-last-segment",
        "for (uint32_t i = 0u; i + 2u <= count; i += step) {",
        "for (uint32_t i = 0u; i + 2u < count; i += step) {",
        "the last line of every list or strip is dropped.",
    ),
    _row(
        "line-capacity-exact-fit",
        "if (used + 2u > capacity) {",
        "if (used + 2u >= capacity) {",
        "an output buffer that is exactly big enough is refused.",
    ),
    _row(
        "line-second-vertex",
        "out[used++] = indices[i + 1u];",
        "out[used++] = indices[i];",
        "every line is a point: both ends are the same vertex.",
    ),
    _row(
        "line-unknown-primitive",
        "        step = 1u;\n        break;\n    default:\n        return UINT32_MAX;",
        "        step = 1u;\n        break;\n    default:\n        return 0u;",
        "LINE_LOOP expands to nothing instead of being refused.",
    ),
    # ------------------------------------------------------------------ program resolution
    _row(
        "program-header-gate-off",
        "if ((backend->allowed_inferences & GPU_PGRAPH_INFER_PROGRAM_HEADER) == 0u) {",
        "if ((backend->allowed_inferences & GPU_PGRAPH_INFER_PROGRAM_HEADER) == 0u && 0) {",
        "the program header version (0x2078) is INFERRED. Without the gate a caller that did "
        "not allow it still gets a replay, with the guess unflagged.",
    ),
    _row(
        "program-mode-mask",
        "(state->execution_mode & 3u) != GPU_PGRAPH_EXECUTION_MODE_PROGRAM) {",
        "(state->execution_mode & 0xFu) != GPU_PGRAPH_EXECUTION_MODE_PROGRAM) {",
        "only the low two bits select the program mode. Widening the mask refuses a draw whose "
        "mode word carries other bits.",
    ),
    _row(
        "program-start-required-off",
        "if (!state->program_start_set) {",
        "if (!state->program_start_set && 0) {",
        "a never-written start slot is taken as slot 0.",
    ),
    _row(
        "program-scan-from-zero",
        "for (uint32_t slot = start; slot < GPU_PGRAPH_PROGRAM_SLOTS; slot++) {",
        "for (uint32_t slot = 0u; slot < GPU_PGRAPH_PROGRAM_SLOTS; slot++) {",
        "the FINAL scan ignores the start slot, so a program that starts mid-file is named by "
        "the wrong instructions.",
    ),
    _row(
        "program-scan-last-slot",
        "for (uint32_t slot = start; slot < GPU_PGRAPH_PROGRAM_SLOTS; slot++) {",
        "for (uint32_t slot = start; slot < GPU_PGRAPH_PROGRAM_SLOTS - 1u; slot++) {",
        "a program whose FINAL instruction is in slot 135 is reported as having none.",
    ),
    _row(
        "program-slot-complete-off",
        "if (!state->slot_written[slot]) {",
        "if (!state->slot_written[slot] && 0) {",
        "a half-written instruction is hashed as if it were complete.",
    ),
    _row(
        "program-final-dword",
        "#define FINAL_DWORD 3u",
        "#define FINAL_DWORD 2u",
        "the FINAL bit lives in the top dword of the instruction.",
    ),
    _row(
        "program-final-bit",
        "#define FINAL_BIT 1u",
        "#define FINAL_BIT 2u",
        "the FINAL flag is bit 0.",
    ),
    _row(
        "program-final-required-off",
        "if (!final_found) {",
        "if (!final_found && 0) {",
        "a program with no FINAL instruction is hashed over every slot to the end of the file.",
    ),
    _row(
        "program-digest-count",
        "put_le16(bytes + 2u, count);",
        "put_le16(bytes + 2u, count + 1u);",
        "the instruction count in the digested header is wrong, so no real program resolves.",
    ),
    _row(
        "program-digest-start",
        "put_le32(bytes + 4u + i * 4u, state->program[start * 4u + i]);",
        "put_le32(bytes + 4u + i * 4u, state->program[i]);",
        "the digested bytes start at slot 0 instead of at the program start.",
    ),
    _row(
        "program-digest-length",
        "gpu_sha256(bytes, 4u + count * 16u, digest);",
        "gpu_sha256(bytes, 3u + count * 16u, digest);",
        "the digest omits the last byte, so no real program resolves.",
    ),
    _row(
        "program-static-name",
        'snprintf(name, sizeof name, "static_%s", out->digest);',
        'snprintf(name, sizeof name, "static-%s", out->digest);',
        "static modules are looked up under a name nothing carries.",
    ),
    _row(
        "program-generated-name",
        'snprintf(name, sizeof name, "generated_%s", out->digest);',
        'snprintf(name, sizeof name, "generated-%s", out->digest);',
        "generated modules are looked up under a name nothing carries.",
    ),
    _row(
        "program-static-flag",
        "    out->is_static = true;\n    if (!gpu_vsh_lookup_name(backend->table, name, &out->module)) {",
        "    out->is_static = false;\n    if (!gpu_vsh_lookup_name(backend->table, name, &out->module)) {",
        "a static module is reported as generated.",
    ),
    _row(
        "program-generated-flag",
        '        out->is_static = false;\n        char made_error[160] = "";\n',
        '        out->is_static = true;\n        char made_error[160] = "";\n',
        "a generated module is reported as static.",
    ),
    _row(
        "program-instruction-count",
        "out->instructions = count;",
        "out->instructions = count + 1u;",
        "the reported instruction count is off by one.",
    ),
    # ------------------------------------------------------------------ fragment resolution
    _row(
        "fragment-inference-mask",
        "(backend->allowed_inferences & GPU_COMBINER_INFER_ALL) |",
        "(backend->allowed_inferences & 0u) |",
        "no combiner inference is ever allowed, so every combiner draw that needs one is refused.",
    ),
    _row(
        "fragment-module-missing-off",
        "if (!gpu_vsh_lookup_name(backend->fragment_table, out->plan.name, &out->module)) {",
        "if (!gpu_vsh_lookup_name(backend->fragment_table, out->plan.name, &out->module) && 0) {",
        "a combiner with no translated module is replayed with module index garbage instead of "
        "refused.",
    ),
    # ------------------------------------------------------------------ vertex fetch
    _row(
        "fetch-default-w",
        "                                         uint32_t *used, gpu_pgraph_report *report)\n{\n    static const float defaults[4] = {0.0f, 0.0f, 0.0f, 1.0f};",
        "                                         uint32_t *used, gpu_pgraph_report *report)\n{\n    static const float defaults[4] = {0.0f, 0.0f, 0.0f, 0.0f};",
        "a disabled slot reads w = 0 instead of 1, which zeroes every perspective divide.",
    ),
    _row(
        "fetch-format-set-ignored",
        "    memcpy(out, defaults, sizeof defaults);\n    gpu_pgraph_format format = {0u, 0u, 0u};\n    if (array->format_set) {",
        "    memcpy(out, defaults, sizeof defaults);\n    gpu_pgraph_format format = {0u, 0u, 0u};\n    if (array->format_set && 0) {",
        "a written format is ignored and every slot reads as disabled.",
    ),
    _row(
        "fetch-disabled-inference-bit",
        'need_inference(backend, GPU_PGRAPH_INFER_COMPONENT_DEFAULTS,\n                              "a disabled vertex slot',
        'need_inference(backend, GPU_PGRAPH_INFER_D3DCOLOR_ORDER,\n                              "a disabled vertex slot',
        "the (0, 0, 0, 1) default for a disabled slot is gated by the wrong inference bit, so "
        "allowing one authorises the other.",
    ),
    _row(
        "fetch-address-required-off",
        "if (!array->address_set) {",
        "if (!array->address_set && 0) {",
        "a slot with a format and no address reads from address 0.",
    ),
    _row(
        "fetch-address-ignored",
        "const uint64_t at = (uint64_t)array->address + (uint64_t)format.stride * vertex;",
        "const uint64_t at = (uint64_t)format.stride * vertex;",
        "every vertex is read from the start of guest memory.",
    ),
    _row(
        "fetch-address-overflow-off",
        "if (at + bytes > UINT32_MAX || backend->read_guest == NULL ||",
        "if (backend->read_guest == NULL ||",
        "a vertex address past 4 GiB wraps to a low address and is read as if valid.",
    ),
    _row(
        "fetch-null-reader-off",
        "if (at + bytes > UINT32_MAX || backend->read_guest == NULL ||",
        "if (at + bytes > UINT32_MAX ||",
        "a backend with no guest reader dereferences NULL instead of being refused.",
    ),
    _row(
        "fetch-d3dcolor-red",
        "        }\n        out[0] = (float)raw[2] / 255.0f;",
        "        }\n        out[0] = (float)raw[0] / 255.0f;",
        "D3DCOLOR red is taken from the blue byte, so every colour is channel-swapped.",
    ),
    _row(
        "fetch-d3dcolor-green-scale",
        "        }\n        out[0] = (float)raw[2] / 255.0f;\n        out[1] = (float)raw[1] / 255.0f;",
        "        }\n        out[0] = (float)raw[2] / 255.0f;\n        out[1] = (float)raw[1] / 256.0f;",
        "255 maps to just under 1.0, so a full green channel never reaches full intensity.",
    ),
    _row(
        "fetch-d3dcolor-alpha",
        "        }\n        out[0] = (float)raw[2] / 255.0f;\n        out[1] = (float)raw[1] / 255.0f;\n        out[2] = (float)raw[0] / 255.0f;\n        out[3] = (float)raw[3] / 255.0f;",
        "        }\n        out[0] = (float)raw[2] / 255.0f;\n        out[1] = (float)raw[1] / 255.0f;\n        out[2] = (float)raw[0] / 255.0f;\n        out[3] = (float)raw[0] / 255.0f;",
        "alpha is read from the blue byte.",
    ),
    _row(
        "fetch-d3dcolor-gate-bit",
        "need_inference(backend, GPU_PGRAPH_INFER_D3DCOLOR_ORDER,",
        "need_inference(backend, GPU_PGRAPH_INFER_COMPONENT_DEFAULTS,",
        "the D3DCOLOR channel order (INFERRED) is gated by the wrong bit.",
    ),
    _row(
        "fetch-s32k-high-byte",
        "const uint32_t half = (uint32_t)raw[lane * 2u] | ((uint32_t)raw[lane * 2u + 1u] << 8);",
        "const uint32_t half = (uint32_t)raw[lane * 2u] | ((uint32_t)raw[lane * 2u + 1u] << 7);",
        "values of 256 and above are corrupt.",
    ),
    _row(
        "fetch-s32k-defaults-off",
        "        return need_inference(backend, GPU_PGRAPH_INFER_COMPONENT_DEFAULTS,\n"
        '                              "components a vertex array does not supply reading as (0, 0, 0, 1)",\n'
        "                              used, report);\n    }\n"
        "    for (uint32_t lane = 0u; lane < format.size; lane++) {\n        uint32_t word",
        "        return GPU_PGRAPH_OK;\n    }\n"
        "    for (uint32_t lane = 0u; lane < format.size; lane++) {\n        uint32_t word",
        "an S32K x2 attribute leaves z and w at (0, 1) without naming that inference.",
    ),
    _row(
        "fetch-float-byte-2",
        "((uint32_t)raw[lane * 4u + 2u] << 16)",
        "((uint32_t)raw[lane * 4u + 2u] << 15)",
        "floats with bits 16 to 22 set are corrupt.",
    ),
    _row(
        "fetch-float-byte-3",
        "((uint32_t)raw[lane * 4u + 3u] << 24)",
        "((uint32_t)raw[lane * 4u + 3u] << 23)",
        "the float's sign and exponent byte is corrupt.",
    ),
    _row(
        "fetch-short-float-defaults-off",
        "    if (format.size < 4u) {\n        return need_inference(",
        "    if (format.size < 3u) {\n        return need_inference(",
        "a float3 attribute fills w = 1 with no inference named.",
    ),
    _row(
        "fetch-full-float-defaults-on",
        "    if (format.size < 4u) {\n        return need_inference(",
        "    if (format.size <= 4u) {\n        return need_inference(",
        "a float4 attribute, which supplies everything, claims a default it did not use and is "
        "refused unless that inference is allowed.",
    ),
    # ------------------------------------------------------------------ point and line state
    _row(
        "line-width-unstated-negative",
        "if (backend->line_width == 0.0f) {",
        "if (backend->line_width <= 0.0f) {",
        "a negative backend line width is read as 'unstated' and inferred instead of refused.",
    ),
    _row(
        "line-width-wider-accepted",
        "if (backend->line_width != 1.0f) {",
        "if (backend->line_width > 1.0f) {",
        "a stated width below 1.0 is drawn one pixel wide with no refusal.",
    ),
    _row(
        "fast-fetch-default-w",
        "static bool fast_fetch(const fast_slot *slot, uint32_t vertex, float *out)\n{\n    static const float defaults[4] = {0.0f, 0.0f, 0.0f, 1.0f};",
        "static bool fast_fetch(const fast_slot *slot, uint32_t vertex, float *out)\n{\n    static const float defaults[4] = {0.0f, 0.0f, 0.0f, 0.0f};",
        "Fast path: a disabled slot reads w = 0 instead of 1, which zeroes every perspective divide.",
    ),
    _row(
        "fast-fetch-format-set-ignored",
        "    fast_slot slot = {FAST_SLOW, NULL, 0u, 0u, 0u, 0u, 0u, 0u};\n    gpu_pgraph_format format = {0u, 0u, 0u};\n    if (array->format_set) {",
        "    fast_slot slot = {FAST_SLOW, NULL, 0u, 0u, 0u, 0u, 0u, 0u};\n    gpu_pgraph_format format = {0u, 0u, 0u};\n    if (array->format_set && 0) {",
        "Fast path: a written format is ignored and every slot reads as disabled.",
    ),
    _row(
        "fast-fetch-d3dcolor-red",
        "    if (slot->mode == FAST_D3D) {\n        out[0] = (float)raw[2] / 255.0f;",
        "    if (slot->mode == FAST_D3D) {\n        out[0] = (float)raw[0] / 255.0f;",
        "Fast path: D3DCOLOR red is taken from the blue byte, so every colour is channel-swapped.",
    ),
    _row(
        "fast-fetch-d3dcolor-green-scale",
        "    if (slot->mode == FAST_D3D) {\n        out[0] = (float)raw[2] / 255.0f;\n        out[1] = (float)raw[1] / 255.0f;",
        "    if (slot->mode == FAST_D3D) {\n        out[0] = (float)raw[2] / 255.0f;\n        out[1] = (float)raw[1] / 256.0f;",
        "Fast path: 255 maps to just under 1.0, so a full green channel never reaches full intensity.",
    ),
    _row(
        "fast-fetch-d3dcolor-alpha",
        "    if (slot->mode == FAST_D3D) {\n        out[0] = (float)raw[2] / 255.0f;\n        out[1] = (float)raw[1] / 255.0f;\n        out[2] = (float)raw[0] / 255.0f;\n        out[3] = (float)raw[3] / 255.0f;",
        "    if (slot->mode == FAST_D3D) {\n        out[0] = (float)raw[2] / 255.0f;\n        out[1] = (float)raw[1] / 255.0f;\n        out[2] = (float)raw[0] / 255.0f;\n        out[3] = (float)raw[0] / 255.0f;",
        "Fast path: alpha is read from the blue byte.",
    ),
    # ------------------------------------------------------------------ assemble_draw
    _row(
        "assemble-capacity",
        "const uint32_t capacity = draw->index_count * 6u;",
        "const uint32_t capacity = draw->index_count * 2u;",
        "a strip or fan of seven or more indices overflows the expansion buffer and is refused.",
    ),
    _row(
        "assemble-vertex-bound",
        "} else if (expanded > GPU_VSH_MAX_VERTICES) {",
        "} else if (expanded >= GPU_VSH_MAX_VERTICES) {",
        "a draw of exactly 65536 expanded vertices is refused although the device accepts it.",
    ),
    _row(
        "assemble-slot-offset",
        "            float *const destination = out->attributes + (size_t)vertex * GPU_VSH_ATTRIBUTE_FLOATS + slot * 4u;",
        "            float *const destination = out->attributes + (size_t)vertex * GPU_VSH_ATTRIBUTE_FLOATS + slot * 3u;",
        "attribute slots overlap in the vertex buffer.",
    ),
    _row(
        "assemble-constants-copy",
        "memcpy(out->constants, state->constants, sizeof out->constants);",
        "memset(out->constants, 0, sizeof out->constants);",
        "every program runs with zeroed constants.",
    ),
    _row(
        "assemble-scale-index",
        "const bool scale_in_stream = state->constant_written[58];",
        "const bool scale_in_stream = state->constant_written[59];",
        "whether the stream wrote c58 is judged from c59, so the viewport scale is fed over (or "
        "not over) a constant the title wrote itself.",
    ),
    _row(
        "assemble-offset-index",
        "const bool offset_in_stream = state->constant_written[59];",
        "const bool offset_in_stream = state->constant_written[58];",
        "whether the stream wrote c59 is judged from c58.",
    ),
    _row(
        "assemble-viewport-either",
        "if ((state->viewport_scale_set && !scale_in_stream) ||\n            (state->viewport_offset_set && !offset_in_stream)) {",
        "if ((state->viewport_scale_set && !scale_in_stream) &&\n            (state->viewport_offset_set && !offset_in_stream)) {",
        "viewport registers are fed into c58 and c59 only when BOTH are missing from the stream.",
    ),
    _row(
        "assemble-viewport-gate-bit",
        "need_inference(backend, GPU_PGRAPH_INFER_VIEWPORT_CONSTANTS,",
        "need_inference(backend, GPU_PGRAPH_INFER_COMPONENT_DEFAULTS,",
        "the viewport-into-constants inference is gated by the wrong bit.",
    ),
    _row(
        "assemble-scale-overwrites-stream",
        "if (result == GPU_PGRAPH_OK && state->viewport_scale_set && !scale_in_stream) {",
        "if (result == GPU_PGRAPH_OK && state->viewport_scale_set) {",
        "the viewport scale register overwrites a c58 the title wrote itself.",
    ),
    _row(
        "assemble-offset-row",
        "memcpy(out->constants + 59u * 4u, state->viewport_offset, 16u);",
        "memcpy(out->constants + 58u * 4u, state->viewport_offset, 16u);",
        "the viewport offset lands in the scale row.",
    ),
    _row(
        "assemble-offset-overwrites-stream",
        "if (result == GPU_PGRAPH_OK && state->viewport_offset_set && !offset_in_stream) {",
        "if (result == GPU_PGRAPH_OK && state->viewport_offset_set) {",
        "the viewport offset register overwrites a c59 the title wrote itself.",
    ),
    # ------------------------------------------------------------------ replay and compositing
    _row(
        "replay-clear-rounding",
        "const float scaled = value * 255.0f + 0.5f;",
        "const float scaled = value * 255.0f;",
        "the clear colour truncates instead of rounding: 0.5 becomes 127.",
    ),
    _row(
        "replay-width-bound",
        "width == 0u || height == 0u || width > 8192u || height > 8192u ||",
        "width == 0u || height == 0u || width > 8193u || height > 8192u ||",
        "a target 8193 wide is accepted past the device's limit.",
    ),
    _row(
        "replay-failed-draw-index",
        "report->failed_draw = index;",
        "report->failed_draw = 0u;",
        "a refusal always names draw 0.",
    ),
    _row(
        "replay-degenerate-not-counted",
        "            report->degenerate++;",
        "            (void)0;",
        "zero-vertex draws are no longer reported.",
    ),
    _row(
        "replay-drawn-not-counted",
        "                report->drawn++;",
        "                (void)0;",
        "the count of replayed draws stops moving.",
    ),
    _row(
        "replay-vertices-not-counted",
        "                report->vertices += assembled.vertex_count;",
        "                (void)0;",
        "the count of replayed vertices stops moving.",
    ),
    _row(
        "replay-program-header-not-reported",
        "report->used_inferences |= assembled.used_inferences | GPU_PGRAPH_INFER_PROGRAM_HEADER |",
        "report->used_inferences |= assembled.used_inferences |",
        "the program header guess is not reported as used, so a replay that depended on it "
        "claims to rest on measurement alone.",
    ),
    _row(
        "replay-combiner-stage-dropped",
        "result = composite(device, &draw_backend, &program, backend->combiner ? &combiner : NULL,",
        "result = composite(device, &draw_backend, &program, NULL,",
        "the combiner stage is resolved and then never used: the pass-through stage draws instead.",
    ),
    _row(
        "replay-flip-y-off",
        "if (backend->flip_y) {",
        "if (backend->flip_y && 0) {",
        "flip_y is accepted and ignored.",
    ),
    _row(
        "replay-flip-half",
        "for (uint32_t y = 0u; y < frame->height / 2u; y++) {",
        "for (uint32_t y = 0u; y < frame->height / 3u; y++) {",
        "only the outer third of the rows is flipped.",
    ),
    _row(
        "composite-line-width",
        ".line_width = 1.0f, /* gpu_pgraph_resolve_primitive accepted only 1.0, stated or inferred */",
        ".line_width = 2.0f, /* gpu_pgraph_resolve_primitive accepted only 1.0, stated or inferred */",
        "every line draw is refused by the device (width must be 1.0).",
    ),
    _row(
        "composite-either-clear",
        "if (!low_clear || !high_clear) {",
        "if (!low_clear && !high_clear) {",
        "a pixel counts as covered only when it differs from BOTH clear colours, so a "
        "shape whose colour equals one clear colour vanishes.",
    ),
    _row(
        "composite-texture-height",
        "fragment.textures[stage].height = backend->test_textures[stage].height;",
        "fragment.textures[stage].height = backend->test_textures[stage].width;",
        "a non-square test texture is sampled with its width as its height.",
        targets=[*_WIDE, _TEXTURE_DEVICE],
    ),
    _row(
        "composite-texture-stage-mask",
        "if ((combiner->plan.texture_stages >> stage) & 1u) {",
        "if ((combiner->plan.texture_stages >> stage) & 0u) {",
        "no test texture is ever handed to the fragment stage.",
        targets=[*_WIDE, _TEXTURE_DEVICE],
    ),
    _row(
        "composite-interface-class",
        "!gpu_spirv_interface_locations(fragment.words, fragment.word_count, 1u, &needed) ||",
        "!gpu_spirv_interface_locations(fragment.words, fragment.word_count, 3u, &needed) ||",
        "the fragment module's INPUT locations are read as its outputs, so a missing varying is "
        "never detected.",
    ),
]


# RECORD of a run with no Vulkan device (`VK_DRIVER_FILES=/nonexistent`, 2026-10-03): the mutations whose
# every target exits 77 there, so the harness reports them SKIPPED and never counts them killed. It is
# a record, not a mechanism: the harness decides from the exit code and prints a NOTE when a skip is
# not listed here. Regenerate it with that command if a test starts or stops needing a device.
_NEEDS_DEVICE = frozenset(
    {
        "gpu-pgr-composite-either-clear",
        "gpu-pgr-composite-interface-class",
        "gpu-pgr-composite-line-width",
        "gpu-pgr-composite-texture-height",
        "gpu-pgr-composite-texture-stage-mask",
        "gpu-pgr-fragment-inference-mask",
        "gpu-pgr-fragment-module-missing-off",
        "gpu-pgr-line-width-unstated-negative",
        "gpu-pgr-line-width-wider-accepted",
        "gpu-pgr-replay-combiner-stage-dropped",
        "gpu-pgr-replay-drawn-not-counted",
        "gpu-pgr-replay-flip-half",
        "gpu-pgr-replay-flip-y-off",
        "gpu-pgr-replay-program-header-not-reported",
        "gpu-pgr-replay-vertices-not-counted",
    }
)
for _mutation in MUTATIONS:
    _mutation["needs_device"] = _mutation["id"] in _NEEDS_DEVICE
