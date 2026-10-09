# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for `src/gpu/d3d8_swap_replay.c`, the opt-in replay at Swap (T84a, T84a1, T84a3).

T259. The T84a commit records "27 mutants killed (3 first survived and got tests)", T84a1 "30
mutants all killed (2 first survived ...)" and T84a3 "22 mutants killed, 4 first survivors got
tests", as counts only. This set recreates the effective ones, grouped by the property a survivor
would break:

    config        enable-time validation: directory, width/height pairing, line_width, dump
                  directory, strict flag, the default config
    switches      per-target segmentation: the entry recorded at SetRenderTarget (stream
                  index, both targets, both sizes measured THEN), the 64 switch bound, the
                  drain before the switch
    chain         `check_switches`: ordered stream indices, an unbroken from/to chain, the
                  last switch must end at the target bound now
    segments      what each segment of the recording is decoded up to, which target and size
                  it replays into (T262: a draw RANGE per segment of one list)
    passes        refusals of a target drawn twice, an explicit size with several targets,
                  an unmeasured size, the device selector precedence
    commit        which pass is the presented frame and which are offscreen, the counters, the
                  begin_frame that empties the draw list after a replayed frame
    reset latch   `follow_gpu_reset`: after CreateDevice the model, the decode position and the
                  pending switches are rebuilt AND the latch about the old model is cleared
    refusal       what a refusal latches, counts and leaves behind

EQUIVALENT OR UNREACHABLE, LEFT OUT (a survivor no input can kill is not a missing test):
  - `load_module`'s `module >= name_count`: the only caller passes an index the name lookup returned.
  - the overflow flag and switch count cleared after a REPLAYED frame or a REFUSED one: a frame that
    overflowed is refused, a refusal latches, and every later present returns before it reads either,
    until a d3d8_gpu_reset, which clears both itself (that clear IS tested:
    gpu-swap-reset-switch-overflow, gpu-swap-reset-switches).
  - `backend.viewport_inverse_modules`: gpu_pgraph_replay.c never reads the field today. The host option
    reaches the config (test_config_options) and stops there; if the replay starts to use it, add the
    mutation then.

T259b. T262 moved the decode from the present to the kick and gave each segment a draw range of one list,
so eleven anchors drifted and were re-derived on the new code: the decode counters and the decode
failure now live in `decode_pending`, `segments-end` reads `switches[segment].draws`, the switch drain
is followed by a draw count (not a decode position), the open-bracket check at the present is
`gpu_pgraph_in_bracket`, and `segments-begin-frame-between` is now `present-begin-frame` (the one
begin_frame is after the frame is replayed, there is none between segments). `enable-strict-passed`
and `reset-strict-flag` share `configure_model`: the first mutates the one `set_strict` line (reaches the
enable AND the rebuild), the second removes the rebuild's `configure_model()` call, which also drops
the vertex snapshot, and needed a new test (`test_reset_reconfigures_the_model`: a fresh model is
lenient and does not snapshot, so the old tests could not see it). `switch-follows-reset` survived
after T262 (every kick follows the reset itself) and needed `test_reset_then_empty_ring_switch`: only a
switch that is the first thing after a reset, with an empty ring, depends on its own
`follow_gpu_reset`, and what is lost is the size measured at that switch.

Most of this runs without a device (the decode, the segmentation and every refusal that precedes
the replay itself). Kills that need a replayed image are DEVICE-DEPENDENT: the harness reports
them SKIPPED, never killed, without one. `_NEEDS_DEVICE` at the bottom records which (41 of 97).
"""

_FILE = "src/gpu/d3d8_swap_replay.c"
_PRIMARY = ["test_d3d8_swap_replay"]


def _row(mutation_id: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"gpu-swap-{mutation_id}",
        "file": _FILE,
        "old": old,
        "new": new,
        "targets": list(_PRIMARY),
        "why": why,
    }


MUTATIONS: list[dict] = [
    # ------------------------------------------------------------------ config
    _row(
        "default-strict",
        "    config.strict = true;",
        "    config.strict = false;",
        "the default config is lenient: an unmeasured method no longer refuses the frame.",
    ),
    _row(
        "default-clear-alpha",
        "    config.clear[3] = 1.0f;",
        "    config.clear[3] = 0.0f;",
        "the default clear colour is transparent black, so every replayed frame is blank in "
        "a viewer.",
    ),
    _row(
        "enable-directory-required",
        "if (config == NULL || config->spv_directory == NULL || config->spv_directory[0] == '\\0') {",
        "if (config == NULL || config->spv_directory == NULL) {",
        "an empty module directory is accepted and then fails to list.",
    ),
    _row(
        "enable-size-pairing",
        "if ((config->width == 0u) != (config->height == 0u)) {",
        "if ((config->width == 0u) != (config->height == 0u) && 0) {",
        "a width with no height is accepted, so every frame is replayed 0 pixels high.",
    ),
    _row(
        "enable-line-width-validation",
        "if (!(config->line_width == 0.0f || config->line_width == 1.0f)) {",
        "if (config->line_width < 0.0f) {",
        "a line width of 2.0 is accepted, and the first line draw is refused by the device "
        "far from the cause.",
    ),
    _row(
        "enable-strict-passed",
        "    gpu_pgraph_set_strict(state.pgraph, state.config.strict);\n    gpu_pgraph_set_visibility(",
        "    gpu_pgraph_set_strict(state.pgraph, true);\n    gpu_pgraph_set_visibility(",
        "the lenient option is ignored: a title that writes any unmodelled method is refused. "
        "`configure_model` is also what the rebuild after a reset calls, so this one defect "
        "reaches both paths.",
    ),
    _row(
        "enable-resets-seen",
        "    state.gpu_resets_seen = d3d8_gpu_reset_count();\n    state.active = true;",
        "    state.gpu_resets_seen = 0u;\n    state.active = true;",
        "enabling after a CreateDevice is read as a reset in progress, so the model is rebuilt "
        "once for nothing and the first frame is skipped.",
    ),
    _row(
        "enable-dump-exists",
        "if (mkdir(state.dump_directory, 0777) != 0 && errno != EEXIST) {",
        "if (mkdir(state.dump_directory, 0777) != 0) {",
        "an existing dump directory is refused, so a second run into the same directory fails.",
    ),
    _row(
        "enable-dump-is-directory",
        "} else if (stat(state.dump_directory, &info) != 0 || !S_ISDIR(info.st_mode)) {",
        "} else if (stat(state.dump_directory, &info) != 0) {",
        "a dump path that is an existing FILE is accepted, and every dump then fails.",
    ),
    _row(
        "list-modules-empty-stem",
        'if (length <= 4u || strcmp(file_name + length - 4u, ".spv") != 0) {',
        'if (length < 4u || strcmp(file_name + length - 4u, ".spv") != 0) {',
        "a file named just `.spv` is listed as a module with an empty name.",
    ),
    _row(
        "list-modules-extension",
        'if (length <= 4u || strcmp(file_name + length - 4u, ".spv") != 0) {',
        "if (length <= 4u) {",
        "every file in the directory is listed as a module, whatever its extension.",
    ),
    _row(
        "load-module-alignment",
        "if (end > 0 && (size_t)end <= MAX_SPV_BYTES && end % 4 == 0 &&",
        "if (end > 0 && (size_t)end <= MAX_SPV_BYTES &&",
        "a module whose size is not a multiple of four bytes is loaded and truncated.",
    ),
    _row(
        "load-module-magic",
        "if (!read || buffer[0] != SPV_MAGIC) {",
        "if (!read) {",
        "a file that is not SPIR-V is handed to the device.",
    ),
    _row(
        "get-config-device-selector",
        "        config.device_selector = state.device_selector;",
        "        config.device_selector = NULL;",
        "the effective device selector is not reported back.",
    ),
    _row(
        "offscreen-index-bound",
        "    if (index >= state.offscreen_count) {\n        return NULL;",
        "    if (index > state.offscreen_count) {\n        return NULL;",
        "an offscreen index one past the end returns a stale image.",
    ),
    _row(
        "offscreen-target-header",
        "        *target_header = state.offscreen[index].target;",
        "        *target_header = 0u;",
        "the caller is told every offscreen image belongs to header 0.",
    ),
    # ------------------------------------------------------------------ the reset latch
    _row(
        "reset-detect",
        "    if (d3d8_gpu_reset_count() == state.gpu_resets_seen) {\n        return;",
        "    if (1) {\n        return;",
        "a d3d8_gpu_reset is never noticed: the model keeps describing a recording that no "
        "longer exists.",
    ),
    _row(
        "reset-remember",
        "    state.gpu_resets_seen = d3d8_gpu_reset_count();\n    fold_vertex_stats();",
        "    fold_vertex_stats();",
        "the reset is rebuilt on every later call, so the replay never replays a frame again.",
    ),
    _row(
        "reset-decode-position",
        "    state.next = 0u;\n    state.unhandled_logged = 0u;\n    state.switch_count = 0u;",
        "    state.unhandled_logged = 0u;\n    state.switch_count = 0u;",
        "the decode position survives a reset, so the new recording is decoded from the middle.",
    ),
    _row(
        "reset-unhandled-log",
        "    state.unhandled_logged = 0u;\n    state.switch_count = 0u;\n    state.switch_overflow = false;\n    state.texture_event_count = 0u;",
        "    state.switch_count = 0u;\n    state.switch_overflow = false;\n    state.texture_event_count = 0u;",
        "the new model's first unhandled methods are not logged because the log position "
        "belongs to the old one.",
    ),
    _row(
        "reset-switches",
        "    state.switch_count = 0u;\n    state.switch_overflow = false;\n    state.texture_event_count = 0u;",
        "    state.switch_overflow = false;\n    state.texture_event_count = 0u;",
        "pending render target switches survive a reset, naming recording positions that no "
        "longer exist.",
    ),
    _row(
        "reset-switch-overflow",
        "    state.switch_overflow = false;\n    state.texture_event_count = 0u;",
        "    state.texture_event_count = 0u;",
        "a switch overflow of the old recording refuses the first frame of the new one.",
    ),
    _row(
        "reset-counted",
        "    state.stats.model_resets++;",
        "    (void)0;",
        "model rebuilds are no longer counted.",
    ),
    _row(
        "reset-latch",
        "    state.stats.latched = false;\n    state.stats.error[0] = '\\0';\n    if (state.pgraph == NULL) {",
        "    state.stats.error[0] = '\\0';\n    if (state.pgraph == NULL) {",
        "a latch about the OLD model survives CreateDevice, so one refusal stops the replay "
        "for the rest of the run even though the model it was about is gone.",
    ),
    _row(
        "reset-error-text",
        "    state.stats.latched = false;\n    state.stats.error[0] = '\\0';",
        "    state.stats.latched = false;",
        "the old refusal text is still reported after the latch is cleared.",
    ),
    _row(
        "reset-strict-flag",
        "    configure_model();\n    (void)d3d8_hle_log()",
        "    (void)d3d8_hle_log()",
        "the rebuilt model is never configured: it forgets the lenient option AND the vertex "
        "snapshot (T262), so a draw after a CreateDevice is read at the replay again.",
    ),
    # ------------------------------------------------------------------ switches
    _row(
        "switch-inactive",
        "    if (!state.active) {\n        return;\n    }\n    follow_gpu_reset();\n    if (previous == target) {",
        "    follow_gpu_reset();\n    if (previous == target) {",
        "a disabled replay still records switches and drains the ring.",
    ),
    _row(
        "switch-follows-reset",
        "    follow_gpu_reset();\n    if (previous == target) {",
        "    if (previous == target) {",
        "a switch recorded right after CreateDevice lands in the old model's list.",
    ),
    _row(
        "switch-same-target",
        "    if (previous == target) {\n        return;",
        "    if (previous == target && 0) {\n        return;",
        "a SetRenderTarget to the already bound target splits the frame into an extra pass, "
        "which is then refused as a target drawn twice.",
    ),
    _row(
        "switch-drain",
        "    d3d8_pushbuffer_drain();\n    /* The drain went through the consumer",
        "    /* The drain went through the consumer",
        "the ring is not drained at the switch, so the draw count noted for it is before "
        "commands that already ran against the previous target.",
    ),
    _row(
        "switch-bound",
        "#define MAX_SWITCHES 64u",
        "#define MAX_SWITCHES 63u",
        "a frame with exactly 64 switches (the documented limit) is refused.",
    ),
    _row(
        "switch-overflow-flag",
        "        state.switch_overflow = true;\n        return;",
        "        return;",
        "switches beyond the bound are dropped silently, so the frame is replayed with the "
        "wrong segmentation.",
    ),
    _row(
        "switch-stream-index",
        "    entry->stream_index = d3d8_gpu_stream_count();",
        "    entry->stream_index = 0u;",
        "every switch is recorded at the start of the stream, so every draw belongs to the "
        "last target.",
    ),
    _row(
        "switch-from",
        "    entry->from = previous;",
        "    entry->from = target;",
        "a switch records the new target as the one it came from, so the chain check refuses.",
    ),
    _row(
        "switch-to",
        "    entry->to = target;",
        "    entry->to = previous;",
        "a switch records the old target as the one it went to.",
    ),
    _row(
        "switch-from-size",
        "    entry->from_size = measure_target(previous);",
        "    entry->from_size = measure_target(target);",
        "the size of the target left is measured from the target entered (the header is live "
        "only at the switch).",
    ),
    _row(
        "switch-to-size",
        "    entry->to_size = measure_target(target);",
        "    entry->to_size = measure_target(previous);",
        "the size of the target entered is measured from the target left.",
    ),
    # ------------------------------------------------------------------ the chain
    _row(
        "chain-order",
        "if (entry->stream_index < earlier || entry->stream_index > recorded) {",
        "if (entry->stream_index > recorded) {",
        "switches may go backwards in the recording.",
    ),
    _row(
        "chain-past-end",
        "if (entry->stream_index < earlier || entry->stream_index > recorded) {",
        "if (entry->stream_index < earlier) {",
        "a switch may lie past the end of the recording (the recording was released).",
    ),
    _row(
        "chain-first-earlier",
        "const size_t earlier = i == 0u ? 0u : state.switches[i - 1u].stream_index;",
        "const size_t earlier = 0u;",
        "the order of switches is not checked against their predecessor.",
    ),
    _row(
        "chain-link",
        "if (i != 0u && entry->from != state.switches[i - 1u].to) {",
        "if (i != 0u && entry->from != state.switches[i - 1u].to && 0) {",
        "a break in the from/to chain (a target changed without SetRenderTarget) is accepted.",
    ),
    _row(
        "chain-ends-at-bound",
        "if (state.switch_count != 0u && state.switches[state.switch_count - 1u].to != bound) {",
        "if (state.switch_count != 0u && state.switches[state.switch_count - 1u].to != bound && 0) {",
        "the last switch may end on a target other than the one bound at the present.",
    ),
    # ------------------------------------------------------------------ the present
    _row(
        "present-inactive",
        "    if (!state.active) {\n        return;\n    }\n    follow_gpu_reset();\n    state.stats.presents++;",
        "    follow_gpu_reset();\n    state.stats.presents++;",
        "a disabled replay still counts presents and decodes.",
    ),
    _row(
        "present-follows-reset",
        "    follow_gpu_reset();\n    state.stats.presents++;",
        "    state.stats.presents++;",
        "the first present after CreateDevice decodes the new recording with the old model.",
    ),
    _row(
        "present-counted",
        "    state.stats.presents++;",
        "    (void)0;",
        "presents are not counted.",
    ),
    _row(
        "latched-skip-counted",
        "        state.stats.frames_skipped++;",
        "        (void)0;",
        "frames skipped after a refusal are not counted.",
    ),
    _row(
        "latched-skip-discard",
        "        state.stats.frames_skipped++;\n        d3d8_gpu_stream_discard(d3d8_gpu_stream_count());",
        "        state.stats.frames_skipped++;",
        "a skipped frame leaves its recording behind, so the ring fills and commands are dropped.",
    ),
    _row(
        "present-dropped-commands",
        "if (d3d8_gpu_get_stats().commands_dropped != 0u) {",
        "if (d3d8_gpu_get_stats().commands_dropped != 0u && 0) {",
        "a frame whose recording lost commands is replayed from an incomplete model.",
    ),
    _row(
        "present-switch-overflow",
        '    if (state.switch_overflow) {\n        snprintf(what, sizeof what, "more than %u render target switches',
        '    if (state.switch_overflow && 0) {\n        snprintf(what, sizeof what, "more than %u render target switches',
        "a frame with more than 64 switches is replayed with some of them missing.",
    ),
    _row(
        "present-chain-check",
        "    if (!check_switches(recorded, bound, what, sizeof what)) {",
        "    if (!check_switches(recorded, bound, what, sizeof what) && 0) {",
        "the switch chain is never validated.",
    ),
    _row(
        "present-open-bracket",
        '    if (gpu_pgraph_in_bracket(state.pgraph)) {\n        refuse_frame(frame_number, "a BEGIN_END bracket was still open at the present"',
        '    if (gpu_pgraph_in_bracket(state.pgraph) && 0) {\n        refuse_frame(frame_number, "a BEGIN_END bracket was still open at the present"',
        "a bracket still open at the present is replayed as a draw cut in half.",
    ),
    # ------------------------------------------------------------------ segments
    _row(
        "segments-count",
        "const size_t segments = state.switch_count + 1u;",
        "const size_t segments = state.switch_count;",
        "the part of the recording after the last switch is never decoded: the frame the "
        "title presents is never replayed.",
    ),
    _row(
        "segments-end",
        "\n        const size_t end = segment < state.switch_count ? state.switches[segment].draws : frame_draws;",
        "\n        const size_t end = frame_draws;",
        "every segment's draw range runs to the end of the list, so every draw lands in the "
        "first target's pass.",
    ),
    _row(
        "segments-decoded-counted",
        "    state.stats.commands_decoded += gpu_pgraph_get_stats(state.pgraph).pairs - pairs_before;",
        "    (void)pairs_before;",
        "decoded commands are not counted.",
    ),
    _row(
        "segments-decode-failure",
        "    if (decoded != GPU_PGRAPH_OK) {\n        state.decode_failed = true;",
        "    if (decoded != GPU_PGRAPH_OK && 0) {\n        state.decode_failed = true;",
        "a decoder refusal at a kick is not kept, so the frame is replayed from a half-decoded "
        "model.",
    ),
    _row(
        "segments-draws-only",
        "        if (end != first) {",
        "        if (1) {",
        "a segment with no draws is replayed as an empty pass and shown as a render target.",
    ),
    _row(
        "segments-first-target",
        "target = segment == 0u ? entry->from : entry->to;",
        "target = entry->to;",
        "the draws before the first switch are attributed to the target it switched TO.",
    ),
    _row(
        "segments-later-target",
        "target = segment == 0u ? entry->from : entry->to;",
        "target = entry->from;",
        "the draws after a switch are attributed to the target it switched FROM.",
    ),
    _row(
        "segments-first-size",
        "size = segment == 0u ? entry->from_size : entry->to_size;",
        "size = entry->to_size;",
        "the draws before the first switch are replayed at the size of the target it switched TO.",
    ),
    _row(
        "segments-later-size",
        "size = segment == 0u ? entry->from_size : entry->to_size;",
        "size = entry->from_size;",
        "the draws after a switch are replayed at the size of the target it switched FROM.",
    ),
    _row(
        "segments-entry-index",
        "const target_switch *entry = segment == 0u ? &state.switches[0] : &state.switches[segment - 1u];",
        "const target_switch *entry = &state.switches[0];",
        "every segment after the first uses the first switch's target and size.",
    ),
    _row(
        "segments-no-switch-target",
        "    *target = bound;\n    *data = surface_data(bound);",
        "    *target = 0u;\n    *data = surface_data(bound);",
        "with no switch in the frame the pass is attributed to header 0, not the bound target.",
    ),
    _row(
        "segments-no-switch-size",
        "    *size = measure_target(bound);\n    if (state.switch_count",
        "    *size = measure_target(0u);\n    if (state.switch_count",
        "with no switch in the frame the pass is replayed at the size of header 0.",
    ),
    _row(
        "present-begin-frame",
        "    (void)gpu_pgraph_begin_frame(state.pgraph); /* cannot refuse",
        "    (void)0; /* cannot refuse",
        "the draw list is not cleared after a replayed frame (T262 moved the begin_frame from "
        "between segments to here), so the next present replays this frame's draws as well.",
    ),
    # ------------------------------------------------------------------ after the decode
    _row(
        "present-discard-recording",
        "    d3d8_gpu_stream_discard(state.next);\n    state.next = 0u;",
        "    state.next = 0u;",
        "the replayed recording is not released, so the next frame decodes it again.",
    ),
    _row(
        "present-decode-position",
        "    d3d8_gpu_stream_discard(state.next);\n    state.next = 0u;",
        "    d3d8_gpu_stream_discard(state.next);",
        "the decode position is not reset after the recording was discarded, so the next "
        "frame starts decoding past its own beginning.",
    ),
    _row(
        "present-switches-spent",
        "    state.next = 0u;\n    state.switch_count = 0u;\n    state.switch_overflow = false;\n    (void)gpu_pgraph_begin_frame",
        "    state.next = 0u;\n    state.switch_overflow = false;\n    (void)gpu_pgraph_begin_frame",
        "switches of a replayed frame carry into the next one.",
    ),
    _row(
        "present-empty-counted",
        "    if (pass_count == 0u) {\n        state.stats.frames_empty++;",
        "    if (pass_count == 0u) {\n        (void)0;",
        "frames with no draws are not counted as empty.",
    ),
    _row(
        "present-offscreen-released",
        "    state.offscreen_count = 0u;\n    bool presented = false;",
        "    bool presented = false;",
        "the previous frame's offscreen images stay listed after being freed.",
    ),
    _row(
        "present-presented-target",
        "if (passes[i].target == bound && !presented) {",
        "if (passes[i].target != bound && !presented) {",
        "the pass of the target bound at the present is treated as offscreen and an "
        "offscreen one as the presented frame.",
    ),
    _row(
        "present-passes-counted",
        "        state.stats.passes_replayed++;",
        "        (void)0;",
        "replayed passes are not counted.",
    ),
    _row(
        "present-draws-counted",
        "        state.stats.draws += passes[i].drawn;",
        "        (void)0;",
        "replayed draws are not counted.",
    ),
    _row(
        "present-inferences-reported",
        "        state.stats.used_inferences |= passes[i].used_inferences;",
        "        (void)0;",
        "the inferences a replay rested on are not reported to the host.",
    ),
    _row(
        "present-last-width",
        "            state.stats.last_width = passes[i].width;",
        "            state.stats.last_width = passes[i].height;",
        "the presented frame's width is reported as its height.",
    ),
    _row(
        "present-last-height",
        "            state.stats.last_height = passes[i].height;",
        "            state.stats.last_height = passes[i].width;",
        "the presented frame's height is reported as its width.",
    ),
    _row(
        "present-frames-replayed",
        "        state.stats.frames_replayed++;",
        "        (void)0;",
        "presented frames are not counted.",
    ),
    _row(
        "present-offscreen-only",
        "        state.stats.frames_offscreen_only++;",
        "        (void)0;",
        "a frame that drew only offscreen targets is not counted as such.",
    ),
    _row(
        "present-offscreen-dump",
        'snprintf(suffix, sizeof suffix, "_target_%08X", (unsigned)state.offscreen[i].target);',
        'snprintf(suffix, sizeof suffix, "_offscreen_%08X", (unsigned)state.offscreen[i].target);',
        "offscreen dumps are written under another name.",
    ),
    # ------------------------------------------------------------------ refusal and dump
    _row(
        "refuse-counted",
        "    state.stats.frames_refused++;",
        "    (void)0;",
        "refused frames are not counted.",
    ),
    _row(
        "refuse-latches",
        "    state.stats.frames_refused++;\n    state.stats.latched = true;",
        "    state.stats.frames_refused++;",
        "a refusal does not stop the replay: later frames are replayed from a broken model.",
    ),
    _row(
        "refuse-fatal",
        "    if (state.config.fatal_on_refusal) {",
        "    if (state.config.fatal_on_refusal && 0) {",
        "the fatal-on-refusal option is accepted and ignored.",
    ),
    _row(
        "dump-name",
        'snprintf(path, length, "%s/frame_%06llu%s.png", state.dump_directory,',
        'snprintf(path, length, "%s/frame_%05llu%s.png", state.dump_directory,',
        "dumps are written with a different frame number width than documented.",
    ),
    _row(
        "dump-counted",
        "        state.stats.dumps_written++;",
        "        (void)0;",
        "written dumps are not counted.",
    ),
    _row(
        "dump-failure-counted",
        '        state.stats.dump_failures++;\n        (void)d3d8_hle_log()("d3d8 swap replay: frame %llu: cannot write %s\\n",',
        '        (void)d3d8_hle_log()("d3d8 swap replay: frame %llu: cannot write %s\\n",',
        "a failed dump write is not counted.",
    ),
    # ------------------------------------------------------------------ device and pass
    _row(
        "device-selector-config",
        '    const char *selector = state.device_selector != NULL ? state.device_selector\n                                                         : getenv("VKRUN_DEVICE");',
        '    const char *selector = getenv("VKRUN_DEVICE");',
        "the config's device selector is ignored in favour of the environment.",
    ),
    _row(
        "device-selector-environment",
        '    const char *selector = state.device_selector != NULL ? state.device_selector\n                                                         : getenv("VKRUN_DEVICE");',
        "    const char *selector = state.device_selector;",
        "$VKRUN_DEVICE is ignored when the config has no selector.",
    ),
    _row(
        "pass-target-twice",
        "        if (passes[i].target == target) {",
        "        if (passes[i].target == target && 0) {",
        "a target drawn in two separate passes is accepted, the second pass overwriting the "
        "first (drawing into an image that already holds draws is not modelled).",
    ),
    _row(
        "pass-explicit-size-second-target",
        '        if (pass_count != 0u) {\n            snprintf(what, what_size,\n                     "an explicit %ux%u size',
        '        if (pass_count != 0u && 0) {\n            snprintf(what, what_size,\n                     "an explicit %ux%u size',
        "an explicit size is applied to a second render target of a different size.",
    ),
    _row(
        "pass-explicit-width",
        "        width = state.config.width;",
        "        width = size.width;",
        "the explicit width is ignored in favour of the measured one.",
    ),
    _row(
        "pass-explicit-height",
        "        height = state.config.height;",
        "        height = size.height;",
        "the explicit height is ignored in favour of the measured one.",
    ),
    _row(
        "pass-unmeasured",
        "    } else if (!size.measured) {",
        "    } else if (!size.measured && 0) {",
        "a target whose size cannot be measured is replayed into a 0x0 image.",
    ),
    _row(
        "pass-flip-y",
        "    backend.flip_y = state.config.flip_y;",
        "    backend.flip_y = false;",
        "the flip_y option is accepted and ignored.",
    ),
    _row(
        "pass-line-width",
        "    backend.line_width = state.config.line_width;",
        "    backend.line_width = 0.0f;",
        "the configured line_width is not passed to the replay, so a stated 1.0 is treated as "
        "unstated and the LINE_WIDTH inference is demanded.",
    ),
    _row(
        "pass-inferences",
        "    backend.allowed_inferences = state.config.allowed_inferences;",
        "    backend.allowed_inferences = 0xFFFFFFFFu;",
        "every inference is allowed regardless of the config.",
    ),
    _row(
        "pass-drawn",
        "    pass->drawn = report.drawn;",
        "    pass->drawn = 0u;",
        "a pass reports no draws.",
    ),
    _row(
        "pass-inference-report",
        "    pass->used_inferences = report.used_inferences |",
        "    pass->used_inferences = 0u |",
        "a pass reports no inferences.",
    ),
    _row(
        "pass-target-header",
        "    pass->target = target;",
        "    pass->target = 0u;",
        "every pass belongs to header 0, so the present never finds its own.",
    ),
]


# RECORD of a run with no Vulkan device (`VK_DRIVER_FILES=/nonexistent`, 2026-10-03): the mutations whose
# every target exits 77 there, so the harness reports them SKIPPED and never counts them killed. It is
# a record, not a mechanism: the harness decides from the exit code and prints a NOTE when a skip is
# not listed here. Regenerate it with that command if a test starts or stops needing a device.
_NEEDS_DEVICE = frozenset(
    {
        "gpu-swap-default-clear-alpha",
        "gpu-swap-device-selector-config",
        "gpu-swap-device-selector-environment",
        "gpu-swap-dump-counted",
        "gpu-swap-dump-failure-counted",
        "gpu-swap-dump-name",
        "gpu-swap-load-module-alignment",
        "gpu-swap-load-module-magic",
        "gpu-swap-offscreen-index-bound",
        "gpu-swap-offscreen-target-header",
        "gpu-swap-pass-drawn",
        "gpu-swap-pass-explicit-height",
        "gpu-swap-pass-explicit-size-second-target",
        "gpu-swap-pass-explicit-width",
        "gpu-swap-pass-flip-y",
        "gpu-swap-pass-inference-report",
        "gpu-swap-pass-inferences",
        "gpu-swap-pass-line-width",
        "gpu-swap-pass-target-header",
        "gpu-swap-pass-target-twice",
        "gpu-swap-present-discard-recording",
        "gpu-swap-present-draws-counted",
        "gpu-swap-present-frames-replayed",
        "gpu-swap-present-inferences-reported",
        "gpu-swap-present-last-height",
        "gpu-swap-present-last-width",
        "gpu-swap-present-offscreen-dump",
        "gpu-swap-present-offscreen-only",
        "gpu-swap-present-begin-frame",
        "gpu-swap-present-offscreen-released",
        "gpu-swap-present-passes-counted",
        "gpu-swap-present-presented-target",
        "gpu-swap-present-switches-spent",
        "gpu-swap-segments-entry-index",
        "gpu-swap-segments-first-size",
        "gpu-swap-segments-first-target",
        "gpu-swap-segments-no-switch-size",
        "gpu-swap-segments-no-switch-target",
        "gpu-swap-switch-follows-reset",
        "gpu-swap-switch-from-size",
        "gpu-swap-switch-same-target",
    }
)
for _mutation in MUTATIONS:
    _mutation["needs_device"] = _mutation["id"] in _NEEDS_DEVICE
