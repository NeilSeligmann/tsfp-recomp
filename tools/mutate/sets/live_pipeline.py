# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for the live pipeline description, key, cache and census (T791): src/gpu/live_pipeline.c.

TARGET. The ctest binary `test_live_pipeline` (oracle against the replay resolvers, a key check per static field and
per dynamic field, LRU cache with destroy, the refusal census). `&& false` style is avoided: each `new` compiles.
"""

FILE = "src/gpu/live_pipeline.c"


def mutation(identifier: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"t791-{identifier}",
        "file": FILE,
        "old": old,
        "new": new,
        "targets": ["test_live_pipeline"],
        "why": why,
    }


MUTATIONS: list[dict] = [
    mutation(
        "key-program",
        "    *word++ = description->program.module;\n",
        "    *word++ = 0u;\n",
        "two vertex programs must be two pipelines.",
    ),
    mutation(
        "key-fragment",
        "    *word++ = description->has_fragment ? description->fragment.module + 1u : 0u;\n",
        "    *word++ = description->has_fragment ? 1u : 0u;\n",
        "two combiner modules must be two pipelines.",
    ),
    mutation(
        "key-texture-stages",
        "    *word++ = description->has_fragment ? description->fragment.plan.texture_stages : 0u;\n",
        "    *word++ = 0u;\n",
        "the sampled stages change the pipeline layout.",
    ),
    mutation(
        "key-topology",
        "    *word++ = description->topology;\n",
        "    *word++ = 0u;\n",
        "topology is baked into the pipeline.",
    ),
    mutation(
        "key-active",
        "    *word++ = active ? 1u : 0u;\n    key->words[LIVE_PIPELINE_KEY_TEXEL_WORD] =",
        "    *word++ = 0u;\n    key->words[LIVE_PIPELINE_KEY_TEXEL_WORD] =",
        "an inactive output and an all-default active one are told apart only by this word.",
    ),
    mutation(
        "key-cull",
        "    *word++ = output->cull_mode;\n",
        "    *word++ = 0u;\n",
        "cull mode is baked.",
    ),
    mutation(
        "key-front-face",
        "    *word++ = output->front_clockwise ? 1u : 0u;\n",
        "    *word++ = 0u;\n",
        "front face winding is baked.",
    ),
    mutation(
        "key-blend-enable",
        "    *word++ = output->blend ? 1u : 0u;\n    /* blend",
        "    *word++ = 0u;\n    /* blend",
        "blend enable is baked.",
    ),
    mutation(
        "key-blend-source",
        "    *word++ = output->blend ? output->blend_source : 0u;\n",
        "    *word++ = 0u;\n",
        "blend source factor is baked.",
    ),
    mutation(
        "key-blend-destination",
        "    *word++ = output->blend ? output->blend_destination : 0u;\n",
        "    *word++ = 0u;\n",
        "blend destination factor is baked.",
    ),
    mutation(
        "key-blend-equation",
        "    *word++ = output->blend ? output->blend_equation : 0u;\n",
        "    *word++ = 0u;\n",
        "blend equation is baked.",
    ),
    mutation(
        "key-blend-always",
        "    *word++ = output->blend ? output->blend_source : 0u;\n",
        "    *word++ = output->blend_source;\n",
        "factors of a disabled blend must NOT split the cache (pipeline explosion).",
    ),
    mutation(
        "key-colour-mask",
        "    *word++ = output->color_write_disable;\n",
        "    *word++ = 0u;\n",
        "colour write mask is baked.",
    ),
    mutation(
        "key-alpha-func",
        "    *word++ = output->alpha_test ? 1u + output->alpha_func : 0u;\n",
        "    *word++ = output->alpha_test ? 1u : 0u;\n",
        "alpha test function is baked.",
    ),
    mutation(
        "key-alpha-ref",
        "    *word++ = output->alpha_test ? output->alpha_ref : 0u;\n",
        "    *word++ = 0u;\n",
        "the alpha reference is a specialisation constant of the fragment stage, so it is baked.",
    ),
    mutation(
        "key-alpha-enable",
        "    *word++ = output->alpha_test ? 1u + output->alpha_func : 0u;\n",
        "    *word++ = 1u + output->alpha_func;\n",
        "alpha test off and alpha test ALWAYS(0) must differ.",
    ),
    mutation(
        "key-depth-enable",
        "    *word++ = output->depth_test ? 1u : 0u;\n    *word++ = output->depth_test ?",
        "    *word++ = 0u;\n    *word++ = output->depth_test ?",
        "depth test enable is baked.",
    ),
    mutation(
        "key-depth-write",
        "(output->depth_write ? 1u : 0u) | (output->depth_func << 1)",
        "(output->depth_func << 1)",
        "depth write is baked.",
    ),
    mutation(
        "key-depth-func",
        "(output->depth_write ? 1u : 0u) | (output->depth_func << 1)",
        "(output->depth_write ? 1u : 0u)",
        "depth function is baked.",
    ),
    mutation(
        "key-stencil-enable",
        "    *word++ = output->stencil_test ? 1u : 0u;\n    if (output->stencil_test) {",
        "    *word++ = 0u;\n    if (output->stencil_test) {",
        "stencil test enable is baked.",
    ),
    mutation(
        "key-stencil-func",
        "        *word++ = output->stencil_func;\n",
        "        *word++ = 0u;\n",
        "stencil function is baked.",
    ),
    mutation(
        "key-stencil-compare-mask",
        "        *word++ = output->stencil_compare_mask;\n",
        "        *word++ = 0u;\n",
        "stencil compare mask is baked.",
    ),
    mutation(
        "key-stencil-write-mask",
        "        *word++ = output->stencil_write_mask;\n",
        "        *word++ = 0u;\n",
        "stencil write mask is baked.",
    ),
    mutation(
        "key-stencil-fail-op",
        "output->stencil_fail_op | (output->stencil_zfail_op << 8)",
        "(output->stencil_zfail_op << 8)",
        "stencil fail op is baked.",
    ),
    mutation(
        "key-stencil-zfail-op",
        "(output->stencil_zfail_op << 8) | (output->stencil_zpass_op << 16)",
        "(output->stencil_zpass_op << 16)",
        "stencil depth-fail op is baked.",
    ),
    mutation(
        "key-stencil-zpass-op",
        "output->stencil_fail_op | (output->stencil_zfail_op << 8) | (output->stencil_zpass_op << 16)",
        "output->stencil_fail_op | (output->stencil_zfail_op << 8)",
        "stencil pass op is baked.",
    ),
    mutation(
        "key-depth-bias",
        "    key->words[LIVE_PIPELINE_KEY_WORDS - 1u] = (output->depth_bias ? 1u : 0u) | (output->polygon_mode << 1); /* T860 */\n",
        "    key->words[LIVE_PIPELINE_KEY_WORDS - 1u] = (output->polygon_mode << 1); /* T860 */\n",
        "depth bias enable is baked.",
    ),
    mutation(
        "key-stencil-ref-baked",
        "        *word++ = output->stencil_func;\n",
        "        *word++ = output->stencil_func | (output->stencil_ref << 16);\n",
        "the stencil reference is dynamic: baking it explodes the cache.",
    ),
    mutation(
        "dynamic-scissor",
        "    out->scissor_y = output->scissor_y;\n",
        "    out->scissor_y = output->scissor_x;\n",
        "scissor origin is dynamic state.",
    ),
    mutation(
        "dynamic-blend-constant",
        "    memcpy(out->blend_constant, output->blend_constant, sizeof out->blend_constant);\n",
        "",
        "blend constant is dynamic state.",
    ),
    mutation(
        "dynamic-viewport",
        "    out->viewport_height = height;\n",
        "    out->viewport_height = height + 1u;\n",
        "viewport height is dynamic state.",
    ),
    mutation(
        "dynamic-bias",
        "    out->depth_bias_slope = output->depth_bias_slope;\n",
        "    out->depth_bias_slope = output->depth_bias_constant;\n",
        "depth bias slope is dynamic state.",
    ),
    mutation(
        "hash-high-bytes",
        "        for (uint32_t shift = 0u; shift < 32u; shift += 8u) {",
        "        for (uint32_t shift = 0u; shift < 8u; shift += 8u) {",
        "the hash covers every byte (a collision is only safe because the key is also compared).",
    ),
    mutation(
        "cache-compare-key",
        "        if (slot->used && slot->hash == hash && memcmp(&slot->key, key, sizeof *key) == 0) {",
        "        if (slot->used && slot->hash == hash && (memcmp(&slot->key, key, sizeof *key) == 0 || hash != 0u)) {",
        "the cache confirms the full key, not just the hash (equal hashes alone must not alias).",
    ),
    mutation(
        "cache-lru",
        "        if (slot->stamp < victim->stamp) {",
        "        if (slot->stamp > victim->stamp) {",
        "eviction takes the least recently used pipeline.",
    ),
    mutation(
        "cache-evict-destroy",
        "    if (cache->ops.destroy != NULL) {\n        cache->ops.destroy(cache->ops.context, victim->handle);\n    }\n    victim->used = false;\n    cache->evictions++;",
        "    victim->used = false;\n    cache->evictions++;",
        "an evicted pipeline is destroyed, not leaked.",
    ),
    mutation(
        "cache-hit-stamp",
        "    slot->stamp = ++cache->clock;\n",
        "",
        "a hit refreshes recency.",
    ),
    mutation(
        "cache-hit-count",
        "        cache->hits++;\n",
        "",
        "hits are counted.",
    ),
    mutation(
        "cache-destroy-all",
        "        if (cache->slots[index].used && cache->ops.destroy != NULL) {",
        "        if (index > 0u && cache->slots[index].used && cache->ops.destroy != NULL) {",
        "cache_destroy releases every pipeline.",
    ),
    mutation(
        "census-stage-device",
        "            census_refuse(census, draw, LIVE_STAGE_DEVICE, GPU_PGRAPH_ERR_DEVICE, error);",
        "            census_refuse(census, draw, LIVE_STAGE_OUTPUT, GPU_PGRAPH_ERR_DEVICE, error);",
        "a device refusal is census stage device.",
    ),
    mutation(
        "census-draw-index",
        "    entry->draw = draw;\n",
        "    entry->draw = draw + 1u;\n",
        "the census names the exact draw index.",
    ),
    mutation(
        "census-selected",
        "        census->selected++;\n",
        "",
        "selected draws are counted.",
    ),
    mutation(
        "census-inferences",
        "        census->used_inferences |= out->description.used_inferences;\n",
        "",
        "the census reports which INFERRED bits the selected draws depended on.",
    ),
    mutation(
        "resolve-stage-primitive",
        "        *stage = LIVE_STAGE_PRIMITIVE;\n",
        "        *stage = LIVE_STAGE_OUTPUT;\n",
        "the point and line gate is stage primitive.",
    ),
    mutation(
        "resolve-skip-output",
        "    out->used_inferences |= out->output.used_inferences;\n",
        "",
        "output inferences reach the draw's used bits.",
    ),
    mutation(
        "resolve-topology",
        "    out->topology = gpu_pgraph_topology_of(primitive);\n",
        "    out->topology = gpu_pgraph_topology_of(primitive & 0u);\n",
        "points and lines are not triangles.",
    ),
]
