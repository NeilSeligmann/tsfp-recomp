# ruff: noqa: E501  (the anchors are verbatim source lines)
"""Mutations for the opt-in window to clip conversion (T560).

The replay hands a window coordinate program's oPos to the rasterizer as clip space, so the copy composition covers
a quarter of the target. The opt-in rule (INFERRED, HQ57) converts x and y of the none and z-only classes with the
same `nv2a_unscale` as the viewport inverse, using constants 58 and 59. Grouped by what a survivor would let through:

    pair      `gpu_pgraph_assemble_draw` takes the whole-target map as a PAIR when no scale is in the stream, announced
              (a lone 0x0A20 write is not a viewport, a written row or a full register pair is never replaced)
    refusal   a draw with no scale is refused by name while the statement is made, never drawn raw
    wiring    the statement reaches the backend (swap replay config) and the host option parser
    position  the GLSL `gl_Position` the none and z-only classes get (viewport.py), the Python twin, the plumbing through
              translate.py and vsh_modules.py, and the flag the two replay tools state to the host

The kills are by `test_gpu_pgraph` and `test_d3d8_swap_replay` (the C half), `test_host_options`, and the pytest suites
that build the module text and the tools' commands without a device. The Python mutants are in `PYTHON_MUTATIONS`.

EQUIVALENT OR NOT MUTATED HERE: `src/host/main.c` (the option reaches the config and the banner is printed): it holds
no unit and is covered by `tests/test_xmv_replay.py` (the real boot, the quarter becomes the whole target), which a
harness run does not include because it needs the disc. The banner text itself is a message.

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

_REPLAY = "src/gpu/gpu_pgraph_replay.c"
_SWAP = "src/gpu/d3d8_swap_replay.c"
_OPTIONS = "src/host/host_options.c"
_VIEWPORT = "tools/nv2a/viewport.py"
_TRANSLATE = "tools/nv2a/translate.py"
_MODULES = "tools/nv2a/vsh_modules.py"
_STEADY = "tools/steady_replay.py"
_XMV = "tools/xmv_replay.py"
_C = ["test_gpu_pgraph", "test_d3d8_swap_replay"]
_GUARD = "test_host_options"
_VIEWPORT_TESTS = ["pytest:tests/test_nv2a_viewport.py", "pytest:tests/test_gpu_vsh_draw.py"]


def _row(file: str, mutation_id: str, old: str, new: str, why: str, targets: list[str]) -> dict:
    return {
        "id": f"gpu-window-clip-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(targets),
        "why": why,
    }


def _c(mutation_id: str, old: str, new: str, why: str, file: str = _REPLAY) -> dict:
    return _row(file, mutation_id, old, new, why, _C)


def _py(
    file: str, mutation_id: str, old: str, new: str, why: str, tests: list[str] | None = None
) -> dict:
    return _row(file, mutation_id, old, new, why, list(tests or _VIEWPORT_TESTS))


MUTATIONS: list[dict] = [
    # ================================================================== pair
    _c(
        "pair-needs-the-statement",
        "    if (result == GPU_PGRAPH_OK && backend->window_clip_modules && backend->viewport_from_target &&\n        !state->constant_written[58]",
        "    if (result == GPU_PGRAPH_OK && true && backend->viewport_from_target &&\n        !state->constant_written[58]",
        "the pair replaces the lone 0x0A20 offset for EVERY replay, which changes the T96 register feed nobody opted out of.",
    ),
    _c(
        "pair-needs-the-target-option",
        "    if (result == GPU_PGRAPH_OK && backend->window_clip_modules && backend->viewport_from_target &&\n",
        "    if (result == GPU_PGRAPH_OK && backend->window_clip_modules && true &&\n",
        "the target size is used without --gpu-replay-viewport-from-target, an inference the user never allowed.",
    ),
    _c(
        "pair-never-replaces-a-written-row",
        "        !state->constant_written[58] && !state->viewport_scale_set) {\n        /* T560.",
        "        !state->viewport_scale_set) {\n        /* T560.",
        "the title's own row 58 and 59 (320, -240 and 320.53125, 240.53125, MEASURED) are overwritten by the target map.",
    ),
    _c(
        "pair-never-replaces-a-register-scale",
        "        !state->constant_written[58] && !state->viewport_scale_set) {\n        /* T560.",
        "        !state->constant_written[58]) {\n        /* T560.",
        "a complete 0x0A20 and 0x0AF0 pair, which is a viewport, is replaced by the target map.",
    ),
    _c(
        "pair-announced",
        '        result = need_inference(backend, GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET,\n                                "deriving whole-target viewport constants 58/59 from the render-target size "\n                                "for the window-to-clip modules",',
        '        result = need_inference(backend, GPU_PGRAPH_INFER_VIEWPORT_CONSTANTS,\n                                "deriving whole-target viewport constants 58/59 from the render-target size "\n                                "for the window-to-clip modules",',
        "the target map is let in under the register feed's inference, so withholding FROM_TARGET no longer stops it.",
    ),
    _c(
        "pair-scale-y-sign",
        "            const float scale[4] = {half_width, -half_height, 1.0f, 0.0f};\n            const float offset[4] = {half_width, half_height, 0.0f, 0.0f};\n            memcpy(out->constants + 58u * 4u, scale, sizeof scale);\n            if (!state->constant_written[59]) {",
        "            const float scale[4] = {half_width, half_height, 1.0f, 0.0f};\n            const float offset[4] = {half_width, half_height, 0.0f, 0.0f};\n            memcpy(out->constants + 58u * 4u, scale, sizeof scale);\n            if (!state->constant_written[59]) {",
        "the y scale loses its sign: every window y is mirrored against the title's own viewport.",
    ),
    _c(
        "pair-scale-z",
        "            const float scale[4] = {half_width, -half_height, 1.0f, 0.0f};\n            const float offset[4] = {half_width, half_height, 0.0f, 0.0f};\n            memcpy(out->constants + 58u * 4u, scale, sizeof scale);\n            if (!state->constant_written[59]) {",
        "            const float scale[4] = {half_width, -half_height, 0.0f, 0.0f};\n            const float offset[4] = {half_width, half_height, 0.0f, 0.0f};\n            memcpy(out->constants + 58u * 4u, scale, sizeof scale);\n            if (!state->constant_written[59]) {",
        "a zero z scale: a z-only module with the inverse divides by nothing and draws z unchanged.",
    ),
    _c(
        "pair-offset-axes",
        "            const float scale[4] = {half_width, -half_height, 1.0f, 0.0f};\n            const float offset[4] = {half_width, half_height, 0.0f, 0.0f};\n            memcpy(out->constants + 58u * 4u, scale, sizeof scale);\n            if (!state->constant_written[59]) {",
        "            const float scale[4] = {half_width, -half_height, 1.0f, 0.0f};\n            const float offset[4] = {half_height, half_width, 0.0f, 0.0f};\n            memcpy(out->constants + 58u * 4u, scale, sizeof scale);\n            if (!state->constant_written[59]) {",
        "the offset axes are swapped (right only on a square target).",
    ),
    _c(
        "pair-keeps-a-written-offset-row",
        "            memcpy(out->constants + 58u * 4u, scale, sizeof scale);\n            if (!state->constant_written[59]) {",
        "            memcpy(out->constants + 58u * 4u, scale, sizeof scale);\n            if (true) {",
        "a row 59 the stream wrote itself is replaced by the target offset.",
    ),
    _c(
        "pair-offset-dropped",
        "            if (!state->constant_written[59]) {\n                memcpy(out->constants + 59u * 4u, offset, sizeof offset);\n            }\n        }\n    }\n    if (result == GPU_PGRAPH_OK && backend->window_clip_modules &&",
        "            if (false) {\n                memcpy(out->constants + 59u * 4u, offset, sizeof offset);\n            }\n        }\n    }\n    if (result == GPU_PGRAPH_OK && backend->window_clip_modules &&",
        "the lone 0x0A20 offset (0.53125) stays in c59 against the whole-target scale.",
    ),
    # ================================================================== refusal
    _c(
        "refuse-needs-the-statement",
        "    if (result == GPU_PGRAPH_OK && backend->window_clip_modules &&\n        out->constants[58u * 4u] == 0.0f",
        "    if (result == GPU_PGRAPH_OK && true &&\n        out->constants[58u * 4u] == 0.0f",
        "every replay without a scale in c58 is refused, whether or not it states window modules.",
    ),
    _c(
        "refuse-both-axes-zero",
        "backend->window_clip_modules &&\n        out->constants[58u * 4u] == 0.0f && out->constants[58u * 4u + 1u] == 0.0f) {\n        result = fail(",
        "backend->window_clip_modules &&\n        (out->constants[58u * 4u] == 0.0f || out->constants[58u * 4u + 1u] == 0.0f)) {\n        result = fail(",
        "a scale with ONE zero axis (the module's guard passes that axis through) is refused.",
    ),
    _c(
        "refuse-x-axis-ignored",
        "        out->constants[58u * 4u] == 0.0f && out->constants[58u * 4u + 1u] == 0.0f) {\n        result = fail(",
        "        true && out->constants[58u * 4u + 1u] == 0.0f) {\n        result = fail(",
        "only the y scale is looked at, so a scale (0, 5) is refused.",
    ),
    _c(
        "refuse-y-axis-ignored",
        "        out->constants[58u * 4u] == 0.0f && out->constants[58u * 4u + 1u] == 0.0f) {\n        result = fail(",
        "        out->constants[58u * 4u] == 0.0f && true) {\n        result = fail(",
        "only the x scale is looked at, so a scale (5, 0) is refused.",
    ),
    _c(
        "refuse-is-an-unmeasured-refusal",
        '        result = fail(report, GPU_PGRAPH_ERR_UNMEASURED,\n                      "window-to-clip modules convert',
        '        result = fail(report, GPU_PGRAPH_ERR_MALFORMED,\n                      "window-to-clip modules convert',
        "the refusal changes class: a missing scale is not a malformed stream.",
    ),
    # ================================================================== wiring
    _c(
        "swap-config-reaches-the-backend",
        "    backend.window_clip_modules = state.config.window_clip_modules;\n",
        "    backend.window_clip_modules = false;\n",
        "the statement never reaches the replay: the frame with no scale is drawn raw instead of refused.",
        file=_SWAP,
    ),
    {
        "id": "gpu-window-clip-option-parsed",
        "file": _OPTIONS,
        "old": "            out->gpu_replay_window_to_clip = true;\n",
        "new": "            out->gpu_replay_window_to_clip = false;\n",
        "targets": [_GUARD],
        "why": "--gpu-replay-window-to-clip is accepted and does nothing.",
    },
    {
        "id": "gpu-window-clip-option-reset",
        "file": _OPTIONS,
        "old": "    out->gpu_replay_window_to_clip = false;\n",
        "new": "    out->gpu_replay_window_to_clip = out->gpu_replay_window_to_clip;\n",
        "targets": [_GUARD],
        "why": "a stale struct keeps the statement of the previous parse.",
    },
    {
        "id": "gpu-window-clip-option-needs-replay",
        "file": _OPTIONS,
        "old": "!out->gpu_replay_flip_y && !out->gpu_replay_undo_viewport && !out->gpu_replay_window_to_clip &&\n",
        "new": "!out->gpu_replay_flip_y && !out->gpu_replay_undo_viewport &&\n",
        "targets": [_GUARD],
        "why": "the statement is accepted without --gpu-replay, where it would silently do nothing.",
    },
]

#: The Python half. NOT run by `tools/mutate/c_suites.py` (its stale binary guard calls a mutation that cannot
#: change a ctest binary INVALID), so it has no `MUTATIONS` entry. `tests/test_mutation_anchors.py` checks every anchor
#: here occurs exactly once, and each was applied by hand (the file restored from git after), the pytest `targets`
#: run, and a failure counted as a kill: see the T560 entry in docs/tasks.md for the result.
PYTHON_MUTATIONS: list[dict] = [
    _py(
        _VIEWPORT,
        "window-needs-the-option",
        "    if window_to_clip and kind in GLSL_BODY_WINDOW:",
        "    if kind in GLSL_BODY_WINDOW:",
        "the conversion is on for every module by default: the default behaviour moves.",
    ),
    _py(
        _VIEWPORT,
        "window-with-the-inverse",
        "        return (GLSL_BODY_WINDOW_UNDO if undo_viewport else GLSL_BODY_WINDOW)[kind]",
        "        return GLSL_BODY_WINDOW[kind]",
        "the z of the z-only class is not unscaled when both options are given.",
    ),
    _py(
        _VIEWPORT,
        "window-y-axis",
        '    "nv2a_unscale(o[0].y, c[58].y, c[59].y) * o[0].w, o[0].z, o[0].w);"',
        '    "nv2a_unscale(o[0].y, c[58].x, c[59].x) * o[0].w, o[0].z, o[0].w);"',
        "y is converted with the x scale and offset.",
    ),
    _py(
        _VIEWPORT,
        "window-w-factor",
        '    "nv2a_unscale(o[0].y, c[58].y, c[59].y) * o[0].w, o[0].z, o[0].w);"',
        '    "nv2a_unscale(o[0].y, c[58].y, c[59].y), o[0].z, o[0].w);"',
        "y is not multiplied by w, so the rasterizer's divide moves it.",
    ),
    _py(
        _VIEWPORT,
        "window-z-untouched",
        '    "nv2a_unscale(o[0].y, c[58].y, c[59].y) * o[0].w, o[0].z, o[0].w);"',
        '    "nv2a_unscale(o[0].y, c[58].y, c[59].y) * o[0].w, o[0].z * o[0].w, o[0].w);"',
        "z is multiplied by w in the window class.",
    ),
    _py(
        _VIEWPORT,
        "twin-z-only-z",
        "            return [unscale(0) * w, unscale(1) * w, unscale(2), w]",
        "            return [unscale(0) * w, unscale(1) * w, unscale(2) * w, w]",
        "the Python twin multiplies the z-only class's z by w.",
    ),
    _py(
        _VIEWPORT,
        "twin-none-z",
        "        return [unscale(0) * w, unscale(1) * w, z, w]",
        "        return [unscale(0) * w, unscale(1) * w, unscale(2), w]",
        "the Python twin unscales the none class's z, which has no transform.",
    ),
    _py(
        _TRANSLATE,
        "translate-plumbing",
        "t.viewport, undo_viewport=undo_viewport, window_to_clip=window_to_clip",
        "t.viewport, undo_viewport=undo_viewport, window_to_clip=False",
        "the shader source ignores the option.",
    ),
    _py(
        _MODULES,
        "modules-plumbing",
        "                undo_viewport=undo_viewport,\n                window_to_clip=window_to_clip,\n                output_init=output_init,",
        "                undo_viewport=undo_viewport,\n                window_to_clip=False,\n                output_init=output_init,",
        "the module generator ignores the option, the replay then runs raw modules it was told are converted.",
        ["pytest:tests/test_nv2a_vsh_modules.py"],
    ),
    _py(
        _MODULES,
        "modules-flag",
        "            window_to_clip=args.window_to_clip,",
        "            window_to_clip=False,",
        "the command line flag is accepted and ignored.",
        ["pytest:tests/test_nv2a_vsh_modules.py"],
    ),
    _py(
        _STEADY,
        "steady-states-it-to-the-host",
        "        if window_to_clip:\n            command.append(WINDOW_TO_CLIP)",
        "        if False:\n            command.append(WINDOW_TO_CLIP)",
        "the steady tool builds converted modules and never says so.",
        ["pytest:tests/test_steady_replay.py"],
    ),
    _py(
        _STEADY,
        "steady-modules",
        '    options: dict[str, object] = {"window_to_clip": True} if window_to_clip else {}',
        "    options: dict[str, object] = {}",
        "the steady tool states converted modules and builds raw ones.",
        ["pytest:tests/test_steady_replay.py"],
    ),
    _py(
        _XMV,
        "xmv-states-it-to-the-host",
        "        if args.window_to_clip:\n            command.append(steady.WINDOW_TO_CLIP)",
        "        if False:\n            command.append(steady.WINDOW_TO_CLIP)",
        "the movie tool builds converted modules and never says so.",
        ["pytest:tests/test_xmv_replay.py"],
    ),
    _py(
        _XMV,
        "xmv-library-modules",
        '    options = {"window_to_clip": True} if window_to_clip else {}',
        "    options = {}",
        "the composition's own program is built raw while everything else is converted.",
        ["pytest:tests/test_xmv_replay.py"],
    ),
]
