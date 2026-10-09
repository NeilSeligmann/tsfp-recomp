# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for `src/gpu/gpu_vsh_draw.c` (the Vulkan draw behind the replay) and
`src/gpu/gpu_vsh_select.c` (the program-name and key lookup tables).

T259. Neither has a mutation set. `gpu_vsh_select.c` is small and device-free, and its lookups
are what turns a recorded program into a module: a binary search that misses the LAST entry or
skips one loses a real program and presents the draw as "not in the table" (or, with a
sentinel, a wrong module). `gpu_vsh_draw.c` needs a Vulkan device for nearly everything, so
almost every kill below is DEVICE-DEPENDENT and the harness reports them SKIPPED, never killed,
on a machine without one. `_NEEDS_DEVICE` at the bottom records which (all 25 draw ones: `test_gpu_vsh_draw` exits 77 whole).

EQUIVALENT, LEFT OUT: a fragment word-count bound of 4 instead of 5 (`make_shader` repeats the same
`word_count < 5u` check, so the render is refused either way), and scaling the viewport's maxDepth (0.5 instead of 1.0). The render has no depth
attachment and the vertex modules never write gl_FragDepth, so no pixel can depend on it.

The draw mutants that already live in `tests/test_gpu_pgraph_primitives.py` (line list and point
list topology, line width, "any topology") and `tests/test_gpu_combiner_replay.py` (sampler
filter and address mode, fragment constants block, texture upload) are NOT repeated.
"""

_DRAW = "src/gpu/gpu_vsh_draw.c"
_SELECT = "src/gpu/gpu_vsh_select.c"
_DRAW_TARGETS = ["test_gpu_vsh_draw", "test_gpu_pgraph_replay", "test_gpu_combiner"]
_SELECT_TARGETS = ["test_gpu_vsh_select", "test_gpu_pgraph_replay", "test_gpu_pgraph"]


def _draw(mutation_id: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"gpu-vsh-{mutation_id}",
        "file": _DRAW,
        "old": old,
        "new": new,
        "targets": list(_DRAW_TARGETS),
        "why": why,
    }


def _select(mutation_id: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"gpu-vsel-{mutation_id}",
        "file": _SELECT,
        "old": old,
        "new": new,
        "targets": list(_SELECT_TARGETS),
        "why": why,
    }


MUTATIONS: list[dict] = [
    # ------------------------------------------------------------------ gpu_vsh_select
    _select(
        "search-skips-last",
        "    uint32_t high = count;",
        "    uint32_t high = count - 1u;",
        "the binary search never looks at the last entry, so the highest key or address in a "
        "table is reported as absent.",
    ),
    _select(
        "search-skips-entries",
        "if (list[middle].id < id) low = middle + 1u;",
        "if (list[middle].id < id) low = middle + 2u;",
        "the search steps over an entry whenever it moves right, so some present ids are lost.",
    ),
    _select(
        "search-inexact-match",
        "if (list[middle].id == id) {",
        "if (list[middle].id >= id) {",
        "a lookup returns the first entry at or above the id, so an absent id resolves to a "
        "neighbour's module.",
    ),
    _select(
        "search-returns-id",
        "*module = list[middle].module;",
        "*module = list[middle].id;",
        "the entry's id is returned in place of its module index.",
    ),
    _select(
        "key-mask-ignored",
        "return find(table->keys, table->key_count, raw_key & table->key_mask, module);",
        "return find(table->keys, table->key_count, raw_key, module);",
        "the key mask is not applied, so a raw key with ignored bits set misses its module.",
    ),
    _select(
        "static-uses-key-table",
        "return find(table->statics, table->static_count, guest_address, module);",
        "return find(table->keys, table->key_count, guest_address, module);",
        "static programs are looked up in the key table.",
    ),
    _select(
        "name-prefix-match",
        "if (strcmp(table->module_names[i], name) == 0) {",
        "if (strncmp(table->module_names[i], name, 8u) == 0) {",
        "any name sharing its first eight characters matches, so every `static_` module "
        "resolves to the first one.",
    ),
    _select(
        "name-returns-count",
        "            *module = i;",
        "            *module = i + 1u;",
        "the module index of a name is off by one.",
    ),
    _select(
        "name-skips-last",
        "for (uint32_t i = 0u; i < table->module_count; i++) {\n        if (strcmp(",
        "for (uint32_t i = 0u; i + 1u < table->module_count; i++) {\n        if (strcmp(",
        "the last module of a table is never found by name.",
    ),
    _select(
        "valid-module-bound",
        "if (list[i].module >= modules) return 0;",
        "if (list[i].module > modules) return 0;",
        "an entry naming module `count`, one past the table, is valid.",
    ),
    _select(
        "valid-duplicate-ids",
        "if (i > 0u && list[i - 1u].id >= list[i].id) return 0;",
        "if (i > 0u && list[i - 1u].id > list[i].id) return 0;",
        "a duplicate id is valid, so a lookup returns whichever copy the search lands on.",
    ),
    _select(
        "valid-unsorted",
        "if (i > 0u && list[i - 1u].id >= list[i].id) return 0;",
        "if (i > 0u && list[i - 1u].id == list[i].id) return 0;",
        "a list that is not sorted is valid, and the binary search then misses entries.",
    ),
    _select(
        "valid-mask-bits",
        "if ((list[i].id & ~mask) != 0u) return 0;",
        "if ((list[i].id & ~mask) != 0u && 0) return 0;",
        "a key with bits outside the mask is valid, but can never be matched by a masked lookup.",
    ),
    _select(
        "valid-both-lists",
        "    return list_valid(table->keys, table->key_count, table->module_count, table->key_mask) &&",
        "    return list_valid(table->keys, table->key_count, table->module_count, table->key_mask) ||",
        "a table is valid when either list is.",
    ),
    # ------------------------------------------------------------------ gpu_vsh_draw: validation
    _draw(
        "check-vertex-zero",
        "!draw->constants || draw->vertex_count == 0u ||\n        draw->vertex_count > GPU_VSH_MAX_VERTICES) {",
        "!draw->constants ||\n        draw->vertex_count > GPU_VSH_MAX_VERTICES) {",
        "a draw of zero vertices reaches the device.",
    ),
    _draw(
        "check-vertex-limit",
        "!draw->constants || draw->vertex_count == 0u ||\n        draw->vertex_count > GPU_VSH_MAX_VERTICES) {",
        "!draw->constants || draw->vertex_count == 0u ||\n        draw->vertex_count >= GPU_VSH_MAX_VERTICES) {",
        "a draw of exactly GPU_VSH_MAX_VERTICES vertices is refused.",
    ),
    _draw(
        "check-constants-null",
        "if (!draw || !draw->attributes || !draw->constants || draw->vertex_count == 0u ||",
        "if (!draw || !draw->attributes || draw->vertex_count == 0u ||",
        "a NULL constants block is copied from.",
    ),
    _draw(
        "check-attributes-null",
        "if (!draw || !draw->attributes || !draw->constants || draw->vertex_count == 0u ||",
        "if (!draw || !draw->constants || draw->vertex_count == 0u ||",
        "a NULL attribute block is copied from.",
    ),
    _draw(
        "check-topology-three",
        "if (draw->topology > GPU_VSH_TOPOLOGY_LINE_LIST ||",
        "if (draw->topology > GPU_VSH_TOPOLOGY_LINE_LIST + 1u ||",
        "topology 3, one past the last defined, is silently drawn as a triangle list.",
    ),
    _draw(
        "check-fragment-skipped",
        "return draw->fragment != NULL ? check_fragment(draw->fragment) : GPU_OK;",
        "return draw->fragment != NULL && 0 ? check_fragment(draw->fragment) : GPU_OK;",
        "an invalid fragment stage (no words, no constants) reaches the device.",
    ),
    _draw(
        "fragment-constants-null",
        "if (fragment->words == NULL || fragment->word_count < 5u || fragment->constants == NULL) {",
        "if (fragment->words == NULL || fragment->word_count < 5u) {",
        "a fragment stage with no constants block is copied from.",
    ),
    _draw(
        "fragment-texture-height",
        "(texture->width == 0u || texture->height == 0u || texture->width > 4096u ||\n             texture->height > 4096u)) {",
        "(texture->width == 0u || texture->width > 4096u ||\n             texture->height > 4096u)) {",
        "a test texture with height 0 is accepted.",
    ),
    _draw(
        "render-dimension-limit",
        "#define MAX_DIMENSION 8192u",
        "#define MAX_DIMENSION 8193u",
        "a render target 8193 pixels wide is accepted past the limit the caller checks.",
    ),
    # ------------------------------------------------------------------ gpu_vsh_draw: content
    _draw(
        "vertices-not-copied",
        "memcpy(ctx->res.vertices.map, draw->attributes,\n           (size_t)draw->vertex_count * VERTEX_STRIDE_BYTES);",
        "memset(ctx->res.vertices.map, 0, (size_t)draw->vertex_count * VERTEX_STRIDE_BYTES);",
        "every vertex is zero.",
    ),
    _draw(
        "vertices-last-dropped",
        "memcpy(ctx->res.vertices.map, draw->attributes,\n           (size_t)draw->vertex_count * VERTEX_STRIDE_BYTES);",
        "memcpy(ctx->res.vertices.map, draw->attributes,\n           (size_t)(draw->vertex_count - 1u) * VERTEX_STRIDE_BYTES);",
        "the last vertex is uninitialised.",
    ),
    _draw(
        "constants-not-copied",
        "memcpy(ctx->res.constants.map, draw->constants, CONSTANT_BYTES);",
        "memset(ctx->res.constants.map, 0, CONSTANT_BYTES);",
        "every program runs with zeroed constants.",
    ),
    _draw(
        "attribute-offset",
        ".offset = 16u * location,",
        ".offset = 8u * location,",
        "attribute locations overlap, each starting half a vec4 apart.",
    ),
    _draw(
        "capture-draw-count",
        "    fn->vkCmdBeginTransformFeedbackEXT(commands, 0u, 0u, NULL, NULL);\n    fn->vkCmdDraw(commands, draw->vertex_count, 1u, 0u, 0u);",
        "    fn->vkCmdBeginTransformFeedbackEXT(commands, 0u, 0u, NULL, NULL);\n    fn->vkCmdDraw(commands, draw->vertex_count - 1u, 1u, 0u, 0u);",
        "the capture omits the last vertex.",
    ),
    _draw(
        "render-draw-count",
        "                                &ctx.res.set, 0u, NULL);\n    fn->vkCmdDraw(commands, draw->vertex_count, 1u, 0u, 0u);",
        "                                &ctx.res.set, 0u, NULL);\n    fn->vkCmdDraw(commands, draw->vertex_count - 1u, 1u, 0u, 0u);",
        "a render omits the last vertex.",
    ),
    _draw(
        "capture-sentinel-fill",
        "        words[i] = GPU_VSH_CAPTURE_SENTINEL_BITS;",
        "        words[i] = 0u;",
        "the capture buffer is not pre-filled with the sentinel, so a lane the "
        "program never wrote reads as 0.0 instead of being recognisable as unwritten.",
    ),
    _draw(
        "capture-readback",
        "        memcpy(out_capture, ctx.res.capture.map, (size_t)capture_bytes);",
        "        memset(out_capture, 0, (size_t)capture_bytes);",
        "the captured vertex outputs are not returned.",
    ),
    _draw(
        "topology-default",
        "    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;",
        "    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;",
        "triangle lists are drawn as strips.",
    ),
    _draw(
        "viewport-height",
        "const VkViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };",
        "const VkViewport viewport = { 0.0f, 0.0f, (float)width, (float)width, 0.0f, 1.0f };",
        "a non-square target is rasterised with a square viewport.",
    ),
    _draw(
        "cull-back",
        "VkCullModeFlags cull_mode = VK_CULL_MODE_NONE;",
        "VkCullModeFlags cull_mode = VK_CULL_MODE_BACK_BIT;",
        "back faces are culled by default, with no output state asked for (cull state is opt-in, T267).",
    ),
    _draw(
        "color-mask-alpha",
        "                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,",
        "                          VK_COLOR_COMPONENT_B_BIT,",
        "alpha is never written, so the two-clear coverage test sees alpha of the clear colour.",
    ),
    _draw(
        "clear-alpha",
        ".color = { .float32 = { clear_rgba[0], clear_rgba[1], clear_rgba[2], clear_rgba[3] } },",
        ".color = { .float32 = { clear_rgba[0], clear_rgba[1], clear_rgba[2], 1.0f } },",
        "the requested clear alpha is ignored.",
    ),
    _draw(
        "clear-green",
        ".color = { .float32 = { clear_rgba[0], clear_rgba[1], clear_rgba[2], clear_rgba[3] } },",
        ".color = { .float32 = { clear_rgba[0], clear_rgba[0], clear_rgba[2], clear_rgba[3] } },",
        "the requested clear green is replaced by red.",
    ),
    _draw(
        "readback-stride",
        "            out_image->stride_bytes = width * 4u;",
        "            out_image->stride_bytes = width * 4u + 4u;",
        "the image reports a stride that does not match its pixels.",
    ),
    _draw(
        "readback-width",
        "            out_image->width = width;\n            out_image->height = height;",
        "            out_image->width = width;\n            out_image->height = height - 1u;",
        "the image reports one row fewer than it holds.",
    ),
]


# RECORD of a run with no Vulkan device (`VK_DRIVER_FILES=/nonexistent`, 2026-10-03): the mutations whose
# every target exits 77 there, so the harness reports them SKIPPED and never counts them killed. It is
# a record, not a mechanism: the harness decides from the exit code and prints a NOTE when a skip is
# not listed here. Regenerate it with that command if a test starts or stops needing a device.
_NEEDS_DEVICE = frozenset(
    {
        "gpu-vsh-attribute-offset",
        "gpu-vsh-capture-draw-count",
        "gpu-vsh-capture-readback",
        "gpu-vsh-capture-sentinel-fill",
        "gpu-vsh-check-attributes-null",
        "gpu-vsh-check-constants-null",
        "gpu-vsh-check-fragment-skipped",
        "gpu-vsh-check-topology-three",
        "gpu-vsh-check-vertex-limit",
        "gpu-vsh-check-vertex-zero",
        "gpu-vsh-clear-alpha",
        "gpu-vsh-clear-green",
        "gpu-vsh-color-mask-alpha",
        "gpu-vsh-constants-not-copied",
        "gpu-vsh-cull-back",
        "gpu-vsh-fragment-constants-null",
        "gpu-vsh-fragment-texture-height",
        "gpu-vsh-readback-stride",
        "gpu-vsh-readback-width",
        "gpu-vsh-render-dimension-limit",
        "gpu-vsh-render-draw-count",
        "gpu-vsh-topology-default",
        "gpu-vsh-vertices-last-dropped",
        "gpu-vsh-vertices-not-copied",
        "gpu-vsh-viewport-height",
    }
)
for _mutation in MUTATIONS:
    _mutation["needs_device"] = _mutation["id"] in _NEEDS_DEVICE
