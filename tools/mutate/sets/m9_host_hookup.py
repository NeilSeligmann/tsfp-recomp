# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the M9 host hook-up (T838): the second `d3d8_present` observer slot, the live seam of `d3d8_swap_replay.c` (model hook,
`live_only`, SetTexture history, target hook, backend) and the `--gpu-live` options of `host_options.c`.

TARGETS. `test_d3d8_present` (the second slot runs after the first, alone, with both, survives a reset), `test_d3d8_swap_replay`
(`test_live_hook`) and `test_host_options` (`test_gpu_live`). `live_only` needs a Vulkan device to be told from a replay that
draws (the mutant draws the frame, which the test sees as `frames_replayed`), ctest reports it SKIPPED without one.
Left out as equivalent: the `--gpu-live` clause "needs --gpu-replay" (the older clause already refuses the output state and the combiner it implies without a
replay directory, T838 removed the redundant clause instead of keeping a mutant nothing can kill).
The window half (`present_sink.c` live ops, `live_render.c`, the `live_vk_frame_run_targets` loop) is covered by
`tests/test_live_render_boot.py` on the retail disc, mutants of it are in the `window` group below as `pytest:` targets: they need
`TSFP_BUILD_DIR`, `TSFP_XBE`, `TSFP_XBOX_ISO` and Xvfb in the environment and are skipped without them.
"""

PRESENT = "src/gpu/d3d8_present.c"
SWAP = "src/gpu/d3d8_swap_replay.c"
OPTIONS = "src/host/host_options.c"


def _row(identifier: str, file: str, old: str, new: str, targets: list[str], why: str) -> dict:
    return {
        "id": f"t838-{identifier}",
        "file": file,
        "old": old,
        "new": new,
        "targets": targets,
        "why": why,
    }


def present(identifier: str, old: str, new: str, why: str) -> dict:
    return _row(identifier, PRESENT, old, new, ["test_d3d8_present"], why)


def swap(identifier: str, old: str, new: str, why: str) -> dict:
    return _row(identifier, SWAP, old, new, ["test_d3d8_swap_replay"], why)


def options(identifier: str, old: str, new: str, why: str) -> dict:
    return _row(identifier, OPTIONS, old, new, ["test_host_options"], why)


MUTATIONS: list[dict] = [
    present(
        "second-never-called",
        "    if (second_observer != NULL) {\n        second_observer(&record);",
        "    if (false) {\n        second_observer(&record);",
        "the live renderer's observer must run at every Swap(4).",
    ),
    present(
        "second-only-without-first",
        "    if (second_observer != NULL) {\n",
        "    if (second_observer != NULL && present_observer == NULL) {\n",
        "the second slot must run together with the first (the frame profile and the live renderer both run).",
    ),
    present(
        "second-cleared-by-reset",
        "    memset(queue, 0, sizeof(queue));\n",
        "    memset(queue, 0, sizeof(queue));\n    second_observer = NULL;\n",
        "the slot is configuration, a reset keeps it like the first.",
    ),
    present(
        "setter-ignored",
        "    second_observer = observer;\n",
        "    (void)observer;\n",
        "the setter must install the observer.",
    ),
    swap(
        "hook-not-called",
        "        live_model_hook(state.pgraph, frame_number, live_model_hook_context);",
        "        (void)frame_number;",
        "the live hook must run at each accepted present.",
    ),
    swap(
        "hook-frames-uncounted",
        "        live_model_frames++;\n",
        "",
        "the hook's frame counter feeds the stop report.",
    ),
    swap(
        "hook-state-pointer",
        "        live_model_hook(state.pgraph, frame_number,",
        "        live_model_hook(NULL, frame_number,",
        "the hook gets the model whose draw list is the frame's.",
    ),
    {
        **swap(
            "live-only-ignored",
            "    if (state.config.live_only) {\n",
            "    if (false) {\n",
            "live_only must skip the CPU replay: no device, no image.",
        ),
        "needs_device": True,
    },
    swap(
        "texture-notes-need-rt-texture",
        "!(state.config.render_target_texture || live_model_hook != NULL)",
        "!state.config.render_target_texture",
        "a live hook must make SetTexture noted (the binding history is the live renderer's input).",
    ),
    swap(
        "target-hook-previous-dropped",
        "            live_target_hook(previous, live_target_hook_context);",
        "            (void)previous;",
        "the target being left is reported too (it may never have been registered).",
    ),
    swap(
        "target-hook-target-dropped",
        "        live_target_hook(target, live_target_hook_context);\n        if (previous != 0u) {",
        "        if (previous != 0u) {",
        "the new target is reported.",
    ),
    swap(
        "target-hook-on-repeat",
        "    if (previous == target) {\n        return;\n    }\n    if (live_target_hook != NULL) {",
        "    if (previous == target) {\n    }\n    if (live_target_hook != NULL) {",
        "a SetRenderTarget that changes nothing reports nothing.",
    ),
    swap(
        "binding-unbound-as-bound",
        'bool d3d8_swap_replay_live_binding(size_t draw, uint32_t stage, uint32_t *header, uint32_t *format, uint32_t *size_word, uint32_t *data,\n                                   const char **reason)\n{\n    const texture_binding *binding = stage < TEXTURE_STAGES ? binding_at(stage, draw) : NULL;\n    if (binding == NULL) {\n        *reason = "no binding observed: no SetTexture of the stage was seen since the live renderer was enabled";\n        return false;\n    }\n    if (!binding->bound) {\n        *reason = "unbound: the stage\'s last SetTexture was 0";\n        return false;\n    }\n    *header = binding->header;\n    *format = binding->format;\n    *size_word = binding->size_word;\n    *data = binding->data;\n    return true;\n}',
        'bool d3d8_swap_replay_live_binding(size_t draw, uint32_t stage, uint32_t *header, uint32_t *format, uint32_t *size_word, uint32_t *data,\n                                   const char **reason)\n{\n    const texture_binding *binding = stage < TEXTURE_STAGES ? binding_at(stage, draw) : NULL;\n    if (binding == NULL) {\n        *reason = "no binding observed: no SetTexture of the stage was seen since the live renderer was enabled";\n        return false;\n    }\n    if (false) {\n        *reason = "unbound: the stage\'s last SetTexture was 0";\n        return false;\n    }\n    *header = binding->header;\n    *format = binding->format;\n    *size_word = binding->size_word;\n    *data = binding->data;\n    return true;\n}',
        "an unbound stage (SetTexture 0) has no binding.",
    ),
    swap(
        "binding-data-is-header",
        'bool d3d8_swap_replay_live_binding(size_t draw, uint32_t stage, uint32_t *header, uint32_t *format, uint32_t *size_word, uint32_t *data,\n                                   const char **reason)\n{\n    const texture_binding *binding = stage < TEXTURE_STAGES ? binding_at(stage, draw) : NULL;\n    if (binding == NULL) {\n        *reason = "no binding observed: no SetTexture of the stage was seen since the live renderer was enabled";\n        return false;\n    }\n    if (!binding->bound) {\n        *reason = "unbound: the stage\'s last SetTexture was 0";\n        return false;\n    }\n    *header = binding->header;\n    *format = binding->format;\n    *size_word = binding->size_word;\n    *data = binding->data;\n    return true;\n}',
        'bool d3d8_swap_replay_live_binding(size_t draw, uint32_t stage, uint32_t *header, uint32_t *format, uint32_t *size_word, uint32_t *data,\n                                   const char **reason)\n{\n    const texture_binding *binding = stage < TEXTURE_STAGES ? binding_at(stage, draw) : NULL;\n    if (binding == NULL) {\n        *reason = "no binding observed: no SetTexture of the stage was seen since the live renderer was enabled";\n        return false;\n    }\n    if (!binding->bound) {\n        *reason = "unbound: the stage\'s last SetTexture was 0";\n        return false;\n    }\n    *header = binding->header;\n    *format = binding->format;\n    *size_word = binding->size_word;\n    *data = binding->header;\n    return true;\n}',
        "the Data word of the binding is the header's Data word, not its address.",
    ),
    swap(
        "binding-format-size",
        'bool d3d8_swap_replay_live_binding(size_t draw, uint32_t stage, uint32_t *header, uint32_t *format, uint32_t *size_word, uint32_t *data,\n                                   const char **reason)\n{\n    const texture_binding *binding = stage < TEXTURE_STAGES ? binding_at(stage, draw) : NULL;\n    if (binding == NULL) {\n        *reason = "no binding observed: no SetTexture of the stage was seen since the live renderer was enabled";\n        return false;\n    }\n    if (!binding->bound) {\n        *reason = "unbound: the stage\'s last SetTexture was 0";\n        return false;\n    }\n    *header = binding->header;\n    *format = binding->format;\n    *size_word = binding->size_word;\n    *data = binding->data;\n    return true;\n}',
        'bool d3d8_swap_replay_live_binding(size_t draw, uint32_t stage, uint32_t *header, uint32_t *format, uint32_t *size_word, uint32_t *data,\n                                   const char **reason)\n{\n    const texture_binding *binding = stage < TEXTURE_STAGES ? binding_at(stage, draw) : NULL;\n    if (binding == NULL) {\n        *reason = "no binding observed: no SetTexture of the stage was seen since the live renderer was enabled";\n        return false;\n    }\n    if (!binding->bound) {\n        *reason = "unbound: the stage\'s last SetTexture was 0";\n        return false;\n    }\n    *header = binding->header;\n    *format = binding->size_word;\n    *size_word = binding->format;\n    *data = binding->data;\n    return true;\n}',
        "Format and Size words are not interchangeable.",
    ),
    swap(
        "backend-inferences-dropped",
        "    out->allowed_inferences = state.config.allowed_inferences;",
        "    out->allowed_inferences = 0u;",
        "the live backend allows the same inferences as the replay it replaces.",
    ),
    swap(
        "backend-flip-inverted",
        "    out->flip_y = state.config.flip_y;",
        "    out->flip_y = !state.config.flip_y;",
        "the live backend orients like the replay.",
    ),
    swap(
        "backend-combiner-forced",
        "    out->combiner = state.config.combiner;\n    out->fragment_table",
        "    out->combiner = true;\n    out->fragment_table",
        "the live backend uses the combiner exactly when the replay does.",
    ),
    swap(
        "backend-when-disabled",
        "bool d3d8_swap_replay_live_backend(gpu_pgraph_backend *out)\n{\n    if (!state.active) {",
        "bool d3d8_swap_replay_live_backend(gpu_pgraph_backend *out)\n{\n    if (false) {",
        "no backend without an enabled replay (its module tables are the replay's).",
    ),
    options(
        "live-default-on",
        "    out->gpu_live = false;\n",
        "    out->gpu_live = true;\n",
        "the live renderer is off by default.",
    ),
    options(
        "live-implies-output-state-dropped",
        "            out->gpu_live = true;\n            out->gpu_replay_output_state = true;",
        "            out->gpu_live = true;",
        "--gpu-live needs the output state the live draws take their state from.",
    ),
    options(
        "live-any-sink",
        '(out->present != NULL && strcmp(out->present, "window") == 0 && !out->gpu_replay_rt_texture &&',
        "(out->present != NULL && !out->gpu_replay_rt_texture &&",
        "--gpu-live needs the window sink.",
    ),
    options(
        "live-with-rt-texture",
        'strcmp(out->present, "window") == 0 && !out->gpu_replay_rt_texture &&',
        'strcmp(out->present, "window") == 0 &&',
        "--gpu-live replaces the replay's image paths, the render target texture bridge is one.",
    ),
    options(
        "live-with-flip-y",
        "          !out->gpu_replay_standin && !out->gpu_replay_flip_y && out->gpu_replay_dump == NULL)) &&",
        "          !out->gpu_replay_standin && out->gpu_replay_dump == NULL)) &&",
        "--gpu-live has no flip_y of the replay's image.",
    ),
    options(
        "live-with-dump",
        "          !out->gpu_replay_standin && !out->gpu_replay_flip_y && out->gpu_replay_dump == NULL)) &&",
        "          !out->gpu_replay_standin && !out->gpu_replay_flip_y)) &&",
        "--gpu-live replays no image, a replay dump directory would stay empty.",
    ),
    options(
        "inferred-without-live",
        "        (!out->gpu_live_inferred || out->gpu_live) &&\n",
        "",
        "--gpu-live-inferred without --gpu-live does nothing and is refused.",
    ),
]
for _mutation in MUTATIONS:
    _mutation.setdefault("needs_device", False)
