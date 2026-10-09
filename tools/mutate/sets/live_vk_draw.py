# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for the live draws into T793 target images (T829): src/gpu/live_vk_draw.c, the guest target pass of live_vk_pipeline.c,
the target hook-up of live_vk_frame.c and the feedback guard of live_vk_bind.c.

TARGETS. `test_live_vk_draw` (every scene drawn into a BGRA8 target against gpu_pgraph_replay, draw then sample in place, two
targets in alternation, the per frame depth reset, the named refusals) and `test_live_vk_frame` (the window frame hook with
`live_vk_frame_enable_target_draws`: draw, CopyRects, draw, present, the feedback refusal). The frame test needs an X display, so
run the harness as `SDL_VIDEODRIVER=x11 TSFP_TEST_GPU_WINDOW_REQUIRED=1 xvfb-run -a ... tools/mutate/c_suites.py --prefix t829`:
under the dummy driver it skips with 77, which kills nothing.

EQUIVALENT, not listed: the bytes per pixel clause of the registration check (the format clause beside it implies it), the image of a target the set holds without an image (the entry build refuses it), the colour layout GENERAL of the target pass (only a validation layer checks the layout, llvmpipe and radv
read and write the image either way), the 28 bit mask of the colour offset (every test offset fits in 28 bits), the zeta nibble
range (the output state decode refuses a surface format other than 0x128 first).
"""

DRAW = "src/gpu/live_vk_draw.c"
PIPELINE = "src/gpu/live_vk_pipeline.c"
FRAME = "src/gpu/live_vk_frame.c"
BIND = "src/gpu/live_vk_bind.c"
TARGET = "src/gpu/live_vk_target.c"


def mutation(identifier: str, file: str, old: str, new: str, targets: list[str], why: str) -> dict:
    return {
        "id": f"t829-{identifier}",
        "file": file,
        "old": old,
        "new": new,
        "targets": targets,
        "why": why,
    }


def draw(identifier: str, old: str, new: str, why: str) -> dict:
    return mutation(identifier, DRAW, old, new, ["test_live_vk_draw"], why)


def frame(identifier: str, file: str, old: str, new: str, why: str) -> dict:
    return mutation(identifier, file, old, new, ["test_live_vk_frame"], why)


MUTATIONS: list[dict] = [
    draw(
        "pitch-unchecked",
        "    if (type == 1u && pitch != desc->pitch) {\n",
        "    if (false) {\n",
        "a surface pitch that is not the registered target's must be refused, not drawn.",
    ),
    draw(
        "colour-format-any",
        "    if (colour != 8u) {\n",
        "    if (colour == 15u) {\n",
        "an X8R8G8B8 or R5G6B5 surface state must be refused by name.",
    ),
    draw(
        "layout-type-any",
        "(type != 1u && type != 2u)",
        "false",
        "a surface type other than pitch (1) or swizzled (2) must be refused.",
    ),
    draw(
        "words-written-unchecked",
        "    if (!words->written) {\n",
        "    if (false) {\n",
        "a stream that never wrote the surface format, pitch or colour offset names no target (T848: one check for the three, draws and clears).",
    ),
    draw(
        "run-not-switched",
        "    if (draw->open != index) {\n",
        "    if (draw->open == NO_RUN) {\n",
        "a draw naming another target must close the run and open its own: A, B, A is three runs.",
    ),
    draw(
        "depth-cleared-once",
        "    if (entry->cleared_epoch != draw->epoch) {\n",
        "    if (entry->cleared_epoch == 0u) {\n",
        "the depth and stencil restart at the first draw into a target of each frame.",
    ),
    draw(
        "depth-epoch-frozen",
        "    (void)live_vk_draw_flush(draw);\n    (void)live_vk_draw_sync(draw);\n    draw->epoch++;\n",
        "    (void)live_vk_draw_flush(draw);\n    (void)live_vk_draw_sync(draw);\n",
        "a new frame must advance the epoch the depth reset keys on.",
    ),
    draw(
        "depth-clear-value",
        "        const VkClearDepthStencilValue depth = {1.0f, 0u}; /* the replay's pass starts here */",
        "        const VkClearDepthStencilValue depth = {0.0f, 0u}; /* the replay's pass starts here */",
        "the depth restarts at 1.0 as the replay's pass does.",
    ),
    draw(
        "generation-not-bumped",
        "    live_vk_target_note_written(draw->targets, entry->data);\n",
        "",
        "a draw changes the target: its generation (present cache, texture invalidation) must move.",
    ),
    draw(
        "run-not-submitted",
        "(draw->command) == VK_SUCCESS &&\n              draw->vkQueueSubmit(draw->device.queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS;\n",
        "(draw->command) == VK_SUCCESS &&\n              ((void)submit, true);\n",
        "a closed run must be submitted: the target would stay empty.",
    ),
    draw(
        "run-not-waited",
        "    const bool ok = draw->vkQueueWaitIdle(draw->device.queue) == VK_SUCCESS;",
        "    const bool ok = true;",
        "pending retired runs must complete before freeing their command buffers or reusing resources; the sync path now owns the wait.",
    ),
    draw(
        "target-pass-kind",
        "LIVE_VK_PASS_TARGET, draw->entries[index].width, draw->entries[index].height};",
        "LIVE_VK_PASS_OFFSCREEN, draw->entries[index].width, draw->entries[index].height};",
        "the draw must use the pipeline cache of the BGRA8 target pass.",
    ),
    draw(
        "entry-size-swapped",
        "    entry->width = extent.width;\n    entry->height = extent.height;\n",
        "    entry->width = extent.height;\n    entry->height = extent.width;\n",
        "the 16 x 8 target is not square: the extent must not be swapped.",
    ),
    mutation(
        "pass-format-rgba",
        PIPELINE,
        "    return kind == LIVE_VK_PASS_TARGET ? GUEST_TARGET_FORMAT : TARGET_FORMAT;",
        "    return TARGET_FORMAT;",
        ["test_live_vk_draw"],
        "the target pass is BGRA8: the guest A8R8G8B8 byte order.",
    ),
    mutation(
        "target-pass-no-depth-state",
        PIPELINE,
        "pass->kind != LIVE_VK_PASS_WINDOW ? &depth_state : NULL",
        "pass->kind == LIVE_VK_PASS_OFFSCREEN ? &depth_state : NULL",
        ["test_live_vk_draw"],
        "a pipeline of the target pass carries the depth and stencil state.",
    ),
    mutation(
        "texture-id-slot-plus-one",
        TARGET,
        "    return slot != NULL && slot->has_image ? (uint32_t)(slot - set->slots) + 1u : 0u;",
        "    return slot != NULL && slot->has_image ? (uint32_t)(slot - set->slots) : 0u;",
        ["test_live_vk_draw"],
        "the texture id is slot + 1 so slot 0 does not read as unknown.",
    ),
    frame(
        "clear-to-window",
        FRAME,
        "    if (frame->draw != NULL) { /* T829: the clear goes to the target of the draw it precedes, not to the swapchain */\n",
        "    if (false) { /* T829: the clear goes to the target of the draw it precedes, not to the swapchain */\n",
        "a clear event of a target frame must reach the target (the swapchain pass would take it otherwise).",
    ),
    frame(
        "copy-not-flushed",
        FRAME,
        "    if (frame->draw != NULL && !live_vk_draw_flush(frame->draw)) { /* the draws before the copy must be in the images */\n",
        "    if (false) { /* the draws before the copy must be in the images */\n",
        "the copy must carry exactly the draws before it: B equals A as of the copy.",
    ),
    frame(
        "frame-end-not-flushed",
        FRAME,
        "    if (frame->draw != NULL && !live_vk_draw_flush(frame->draw)) {\n        all_drawn = false;\n        if (device_ok != NULL) {\n            *device_ok = false;\n        }\n    }\n",
        "    (void)device_ok;\n",
        "the last run of the frame must be submitted before the hook returns.",
    ),
    frame(
        "frame-begin-not-reset",
        FRAME,
        "    if (frame->draw != NULL) {\n        live_vk_draw_begin_frame(frame->draw);\n    }\n",
        "",
        "the frame hook starts a draw frame: the depth reset epoch moves once per frame.",
    ),
    frame(
        "enable-no-hook",
        FRAME,
        "    live_vk_renderer_set_target_hook(frame->renderer, &hook);\n    live_vk_renderer_set_target_command(",
        "    (void)hook;\n    live_vk_renderer_set_target_command(",
        "enabling the target draws must install the renderer's target hook.",
    ),
    frame(
        "feedback-id-not-set",
        FRAME,
        "        live_vk_bind_set_draw_target(frame->config.textures, accepted ? live_vk_draw_current_texture_id(frame->draw) : 0u);\n",
        "",
        "the bridge must learn which target the draw renders into.",
    ),
    mutation(
        "feedback-guard",
        BIND,
        "        result.source == LIVE_TEXTURE_SOURCE_TARGET && bind->draw_target_id != 0u && result.target_id == bind->draw_target_id;\n",
        "        false;\n",
        ["test_live_vk_frame"],
        "a draw that samples the target it renders into reads a snapshot of it (T1206), refused by name without a provider.",
    ),
    mutation(
        "current-id-unset",
        DRAW,
        "    draw->current_id = draw->entries[index].texture_id;\n",
        "",
        ["test_live_vk_frame"],
        "the provider reports the target of the accepted draw (the frame test observes it through the feedback refusal).",
    ),
]
