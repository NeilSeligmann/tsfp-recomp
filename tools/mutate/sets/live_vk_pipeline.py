# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for the live Vulkan draw renderer (T791): src/gpu/live_vk_pipeline.c, live_vk_bind.c and the texel and census
additions of live_pipeline.c.

TARGETS. `test_live_vk_pipeline` (pixel parity with gpu_pgraph_replay over 22 scenes (T828 added clears and flip_y) in the window and the offscreen pass, cache,
census, hooks) and `test_live_vk_bind` (textured draws against the replay, swizzled and linear, a guest rewrite through the write
watch, refusals). Both skip with 77 without a Vulkan device, so a run without one kills nothing: the harness reports that as a
skip. live_vk_frame.c (needs an X display) is mutation-checked by hand under Xvfb, see docs/live-pipeline.md.

EQUIVALENT, not listed: the arena alignment (every vertex block is a multiple of 256 bytes and llvmpipe accepts a 64 byte uniform
offset), the texel size height lane for a square texture, and the INFERRED sampler gate in live_vk_bind.c (gpu_pgraph_resolve_output
refuses every sampler word but the measured ones first). The descriptor layout of a target texture (GENERAL, T793's images)
is only checked by a validation layer, llvmpipe reads the image either way, so it is not mutated.
"""

PIPELINE = "src/gpu/live_vk_pipeline.c"
BIND = "src/gpu/live_vk_bind.c"
CORE = "src/gpu/live_pipeline.c"


def mutation(identifier: str, file: str, old: str, new: str, targets: list[str], why: str) -> dict:
    return {
        "id": f"t791vk-{identifier}",
        "file": file,
        "old": old,
        "new": new,
        "targets": targets,
        "why": why,
    }


def pipeline(identifier: str, old: str, new: str, why: str) -> dict:
    return mutation(identifier, PIPELINE, old, new, ["test_live_vk_pipeline"], why)


def bind(identifier: str, file: str, old: str, new: str, why: str) -> dict:
    return mutation(identifier, file, old, new, ["test_live_vk_bind"], why)


MUTATIONS: list[dict] = [
    pipeline(
        "scissor-dynamic",
        "    if (dynamic->scissor) {\n",
        "    if (false) {\n",
        "the scissor scene must show the rectangle.",
    ),
    pipeline(
        "blend-constants",
        "    r->fn.vkCmdSetBlendConstants(command, dynamic->blend_constant);\n",
        "",
        "the blend constant scene reads the constant colour.",
    ),
    pipeline(
        "stencil-reference",
        "        r->fn.vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, dynamic->stencil_ref);\n",
        "        r->fn.vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0u);\n",
        "the stencil scene draws only where the first draw wrote the reference 7.",
    ),
    pipeline(
        "depth-bias",
        "        r->fn.vkCmdSetDepthBias(command, dynamic->depth_bias_constant, 0.0f, dynamic->depth_bias_slope);\n",
        "        r->fn.vkCmdSetDepthBias(command, 0.0f, 0.0f, dynamic->depth_bias_slope);\n",
        "the offset scene pushes a coplanar triangle behind with the constant factor.",
    ),
    pipeline(
        "vertex-offset",
        "    const VkDeviceSize vertex_offset_bytes = vertex_offset;\n",
        "    const VkDeviceSize vertex_offset_bytes = 0u;\n",
        "the second draw of a frame must read its own vertices.",
    ),
    pipeline(
        "vertex-constants",
        "    memcpy(block->map + constant_offset, assembled.constants,\n           CONSTANT_FILE_BYTES);\n",
        "    memset(block->map + constant_offset, 0, CONSTANT_FILE_BYTES);\n",
        "the vertex program reads the constant file.",
    ),
    pipeline(
        "fragment-constants",
        "        memcpy(block->map + fragment_offset, selection.description.fragment.plan.constants, FRAGMENT_BYTES);\n",
        "        memset(block->map + fragment_offset, 0, FRAGMENT_BYTES);\n",
        "the combiner scenes read the factor block and it changes between two draws.",
    ),
    pipeline(
        "cull-front",
        "            cull_mode = output->cull_mode == GPU_VSH_CULL_FRONT  ? VK_CULL_MODE_FRONT_BIT\n",
        "            cull_mode = output->cull_mode == GPU_VSH_CULL_FRONT  ? VK_CULL_MODE_BACK_BIT\n",
        "front and back culling are different.",
    ),
    pipeline(
        "front-face",
        "            front_face = output->front_clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;\n",
        "            front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;\n",
        "the cull scene's two windings must be told apart.",
    ),
    pipeline(
        "blend-enable",
        "                blend_attachment.blendEnable = VK_TRUE;\n",
        "                blend_attachment.blendEnable = VK_FALSE;\n",
        "blending must blend.",
    ),
    pipeline(
        "colour-mask",
        "                blend_attachment.colorWriteMask &= ~(VkColorComponentFlags)VK_COLOR_COMPONENT_R_BIT;\n",
        "                blend_attachment.colorWriteMask &= ~(VkColorComponentFlags)VK_COLOR_COMPONENT_G_BIT;\n",
        "the colour mask scene withholds red.",
    ),
    pipeline(
        "depth-test",
        "            depth_state.depthTestEnable = output->depth_test ? VK_TRUE : VK_FALSE;\n",
        "            depth_state.depthTestEnable = VK_FALSE;\n",
        "the depth scene hides a triangle behind another.",
    ),
    pipeline(
        "depth-write",
        "            depth_state.depthWriteEnable = output->depth_test && output->depth_write ? VK_TRUE : VK_FALSE;\n",
        "            depth_state.depthWriteEnable = VK_FALSE;\n",
        "without depth writes nothing is behind anything.",
    ),
    pipeline(
        "stencil-test",
        "            depth_state.stencilTestEnable = output->stencil_test ? VK_TRUE : VK_FALSE;\n",
        "            depth_state.stencilTestEnable = VK_FALSE;\n",
        "the stencil scene's second draw is limited by the stencil.",
    ),
    pipeline(
        "alpha-specialisation",
        "                .pSpecializationInfo = alpha_test ? &alpha_specialization : NULL,\n",
        "                .pSpecializationInfo = alpha_test && alpha_values[1] == 0xFFFFFFFFu ? &alpha_specialization : NULL,\n",
        "the alpha test function and reference reach the fragment stage.",
    ),
    pipeline(
        "alpha-reference",
        "        const uint32_t alpha_values[2] = {alpha_test ? output->alpha_func : 0u, alpha_test ? output->alpha_ref : 0u};\n",
        "        const uint32_t alpha_values[2] = {alpha_test ? output->alpha_func : 0u, 0u};\n",
        "the alpha scene's faint triangle must fail the reference 100.",
    ),
    pipeline(
        "viewport",
        "    const VkViewport viewport = {0.0f, 0.0f, (float)target->width, (float)target->height, 0.0f, 1.0f};\n",
        "    const VkViewport viewport = {0.0f, 0.0f, (float)target->width, (float)target->height * 0.5f, 0.0f, 1.0f};\n",
        "the viewport covers the target.",
    ),
    pipeline(
        "window-depth-refusal",
        "    if (needs_depth && pass->kind == LIVE_VK_PASS_WINDOW) {\n",
        "    if (false) {\n",
        "a depth tested draw in the colour only pass is refused by name, not drawn without its test.",
    ),
    pipeline(
        "arena-reset",
        "        r->arena_blocks[block].used = 0u;\n",
        "        (void)block;\n",
        "the arena starts over every frame (the peak of the second frame equals the first).",
    ),
    pipeline(
        "arena-growth",
        "        if (r->arena_block >= r->arena_block_count && !make_arena(r, total > r->arena_block_bytes ? total : r->arena_block_bytes)) {\n",
        "        if (r->arena_block >= r->arena_block_count) {\n",
        "T1339: a full arena block makes the next block instead of refusing the draw (test_arena_growth).",
    ),
    pipeline(
        "arena-refused-frames",
        "    r->frame_refused++;\n",
        "",
        "T1339: a frame with a refused draw is counted (frames_with_refusals, first_refused_frame).",
    ),
    pipeline(
        "pipelines-created",
        "    r->stats.pipelines_created++;\n",
        "",
        "the created pipelines are counted.",
    ),
    pipeline(
        "flip-refused",
        "    if (backend->flip_y && !device->negative_viewport) {\n",
        "    if (false) {\n",
        "flip_y on a device without VK_KHR_maintenance1 is refused at create (T828).",
    ),
    pipeline(
        "refuse-selected",
        "    if (selected) {\n        live_pipeline_census_refuse_selected(",
        "    if (!selected) {\n        live_pipeline_census_refuse_selected(",
        "a refusal after selection takes the draw out of the selected count, one before it does not.",
    ),
    pipeline(
        "refuse-census",
        "        live_pipeline_census_refuse(&r->census, draw, stage, GPU_PGRAPH_ERR_DEVICE, reason);\n",
        "        (void)stage;\n",
        "a target hook refusal is in the census.",
    ),
    bind(
        "texel-rewrite",
        PIPELINE,
        "            if (description->texel_stages != 0u) {\n",
        "            if (false) {\n",
        "a linear texture's coordinate is in texels and the module is rewritten for it.",
    ),
    pipeline(
        "default-inputs-reported",
        "                    rewrite |= GPU_PGRAPH_INFER_OUTPUT_UNWRITTEN_VARYING;\n",
        "                    rewrite |= 0u;\n",
        "the INFERRED varying default is reported in the stats.",
    ),
    pipeline(
        "interface-check",
        "            if ((needed & ~provided) != 0u) {\n",
        "            if (false) {\n",
        "a combiner reading varyings the program never wrote needs the default rewrite.",
    ),
    pipeline(
        "fragment-binding",
        "                                                      .dstBinding = 1u,\n",
        "                                                      .dstBinding = 0u,\n",
        "the combiner block is binding 1.",
    ),
    bind(
        "linear-filter",
        BIND,
        "    out->linear = sampler.mag_filter == VK_FILTER_LINEAR;\n",
        "    out->linear = false;\n",
        "the replay's sampling follows the decoded filter.",
    ),
    bind(
        "repeat-address",
        BIND,
        "    out->repeat = sampler.address_u == VK_SAMPLER_ADDRESS_MODE_REPEAT;\n",
        "    out->repeat = false;\n",
        "the wrap case must repeat.",
    ),
    bind(
        "texel-flag",
        BIND,
        "    out->unnormalised = result.source == LIVE_TEXTURE_SOURCE_TARGET ? !result.target_swizzled\n                                                                    : !bind->cache->entries[result.entry].plan.swizzled;\n",
        "    out->unnormalised = false;\n",
        "a linear texture takes texel coordinates, so its frame differs from the swizzled one.",
    ),
    bind(
        "unwritten-sampler-refusal",
        BIND,
        "        out->rgba = NULL;\n        out->refusal = reason;\n",
        "        out->rgba = bind->placeholder;\n        out->refusal = reason;\n",
        "a stage whose sampler words were never written has no texture and the draw is refused.",
    ),
    bind(
        "target-view",
        BIND,
        "            view = real_view;\n",
        "            view = VK_NULL_HANDLE;\n",
        "a render target texture is sampled in place through the view of the T793 image.",
    ),
    bind(
        "watch-drain",
        BIND,
        "    (void)live_texture_watch_drain(bind->cache);\n",
        "    (void)bind;\n",
        "a guest write noted through the watch re-decodes and re-uploads the texture.",
    ),
    bind(
        "texel-collect",
        CORE,
        "                draw_backend.test_textures[texture].unnormalised) {\n",
        "                false) {\n",
        "the texel stages of a draw come from the planning textures.",
    ),
    mutation(
        "key-texel-mask",
        CORE,
        "    key->words[LIVE_PIPELINE_KEY_TEXEL_WORD] = description->has_fragment ? description->texel_stages : 0u;\n",
        "    key->words[LIVE_PIPELINE_KEY_TEXEL_WORD] = 0u;\n",
        ["test_live_pipeline"],
        "the texel stage mask is baked.",
    ),
    mutation(
        "key-texel-gate",
        CORE,
        "    key->words[LIVE_PIPELINE_KEY_TEXEL_WORD] = description->has_fragment ? description->texel_stages : 0u;\n",
        "    key->words[LIVE_PIPELINE_KEY_TEXEL_WORD] = description->texel_stages;\n",
        ["test_live_pipeline"],
        "no combiner, no rewrite.",
    ),
    mutation(
        "key-texel-width",
        CORE,
        "            key->words[LIVE_PIPELINE_KEY_TEXEL_WORD + 1u + 2u * texture] = description->texel_size[texture][0];\n",
        "            key->words[LIVE_PIPELINE_KEY_TEXEL_WORD + 1u + 2u * texture] = 0u;\n",
        ["test_live_pipeline"],
        "the rewrite bakes the texture width.",
    ),
    mutation(
        "key-texel-height",
        CORE,
        "            key->words[LIVE_PIPELINE_KEY_TEXEL_WORD + 2u + 2u * texture] = description->texel_size[texture][1];\n",
        "            key->words[LIVE_PIPELINE_KEY_TEXEL_WORD + 2u + 2u * texture] = 0u;\n",
        ["test_live_pipeline"],
        "the rewrite bakes the texture height.",
    ),
    mutation(
        "census-selected-decrement",
        CORE,
        "    if (census->selected != 0u) {\n        census->selected--;\n    }\n",
        "",
        ["test_live_pipeline"],
        "a draw refused after selection is no longer selected.",
    ),
    mutation(
        "stage-name-vertex",
        CORE,
        '    case LIVE_STAGE_VERTEX:\n        return "vertex";\n',
        '    case LIVE_STAGE_VERTEX:\n        return "none";\n',
        ["test_live_pipeline"],
        "the vertex stage has its own name.",
    ),
    # T828: CLEAR_SURFACE and flip_y. The clear and flip scenes of test_live_vk_pipeline compare every pixel with the replay.
    pipeline(
        "flip-viewport-origin",
        "    const VkViewport viewport = {0.0f, r->flip_y ? (float)target->height : 0.0f, (float)target->width,\n",
        "    const VkViewport viewport = {0.0f, 0.0f, (float)target->width,\n",
        "the negative viewport starts at the bottom row, from row 0 it draws nothing.",
    ),
    pipeline(
        "flip-viewport-height",
        "                                 r->flip_y ? -(float)target->height : (float)target->height, 0.0f, 1.0f};\n",
        "                                 (float)target->height, 0.0f, 1.0f};\n",
        "the flip scenes are mirrored by the negative height.",
    ),
    pipeline(
        "flip-resolvers-unmirrored",
        "    r->backend.flip_y = false;\n",
        "",
        "the resolvers run as for the finished image, mirroring the scissor, winding and clear twice would break the flip scenes.",
    ),
    pipeline(
        "clear-whole-colour",
        "    bool whole_colour = resolved.colour && resolved.channels == all_channels;\n",
        "    bool whole_colour = resolved.colour;\n",
        "a partial channel mask must not take the all channel clear (the clear mask scene).",
    ),
    pipeline(
        "clear-colour-scale",
        "        colour[lane] = (float)resolved.rgba[lane] / 255.0f;\n",
        "        colour[lane] = (float)resolved.rgba[lane] / 256.0f;\n",
        "the clear colour bytes reach the target exactly.",
    ),
    pipeline(
        "clear-rect-origin",
        "        .rect = {{(int32_t)resolved.x_min, (int32_t)resolved.y_min},\n",
        "        .rect = {{0, 0},\n",
        "the partial clear rectangle starts at its x_min, y_min.",
    ),
    pipeline(
        "clear-rect-width",
        "                 {resolved.x_max - resolved.x_min + 1u, resolved.y_max - resolved.y_min + 1u}},\n",
        "                 {resolved.x_max - resolved.x_min, resolved.y_max - resolved.y_min + 1u}},\n",
        "the inclusive rectangle covers its last column.",
    ),
    pipeline(
        "clear-rect-height",
        "                 {resolved.x_max - resolved.x_min + 1u, resolved.y_max - resolved.y_min + 1u}},\n",
        "                 {resolved.x_max - resolved.x_min + 1u, resolved.y_max - resolved.y_min}},\n",
        "the inclusive rectangle covers its last row.",
    ),
    pipeline(
        "clear-depth-value",
        "            depth_attachment.clearValue.depthStencil.depth = resolved.z;\n",
        "            depth_attachment.clearValue.depthStencil.depth = 1.0f;\n",
        "the depth clear value decides which draws pass (the clear depth scene).",
    ),
    pipeline(
        "clear-stencil-value",
        "            depth_attachment.clearValue.depthStencil.stencil = resolved.stencil_value;\n",
        "            depth_attachment.clearValue.depthStencil.stencil = 0u;\n",
        "the stencil clear value decides where the EQUAL test passes (the clear stencil scene).",
    ),
    pipeline(
        "clear-depth-aspect",
        "            depth_attachment.aspectMask = (resolved.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) |\n",
        "            depth_attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |\n",
        "a stencil only clear must leave the depth alone.",
    ),
    pipeline(
        "clear-stencil-aspect",
        "                                          (resolved.stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);\n",
        "                                          VK_IMAGE_ASPECT_STENCIL_BIT;\n",
        "a depth only clear must leave the stencil alone.",
    ),
    pipeline(
        "clear-mask-write-mask",
        "        const VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = mask};\n",
        "        const VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = 0xFu};\n",
        "the masked clear writes only the flagged channels.",
    ),
    pipeline(
        "clear-mask-scissor",
        "        r->fn.vkCmdSetScissor(target->command_buffer, 0u, 1u, &rect.rect);\n",
        "        r->fn.vkCmdSetScissor(target->command_buffer, 0u, 1u, &(VkRect2D){{0, 0}, {target->width, target->height}});\n",
        "the masked clear stays inside the rectangle.",
    ),
    pipeline(
        "clear-masked-counted",
        "        r->stats.clears_masked++;\n",
        "",
        "masked clears are counted.",
    ),
    pipeline(
        "clear-applied-counted",
        "    r->stats.clears_applied++;\n",
        "",
        "applied clears are counted.",
    ),
    pipeline(
        "clear-inference-reported",
        "    r->stats.used_inferences |= resolved.used_inferences;\n    r->census.used_inferences |= resolved.used_inferences;\n",
        "",
        "the INFERRED clear model a clear used is reported, like the replay reports it.",
    ),
    pipeline(
        "clear-group-gate",
        "    if ((r->backend.output_groups & GPU_PGRAPH_OUTPUT_CLEAR) == 0u) {\n",
        "    if (false) {\n",
        "a clear without the CLEAR output group is refused by name, not silently applied.",
    ),
    pipeline(
        "clear-refusal-counted",
        '    r->stats.clears_refused++;\n    set_error(error, error_bytes, "%s", r->clear_refusal);\n',
        '    set_error(error, error_bytes, "%s", r->clear_refusal);\n',
        "refused clears are counted.",
    ),
]
