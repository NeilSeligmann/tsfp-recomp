# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for the T849 swapchain pre-pass blit present (`--gpu-live-blit`).

TARGET. `test_live_vk_present` (the blit route and the readback route are compared byte for byte on the captured swapchain
image, needs a Vulkan window: run it with `SDL_VIDEODRIVER=x11 TSFP_TEST_GPU_WINDOW_REQUIRED=1 xvfb-run -a`, it skips with 77
without one) and `test_host_options` (`--gpu-live-blit` needs `--gpu-live`).
"""

TARGET = "src/gpu/live_vk_target.c"
WINDOW = "src/gpu/gpu_window.c"


def mutation(
    identifier: str, file: str, old: str, new: str, why: str, target: str = "test_live_vk_present"
) -> dict:
    return {
        "id": f"t849-{identifier}",
        "file": file,
        "old": old,
        "new": new,
        "targets": [target],
        "why": why,
    }


MUTATIONS: list[dict] = [
    mutation(
        "front-extent",
        TARGET,
        "        const VkImageBlit region = blit_region(canvas_width, canvas_height, 0, 0, (int32_t)extent.width, (int32_t)extent.height);\n",
        "        const VkImageBlit region = blit_region(canvas_width, canvas_height, 0, 0, (int32_t)extent.width / 2, (int32_t)extent.height);\n",
        "the front covers the whole acquired image, the same pixels as the readback route.",
    ),
    mutation(
        "front-filter",
        TARGET,
        "                            &region, VK_FILTER_NEAREST);\n",
        "                            &region, VK_FILTER_LINEAR);\n",
        "nearest sampling, like the readback route's nearest scaling, a linear blit blurs the gradient.",
    ),
    mutation(
        "overlay-skipped",
        TARGET,
        "    if (has_overlay) {\n        if (!has_front) {\n",
        "    if (false) {\n        if (!has_front) {\n",
        "the movie overlay layer is blitted over the front.",
    ),
    mutation(
        "overlay-left-unscaled",
        TARGET,
        "set->overlay_width, set->overlay_height, (int32_t)(((uint64_t)left * extent.width) / canvas_width),\n",
        "set->overlay_width, set->overlay_height, (int32_t)left,\n",
        "the pillarbox rectangle is scaled from canvas texels to the destination (portrait picture, left 80).",
    ),
    mutation(
        "overlay-top-unscaled",
        TARGET,
        "                (int32_t)(((uint64_t)top * extent.height) / canvas_height),\n",
        "                (int32_t)top,\n",
        "the letterbox rectangle is scaled from canvas texels to the destination (16:9 picture, top 30).",
    ),
    mutation(
        "overlay-bottom",
        TARGET,
        "                (int32_t)(((uint64_t)(top + fit_height) * extent.height) / canvas_height));\n",
        "                (int32_t)(((uint64_t)(top + fit_height) * extent.height) / canvas_height) - 4);\n",
        "the overlay rectangle ends where the compositor's does.",
    ),
    mutation(
        "overlay-channel-order",
        TARGET,
        "        staging.mapped[i * 4u] = set->overlay_rgb[i * 3u];\n",
        "        staging.mapped[i * 4u] = set->overlay_rgb[i * 3u + 2u];\n",
        "the overlay picture's RGB24 goes into the texture in channel order.",
    ),
    mutation(
        "overlay-stale-texture",
        TARGET,
        "    if (set->overlay_uploaded && set->overlay_uploaded_sequence == set->overlay_sequence) {\n",
        "    if (set->overlay_uploaded) {\n",
        "a new movie picture is uploaded again, not served from the first texture.",
    ),
    mutation(
        "direct-gate",
        TARGET,
        "    if (set->direct != NULL && !set->media_mode && (!has_front || front_blittable(set, use->front_data, &probe))) {\n",
        "    if (set->direct != NULL && !set->media_mode && (!has_front || front_blittable(set, use->front_data, &probe) || true)) {\n",
        "a front the blit cannot read (16 bit) falls back to the readback route.",
    ),
    mutation(
        "direct-ignored",
        TARGET,
        "    if (set->direct != NULL && !set->media_mode && (!has_front || front_blittable(set, use->front_data, &probe))) {\n",
        "    if (false && set->direct != NULL && !set->media_mode && (!has_front || front_blittable(set, use->front_data, &probe))) {\n",
        "with the direct route set, the blit route is the one presenting.",
    ),
    mutation(
        "last-layers",
        TARGET,
        "    set->last_has_front = has_front;\n",
        "    set->last_has_front = false;\n",
        "read_pixel and capture of the blit route compose the last vblank's front.",
    ),
    mutation(
        "blit-count",
        TARGET,
        "            set->stats.blit_frames++;\n",
        "            set->stats.readback_frames++;\n",
        "frames are counted by the route that presented them.",
    ),
    mutation(
        "first-frame-time",
        TARGET,
        "            set->stats.first_frame_ns = finished;\n",
        "",
        "the frame rate is measured from the first presented frame.",
    ),
    mutation(
        "refusal-count",
        WINDOW,
        "        r->stats.blit_refusals++;\n",
        "",
        "a refusing hook is counted and a black frame still presented.",
    ),
    mutation(
        "refusal-result",
        WINDOW,
        "    return recorded;\n}\n\nbool gpu_window_set_capture(",
        "    return true;\n}\n\nbool gpu_window_set_capture(",
        "a refused blit reports false to the caller.",
    ),
    mutation(
        "capture-channel-order",
        WINDOW,
        "    const bool swap_rb = r->format == VK_FORMAT_B8G8R8A8_UNORM || r->format == VK_FORMAT_B8G8R8A8_SRGB;\n    for (size_t i = 0u; i < bytes; i += 4u) {",
        "    const bool swap_rb = false;\n    for (size_t i = 0u; i < bytes; i += 4u) {",
        "the captured swapchain image is converted to RGBA in channel order.",
    ),
    mutation(
        "host-needs-live",
        "src/host/host_options.c",
        "        (!out->gpu_live_blit || out->gpu_live) &&\n",
        "",
        "--gpu-live-blit without --gpu-live does nothing and is refused.",
        "test_host_options",
    ),
]
