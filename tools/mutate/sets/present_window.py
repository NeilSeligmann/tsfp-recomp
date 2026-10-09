# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the SDL3 window sink and the `d3d8_overlay` present hook (T760).

Grouped by what a survivor would let through:

    hook      the per-UpdateOverlay picture hook and the per-vblank hook of src/gpu/d3d8_overlay.c: called once per update,
              also past the dump limit, also for a picture that could not be built, the picture's width and height, the
              resolver the hook uses, the buffer allocation when no dump directory is set
    sink      the counting of present_video_sink (submitted, failed, vblanks), the latch (a picture shows at the NEXT
              vblank, only when new), the hold's flush, the letterbox, the BMP capture
    options   `--present-hold-ms` and `--present-capture` (parse edge, need a sink)

The kills are by `test_d3d8_overlay` (hook), `test_present_sink` (sinks, selection, options) and `test_present_window`
(window sink on SDL's dummy video driver, pixel readback, which is SKIPPED without SDL3 and then kills nothing).

NOT MUTATED, and why: the `main.c` wiring (sink open, hook install, the report line, capture and hold call order) because
`tsfp_host` is not a ctest binary. It is covered by the recorded real run in docs/tasks.md T760 (169 pictures presented, equal
to the 169 overlay dump files). EQUIVALENT: the `<` against `<=` of the hold deadline (a 20 ms poll granularity), and the NULL report of the `else` branch after the
conversion (the YUY2 to RGB, plane and scale helpers fail only for a NULL or empty argument, and the rectangle, pitch and
resolver checks above them already rejected those, so the branch is a defence no input reaches: mutant `hook-unbuildable-silent`
survived and was removed, the four reachable unbuildable cases are killed).

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

_OVERLAY = "src/gpu/d3d8_overlay.c"
_SINK = "src/host/present_sink.c"
_OPTIONS = "src/host/host_options.c"

_OVERLAY_TEST = ["test_d3d8_overlay"]
_SINK_TEST = ["test_present_sink", "test_present_window"]
_OPTION_TEST = ["test_present_sink", "test_host_options"]


def _row(file: str, mutation_id: str, old: str, new: str, why: str, targets: list[str]) -> dict:
    return {
        "id": f"present-window-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(targets),
        "why": why,
    }


MUTATIONS: list[dict] = [
    # ==================================================================== hook
    _row(
        _OVERLAY,
        "hook-vblank-dropped",
        "        present_vblank_hook(present_context);\n",
        "        (void)present_context;\n",
        "the sink never hears a vblank: nothing is ever presented and the count stays 0.",
        _OVERLAY_TEST,
    ),
    _row(
        _OVERLAY,
        "hook-picture-not-delivered",
        "        present_notify(number, dump_destination_rgb, destination_width, destination_height);\n",
        "        (void)number;\n",
        "a built picture never reaches the sink: the window stays black.",
        _OVERLAY_TEST,
    ),
    _row(
        _OVERLAY,
        "hook-size-swapped",
        "        present_notify(number, dump_destination_rgb, destination_width, destination_height);\n",
        "        present_notify(number, dump_destination_rgb, destination_height, destination_width);\n",
        "the picture is handed over with width and height swapped: a sheared image in the window.",
        _OVERLAY_TEST,
    ),
    _row(
        _OVERLAY,
        "hook-unresolved-silent",
        '        present_notify(number, NULL, 0u, 0u);\n        dump_fail(number, "the surface Data word',
        '        dump_fail(number, "the surface Data word',
        "an update whose surface is not a registered allocation is not counted by the sink.",
        _OVERLAY_TEST,
    ),
    _row(
        _OVERLAY,
        "hook-stops-at-dump-limit",
        "    if (!write_files && present_picture_hook == NULL && !d3d8_overlay_key_enabled()) {",
        "    if (!write_files && !d3d8_overlay_key_enabled()) {",
        "the hook stops with the dump file limit: the window freezes after --dump-overlay-max pictures.",
        _OVERLAY_TEST,
    ),
    _row(
        _OVERLAY,
        "hook-uses-dump-resolver-only",
        "resolver = present_resolver; /* the hook's resolver wins over the key composition's */",
        "resolver = dump_resolver;",
        "the hook has no resolver when the dump is off: every picture of a window-only run fails.",
        _OVERLAY_TEST,
    ),
    _row(
        _OVERLAY,
        "hook-no-buffers-without-dump",
        "    if (directory == NULL && present_picture_hook == NULL && !d3d8_overlay_key_enabled()) {",
        "    if (directory == NULL && !d3d8_overlay_key_enabled()) {",
        "the picture buffers are not allocated for a hook without a dump: set_present_hook fails or crashes.",
        _OVERLAY_TEST,
    ),
    # ==================================================================== sink
    _row(
        _SINK,
        "sink-submitted-not-counted",
        "    sink->counts.submitted++;\n",
        "    (void)sink;\n",
        "the null sink cannot count frames, the reason this task exists.",
        _SINK_TEST,
    ),
    _row(
        _SINK,
        "sink-vblank-not-counted",
        "    sink->counts.vblanks++;\n",
        "    (void)sink;\n",
        "modelled vblanks are not counted.",
        _SINK_TEST,
    ),
    _row(
        _SINK,
        "sink-failed-not-counted",
        "    if (rgb == NULL || width == 0u || height == 0u) {\n        sink->counts.failed++;",
        "    if (rgb == NULL || width == 0u || height == 0u) {\n        (void)sink;",
        "an unbuildable picture is counted as shown.",
        _SINK_TEST,
    ),
    _row(
        _SINK,
        "sink-picture-never-latched",
        "    sink->height = height;\n    sink->dirty = true;",
        "    sink->height = height;\n    sink->dirty = false;",
        "a picture is copied but never marked new: the window never shows it.",
        _SINK_TEST,
    ),
    _row(
        _SINK,
        "sink-vblank-presents-stale",
        "    gpu_phase_add(GPU_PHASE_WINDOW_PUMP, pump_start);\n    if (sink->dirty) {",
        "    gpu_phase_add(GPU_PHASE_WINDOW_PUMP, pump_start);\n    if (sink->dirty || sink->counts.presented > 0u) {",
        "a vblank without a new picture presents again: more than one picture per new picture.",
        _SINK_TEST,
    ),
    _row(
        _SINK,
        "sink-hold-no-flush",
        "    if (sink->dirty) { /* a picture latched after the last vblank is shown at the stop */",
        "    if (sink->dirty && false) { /* a picture latched after the last vblank is shown at the stop */",
        "the last picture is lost when the run stops between vblanks.",
        _SINK_TEST,
    ),
    _row(
        _SINK,
        "sink-letterbox-fills",
        "    if (vertical < scale) {",
        "    if (vertical > scale) {",
        "a picture of another aspect overflows the window instead of being letterboxed.",
        _SINK_TEST,
    ),
    _row(
        _SINK,
        "sink-capture-writes-nothing",
        "    request->good = surface != NULL && SDL_SaveBMP(surface, request->path);",
        "    request->good = surface != NULL && request->path != NULL;",
        "--present-capture reports a file it never wrote.",
        _SINK_TEST,
    ),
    _row(
        _SINK,
        "sink-sdl-runs-on-caller-thread",
        "static void sdl_run_kind(present_video_sink *sink, present_job_kind kind, void (*job)(present_video_sink *, void *), void *argument)\n{\n    const int64_t start = monotonic_ns();\n    gft_enter(GFT_SYNC_SLOT, kind);\n    pthread_mutex_lock(&sink->job_lock);",
        "static void sdl_run_kind(present_video_sink *sink, present_job_kind kind, void (*job)(present_video_sink *, void *), void *argument)\n{\n    job(sink, argument);\n    return;\n    const int64_t start = monotonic_ns();\n    gft_enter(GFT_SYNC_SLOT, kind);\n    pthread_mutex_lock(&sink->job_lock);",
        "T815: SDL work runs on the guest thread, the OpenGL renderer is not current there and the window stays black.",
        _SINK_TEST,
    ),
    _row(
        _SINK,
        "sink-pace-off",
        "    if (sink->pace_millihertz == 0u) {\n        pthread_mutex_unlock(&sink->lock);\n        return;\n    }",
        "    pthread_mutex_unlock(&sink->lock); return;",
        "T815: the window free-runs the modelled vblanks and a movie flashes past.",
        _SINK_TEST,
    ),
    _row(
        _SINK,
        "sink-select-window-is-null",
        "        *kind = PRESENT_VIDEO_WINDOW;\n        return true;",
        "        *kind = PRESENT_VIDEO_NULL;\n        return true;",
        "--present window silently selects the null sink.",
        _SINK_TEST,
    ),
    # ================================================================== options
    _row(
        _OPTIONS,
        "options-hold-without-sink",
        "        (out->present_hold_ms == 0u || out->present != NULL) &&\n",
        "        (out->present_hold_ms == 0u || true) &&\n",
        "--present-hold-ms is accepted without a sink and does nothing.",
        _OPTION_TEST,
    ),
    _row(
        _OPTIONS,
        "options-hold-over-limit",
        "hold > 86400000u) {",
        "hold > 86400001u) {",
        "the hold limit is one over.",
        _OPTION_TEST,
    ),
    _row(
        _OPTIONS,
        "options-capture-without-sink",
        "        (out->present_capture == NULL || out->present != NULL) &&\n",
        "        (out->present_capture == NULL || true) &&\n",
        "--present-capture is accepted without a sink and writes nothing.",
        _OPTION_TEST,
    ),
]
