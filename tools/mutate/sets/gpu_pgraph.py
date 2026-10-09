# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for `src/gpu/gpu_pgraph.c`, the pushbuffer decoder (T84, T84a, T75 decode half).

T259. The T84 commit records "26 of 26 C mutants killed" and T84a "27 mutants killed (3 first
survived and got tests)", but those were run by hand and only the COUNT survived. This set
recreates the ones that matter as unique-anchor mutations a later edit can re-run:

    format word       the type/size/stride bit fields of an array format register
    combiner index    the method-number to shadow-index map of the 57 combiner words, whose
                      failure mode is the one `docs/d3d8-usage.md` 2.1 records: a real method
                      sitting one slot off, every row still plausible
    strict refusals   unknown method, nested/unopened/over-range BEGIN_END, vertices outside a
                      bracket, cursors past the program and constant files, the load/start
                      range checks
    brackets          begin/end bookkeeping: primitive, first command, first index, count,
                      empty brackets, the snapshot taken at the end of a bracket
    snapshots         when a state write forces a new snapshot (a draw replayed against the
                      state of a LATER draw looks exactly like a correct one in a single-draw
                      test)
    reset/begin_frame what each one clears, including the T262 vertex pool (`pool_held`)
    element size      `gpu_pgraph_element_bytes`, the rule the vertex snapshot and the fetch share

The Python-side mutants of `tests/test_gpu_pgraph.py` (digest header, stride, viewport scale row,
program start) and `tests/test_gpu_combiner_replay.py` (combiner word without a bracket, constant 1
index) stay there and are NOT repeated: their anchors are different lines, and they are killed by a
runner rebuilt from copies, not by a ctest binary.

T259b. T262 rewrote `end_bracket` (a `built` draw that also captures vertex bytes), `begin_frame` and
`reset` (the pool) and moved the element sizes here, so 16 anchors drifted and were re-derived. The
vertex capture itself (`capture_vertices`, the span merge, the budget) is pinned by the T262 tests in
`tests/c/test_gpu_pgraph.c` and `test_d3d8_swap_replay.c` and has no mutants in this set yet.

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

_FILE = "src/gpu/gpu_pgraph.c"
_PRIMARY = [
    "test_gpu_pgraph",
    "test_gpu_pgraph_edges",
    "test_d3d8_gpu_pgraph",
    "test_gpu_pgraph_replay",
    "test_gpu_combiner",
    "test_gpu_pgraph_vtxtypes",
    "test_d3d8_swap_replay",
]


def _row(mutation_id: str, old: str, new: str, why: str, targets: list[str] | None = None) -> dict:
    return {
        "id": f"gpu-pg-{mutation_id}",
        "file": _FILE,
        "old": old,
        "new": new,
        "targets": list(targets or _PRIMARY),
        "why": why,
    }


MUTATIONS: list[dict] = [
    # ------------------------------------------------------------------ format word
    _row(
        "format-size-shift",
        "format.size = (format_word >> 4) & 0xFu;",
        "format.size = (format_word >> 5) & 0xFu;",
        "the component count sits at bits 4..7. A shifted field still decodes a plausible size "
        "for every power-of-two count, so only a non-trivial count exposes it.",
    ),
    _row(
        "format-stride-shift",
        "format.stride = format_word >> 8;",
        "format.stride = format_word >> 9;",
        "the stride is the top 24 bits. Halving it makes every vertex after the first read from "
        "the wrong address with a correct first vertex.",
    ),
    _row(
        "format-type-mask",
        "format.type = format_word & 0xFu;",
        "format.type = format_word & 0x7u;",
        "the type field is four bits wide. A three bit mask loses nothing for the types the "
        "title uses (0 to 6) and so only a type above 7 can expose it.",
    ),
    # ------------------------------------------------------------------ combiner index map
    _row(
        "comb-index-first-run-end",
        "if (method >= 0x0260u && method <= 0x027Cu) {",
        "if (method >= 0x0260u && method < 0x027Cu) {",
        "0x027C is the last alpha input word of the first run. An exclusive bound drops one "
        "word of eight and the other seven still map correctly.",
    ),
    _row(
        "comb-index-final-base",
        "return 8 + (int)((method - 0x0288u) / 4u);",
        "return 9 + (int)((method - 0x0288u) / 4u);",
        "final combiner words 0 and 1 land one slot late, on the first factor and on a slot "
        "the analysis never reads.",
    ),
    _row(
        "comb-index-factor-run-end",
        "if (method >= 0x0A60u && method <= 0x0ADCu) {",
        "if (method >= 0x0A60u && method < 0x0ADCu) {",
        "0x0ADC is the last word of the 0x0A60 run (factors, alpha outputs, colour inputs).",
    ),
    _row(
        "comb-index-factor-base",
        "return 10 + (int)((method - 0x0A60u) / 4u);",
        "return 11 + (int)((method - 0x0A60u) / 4u);",
        "the whole 0x0A60 run is one slot off: every factor, alpha output and colour input is "
        "a real word on the wrong number.",
    ),
    _row(
        "comb-index-17f8",
        "return 42;",
        "return 41;",
        "0x17F8 is the single word between the runs. One slot early lands it on a colour input "
        "word.",
    ),
    _row(
        "comb-index-final-constants-run",
        "if (method == 0x1E20u || method == 0x1E24u) {",
        "if (method == 0x1E20u) {",
        "the second final combiner constant (0x1E24) is no longer decoded.",
    ),
    _row(
        "comb-index-colour-output-base",
        "return 45 + (int)((method - 0x1E40u) / 4u);",
        "return 46 + (int)((method - 0x1E40u) / 4u);",
        "the colour output run 0x1E40..0x1E60 including the control word at its end is one slot off.",
    ),
    _row(
        "comb-index-colour-output-end",
        "if (method >= 0x1E40u && method <= 0x1E60u) {",
        "if (method >= 0x1E40u && method < 0x1E60u) {",
        "0x1E60 is the control word (stage count). Not decoding it leaves the count 0.",
    ),
    _row(
        "comb-index-stage-program",
        "return 54;",
        "return 53;",
        "0x1E70 (the texture stage program) lands on the control word.",
    ),
    _row(
        "comb-index-tail-base",
        "return 55 + (int)((method - 0x1E74u) / 4u);",
        "return 56 + (int)((method - 0x1E74u) / 4u);",
        "the last two decoded words land one slot late, the last one past the 57-word shadow.",
    ),
    _row(
        "comb-index-unaligned",
        "    if ((method & 3u) != 0u) {\n        return -1;",
        "    if ((method & 3u) != 0u && 0) {\n        return -1;",
        "a method that is not 4-aligned must not map to a word. Without the guard 0x0261 "
        "decodes as 0x0260.",
    ),
    # ------------------------------------------------------------------ set_combiner and reset
    _row(
        "set-combiner-flag",
        "pgraph->combiner = enabled;",
        "pgraph->combiner = !enabled;",
        "the opt-in itself: inverted, the default decodes combiner words and an enabled model "
        "does not.",
    ),
    _row(
        "set-combiner-captured",
        "pgraph->state.combiner_captured = enabled;",
        "pgraph->state.combiner_captured = false;",
        "the plan refuses a model that never decoded combiner methods. A captured flag that "
        "stays false refuses every combiner draw with a misleading reason.",
    ),
    _row(
        "reset-keeps-nothing-of-combiner",
        "pgraph->state.combiner_captured = pgraph->combiner;",
        "pgraph->state.combiner_captured = false;",
        "reset zeroes the state and must restore the opt-in, or a reset (CreateDevice) silently "
        "turns the combiner stage off.",
    ),
    _row(
        "reset-state",
        "memset(&pgraph->state, 0, sizeof pgraph->state);",
        "memset(&pgraph->state, 0, 0);",
        "state survives a reset, so the next stream starts from the previous run's program, "
        "constants and viewport.",
    ),
    _row(
        "reset-arrays",
        "memset(pgraph->arrays, 0, sizeof pgraph->arrays);",
        "memset(pgraph->arrays, 0, 0);",
        "vertex array registers survive a reset.",
    ),
    _row(
        "reset-bracket",
        "    pgraph->snapshot_dirty = false;\n    pgraph->in_bracket = false;\n    pgraph->bracket_primitive = 0u;",
        "    pgraph->snapshot_dirty = false;\n    pgraph->bracket_primitive = 0u;",
        "a reset in the middle of a bracket leaves it open and the next stream is refused.",
    ),
    _row(
        "reset-cursors",
        "    pgraph->program_cursor = 0u;\n    pgraph->constant_cursor = 0u;\n    pgraph->query_count = 0u;\n    pgraph->clear_count = 0u;",
        "    pgraph->query_count = 0u;\n    pgraph->clear_count = 0u;",
        "the load cursors survive a reset, so a stream that writes data without a load lands "
        "mid-file.",
    ),
    _row(
        "reset-draws",
        "    pgraph->draw_count = 0u;\n    pgraph->index_count = 0u;\n    pgraph->snapshot_count = 0u;\n    pgraph->pool_held = 0u;\n    memset(&pgraph->stats",
        "    pgraph->index_count = 0u;\n    pgraph->snapshot_count = 0u;\n    pgraph->pool_held = 0u;\n    memset(&pgraph->stats",
        "the draw list survives a reset.",
    ),
    _row(
        "reset-vertex-pool",
        "    pgraph->snapshot_count = 0u;\n    pgraph->pool_held = 0u;\n    memset(&pgraph->stats",
        "    pgraph->snapshot_count = 0u;\n    memset(&pgraph->stats",
        "T262: the vertex snapshot pool is still counted as held after a reset, so the next "
        "stream starts with part of its byte budget already spent.",
    ),
    _row(
        "reset-stats",
        "memset(&pgraph->stats, 0, sizeof pgraph->stats);",
        "memset(&pgraph->stats, 0, 0);",
        "counters survive a reset, so the refusal pair index and the pair count are those of "
        "the previous stream.",
    ),
    _row(
        "reset-unhandled-count",
        "    pgraph->unhandled_used = 0u;\n    pgraph->unhandled_overflow = 0u;\n    pgraph->error[0]",
        "    pgraph->unhandled_overflow = 0u;\n    pgraph->error[0]",
        "the unhandled list keeps its length after a reset, so a replayed log reports methods "
        "from a stream that no longer exists.",
    ),
    _row(
        "reset-error",
        "pgraph->error[0] = '\\0';",
        "(void)0;",
        "the last refusal text survives a reset and reads as a current failure.",
    ),
    # ------------------------------------------------------------------ begin_frame
    _row(
        "begin-frame-open-bracket-allowed",
        "    if (pgraph->in_bracket) {\n        return GPU_PGRAPH_ERR_MALFORMED;\n    }\n    pgraph->query_count = 0u;\n    pgraph->clear_count = 0u;",
        "    if (pgraph->in_bracket && 0) {\n        return GPU_PGRAPH_ERR_MALFORMED;\n    }\n    pgraph->query_count = 0u;\n    pgraph->clear_count = 0u;",
        "d3d8_swap_replay relies on this refusal to name an open BEGIN_END at the present. "
        "Without it the frame is cut in the middle of a draw.",
    ),
    _row(
        "begin-frame-keeps-draws",
        "    pgraph->draw_count = 0u;\n    pgraph->index_count = 0u;\n    pgraph->snapshot_count = 0u;\n    pgraph->snapshot_dirty = false;\n    pgraph->pool_held = 0u;\n    return GPU_PGRAPH_OK;",
        "    pgraph->index_count = 0u;\n    pgraph->snapshot_count = 0u;\n    pgraph->snapshot_dirty = false;\n    pgraph->pool_held = 0u;\n    return GPU_PGRAPH_OK;",
        "a frame's draws accumulate over the previous frames, so each present replays them all.",
    ),
    _row(
        "begin-frame-keeps-indices",
        "    pgraph->draw_count = 0u;\n    pgraph->index_count = 0u;\n    pgraph->snapshot_count = 0u;\n    pgraph->snapshot_dirty = false;\n    pgraph->pool_held = 0u;\n    return GPU_PGRAPH_OK;",
        "    pgraph->draw_count = 0u;\n    pgraph->snapshot_count = 0u;\n    pgraph->snapshot_dirty = false;\n    pgraph->pool_held = 0u;\n    return GPU_PGRAPH_OK;",
        "the index list is not released between frames: the next frame's draws index from a "
        "first_index that no longer lines up with the list.",
    ),
    _row(
        "begin-frame-keeps-snapshots",
        "    pgraph->draw_count = 0u;\n    pgraph->index_count = 0u;\n    pgraph->snapshot_count = 0u;\n    pgraph->snapshot_dirty = false;\n    pgraph->pool_held = 0u;\n    return GPU_PGRAPH_OK;",
        "    pgraph->draw_count = 0u;\n    pgraph->index_count = 0u;\n    pgraph->snapshot_dirty = false;\n    pgraph->pool_held = 0u;\n    return GPU_PGRAPH_OK;",
        "the next frame reuses the previous frame's last snapshot when nothing changed in "
        "between, which is only right if the state really is unchanged.",
    ),
    _row(
        "begin-frame-keeps-vertex-pool",
        "    pgraph->snapshot_dirty = false;\n    pgraph->pool_held = 0u;\n    return GPU_PGRAPH_OK;",
        "    pgraph->snapshot_dirty = false;\n    return GPU_PGRAPH_OK;",
        "T262: the vertex snapshot pool is not emptied at the start of a frame, so the byte "
        "budget (per frame) is spent by every earlier frame and a long run is refused.",
    ),
    # ------------------------------------------------------------------ element size (T262)
    # `gpu_pgraph_element_bytes` is the one size rule the vertex snapshot and the replay's fetch share.
    # Before T262 the same numbers sat in `fetch_attribute` (gpu-pgr-fetch-float-size-bound, -float-bytes,
    # -ub-size-bound, -ub-bytes, -s32k-bytes); they moved here with the rule, so the mutants moved too.
    _row(
        "element-float-size-bound",
        "if (type == GPU_PGRAPH_TYPE_F && size >= 1u && size <= 4u) {",
        "if (type == GPU_PGRAPH_TYPE_F && size >= 1u && size < 4u) {",
        "float4 attributes (positions with w, the commonest shape) are refused.",
    ),
    _row(
        "element-float-bytes",
        "        return size * 4u;",
        "        return size * 2u;",
        "a float attribute is snapshotted and fetched with half its bytes.",
    ),
    _row(
        "element-ub-size-bound",
        "if (type == GPU_PGRAPH_TYPE_UB_D3D && size == 4u) {",
        "if (type == GPU_PGRAPH_TYPE_UB_D3D && size <= 4u) {",
        "UB_D3D with 1 to 3 components is accepted although only size 4 has a measured conversion.",
    ),
    _row(
        "element-ub-bytes",
        "        return 4u;\n    }\n    if (type == GPU_PGRAPH_TYPE_S32K && size == 2u) {",
        "        return 3u;\n    }\n    if (type == GPU_PGRAPH_TYPE_S32K && size == 2u) {",
        "a packed colour reads three bytes, so the alpha byte is stale.",
    ),
    _row(
        "element-s32k-bytes",
        "return 4u; /* the one S32K shape the title's builder emits (v0, T84d) */",
        "return 2u; /* the one S32K shape the title's builder emits (v0, T84d) */",
        "an S32K x2 attribute reads two bytes, so lane 1 is stale.",
    ),
    # ------------------------------------------------------------------ unhandled table
    _row(
        "unhandled-repeat-count",
        "pgraph->unhandled[i].pairs++;",
        "pgraph->unhandled[i].pairs += 0u;",
        "a method seen twice is reported once: the per-method pair count is what tells a "
        "one-off from a flood.",
    ),
    _row(
        "unhandled-first-count",
        "pgraph->unhandled[pgraph->unhandled_used].pairs = 1u;",
        "pgraph->unhandled[pgraph->unhandled_used].pairs = 0u;",
        "the first sighting of a method counts as zero pairs.",
    ),
    _row(
        "unhandled-overflow-not-counted",
        "        pgraph->unhandled_overflow++;\n        return;",
        "        return;",
        "methods beyond the table are dropped without a trace: the 'N pairs not listed' "
        "message would never fire.",
    ),
    # ------------------------------------------------------------------ index list and snapshots
    _row(
        "index-list-bound-inclusive",
        "if (pgraph->index_count >= GPU_PGRAPH_MAX_INDICES) {",
        "if (pgraph->index_count > GPU_PGRAPH_MAX_INDICES) {",
        "the bounded budget admits one index too many.",
    ),
    _row(
        "snapshot-always-new",
        "if (pgraph->snapshot_count != 0u && !pgraph->snapshot_dirty) {",
        "if (pgraph->snapshot_count != 0u && 0) {",
        "every draw takes its own snapshot even when nothing changed: harmless to the picture, "
        "but the 512 snapshot bound then caps a frame at 512 draws instead of 4096.",
    ),
    _row(
        "snapshot-reuse-without-first",
        "if (pgraph->snapshot_count != 0u && !pgraph->snapshot_dirty) {",
        "if (!pgraph->snapshot_dirty) {",
        "the first draw of a frame with no state writes reuses snapshot number -1.",
    ),
    _row(
        "snapshot-bound-inclusive",
        "if (pgraph->snapshot_count >= GPU_PGRAPH_MAX_SNAPSHOTS) {",
        "if (pgraph->snapshot_count > GPU_PGRAPH_MAX_SNAPSHOTS) {",
        "the snapshot budget admits one too many.",
    ),
    _row(
        "snapshot-not-copied",
        "pgraph->snapshots[pgraph->snapshot_count] = pgraph->state;",
        "memset(&pgraph->snapshots[pgraph->snapshot_count], 0, sizeof pgraph->snapshots[0]);",
        "a draw replays against an empty state instead of the state it was recorded with.",
    ),
    _row(
        "snapshot-dirty-never-cleared",
        "    pgraph->snapshot_count++;\n"
        "    if (pgraph->snapshot_count > pgraph->stats.snapshots_peak) {\n"
        "        pgraph->stats.snapshots_peak = pgraph->snapshot_count;\n"
        "    }\n"
        "    pgraph->snapshot_dirty = false;",
        "    pgraph->snapshot_count++;\n"
        "    if (pgraph->snapshot_count > pgraph->stats.snapshots_peak) {\n"
        "        pgraph->stats.snapshots_peak = pgraph->snapshot_count;\n"
        "    }",
        "every draw after the first state write takes a new snapshot: wasteful, and the "
        "snapshot bound is reached 4096 / 512 times sooner.",
    ),
    # ------------------------------------------------------------------ end of bracket
    _row(
        "end-bracket-stays-open",
        "    pgraph->in_bracket = false;\n    if (count == 0u) {",
        "    if (count == 0u) {",
        "the bracket is still open after its END, so the next BEGIN is refused as nested.",
    ),
    _row(
        "end-bracket-empty-becomes-draw",
        "    if (count == 0u) {\n        pgraph->stats.empty_brackets++;",
        "    if (count == 0u && 0) {\n        pgraph->stats.empty_brackets++;",
        "an empty BEGIN/END becomes a zero-index draw that the replay then has to special-case.",
    ),
    _row(
        "end-bracket-empty-not-counted",
        "        pgraph->stats.empty_brackets++;",
        "        (void)0;",
        "empty brackets are the only evidence of a title that opens and closes without "
        "vertices, and they stop being reported.",
    ),
    _row(
        "end-bracket-draw-bound-inclusive",
        "if (pgraph->draw_count >= GPU_PGRAPH_MAX_DRAWS) {",
        "if (pgraph->draw_count > GPU_PGRAPH_MAX_DRAWS) {",
        "the draw budget admits one too many.",
    ),
    _row(
        "end-bracket-primitive",
        "built.primitive = pgraph->bracket_primitive;",
        "built.primitive = GPU_PGRAPH_OP_TRIANGLES;",
        "every draw is a triangle list whatever the BEGIN_END operand was.",
    ),
    _row(
        "end-bracket-first-command",
        "built.first_command = pgraph->bracket_command;",
        "built.first_command = 0u;",
        "the recorded pair index of the BEGIN, used to point a refusal at the right command.",
    ),
    _row(
        "end-bracket-first-index",
        "built.first_index = first;",
        "built.first_index = 0u;",
        "every draw reads the first draw's indices.",
    ),
    _row(
        "end-bracket-index-count",
        "built.index_count = count;",
        "built.index_count = count + 1u;",
        "every draw also takes one index of the next.",
    ),
    _row(
        "end-bracket-snapshot",
        "built.snapshot = snapshot;",
        "built.snapshot = 0u;",
        "every draw replays against the first draw's state.",
    ),
    _row(
        "end-bracket-arrays",
        "memcpy(built.arrays, pgraph->arrays, sizeof built.arrays);",
        "memset(built.arrays, 0, sizeof built.arrays);",
        "a draw forgets which vertex arrays were bound when it was recorded (and, since T262, "
        "the vertex capture that runs on `built` copies nothing for it).",
    ),
    _row(
        "end-bracket-draws-not-counted",
        "    pgraph->stats.draws++;",
        "    (void)0;",
        "the draw counter the host reports stops moving.",
    ),
    # ------------------------------------------------------------------ vertex-state methods in a bracket
    _row(
        "bracket-state-check-off",
        "if (pgraph->in_bracket && (is_vertex_state_method(method) || combiner_word >= 0)) {",
        "if (pgraph->in_bracket && 0 && (is_vertex_state_method(method) || combiner_word >= 0)) {",
        "no emitter writes vertex-stage state inside a bracket, so the decoder refuses it. "
        "Without the refusal a snapshot taken at END would silently hold the last value.",
    ),
    _row(
        "bracket-state-offset-run",
        "(method >= GPU_PGRAPH_VIEWPORT_OFFSET && method < GPU_PGRAPH_VIEWPORT_OFFSET + 16u) ||",
        "(method >= GPU_PGRAPH_VIEWPORT_OFFSET && method < GPU_PGRAPH_VIEWPORT_OFFSET + 12u) ||",
        "the last viewport offset register (w) stops counting as vertex state.",
    ),
    _row(
        "bracket-state-scale-run",
        "(method >= GPU_PGRAPH_VIEWPORT_SCALE && method < GPU_PGRAPH_VIEWPORT_SCALE + 16u) ||",
        "(method >= GPU_PGRAPH_VIEWPORT_SCALE && method < GPU_PGRAPH_VIEWPORT_SCALE + 12u) ||",
        "the last viewport scale register (w) stops counting as vertex state.",
    ),
    _row(
        "bracket-state-constant-data",
        "(method >= GPU_PGRAPH_PROGRAM_DATA && method < GPU_PGRAPH_CONSTANT_DATA_END) ||",
        "(method >= GPU_PGRAPH_PROGRAM_DATA && method < GPU_PGRAPH_PROGRAM_DATA_END) ||",
        "constant uploads inside a bracket are accepted.",
    ),
    _row(
        "bracket-state-array-format-end",
        "method < GPU_PGRAPH_ARRAY_FORMAT + 4u * GPU_PGRAPH_ATTRIBUTES) ||",
        "method < GPU_PGRAPH_ARRAY_FORMAT + 4u * (GPU_PGRAPH_ATTRIBUTES - 1u)) ||",
        "the format register of the last attribute may change inside a bracket.",
    ),
    _row(
        "bracket-state-execution-mode",
        "method == GPU_PGRAPH_EXECUTION_MODE || method == GPU_PGRAPH_PROGRAM_LOAD ||",
        "method == GPU_PGRAPH_PROGRAM_LOAD ||",
        "the transform execution mode may change inside a bracket.",
    ),
    _row(
        "bracket-state-program-load",
        "method == GPU_PGRAPH_EXECUTION_MODE || method == GPU_PGRAPH_PROGRAM_LOAD ||",
        "method == GPU_PGRAPH_EXECUTION_MODE ||",
        "a program load cursor move may happen inside a bracket.",
    ),
    _row(
        "bracket-state-program-start",
        "method == GPU_PGRAPH_PROGRAM_START || method == GPU_PGRAPH_CONSTANT_LOAD;",
        "method == GPU_PGRAPH_CONSTANT_LOAD;",
        "the program start slot may change inside a bracket.",
    ),
    _row(
        "bracket-state-constant-load",
        "method == GPU_PGRAPH_PROGRAM_START || method == GPU_PGRAPH_CONSTANT_LOAD;",
        "method == GPU_PGRAPH_PROGRAM_START;",
        "a constant load cursor move may happen inside a bracket.",
    ),
    # ------------------------------------------------------------------ combiner words
    _row(
        "combiner-word-value",
        "state->combiner[combiner_word] = data;",
        "state->combiner[combiner_word] = data ^ 1u;",
        "the stored combiner word is not the written one.",
    ),
    _row(
        "combiner-word-written",
        "state->combiner_written[combiner_word] = true;",
        "state->combiner_written[combiner_word] = false;",
        "the plan treats every word as never written and infers all of them as zero.",
    ),
    _row(
        "combiner-word-snapshot",
        "state->combiner_written[combiner_word] = true;\n        pgraph->snapshot_dirty = true;",
        "state->combiner_written[combiner_word] = true;",
        "a combiner write between two draws does not start a new snapshot, so the second draw "
        "is planned from the first one's combiner.",
    ),
    # ------------------------------------------------------------------ viewport
    _row(
        "viewport-offset-slot",
        "state->viewport_offset[(method - GPU_PGRAPH_VIEWPORT_OFFSET) / 4u] = float_from_bits(data);",
        "state->viewport_offset[(method - GPU_PGRAPH_VIEWPORT_OFFSET) / 8u] = float_from_bits(data);",
        "offset components land in the wrong lanes.",
    ),
    _row(
        "viewport-offset-set",
        "state->viewport_offset_set = true;",
        "state->viewport_offset_set = false;",
        "a written viewport offset is reported as never written, so it is not fed into c59.",
    ),
    _row(
        "viewport-offset-snapshot",
        "state->viewport_offset_set = true;\n        pgraph->snapshot_dirty = true;",
        "state->viewport_offset_set = true;",
        "a viewport offset change between two draws does not start a new snapshot.",
    ),
    _row(
        "viewport-scale-slot",
        "state->viewport_scale[(method - GPU_PGRAPH_VIEWPORT_SCALE) / 4u] = float_from_bits(data);",
        "state->viewport_scale[(method - GPU_PGRAPH_VIEWPORT_SCALE) / 8u] = float_from_bits(data);",
        "scale components land in the wrong lanes.",
    ),
    _row(
        "viewport-scale-set",
        "state->viewport_scale_set = true;",
        "state->viewport_scale_set = false;",
        "a written viewport scale is reported as never written, so it is not fed into c58.",
    ),
    _row(
        "viewport-scale-snapshot",
        "state->viewport_scale_set = true;\n        pgraph->snapshot_dirty = true;",
        "state->viewport_scale_set = true;",
        "a viewport scale change between two draws does not start a new snapshot.",
    ),
    # ------------------------------------------------------------------ program data
    _row(
        "program-data-bound-inclusive",
        "if (pgraph->program_cursor >= GPU_PGRAPH_PROGRAM_SLOTS * 4u) {",
        "if (pgraph->program_cursor > GPU_PGRAPH_PROGRAM_SLOTS * 4u) {",
        "the 137th slot is accepted past the 136-slot program file.",
    ),
    _row(
        "program-data-slot-start-unmarked",
        "        if (cursor % 4u == 0u) {\n            state->slot_written[cursor / 4u] = false;",
        "        if (cursor % 4u == 0u && 0) {\n            state->slot_written[cursor / 4u] = false;",
        "rewriting a slot partly leaves it marked complete from its previous contents.",
    ),
    _row(
        "program-data-slot-complete-early",
        "        if (cursor % 4u == 3u) {\n            state->slot_written[cursor / 4u] = true;",
        "        if (cursor % 4u == 2u) {\n            state->slot_written[cursor / 4u] = true;",
        "a slot is complete after three dwords instead of four.",
    ),
    _row(
        "program-data-value",
        "state->program[cursor] = data;",
        "state->program[cursor] = data ^ 1u;",
        "the stored instruction dword is not the written one (the digest would name another program).",
    ),
    _row(
        "program-data-snapshot",
        "pgraph->stats.program_dwords++;\n        pgraph->snapshot_dirty = true;",
        "pgraph->stats.program_dwords++;",
        "a program rewritten between two draws is not snapshotted, so the first draw replays "
        "the second's program.",
    ),
    _row(
        "program-data-not-counted",
        "pgraph->stats.program_dwords++;",
        "(void)0;",
        "the program dword counter stops moving.",
    ),
    # ------------------------------------------------------------------ constant data
    _row(
        "constant-data-bound-inclusive",
        "if (pgraph->constant_cursor >= GPU_PGRAPH_CONSTANT_ROWS * 4u) {",
        "if (pgraph->constant_cursor > GPU_PGRAPH_CONSTANT_ROWS * 4u) {",
        "the 193rd row is accepted past the 192-row constant file.",
    ),
    _row(
        "constant-data-row-start-unmarked",
        "        if (cursor % 4u == 0u) {\n            state->constant_written[cursor / 4u] = false;",
        "        if (cursor % 4u == 0u && 0) {\n            state->constant_written[cursor / 4u] = false;",
        "rewriting a constant row partly leaves it marked complete (it decides whether the "
        "viewport registers are fed into c58 and c59).",
    ),
    _row(
        "constant-data-row-complete-early",
        "        if (cursor % 4u == 3u) {\n            state->constant_written[cursor / 4u] = true;",
        "        if (cursor % 4u == 2u) {\n            state->constant_written[cursor / 4u] = true;",
        "a constant row is complete after three floats.",
    ),
    _row(
        "constant-data-value",
        "state->constants[cursor] = float_from_bits(data);",
        "state->constants[cursor] = float_from_bits(data ^ 1u);",
        "the stored constant is not the written one.",
    ),
    _row(
        "constant-data-snapshot",
        "pgraph->stats.constant_dwords++;\n        pgraph->snapshot_dirty = true;",
        "pgraph->stats.constant_dwords++;",
        "a constant rewritten between two draws is not snapshotted.",
    ),
    _row(
        "constant-data-not-counted",
        "pgraph->stats.constant_dwords++;",
        "(void)0;",
        "the constant dword counter stops moving.",
    ),
    # ------------------------------------------------------------------ vertex arrays
    _row(
        "array-address-slot",
        "gpu_pgraph_array *array = &pgraph->arrays[(method - GPU_PGRAPH_ARRAY_OFFSET) / 4u];",
        "gpu_pgraph_array *array = &pgraph->arrays[(method - GPU_PGRAPH_ARRAY_OFFSET) / 8u];",
        "array addresses are bound to the wrong attribute slots.",
    ),
    _row(
        "array-address-value",
        "array->address = data;",
        "array->address = data + 4u;",
        "the recorded address is not the written one.",
    ),
    _row(
        "array-address-set",
        "array->address_set = true;",
        "array->address_set = false;",
        "an array with a format and an address is reported as having no address.",
    ),
    _row(
        "array-format-slot",
        "gpu_pgraph_array *array = &pgraph->arrays[(method - GPU_PGRAPH_ARRAY_FORMAT) / 4u];",
        "gpu_pgraph_array *array = &pgraph->arrays[(method - GPU_PGRAPH_ARRAY_FORMAT) / 8u];",
        "array formats are bound to the wrong attribute slots.",
    ),
    _row(
        "array-format-value",
        "array->format = data;",
        "array->format = data ^ 0x10u;",
        "the recorded format word is not the written one.",
    ),
    _row(
        "array-format-set",
        "array->format_set = true;",
        "array->format_set = false;",
        "a written format is ignored, so the slot reads as disabled.",
    ),
    # ------------------------------------------------------------------ execution mode, loads, start
    _row(
        "exec-mode-value",
        "state->execution_mode = data;",
        "state->execution_mode = data | 1u;",
        "the stored mode differs from the written one.",
    ),
    _row(
        "exec-mode-set",
        "state->execution_mode_set = true;",
        "state->execution_mode_set = false;",
        "a written execution mode reads as never set.",
    ),
    _row(
        "exec-mode-snapshot",
        "state->execution_mode_set = true;\n        pgraph->snapshot_dirty = true;",
        "state->execution_mode_set = true;",
        "a mode change between two draws is not snapshotted.",
    ),
    _row(
        "program-load-bound-inclusive",
        "    case GPU_PGRAPH_PROGRAM_LOAD:\n        if (data >= GPU_PGRAPH_PROGRAM_SLOTS) {",
        "    case GPU_PGRAPH_PROGRAM_LOAD:\n        if (data > GPU_PGRAPH_PROGRAM_SLOTS) {",
        "a load of slot 136 (one past the file) is accepted.",
    ),
    _row(
        "program-load-cursor",
        "pgraph->program_cursor = data * 4u;",
        "pgraph->program_cursor = data * 3u;",
        "the load cursor is in the wrong units, so every slot after the first is misplaced.",
    ),
    _row(
        "program-start-bound-inclusive",
        "    case GPU_PGRAPH_PROGRAM_START:\n        if (data >= GPU_PGRAPH_PROGRAM_SLOTS) {",
        "    case GPU_PGRAPH_PROGRAM_START:\n        if (data > GPU_PGRAPH_PROGRAM_SLOTS) {",
        "a program start of slot 136 is accepted.",
    ),
    _row(
        "program-start-value",
        "state->program_start = data;",
        "state->program_start = data + 1u;",
        "the stored start slot is not the written one.",
    ),
    _row(
        "program-start-set",
        "state->program_start_set = true;",
        "state->program_start_set = false;",
        "a written start slot reads as never written, refusing every draw.",
    ),
    _row(
        "program-start-snapshot",
        "state->program_start_set = true;\n        pgraph->snapshot_dirty = true;",
        "state->program_start_set = true;",
        "a start slot change between two draws is not snapshotted.",
    ),
    _row(
        "constant-load-bound-inclusive",
        "    case GPU_PGRAPH_CONSTANT_LOAD:\n        if (data >= GPU_PGRAPH_CONSTANT_ROWS) {",
        "    case GPU_PGRAPH_CONSTANT_LOAD:\n        if (data > GPU_PGRAPH_CONSTANT_ROWS) {",
        "a load of row 192 (one past the file) is accepted.",
    ),
    _row(
        "constant-load-cursor",
        "pgraph->constant_cursor = data * 4u;",
        "pgraph->constant_cursor = data * 3u;",
        "the constant load cursor is in the wrong units.",
    ),
    # ------------------------------------------------------------------ BEGIN_END
    _row(
        "end-without-begin-allowed",
        '            if (!pgraph->in_bracket) {\n                return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,\n                              "BEGIN_END(0) with no bracket open");',
        '            if (!pgraph->in_bracket && 0) {\n                return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,\n                              "BEGIN_END(0) with no bracket open");',
        "an END with no BEGIN is accepted and closes nothing, hiding a lost command.",
    ),
    _row(
        "begin-inside-bracket-allowed",
        '        if (pgraph->in_bracket) {\n            return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,\n                          "BEGIN_END opened inside an open bracket");',
        '        if (pgraph->in_bracket && 0) {\n            return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,\n                          "BEGIN_END opened inside an open bracket");',
        "a nested BEGIN is accepted and restarts the bracket, dropping the vertices before it.",
    ),
    _row(
        "begin-operation-bound",
        "if (data > GPU_PGRAPH_OP_POLYGON) {",
        "if (data > GPU_PGRAPH_OP_POLYGON + 1u) {",
        "operation 11 is accepted as a primitive.",
    ),
    _row(
        "begin-operation-bound-exclusive",
        "if (data > GPU_PGRAPH_OP_POLYGON) {",
        "if (data >= GPU_PGRAPH_OP_POLYGON) {",
        "POLYGON (10) is refused at decode instead of later at replay, which changes who "
        "reports it and with what pair index.",
    ),
    _row(
        "begin-primitive",
        "pgraph->bracket_primitive = data;",
        "pgraph->bracket_primitive = GPU_PGRAPH_OP_TRIANGLES;",
        "the primitive operand is not remembered (also killed through end-bracket-primitive).",
    ),
    _row(
        "begin-command-index",
        "pgraph->bracket_command = (uint32_t)index;",
        "pgraph->bracket_command = 0u;",
        "the BEGIN pair index is not remembered.",
    ),
    _row(
        "begin-first-index",
        "pgraph->bracket_first_index = (uint32_t)pgraph->index_count;",
        "pgraph->bracket_first_index = 0u;",
        "a second bracket's first index is 0, so it draws the first bracket's vertices too.",
    ),
    # ------------------------------------------------------------------ vertex submission
    _row(
        "vertices-outside-bracket-allowed",
        '        if (!pgraph->in_bracket) {\n            return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,\n                          "vertices submitted outside',
        '        if (!pgraph->in_bracket && 0) {\n            return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,\n                          "vertices submitted outside',
        "vertices outside a bracket are accepted into an index list nothing will draw.",
    ),
    _row(
        "draw-arrays-count",
        "const uint32_t count = (data >> 24) + 1u;",
        "const uint32_t count = (data >> 24);",
        "the count field is minus one: forgetting the +1 draws one vertex too few.",
    ),
    _row(
        "draw-arrays-start-mask",
        "const uint32_t start = data & 0x00FFFFFFu;",
        "const uint32_t start = data & 0x0000FFFFu;",
        "start indices above 65535 wrap.",
    ),
    _row(
        "draw-arrays-loop-bound",
        "for (uint32_t i = 0u; i < count && pushed == GPU_PGRAPH_OK; i++) {",
        "for (uint32_t i = 0u; i <= count && pushed == GPU_PGRAPH_OK; i++) {",
        "one extra index per DRAW_ARRAYS.",
    ),
    _row(
        "draw-arrays-start-advance",
        "pushed = push_index(pgraph, start + i);",
        "pushed = push_index(pgraph, start);",
        "every index of a DRAW_ARRAYS is the start index.",
    ),
    _row(
        "element16-order",
        "            pushed = push_index(pgraph, data & 0xFFFFu);\n            if (pushed == GPU_PGRAPH_OK) {\n                pushed = push_index(pgraph, data >> 16);",
        "            pushed = push_index(pgraph, data >> 16);\n            if (pushed == GPU_PGRAPH_OK) {\n                pushed = push_index(pgraph, data & 0xFFFFu);",
        "the two 16-bit indices of one dword are pushed high first: every triangle is wound "
        "backwards in the replay and the picture looks plausible.",
    ),
    _row(
        "element16-low-mask",
        "pushed = push_index(pgraph, data & 0xFFFFu);",
        "pushed = push_index(pgraph, data & 0x7FFFu);",
        "indices with bit 15 set lose it.",
    ),
    _row(
        "element16-high-shift",
        "pushed = push_index(pgraph, data >> 16);",
        "pushed = push_index(pgraph, data >> 17);",
        "the second index is halved.",
    ),
    _row(
        "element32-masked",
        "pushed = push_index(pgraph, data);",
        "pushed = push_index(pgraph, data & 0xFFFFu);",
        "32-bit indices above 65535 wrap.",
    ),
    # ------------------------------------------------------------------ the decode loop
    _row(
        "decode-null-commands-allowed",
        "if (pgraph == NULL || (commands == NULL && count != 0u)) {",
        "if (pgraph == NULL) {",
        "a NULL command array with a count dereferences NULL instead of being refused.",
    ),
    _row(
        "decode-strict-off",
        "if (!handled && pgraph->strict) {",
        "if (!handled && pgraph->strict && 0) {",
        "strict mode no longer refuses an unmeasured method: the replay carries on with state "
        "that was never applied.",
    ),
    _row(
        "decode-refusal-index",
        "const uint64_t index = pgraph->stats.pairs;",
        "const uint64_t index = 0u;",
        "every refusal names pair 0, so the log cannot say where the stream broke.",
    ),
    _row(
        "decode-pairs-not-counted",
        "        pgraph->stats.pairs++;\n        if (handled) {",
        "        if (handled) {",
        "the pair counter (and with it every refusal index) stops moving.",
    ),
    _row(
        "decode-handled-not-counted",
        "            pgraph->stats.pairs_handled++;",
        "            (void)0;",
        "the handled counter stops moving, so coverage reads as zero.",
    ),
    _row(
        "decode-unhandled-not-noted",
        "        note_unhandled(pgraph, commands[i].method);",
        "        if (0) note_unhandled(pgraph, commands[i].method);",
        "lenient mode loses the list of methods whose state was NOT applied.",
    ),
]


# RECORD of a run with no Vulkan device (`VK_DRIVER_FILES=/nonexistent`, 2026-10-03): the mutations whose
# every target exits 77 there, so the harness reports them SKIPPED and never counts them killed. It is
# a record, not a mechanism: the harness decides from the exit code and prints a NOTE when a skip is
# not listed here. Regenerate it with that command if a test starts or stops needing a device.
_NEEDS_DEVICE = frozenset({})
for _mutation in MUTATIONS:
    _mutation["needs_device"] = _mutation["id"] in _NEEDS_DEVICE
