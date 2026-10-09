# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the swap replay's render target texture option (T510).

T510 lets a texture stage bound to a render target header that an EARLIER pass of the same frame drew be sampled from that
pass's image (`render_target_texture`, `render_target_texture_census`, host flags `--gpu-replay-rt-texture` and
`--gpu-replay-rt-texture-census`). `d3d8_swap_replay` notes every SetTexture (`d3d8_swap_replay_on_texture`, called by
`d3d8_set_texture`), plans the frame's passes before the first one runs, resolves each stage of each draw through
`provide_texture` (a named refusal for every cause that is not exact) and hands the combiner an unnormalised, bilinear RGBA
image. `gpu_spirv_texel_coordinates` divides the sample's coordinate by the texture size. Grouped by what a survivor would let
through:

    config      enable refusing the option without the combiner, with a stand-in or flip_y, without the TEXTURE group or
                the announced inference, and the census without the option, the header constants
    reset       `follow_gpu_reset` forgetting the noted bindings, the overflow flag and the carried stage state
    measure     `measure_target` pitch and Format word, `read_texture_binding` and its Size word fields,
                `same_texture_binding`, `binding_at`
    note        `d3d8_swap_replay_on_texture`: guards, dedupe, drain, the 4096 event limit, what an event records, the call in
                `d3d8_set_texture`
    refuse      `provide_texture`: every refusal category (condition and census name) in order
    resolve     the success path (image, size, linear, unnormalised), the census source text, `texture_sampled` and the tally
    census      `census_frame`, the census lines, `d3d8_swap_replay_texture_census_at`, the two summary texts
    plan        `segment_target`, the pass plan built in `present_frame`, the overflow refusal, the census early return,
                `roll_texture_events`, the backend wiring and the BRIDGE inference bit of `replay_pass`
    combiner    the named refusal text of `gpu_combiner_plan_build`
    texel       `gpu_spirv_texel_coordinates`: argument validation, the scan, the rewrite words and constants
    replay      `gpu_pgraph_replay_pass` and `render_draw`: the per draw resolve, the sampled callback, texel stages, the
                linear flag, and the sampler filter of `gpu_vsh_draw`
    hostopt     the two flags, their defaults and the requirements

The kills are by `test_d3d8_swap_replay` (the configuration, the notes, the 4096 limit and every census scene need no device,
the exact pixel, refusal replay, rebinding and tally scenes need one), `test_gpu_combiner_edges` (`test_texel_coordinates` and
the named refusals of `test_textures`), `test_gpu_combiner` and `test_host_options`. DEVICE-DEPENDENT kills (exit 77 without a
Vulkan device) are reported SKIPPED by the harness, never killed: `_NEEDS_DEVICE` at the bottom records which ones, from a run with
the Vulkan loader hidden (`VK_DRIVER_FILES=/nonexistent VK_ICD_FILENAMES=/nonexistent`).

`tests/test_texel_coordinates.py` (spirv-val over the rewritten module) is NOT a target here: its module fixture runs
`test_gpu_combiner_edges` and ERRORS, not FAILS, when that binary fails, which the harness reads as no verdict
(`pytest exit 1 without a failed test`). A mutant the binary kills therefore cannot also name it, and no mutant of the rewrite
passes the binary's word by word pins and fails spirv-val.

NOT MUTATED, and why: `src/host/main.c` (the announcement and the end-of-run census print) because `tsfp_host` is not a ctest
binary, the `{false, false, NULL}` initialisers in `d3d8_frame_profile.c` (a compile guard only) and the three pointer resets in
`d3d8_swap_replay_on_present` (`provider.passes`, `provider.plan`, `provider.plan_count` after the frame: nothing reads them
until `present_frame` or `replay_pass` assigns them again).

EQUIVALENT, so left out (a survivor no input can kill is not a missing test):
  - `texture_census_overflow` is only written, never read (no summary, no getter): the two `++` are dead state.
  - the `bound` term of `same_texture_binding` (`bound` is `header != 0`, the header term says the same), the `!state.active` term
    of `on_texture` and of the summary (`disable` memsets the state, so `active` and the option clear together), the
    `!output_written` half of the address and filter tests (an unwritten word reads 0, which is neither constant).
  - the first match kept (`match == SIZE_MAX`: more than one match refuses before it is used), `match >= provider.current` for `>`
    (`==` is refused first), the `!measured` term of the inexact relationship (a zero width fails the width term).
  - the resets of `provider.source[stage]` and `provider.category[stage]` in `provide_texture` (a stage the combiner reads resolved
    in that very call, and a stale refusal text names another draw), `provider.dry` and `provider.passes` after `census_frame`
    (census mode never replays), the `memset` of `provider.source` in `present_frame`, the `provider.sampled` reset (the passes'
    inferences are ORed), the `end > frame_draws || end < first` guards of the plan (the replay loop refuses a malformed segment, the
    census has none), `switch_overflow = false` in the census early return (an overflowed frame is refused before it), the overflow
    flag cleared by `roll_texture_events` (a refused frame latches and only a reset clears it, which clears it itself).
  - the `provider.category[stage] != NULL` guard of the census (once any draw was provided every stage's refusal text is non
    empty and names its own draw, so `strstr` never matches a stage that did not refuse), the last refused stage instead of the
    first (the texts are unique), the pitch of a swizzled producer through `d3d8_surface_header_pitch` (its Format differs anyway,
    only the printed pitch changes), the four `image not packed` tests (every replayed image is a tight RGBA image of its measured
    size, and the relationship already compared the size).
  - the draw index handed to `texture_sampled` (the callback ignores it), the `backend->combiner` term of the sampled loop (no
    combiner, no texture stages), the `memset` of `draw_backend.test_textures` (enable refuses a stand-in with the option, so they are
    zero), the sampler `minFilter` (the replay samples 1:1, so every sample is a magnification), `out->height` read from the width (the
    coordinate is divided by the same wrong size, so the pixels agree, only a sanitizer sees the over-read), `free(texeled)` and the
    `malloc` capacity terms (`13u`, `5u`, `result == NULL`, only a sanitizer sees them).
  - `gpu_spirv_texel_coordinates`: one float32 and one vec2 type exist in a valid module (first and last agree) and both precede the
    first function, a module of 4 words, an empty stage set, no float, no function, no vec2 or no sampler variable each fail a later
    check anyway (`samples == 0`, `vector_type == 0`, `found.active`), the decorated id of a binding is the variable id, the load result
    skip and the else-if that tolerates it are redundant with each other, the order of the width and height ids is a naming.
  - `--gpu-replay-rt-texture`'s own `--gpu-replay` requirement (`--gpu-replay-combiner` already needs the directory).

REAL GAPS, so left out until a fixture exists (each would survive the current tests):
  - stage 1 or higher on the REPLAY path: `sampled-stage-bound`, `sampled-wrong-stage-source`, the stage loop and the stage argument
    of the sampled callback, the `texel_stages` mask (`|=`, `1u << stage`) of `render_draw` and the binding `2u + stage` of the
    rewrite. They need a stage 1 combiner module in the spv directory and a SPIR-V with a sampler at binding 3. The census covers
    stage 1 (`test_rt_texture_census_stage_one`) without a module.
  - `render_draw`'s refusal when the rewrite fails (no scene gives it a module of an unsupported shape), the rewrite's scan of a
    vec2 of another scalar, a sampler loaded twice and a non UniformConstant or undefined decorated variable (crafted modules).
  - `linear` and the sampler's `magFilter` for a stand-in texture (nothing samples a stand-in checker at a texel edge, where bilinear
    blends).

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

_SWAP = "src/gpu/d3d8_swap_replay.c"
_SWAP_H = "src/gpu/d3d8_swap_replay.h"
_COMBINER = "src/gpu/gpu_combiner.c"
_REPLAY = "src/gpu/gpu_pgraph_replay.c"
_REPLAY_H = "src/gpu/gpu_pgraph_replay.h"
_VSH = "src/gpu/gpu_vsh_draw.c"
_OPTIONS = "src/host/host_options.c"
_BIND = "src/gpu/d3d8_bind.c"

_SWAP_TEST = ["test_d3d8_swap_replay"]
_COMBINER_TEST = ["test_gpu_combiner_edges", "test_gpu_combiner", "test_d3d8_swap_replay"]
_TEXEL_TEST = ["test_gpu_combiner_edges"]
_OPTION_TEST = ["test_host_options"]


def _row(file: str, mutation_id: str, old: str, new: str, why: str, targets: list[str]) -> dict:
    return {
        "id": f"gpu-rt-texture-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(targets),
        "why": why,
    }


def _sw(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_SWAP, mutation_id, old, new, why, _SWAP_TEST)


def _tx(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_COMBINER, mutation_id, old, new, why, _TEXEL_TEST)


def _rp(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_REPLAY, mutation_id, old, new, why, _SWAP_TEST)


def _ho(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_OPTIONS, mutation_id, old, new, why, _OPTION_TEST)


def _within(block: str, piece: str, replacement: str) -> tuple[str, str]:
    """(block, block with `piece` replaced once): an anchor that spans lines the mutant changes one part of."""
    assert block.count(piece) == 1, piece
    return block, block.replace(piece, replacement)


def _category(name: str, anchor: str | None = None) -> dict:
    """The census name of one refusal category changed (the message and the condition are untouched)."""
    old = anchor or f'stage_refusal(stage, out, "{name}",'
    new = old.replace(f'"{name}"', '"renamed"')
    return _sw(
        f"category-{name.replace(' ', '-')}",
        old,
        new,
        f"the census names the refusal `{name}` something else, so the per cause tally of the run is wrong.",
    )


_CENSUS_TALLY_TAIL = (
    "    if (state.texture_census_count == D3D8_SWAP_REPLAY_TEXTURE_CENSUS) {\n"
    "        state.texture_census_overflow++;\n"
    "        return;\n"
    "    }\n"
    "    texture_census_line *entry = &state.texture_census[state.texture_census_count++];\n"
    '    (void)snprintf(entry->text, sizeof entry->text, "%s", line);\n'
    "    entry->draws = 1u;\n"
    "}\n"
    "\n"
    "static void texture_sampled"
)
_CENSUS_LINE_TAIL = _CENSUS_TALLY_TAIL.replace(
    "static void texture_sampled", "static void census_frame"
)

_PRESENT_CENSUS = (
    "        census_frame(plan, plan_count);\n"
    "        state.stats.census_frames++;\n"
    "        d3d8_gpu_stream_discard(state.next);\n"
    "        state.next = 0u;\n"
    "        state.switch_count = 0u;\n"
    "        state.switch_overflow = false;\n"
    "        (void)gpu_pgraph_begin_frame(state.pgraph);\n"
    "        state.stats.frames_empty++;\n"
    "        return;\n"
    "    }\n"
    "    for (size_t segment = 0u; segment < segments; segment++) {\n"
)

_RESET = (
    "    state.texture_event_count = 0u;\n"
    "    state.texture_overflow = false;\n"
    "    memset(state.texture_carry_known, 0, sizeof state.texture_carry_known); /* CreateDevice unbinds every stage */\n"
)

_PROVIDER_RESET = (
    "    provider.source[stage][0] = '\\0';\n"
    "    provider.category[stage] = NULL;\n"
    "    provider.detail[stage][0] = '\\0';\n"
    "    const texture_binding *binding = binding_at(stage, draw);\n"
)

_RESOLVED_TAIL = (
    "    out->rgba = provider.dry ? placeholder : image->pixels;\n"
    "    out->width = provider.dry ? 1u : image->width;\n"
    "    out->height = provider.dry ? 1u : image->height;\n"
    "    out->linear = true;\n"
    "    out->unnormalised = true;\n"
)

_REWRITE_CONSTANTS = (
    "        result[used++] = (4u << 16) | SPV_OP_CONSTANT;\n"
    "        result[used++] = float_type;\n"
    "        result[used++] = width;\n"
    "        result[used++] = bits[0];\n"
    "        result[used++] = (4u << 16) | SPV_OP_CONSTANT;\n"
    "        result[used++] = float_type;\n"
    "        result[used++] = height;\n"
    "        result[used++] = bits[1];\n"
    "        result[used++] = (5u << 16) | SPV_OP_CONSTANT_COMPOSITE;\n"
    "        result[used++] = vector_type;\n"
    "        result[used++] = found[stage].divisor;\n"
    "        result[used++] = width;\n"
    "        result[used++] = height;\n"
)

_REQUIREMENT = (
    "        (!out->gpu_replay_rt_texture ||\n"
    "         (out->gpu_replay_combiner && out->gpu_replay_output_state && !out->gpu_replay_standin && !out->gpu_replay_flip_y &&\n"
    "          out->gpu_replay_dir != NULL)) &&\n"
)


MUTATIONS: list[dict] = [
    # ================================================================== config
    _sw(
        "config-needs-combiner",
        '        if (!config->combiner) {\n            reason = "the render target texture only feeds',
        '        if (false) {\n            reason = "the render target texture only feeds',
        "the option is accepted without the combiner, where nothing reads the image.",
    ),
    _sw(
        "config-standin-exclusive",
        '} else if (config->standin_texture) {\n            reason = "the render target texture and the stand-in',
        '} else if (false) {\n            reason = "the render target texture and the stand-in',
        "both options are accepted, so a stand-in would stand in for a binding that was refused by name.",
    ),
    _sw(
        "config-flip-y-exclusive",
        '} else if (config->flip_y) {\n            reason = "the render target texture cannot',
        '} else if (false) {\n            reason = "the render target texture cannot',
        "flip_y is accepted although the orientation of a replayed image used as a texture is undecided.",
    ),
    _sw(
        "config-needs-texture-group",
        "} else if ((config->output_groups & GPU_PGRAPH_OUTPUT_TEXTURE) == 0u) {",
        "} else if (false) {",
        "the option is accepted without the TEXTURE group, so the address and filter words it checks are never decoded.",
    ),
    _sw(
        "config-needs-announced-inference",
        "} else if ((config->allowed_inferences & D3D8_SWAP_REPLAY_INFER_RT_TEXTURE) != D3D8_SWAP_REPLAY_INFER_RT_TEXTURE) {",
        "} else if (false) {",
        "the sampling model is used without the caller having announced it.",
    ),
    _sw(
        "config-one-inference-is-enough",
        "} else if ((config->allowed_inferences & D3D8_SWAP_REPLAY_INFER_RT_TEXTURE) != D3D8_SWAP_REPLAY_INFER_RT_TEXTURE) {",
        "} else if ((config->allowed_inferences & D3D8_SWAP_REPLAY_INFER_RT_TEXTURE) == 0u) {",
        "one of the two inference bits is enough, so a half announced model runs.",
    ),
    _sw(
        "config-census-needs-option",
        "    if (config->render_target_texture_census && !config->render_target_texture) {",
        "    if (config->render_target_texture_census && false) {",
        "the census is accepted without the option and silently does nothing.",
    ),
    _sw(
        "config-reason-ignored",
        '        if (reason != NULL) {\n            fail(error, error_size, "%s", reason);\n            return false;',
        '        if (false) {\n            fail(error, error_size, "%s", reason);\n            return false;',
        "no combination is ever refused.",
    ),
    _sw(
        "config-option-not-validated",
        "    if (config->render_target_texture) {\n        const char *reason = NULL;",
        "    if (false) {\n        const char *reason = NULL;",
        "the option is never validated.",
    ),
    _row(
        _SWAP_H,
        "header-inference-bridge-only",
        "    (GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE | GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING)",
        "    (GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE)",
        "the announced inference no longer allows the combiner to sample a texture, so no draw is planned.",
        _SWAP_TEST,
    ),
    _row(
        _SWAP_H,
        "header-inference-sampling-only",
        "    (GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE | GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING)",
        "    (GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING)",
        "the announced inference no longer names the bridge, so a bridged replay reports an inference nobody allowed.",
        _SWAP_TEST,
    ),
    _row(
        _SWAP_H,
        "header-format-word",
        "#define D3D8_SWAP_REPLAY_RT_TEXTURE_FORMAT 0x00011229u",
        "#define D3D8_SWAP_REPLAY_RT_TEXTURE_FORMAT 0x00011228u",
        "the one Format word the bridge takes is another one, so the measured A8R8G8B8 header is refused.",
        _SWAP_TEST,
    ),
    _row(
        _SWAP_H,
        "header-event-limit-below",
        "#define D3D8_SWAP_REPLAY_TEXTURE_EVENTS 4096u",
        "#define D3D8_SWAP_REPLAY_TEXTURE_EVENTS 4095u",
        "a frame with exactly 4096 SetTexture bindings is refused.",
        _SWAP_TEST,
    ),
    _row(
        _SWAP_H,
        "header-event-limit-above",
        "#define D3D8_SWAP_REPLAY_TEXTURE_EVENTS 4096u",
        "#define D3D8_SWAP_REPLAY_TEXTURE_EVENTS 4097u",
        "a frame with 4097 SetTexture bindings is accepted.",
        _SWAP_TEST,
    ),
    _row(
        _SWAP_H,
        "header-census-limit-below",
        "#define D3D8_SWAP_REPLAY_TEXTURE_CENSUS 16u",
        "#define D3D8_SWAP_REPLAY_TEXTURE_CENSUS 15u",
        "the census keeps one distinct line fewer than documented.",
        _SWAP_TEST,
    ),
    _row(
        _SWAP_H,
        "header-census-limit-above",
        "#define D3D8_SWAP_REPLAY_TEXTURE_CENSUS 16u",
        "#define D3D8_SWAP_REPLAY_TEXTURE_CENSUS 17u",
        "the census keeps one distinct line more than documented.",
        _SWAP_TEST,
    ),
    _row(
        _REPLAY_H,
        "header-bridge-bit-collides",
        "#define GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE 0x10000000u",
        "#define GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE 0x8000000u",
        "the bridge inference shares its bit with the blit model, so each reports the other.",
        _SWAP_TEST,
    ),
    # ================================================================== reset
    _sw(
        "reset-keeps-events",
        *_within(_RESET, "    state.texture_event_count = 0u;\n", ""),
        "a device reset leaves the bindings noted for the old device in this frame's list.",
    ),
    _sw(
        "reset-keeps-overflow",
        *_within(_RESET, "    state.texture_overflow = false;\n", ""),
        "an overflow flag of the old device refuses the first frame of the new one.",
    ),
    _sw(
        "reset-keeps-carry",
        *_within(
            _RESET,
            "    memset(state.texture_carry_known, 0, sizeof state.texture_carry_known); /* CreateDevice unbinds every stage */\n",
            "",
        ),
        "a stage still holds the binding the old device had, although CreateDevice unbinds every stage.",
    ),
    _sw(
        "reset-forgets-stage-zero-only",
        *_within(_RESET, "sizeof state.texture_carry_known)", "1u)"),
        "only stage 0 is forgotten at a device reset.",
    ),
    # ================================================================== measure
    _sw(
        "measure-pitch-inverted",
        "size.pitch = d3d8_guest_load32(header + D3D8_SURFACE_SIZE) != 0u ? d3d8_surface_header_pitch(header) : 0u;",
        "size.pitch = d3d8_guest_load32(header + D3D8_SURFACE_SIZE) == 0u ? d3d8_surface_header_pitch(header) : 0u;",
        "the pitch is measured only of a swizzled header, the other way round.",
    ),
    _sw(
        "measure-pitch-never",
        "size.pitch = d3d8_guest_load32(header + D3D8_SURFACE_SIZE) != 0u ? d3d8_surface_header_pitch(header) : 0u;",
        "size.pitch = 0u;",
        "a render target's pitch is never measured, so no producer matches a texture.",
    ),
    _sw(
        "measure-format-lost",
        "        size.format = d3d8_guest_load32(header + D3D8_SURFACE_FORMAT);",
        "        size.format = 0u;",
        "a render target's Format word is never measured.",
    ),
    _sw(
        "measure-format-wrong-word",
        "        size.format = d3d8_guest_load32(header + D3D8_SURFACE_FORMAT);",
        "        size.format = d3d8_guest_load32(header + D3D8_SURFACE_DATA);",
        "the Data word is taken for the Format word.",
    ),
    _sw(
        "measure-header-guard",
        "    if (header != 0u) {\n        /* the pitch only of a linear header",
        "    if (false) {\n        /* the pitch only of a linear header",
        "neither pitch nor Format is measured.",
    ),
    # ================================================================== binding read
    _sw(
        "binding-unbound-not-detected",
        "    if (header == 0u) {\n        return binding;\n    }\n    binding.bound = true;",
        "    if (false) {\n        return binding;\n    }\n    binding.bound = true;",
        "SetTexture 0 reads the words of header 0.",
    ),
    _sw(
        "binding-never-bound",
        "    binding.bound = true;",
        "    binding.bound = false;",
        "every header is read as an unbound stage.",
    ),
    _sw(
        "binding-data-mask-lost",
        "binding.data = d3d8_guest_load32(header + D3D8_SURFACE_DATA) & 0x0FFFFFFFu;",
        "binding.data = d3d8_guest_load32(header + D3D8_SURFACE_DATA) & 0xFFFFFFFFu;",
        "the Data word keeps its top bits, so the address no longer equals the producer's physical address.",
    ),
    _sw(
        "binding-format-wrong-word",
        "    binding.format = d3d8_guest_load32(header + D3D8_SURFACE_FORMAT);",
        "    binding.format = d3d8_guest_load32(header + D3D8_SURFACE_DATA);",
        "the Data word is taken for the Format word.",
    ),
    _sw(
        "binding-size-wrong-word",
        "    binding.size_word = d3d8_guest_load32(header + D3D8_SURFACE_SIZE);",
        "    binding.size_word = d3d8_guest_load32(header + D3D8_SURFACE_FORMAT);",
        "the Format word is taken for the Size word.",
    ),
    _sw(
        "binding-measured-inverted",
        "    if (binding.size_word != 0u) {\n        binding.measured = true;",
        "    if (binding.size_word == 0u) {\n        binding.measured = true;",
        "a swizzled header (no Size word) reads as measured and a linear one as not.",
    ),
    _sw(
        "binding-never-measured",
        "        binding.measured = true;",
        "        binding.measured = false;",
        "no header is ever measured, so every texture is an unsupported format.",
    ),
    _sw(
        "binding-width-plus-zero",
        "binding.width = (binding.size_word & 0xFFFu) + 1u;",
        "binding.width = (binding.size_word & 0xFFFu) + 0u;",
        "the Size word stores width minus one and the replay forgets the plus one.",
    ),
    _sw(
        "binding-width-mask",
        "binding.width = (binding.size_word & 0xFFFu) + 1u;",
        "binding.width = (binding.size_word & 0x3Fu) + 1u;",
        "the width field is read too narrow.",
    ),
    _sw(
        "binding-height-shift",
        "binding.height = ((binding.size_word >> 12) & 0xFFFu) + 1u;",
        "binding.height = ((binding.size_word >> 11) & 0xFFFu) + 1u;",
        "the height field starts one bit low.",
    ),
    _sw(
        "binding-height-plus-zero",
        "binding.height = ((binding.size_word >> 12) & 0xFFFu) + 1u;",
        "binding.height = ((binding.size_word >> 12) & 0xFFFu) + 0u;",
        "the height forgets the plus one.",
    ),
    _sw(
        "binding-pitch-shift",
        "binding.pitch = (((binding.size_word >> 24) & 0xFFu) + 1u) << 6;",
        "binding.pitch = (((binding.size_word >> 23) & 0xFFu) + 1u) << 6;",
        "the pitch field starts one bit low and picks up the top bit of the height.",
    ),
    _sw(
        "binding-pitch-unit",
        "binding.pitch = (((binding.size_word >> 24) & 0xFFu) + 1u) << 6;",
        "binding.pitch = (((binding.size_word >> 24) & 0xFFu) + 1u) << 5;",
        "the pitch unit is 32 bytes instead of 64.",
    ),
    _sw(
        "binding-pitch-plus-zero",
        "binding.pitch = (((binding.size_word >> 24) & 0xFFu) + 1u) << 6;",
        "binding.pitch = (((binding.size_word >> 24) & 0xFFu) + 0u) << 6;",
        "the pitch forgets the plus one.",
    ),
    _sw(
        "same-ignores-header",
        "left->header == right->header &&",
        "true &&",
        "a different header with the same words is a repeat, so the new binding is never noted.",
    ),
    _sw(
        "same-ignores-data",
        "left->data == right->data &&",
        "true &&",
        "a header whose Data moved is a repeat.",
    ),
    _sw(
        "same-ignores-format",
        "left->format == right->format &&",
        "true &&",
        "a header whose Format changed is a repeat.",
    ),
    _sw(
        "same-ignores-size",
        "left->size_word == right->size_word;",
        "true;",
        "a header whose Size word changed is a repeat.",
    ),
    _sw(
        "binding-at-ignores-carry",
        "state.texture_carry_known[stage] ? &state.texture_carry[stage] : NULL;",
        "NULL;",
        "a frame that does not call SetTexture loses what the last frame left.",
    ),
    _sw(
        "binding-at-carry-always-known",
        "state.texture_carry_known[stage] ? &state.texture_carry[stage] : NULL;",
        "&state.texture_carry[stage];",
        "a stage nobody bound has a carried binding of zeros, which reads as bound header 0 instead of unobserved.",
    ),
    _sw(
        "binding-at-any-stage",
        "state.texture_events[i].stage == stage && state.texture_events[i].draw <= draw",
        "true && state.texture_events[i].draw <= draw",
        "a binding of another stage answers for this one.",
    ),
    _sw(
        "binding-at-before-draw",
        "state.texture_events[i].draw <= draw",
        "state.texture_events[i].draw < draw",
        "a binding noted at the draw's own index applies only from the next draw.",
    ),
    _sw(
        "binding-at-first-not-last",
        "            found = &state.texture_events[i].binding;\n",
        "            found = &state.texture_events[i].binding;\n            break;\n",
        "the first note of the frame answers instead of the last one at or before the draw.",
    ),
    # ================================================================== note
    _sw(
        "note-ignores-option",
        "    if (!state.active || !(state.config.render_target_texture || live_model_hook != NULL) || stage >= TEXTURE_STAGES) {",
        "    if (!state.active || stage >= TEXTURE_STAGES) {",
        "SetTexture is noted and drained with the option off.",
    ),
    _sw(
        "note-stage-four",
        "    if (!state.active || !(state.config.render_target_texture || live_model_hook != NULL) || stage >= TEXTURE_STAGES) {",
        "    if (!state.active || !(state.config.render_target_texture || live_model_hook != NULL) || stage > TEXTURE_STAGES) {",
        "stage 4 is read as a stage, past the arrays.",
    ),
    _sw(
        "note-no-reset-follow",
        "    follow_gpu_reset();\n    if (state.pgraph == NULL || state.stats.latched) {\n        return;\n    }\n    const texture_binding binding",
        "    if (state.pgraph == NULL || state.stats.latched) {\n        return;\n    }\n    const texture_binding binding",
        "a SetTexture after CreateDevice is noted against the old device's state.",
    ),
    _sw(
        "note-after-latch",
        "    if (state.pgraph == NULL || state.stats.latched) {\n        return;\n    }\n    const texture_binding binding",
        "    if (state.pgraph == NULL) {\n        return;\n    }\n    const texture_binding binding",
        "a latched replay keeps noting bindings.",
    ),
    _sw(
        "note-dedupe-dropped",
        "    if (current != NULL && same_texture_binding(current, &binding)) {",
        "    if (current != NULL && same_texture_binding(current, &binding) && false) {",
        "a repeat of the stage's binding is noted again and counted.",
    ),
    _sw(
        "note-dedupe-inverted",
        "    if (current != NULL && same_texture_binding(current, &binding)) {",
        "    if (current != NULL && !same_texture_binding(current, &binding)) {",
        "a changed binding is dropped and a repeat is noted.",
    ),
    _sw(
        "note-no-drain",
        "    /* a drain, not a kick: the recording up to here is what ran against the previous binding */\n    d3d8_pushbuffer_drain();\n",
        "    /* a drain, not a kick: the recording up to here is what ran against the previous binding */\n",
        "the draws recorded before the SetTexture are not handed to the model, so the binding is placed after them.",
    ),
    _sw(
        "note-limit-early",
        "    if (state.texture_event_count == D3D8_SWAP_REPLAY_TEXTURE_EVENTS) {",
        "    if (state.texture_event_count == D3D8_SWAP_REPLAY_TEXTURE_EVENTS - 1u) {",
        "the last of the 4096 bindings is dropped.",
    ),
    _sw(
        "note-overflow-not-flagged",
        "        state.texture_overflow = true;\n        return;",
        "        state.texture_overflow = false;\n        return;",
        "a frame past the event limit is replayed with the later bindings missing.",
    ),
    _sw(
        "note-draw-index-off-by-one",
        "    entry->draw = gpu_pgraph_draw_count(state.pgraph);",
        "    entry->draw = gpu_pgraph_draw_count(state.pgraph) + 1u;",
        "a binding applies one draw late.",
    ),
    _sw(
        "note-draw-index-zero",
        "    entry->draw = gpu_pgraph_draw_count(state.pgraph);",
        "    entry->draw = 0u;",
        "every binding applies from the first draw.",
    ),
    _sw(
        "note-stage-lost",
        "    entry->stage = stage;",
        "    entry->stage = 0u;",
        "every binding is noted for stage 0.",
    ),
    _sw(
        "note-binding-lost",
        "    entry->binding = binding;",
        "    memset(&entry->binding, 0, sizeof entry->binding);",
        "the noted binding is an unbound stage.",
    ),
    _sw(
        "note-not-counted",
        "    state.stats.texture_bindings++;",
        "    (void)state.stats.texture_bindings;",
        "the bindings noted are not counted.",
    ),
    _row(
        _BIND,
        "bind-hook-dropped",
        "    d3d8_swap_replay_on_texture(stage, texture); /* T510: no-op unless the render target texture option is on */\n",
        "",
        "SetTexture never reaches the replay, so no binding is ever observed.",
        _SWAP_TEST,
    ),
    _row(
        _BIND,
        "bind-hook-stage",
        "    d3d8_swap_replay_on_texture(stage, texture); /* T510: no-op unless the render target texture option is on */\n",
        "    d3d8_swap_replay_on_texture(0u, texture); /* T510: no-op unless the render target texture option is on */\n",
        "every SetTexture is reported for stage 0.",
        _SWAP_TEST,
    ),
    _row(
        _BIND,
        "bind-hook-texture",
        "    d3d8_swap_replay_on_texture(stage, texture); /* T510: no-op unless the render target texture option is on */\n",
        "    d3d8_swap_replay_on_texture(stage, 0u); /* T510: no-op unless the render target texture option is on */\n",
        "every SetTexture is reported as an unbind.",
        _SWAP_TEST,
    ),
    # ================================================================== refuse
    _sw(
        "provide-clears-detail",
        *_within(_PROVIDER_RESET, "    provider.detail[stage][0] = '\\0';\n", ""),
        "the Format and Size detail of one refusal is printed on the next draw's refusal.",
    ),
    _sw(
        "refuse-no-binding-dropped",
        '    if (binding == NULL) {\n        stage_refusal(stage, out, "no binding observed"',
        '    if (false) {\n        stage_refusal(stage, out, "no binding observed"',
        "a stage nobody bound is read through a null pointer.",
    ),
    _sw(
        "refuse-unbound-inverted",
        "    if (!binding->bound) {\n        stage_refusal(",
        "    if (binding->bound) {\n        stage_refusal(",
        "a bound stage is refused as unbound and an unbound one passes.",
    ),
    _sw(
        "refuse-format-without-measured",
        "    if (!dxt1 && (!binding->measured || binding->format != D3D8_SWAP_REPLAY_RT_TEXTURE_FORMAT)) {",
        "    if (binding->format != D3D8_SWAP_REPLAY_RT_TEXTURE_FORMAT) {",
        "an unmeasured (swizzled) header with the right Format word passes the format test.",
    ),
    _sw(
        "refuse-format-without-word",
        "    if (!dxt1 && (!binding->measured || binding->format != D3D8_SWAP_REPLAY_RT_TEXTURE_FORMAT)) {",
        "    if (!binding->measured) {",
        "any Format word of a linear header passes.",
    ),
    _sw(
        "refuse-format-inverted",
        "    if (!dxt1 && (!binding->measured || binding->format != D3D8_SWAP_REPLAY_RT_TEXTURE_FORMAT)) {",
        "    if (!dxt1 && (!binding->measured || binding->format == D3D8_SWAP_REPLAY_RT_TEXTURE_FORMAT)) {",
        "the supported Format is the one refused.",
    ),
    _sw(
        "refuse-layout-inverted",
        "    if (!dxt1 && binding->pitch != binding->width * 4u) {",
        "    if (binding->pitch == binding->width * 4u) {",
        "the tight pitch is refused and a padded one passes.",
    ),
    _sw(
        "refuse-layout-wider-pitch",
        "    if (!dxt1 && binding->pitch != binding->width * 4u) {",
        "    if (binding->pitch < binding->width * 4u) {",
        "a pitch wider than the tight four bytes per texel passes.",
    ),
    _sw(
        "refuse-layout-bytes-per-texel",
        "    if (!dxt1 && binding->pitch != binding->width * 4u) {",
        "    if (!dxt1 && binding->pitch != binding->width * 2u) {",
        "two bytes per texel is taken for the A8R8G8B8 pitch.",
    ),
    _sw(
        "address-index-stage",
        "    const uint32_t address_index = GPU_PGRAPH_OUT_TEXTURE_ADDRESS + stage;",
        "    const uint32_t address_index = GPU_PGRAPH_OUT_TEXTURE_ADDRESS;",
        "the address word of stage 0 answers for every stage.",
    ),
    _sw(
        "filter-index-stage",
        "    const uint32_t filter_index = GPU_PGRAPH_OUT_TEXTURE_FILTER + stage;",
        "    const uint32_t filter_index = GPU_PGRAPH_OUT_TEXTURE_FILTER;",
        "the filter word of stage 0 answers for every stage.",
    ),
    _sw(
        "address-word-inverted",
        "snapshot->output[address_index] != RT_ADDRESS_CLAMP && !(dxt1 && wrap)",
        "snapshot->output[address_index] == RT_ADDRESS_CLAMP && !(dxt1 && wrap)",
        "clamp is refused and every other mode passes.",
    ),
    _sw(
        "address-constant",
        "#define RT_ADDRESS_CLAMP 0x00000303u",
        "#define RT_ADDRESS_CLAMP 0x00000301u",
        "the clamp word the replay takes is not the one the sampler implements.",
    ),
    _sw(
        "address-message-written",
        'snapshot->output_written[address_index] ? "is not clamp" : "was never written by the stream"',
        'snapshot->output_written[address_index] ? "was never written by the stream" : "is not clamp"',
        "a wrong word and a missing word swap their explanations.",
    ),
    _sw(
        "filter-word-inverted",
        "snapshot->output[filter_index] != RT_FILTER_LINEAR) {",
        "snapshot->output[filter_index] == RT_FILTER_LINEAR) {",
        "the measured filter is refused and every other one passes.",
    ),
    _sw(
        "filter-constant",
        "#define RT_FILTER_LINEAR 0x02062000u",
        "#define RT_FILTER_LINEAR 0x02062001u",
        "the filter word the replay takes is not the measured one.",
    ),
    _sw(
        "filter-message-written",
        'snapshot->output_written[filter_index] ? "is not the measured word" : "was never written by the stream"',
        'snapshot->output_written[filter_index] ? "was never written by the stream" : "is not the measured word"',
        "a wrong filter and a missing one swap their explanations.",
    ),
    _sw(
        "address-register-text",
        "(unsigned)RT_ADDRESS_WRAP, (unsigned)(0x1B08u + 0x40u * stage));",
        "(unsigned)RT_ADDRESS_WRAP, (unsigned)(0x1B0Cu + 0x40u * stage));",
        "the refusal names the wrong method for the address word.",
    ),
    _sw(
        "filter-register-text",
        "(unsigned)RT_FILTER_LINEAR, (unsigned)(0x1B14u + 0x40u * stage));",
        "(unsigned)RT_FILTER_LINEAR, (unsigned)(0x1B18u + 0x40u * stage));",
        "the refusal names the wrong method for the filter word.",
    ),
    # ---- the mapping
    _sw(
        "map-segment-data-inverted",
        "        if (segment->data == binding->data) {",
        "        if (segment->data != binding->data) {",
        "every pass but the producer counts as the producer.",
    ),
    _sw(
        "map-match-not-counted",
        "            matches++; /* a second pass or a second header on the same bytes makes it ambiguous */",
        "            matches = 1u; /* a second pass or a second header on the same bytes makes it ambiguous */",
        "two passes on the same bytes are not ambiguous.",
    ),
    _sw(
        "map-alias-not-recorded",
        "        } else if (ranges_overlap(binding->data, texture_bytes, segment->data, (uint64_t)segment->size.pitch * segment->size.height)) {",
        "        } else if (ranges_overlap(binding->data, texture_bytes, segment->data, (uint64_t)segment->size.pitch * segment->size.height) && false) {",
        "a partial overlap with another target is never noticed.",
    ),
    _sw(
        "map-alias-segment-bytes",
        "(uint64_t)segment->size.pitch * segment->size.height)) {",
        "(uint64_t)segment->size.pitch)) {",
        "the producer's extent for the overlap test is one row.",
    ),
    _sw(
        "map-texture-bytes",
        "    const uint64_t texture_bytes = (uint64_t)binding->pitch * binding->height;",
        "    const uint64_t texture_bytes = (uint64_t)binding->pitch;",
        "the texture's extent for the overlap test is one row.",
    ),
    _sw(
        "overlap-first-inclusive",
        "    return a < b + b_bytes && b < a + a_bytes;",
        "    return a <= b + b_bytes && b < a + a_bytes;",
        "a range that starts exactly where the other ends overlaps it.",
    ),
    _sw(
        "overlap-second-inclusive",
        "    return a < b + b_bytes && b < a + a_bytes;",
        "    return a < b + b_bytes && b <= a + a_bytes;",
        "a range that ends exactly where the other starts overlaps it.",
    ),
    _sw(
        "overlap-either",
        "    return a < b + b_bytes && b < a + a_bytes;",
        "    return a < b + b_bytes || b < a + a_bytes;",
        "disjoint ranges overlap.",
    ),
    _sw(
        "overlap-first-only",
        "    return a < b + b_bytes && b < a + a_bytes;",
        "    return (void)a_bytes, a < b + b_bytes;",
        "a texture entirely above a target overlaps it.",
    ),
    _sw(
        "overlap-second-only",
        "    return a < b + b_bytes && b < a + a_bytes;",
        "    return (void)b_bytes, b < a + a_bytes;",
        "a texture entirely below a target overlaps it.",
    ),
    _sw(
        "refuse-ambiguous-threshold",
        "    if (matches > 1u) {",
        "    if (matches > 2u) {",
        "two passes on the same bytes are not ambiguous.",
    ),
    _sw(
        "refuse-ambiguous-any",
        "    if (matches > 1u) {",
        "    if (matches > 0u) {",
        "a single exact match is ambiguous.",
    ),
    _sw(
        "refuse-no-match-inverted",
        "    if (matches == 0u) {\n        if (alias != SIZE_MAX) {",
        "    if (matches != 0u) {\n        if (alias != SIZE_MAX) {",
        "a texture with a producer is refused and one without passes.",
    ),
    _sw(
        "refuse-alias-or-none",
        '        if (alias != SIZE_MAX) {\n            stage_refusal(stage, out, "alias",',
        '        if (alias == SIZE_MAX) {\n            stage_refusal(stage, out, "alias",',
        "the alias and no earlier image causes swap names.",
    ),
    _sw(
        "refuse-exact-with-alias-dropped",
        '    if (alias != SIZE_MAX) {\n        stage_refusal(stage, out, "ambiguous mapping",',
        '    if (false) {\n        stage_refusal(stage, out, "ambiguous mapping",',
        "an exact producer is taken although a second target overlaps the texture at another offset.",
    ),
    _sw(
        "refuse-feedback-dropped",
        "    if (match == provider.current) {",
        "    if (false) {",
        "a draw samples the target it draws into.",
    ),
    _sw(
        "refuse-feedback-inverted",
        "    if (match == provider.current) {",
        "    if (match != provider.current) {",
        "every producer but the pass itself is feedback.",
    ),
    _sw(
        "refuse-order-dropped",
        "    if (match > provider.current) {",
        "    if (false) {",
        "a producer that runs after the draw is sampled.",
    ),
    _sw(
        "refuse-order-earlier",
        "    if (match > provider.current) {",
        "    if (match < provider.current) {",
        "an earlier producer is out of order.",
    ),
    _sw(
        "inexact-width",
        "producer->size.width != binding->width ||",
        "false ||",
        "a producer of another width is taken.",
    ),
    _sw(
        "inexact-height",
        "producer->size.height != binding->height ||",
        "false ||",
        "a producer of another height is taken.",
    ),
    _sw(
        "dxt1-wrap-for-any",
        "(snapshot->output[address_index] != RT_ADDRESS_CLAMP && !(dxt1 && wrap))",
        "(snapshot->output[address_index] != RT_ADDRESS_CLAMP && !wrap)",
        "T735: the wrap address word is taken for a render target texture too, not only the DXT1 one.",
    ),
    _sw(
        "inexact-pitch",
        "        producer->size.pitch != binding->pitch || producer->size.format != binding->format) {",
        "        producer->size.format != binding->format) {",
        "a producer of another pitch is taken.",
    ),
    _sw(
        "inexact-format",
        "        producer->size.pitch != binding->pitch || producer->size.format != binding->format) {",
        "        producer->size.pitch != binding->pitch) {",
        "a producer of another Format word is taken.",
    ),
    _category("no binding observed"),
    _category("unbound"),
    _category("unsupported format"),
    _category("unsupported layout"),
    _category("address mode"),
    _category("filter"),
    _category("alias"),
    _category("no earlier image"),
    _category("same-target feedback"),
    _category("out of order"),
    _category("inexact relationship"),
    _category(
        "ambiguous mapping",
        'stage_refusal(stage, out, "ambiguous mapping",\n                      "render target texture: ambiguous mapping, Data',
    ),
    _sw(
        "category-ambiguous-mapping-alias",
        'stage_refusal(stage, out, "ambiguous mapping",\n                      "render target texture: ambiguous mapping, header',
        'stage_refusal(stage, out, "renamed",\n                      "render target texture: ambiguous mapping, header',
        "the census names the overlap refusal something else.",
    ),
    # ================================================================== resolve
    _sw(
        "resolve-rgba-first-pass",
        *_within(_RESOLVED_TAIL, "image->pixels;", "provider.passes[0].image.pixels;"),
        "the first pass's image is sampled instead of the producer's.",
    ),
    _sw(
        "resolve-width-height-swapped",
        *_within(
            _RESOLVED_TAIL,
            "    out->width = provider.dry ? 1u : image->width;",
            "    out->width = provider.dry ? 1u : image->height;",
        ),
        "the width handed to the sampler is the height.",
    ),
    _sw(
        "resolve-dry-size",
        *_within(
            _RESOLVED_TAIL,
            "    out->width = provider.dry ? 1u : image->width;",
            "    out->width = provider.dry ? 0u : image->width;",
        ),
        "the census's placeholder has a zero width, which the planner refuses.",
    ),
    _sw(
        "resolve-not-linear",
        *_within(_RESOLVED_TAIL, "    out->linear = true;", "    out->linear = false;"),
        "the image is sampled nearest although the stream's filter is bilinear.",
    ),
    _sw(
        "resolve-normalised",
        *_within(_RESOLVED_TAIL, "    out->unnormalised = true;", "    out->unnormalised = false;"),
        "the coordinate is taken as normalised although a linear texture takes it in texels.",
    ),
    _sw(
        "resolve-source-size-swapped",
        "(unsigned)producer->target, (unsigned)producer->data, (unsigned)binding->width, (unsigned)binding->height,",
        "(unsigned)producer->target, (unsigned)producer->data, (unsigned)binding->height, (unsigned)binding->width,",
        "the census source line names the texture as height by width.",
    ),
    _sw(
        "resolve-source-target-header",
        "(unsigned)producer->target, (unsigned)producer->data, (unsigned)binding->width, (unsigned)binding->height,",
        "(unsigned)binding->header, (unsigned)producer->data, (unsigned)binding->width, (unsigned)binding->height,",
        "the census source line names the texture header instead of the render target.",
    ),
    _sw(
        "resolve-source-text",
        'drawn by an earlier pass of the same frame",',
        'drawn by a later pass of the same frame",',
        "the census source line says the wrong thing about when the image was drawn.",
    ),
    # ---- the tally
    _sw(
        "tally-line-text",
        '    (void)snprintf(line, sizeof line, "stage %u from %s", (unsigned)stage, source);\n    for (size_t i = 0u;',
        '    (void)snprintf(line, sizeof line, "stage %u: %s", (unsigned)stage, source);\n    for (size_t i = 0u;',
        "the tally line of a sampled stage does not begin `stage N from`.",
    ),
    _sw(
        "tally-no-dedupe",
        '"stage %u from %s", (unsigned)stage, source);\n    for (size_t i = 0u; i < state.texture_census_count; i++) {\n        if (strcmp(state.texture_census[i].text, line) == 0) {',
        '"stage %u from %s", (unsigned)stage, source);\n    for (size_t i = 0u; i < state.texture_census_count; i++) {\n        if (false) {',
        "every sample is a new census line, so the same source is listed once per draw.",
    ),
    _sw(
        "tally-dedupe-first-only",
        '"stage %u from %s", (unsigned)stage, source);\n    for (size_t i = 0u; i < state.texture_census_count; i++) {',
        '"stage %u from %s", (unsigned)stage, source);\n    for (size_t i = 0u; i < 1u && i < state.texture_census_count; i++) {',
        "only the first line of the census is matched against, a later source is listed again.",
    ),
    _sw(
        "tally-no-increment",
        '        if (strcmp(state.texture_census[i].text, line) == 0) {\n            state.texture_census[i].draws++;\n            return;\n        }\n    }\n    if (state.texture_census_count == D3D8_SWAP_REPLAY_TEXTURE_CENSUS) {\n        state.texture_census_overflow++;\n        return;\n    }\n    texture_census_line *entry = &state.texture_census[state.texture_census_count++];\n    (void)snprintf(entry->text, sizeof entry->text, "%s", line);\n    entry->draws = 1u;\n}\n\nstatic void texture_sampled',
        '        if (strcmp(state.texture_census[i].text, line) == 0) {\n            state.texture_census[i].draws += 0u;\n            return;\n        }\n    }\n    if (state.texture_census_count == D3D8_SWAP_REPLAY_TEXTURE_CENSUS) {\n        state.texture_census_overflow++;\n        return;\n    }\n    texture_census_line *entry = &state.texture_census[state.texture_census_count++];\n    (void)snprintf(entry->text, sizeof entry->text, "%s", line);\n    entry->draws = 1u;\n}\n\nstatic void texture_sampled',
        "a repeated source stays at one draw.",
    ),
    _sw(
        "tally-full-early",
        *_within(
            _CENSUS_TALLY_TAIL,
            "state.texture_census_count == D3D8_SWAP_REPLAY_TEXTURE_CENSUS",
            "state.texture_census_count == D3D8_SWAP_REPLAY_TEXTURE_CENSUS - 1u",
        ),
        "the last census line is dropped as if the table were full.",
    ),
    _sw(
        "tally-full-never",
        *_within(
            _CENSUS_TALLY_TAIL,
            "state.texture_census_count == D3D8_SWAP_REPLAY_TEXTURE_CENSUS",
            "false",
        ),
        "a new line is written past the end of the table.",
    ),
    _sw(
        "tally-first-draws",
        *_within(_CENSUS_TALLY_TAIL, "    entry->draws = 1u;", "    entry->draws = 0u;"),
        "a new line starts at no draw.",
    ),
    _sw(
        "sampled-flag-lost",
        "        provider.sampled = true;\n        state.stats.rt_texture_draws++;",
        "        state.stats.rt_texture_draws++;",
        "the pass does not report the BRIDGE inference although it sampled an image.",
    ),
    _sw(
        "sampled-not-counted",
        "        state.stats.rt_texture_draws++;\n        tally_texture_source(stage, provider.source[stage]);",
        "        tally_texture_source(stage, provider.source[stage]);",
        "the sampled (draw, stage) pairs are not counted.",
    ),
    _sw(
        "sampled-not-tallied",
        "        tally_texture_source(stage, provider.source[stage]);\n",
        "        if (false) {\n            tally_texture_source(stage, provider.source[stage]);\n        }\n",
        "the census never lists what was sampled.",
    ),
    # ================================================================== census
    _sw(
        "census-at-bound",
        "    if (index >= state.texture_census_count) {\n        return false;",
        "    if (index > state.texture_census_count) {\n        return false;",
        "the line one past the end is returned.",
    ),
    _sw(
        "census-at-text-guard",
        '    if (text != NULL && size != 0u) {\n        (void)snprintf(text, size, "%s", state.texture_census[index].text);',
        '    if (false) {\n        (void)snprintf(text, size, "%s", state.texture_census[index].text);',
        "the text of a line is never copied out.",
    ),
    _sw(
        "census-at-draws-guard",
        "    if (draws != NULL) {\n        *draws = state.texture_census[index].draws;",
        "    if (false) {\n        *draws = state.texture_census[index].draws;",
        "the draw count of a line is never copied out.",
    ),
    _sw(
        "summary-option-off",
        "    if (!state.active || !state.config.render_target_texture) {\n        return 0u;\n    }\n    if (state.config.render_target_texture_census) {",
        "    if (!state.active) {\n        return 0u;\n    }\n    if (state.config.render_target_texture_census) {",
        "a replay without the option prints a render target texture summary.",
    ),
    _sw(
        "summary-census-branch",
        "    if (state.config.render_target_texture_census) {\n        const int census = snprintf(",
        "    if (false) {\n        const int census = snprintf(",
        "a census run prints the replay summary and claims pixels were replayed.",
    ),
    _sw(
        "summary-census-only-text",
        "CENSUS ONLY, no pixel was replayed.",
        "census, pixels were replayed.",
        "the census summary no longer says that no pixel was replayed.",
    ),
    _sw(
        "summary-census-counts-swapped",
        "(unsigned long long)state.stats.census_frames, (unsigned long long)state.stats.census_draws,",
        "(unsigned long long)state.stats.census_draws, (unsigned long long)state.stats.census_frames,",
        "the census summary swaps its frame and draw counts.",
    ),
    _sw(
        "summary-census-bindings",
        "(unsigned long long)state.stats.texture_bindings);\n        if (census < 0) {",
        "(unsigned long long)state.stats.census_draws);\n        if (census < 0) {",
        "the census summary reports the draws as the bindings noted.",
    ),
    _sw(
        "summary-never-white",
        "Every other bound texture is REFUSED by name, never a white or default texture.",
        "Every other bound texture is a white or default texture.",
        "the summary no longer says a refused binding is never replaced by a default texture.",
    ),
    _sw(
        "summary-inferred",
        "INFERRED: the replayed image is the surface the hardware samples,",
        "the replayed image is the surface the hardware samples,",
        "the summary no longer calls the sampling model inferred.",
    ),
    _sw(
        "summary-counts-swapped",
        "(unsigned long long)state.stats.texture_bindings, (unsigned long long)state.stats.rt_texture_draws,\n        (unsigned long long)state.stats.draws);",
        "(unsigned long long)state.stats.rt_texture_draws, (unsigned long long)state.stats.texture_bindings,\n        (unsigned long long)state.stats.draws);",
        "the summary swaps the bindings noted and the samples read.",
    ),
    _sw(
        "summary-draws",
        "(unsigned long long)state.stats.rt_texture_draws,\n        (unsigned long long)state.stats.draws);",
        "(unsigned long long)state.stats.rt_texture_draws,\n        (unsigned long long)state.stats.census_draws);",
        "the summary reports the census draws as the replayed draws.",
    ),
    _sw(
        "census-frame-not-dry",
        "    provider.dry = true;\n    provider.passes = NULL;",
        "    provider.dry = false;\n    provider.passes = NULL;",
        "the census reads an image of a pass that was never replayed.",
    ),
    _sw(
        "census-current-segment",
        "        provider.current = segment;\n        for (size_t index = plan[segment].first;",
        "        provider.current = 0u;\n        for (size_t index = plan[segment].first;",
        "every pass is judged as the first one, so a producer is never earlier.",
    ),
    _sw(
        "census-draw-range-end",
        "index < plan[segment].end; index++) {\n            const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(state.pgraph, index);",
        "index <= plan[segment].end; index++) {\n            const gpu_pgraph_draw *draw = gpu_pgraph_draw_at(state.pgraph, index);",
        "the first draw of the next pass is classified under this one.",
    ),
    _sw(
        "census-draw-range-start",
        "        for (size_t index = plan[segment].first; index < plan[segment].end; index++) {\n            const gpu_pgraph_draw *draw",
        "        for (size_t index = plan[segment].first + 1u; index < plan[segment].end; index++) {\n            const gpu_pgraph_draw *draw",
        "the first draw of every pass is not classified.",
    ),
    _sw(
        "census-draw-index",
        "provide_texture(NULL, index, snapshot, stage, &textures[stage]);",
        "provide_texture(NULL, plan[segment].first, snapshot, stage, &textures[stage]);",
        "every draw of a pass is judged against the binding at the pass's first draw.",
    ),
    _sw(
        "census-allows-all",
        "state.config.allowed_inferences & GPU_COMBINER_INFER_ALL,",
        "GPU_COMBINER_INFER_ALL,",
        "the census plans with inferences nobody allowed.",
    ),
    _sw(
        "census-allows-none",
        "state.config.allowed_inferences & GPU_COMBINER_INFER_ALL,",
        "0u,",
        "the census plans with no inference, so every draw that needs one is refused.",
    ),
    _sw(
        "census-draws-not-counted",
        "            state.stats.census_draws++;\n",
        "",
        "the census draws are not counted.",
    ),
    _sw(
        "census-plan-inverted",
        "            if (planned != GPU_PGRAPH_OK) {\n                const char *category = NULL;",
        "            if (planned == GPU_PGRAPH_OK) {\n                const char *category = NULL;",
        "the draws that plan are counted as refused and the refused ones as planned.",
    ),
    _sw(
        "census-category-no-text-match",
        "if (provider.category[stage] != NULL && strstr(error, provider.refusal[stage]) != NULL) {",
        "if (provider.category[stage] != NULL) {",
        "a refusal of a stage the combiner does not read is blamed for a refusal of another stage.",
    ),
    _sw(
        "census-category-stage-lost",
        "                        refused_stage = stage;\n                        break;",
        "                        refused_stage = 0u;\n                        break;",
        "the refusal is always attributed to stage 0.",
    ),
    _sw(
        "census-refused-text",
        '"REFUSED stage %u texture: %s,%s (drawing into target 0x%08X)"',
        '"REFUSED stage %u: %s,%s (drawing into target 0x%08X)"',
        "the census refusal line does not begin `REFUSED stage N texture:`.",
    ),
    _sw(
        "census-refused-detail-stage",
        "category, provider.detail[refused_stage], (unsigned)plan[segment].target);",
        "category, provider.detail[0], (unsigned)plan[segment].target);",
        "the Format and Size detail of stage 0 is printed for another stage's refusal.",
    ),
    _sw(
        "census-refused-target",
        "category, provider.detail[refused_stage], (unsigned)plan[segment].target);",
        "category, provider.detail[refused_stage], (unsigned)plan[0].target);",
        "the refusal names the first pass's target as the one drawn into.",
    ),
    _sw(
        "census-non-texture-text",
        '"REFUSED by the combiner plan, not a texture: %.100s"',
        '"REFUSED by the combiner plan: %.100s"',
        "the census line of a refusal that is not a texture does not say so.",
    ),
    _sw(
        "census-non-texture-length",
        '"REFUSED by the combiner plan, not a texture: %.100s"',
        '"REFUSED by the combiner plan, not a texture: %.10s"',
        "the combiner's reason is cut to ten characters, so unlike reasons merge.",
    ),
    _sw(
        "census-no-texture-inverted",
        "            if (combiner.texture_stages == 0u) {\n                census_line(",
        "            if (combiner.texture_stages != 0u) {\n                census_line(",
        "a draw that reads a texture is counted as reading none.",
    ),
    _sw(
        "census-no-texture-text",
        '"no texture read by the combiner"',
        '"no texture"',
        "the census line of a draw that reads no texture is renamed.",
    ),
    _sw(
        "census-stage-bit-inverted",
        'if (((combiner.texture_stages >> stage) & 1u) != 0u) {\n                    (void)snprintf(line, sizeof line, "stage %u from %s"',
        'if (((combiner.texture_stages >> stage) & 1u) == 0u) {\n                    (void)snprintf(line, sizeof line, "stage %u from %s"',
        "the stages the combiner does not read are tallied.",
    ),
    _sw(
        "census-stage-source",
        '(void)snprintf(line, sizeof line, "stage %u from %s", (unsigned)stage, provider.source[stage]);\n                    census_line(line);',
        '(void)snprintf(line, sizeof line, "stage %u from %s", (unsigned)stage, provider.source[0]);\n                    census_line(line);',
        "every stage is tallied under the source of stage 0.",
    ),
    _sw(
        "census-resolved-not-counted",
        "                    census_line(line);\n                    state.stats.rt_texture_draws++;",
        "                    census_line(line);",
        "the census does not count the (draw, stage) samples it resolved.",
    ),
    _sw(
        "census-line-text-copy",
        '    char line[RT_SOURCE_BYTES + 160];\n    (void)snprintf(line, sizeof line, "%s", text);\n    for (size_t i = 0u; i < state.texture_census_count; i++) {\n        if (strcmp(state.texture_census[i].text, line) == 0) {',
        '    char line[RT_SOURCE_BYTES + 160];\n    (void)snprintf(line, sizeof line, "%s", text);\n    for (size_t i = 0u; i < state.texture_census_count; i++) {\n        if (false) {',
        "a census line that repeats is listed again instead of counted.",
    ),
    _sw(
        "census-line-full-early",
        *_within(
            _CENSUS_LINE_TAIL,
            "state.texture_census_count == D3D8_SWAP_REPLAY_TEXTURE_CENSUS",
            "state.texture_census_count == D3D8_SWAP_REPLAY_TEXTURE_CENSUS - 1u",
        ),
        "the last census line is dropped as if the table were full.",
    ),
    _sw(
        "census-line-full-never",
        *_within(
            _CENSUS_LINE_TAIL,
            "state.texture_census_count == D3D8_SWAP_REPLAY_TEXTURE_CENSUS",
            "false",
        ),
        "a new census line is written past the end of the table.",
    ),
    _sw(
        "census-line-first-draws",
        *_within(_CENSUS_LINE_TAIL, "    entry->draws = 1u;", "    entry->draws = 0u;"),
        "a new census line starts at no draw.",
    ),
    # ================================================================== plan
    _sw(
        "segment-target-from-to",
        "        *target = segment == 0u ? entry->from : entry->to;",
        "        *target = segment == 0u ? entry->to : entry->from;",
        "the first segment's target is the one switched to, the later ones the one switched from.",
    ),
    _sw(
        "segment-data-from-to",
        "        *data = segment == 0u ? entry->from_data : entry->to_data;",
        "        *data = segment == 0u ? entry->to_data : entry->from_data;",
        "the segments' Data words are the neighbouring switch's.",
    ),
    _sw(
        "segment-size-from-to",
        "        *size = segment == 0u ? entry->from_size : entry->to_size;",
        "        *size = segment == 0u ? entry->to_size : entry->from_size;",
        "the segments' measured sizes are the neighbouring switch's.",
    ),
    _sw(
        "segment-entry-index",
        "const target_switch *entry = segment == 0u ? &state.switches[0] : &state.switches[segment - 1u];",
        "const target_switch *entry = segment == 0u ? &state.switches[0] : &state.switches[segment];",
        "a later segment reads the switch after the one that bound its target.",
    ),
    _sw(
        "segment-no-switch-case",
        "    if (state.switch_count != 0u) {\n        const target_switch *entry",
        "    if (false) {\n        const target_switch *entry",
        "a frame with a switch takes the target bound at the present for every segment.",
    ),
    _sw(
        "segment-bound-data",
        "    *data = surface_data(bound);",
        "    *data = 0u;",
        "a frame with no switch has no Data word for its one pass.",
    ),
    _sw(
        "segment-bound-size",
        "    *size = measure_target(bound);\n    if (state.switch_count",
        "    *size = (target_size){false, 0u, 0u, 0u, 0u};\n    if (state.switch_count",
        "a frame with no switch has no measured size for its one pass.",
    ),
    _sw(
        "present-overflow-refusal-dropped",
        "    if (state.texture_overflow) {\n        snprintf(what, sizeof what,",
        "    if (false) {\n        snprintf(what, sizeof what,",
        "a frame that overflowed the binding list is replayed with the later bindings missing.",
    ),
    _sw(
        "present-overflow-text",
        '"more than %u SetTexture bindings in one frame (render target texture)"',
        '"more than %u SetTexture bindings in one frame"',
        "the overflow refusal no longer names the option.",
    ),
    _sw(
        "present-plan-guard",
        "    if (state.config.render_target_texture) {\n        /* every pass the frame will replay",
        "    if (false) {\n        /* every pass the frame will replay",
        "no pass is planned, so no texture has a producer.",
    ),
    _sw(
        "plan-first-draw",
        "const size_t first = segment == 0u ? 0u : state.switches[segment - 1u].draws;\n            const size_t end = segment < state.switch_count ? state.switches[segment].draws : frame_draws;\n            if (end > frame_draws",
        "const size_t first = 0u;\n            const size_t end = segment < state.switch_count ? state.switches[segment].draws : frame_draws;\n            if (end > frame_draws",
        "every planned pass starts at the first draw of the frame.",
    ),
    _sw(
        "plan-last-draw",
        "const size_t end = segment < state.switch_count ? state.switches[segment].draws : frame_draws;\n            if (end > frame_draws",
        "const size_t end = frame_draws;\n            if (end > frame_draws",
        "every planned pass runs to the end of the frame.",
    ),
    _sw(
        "plan-empty-segment",
        "            if (end > frame_draws || end < first || end == first) {",
        "            if (end > frame_draws || end < first) {",
        "an empty segment is planned as a pass, shifting every later producer's index off its replayed image.",
    ),
    _sw(
        "plan-first-lost",
        "            entry->first = first;\n",
        "            entry->first = 0u;\n",
        "the 'out of order' refusal names draw 0 as the producer's first draw.",
    ),
    _sw(
        "plan-end-lost",
        "            entry->end = end;\n",
        "            entry->end = frame_draws;\n",
        "the census classifies every draw of the frame under each pass.",
    ),
    _sw(
        "plan-count-lost",
        "        provider.plan_count = plan_count;\n",
        "        provider.plan_count = 0u;\n",
        "no pass is visible to the provider.",
    ),
    _sw(
        "present-census-branch",
        "    if (state.config.render_target_texture_census) {\n        census_frame(plan, plan_count);",
        "    if (false) {\n        census_frame(plan, plan_count);",
        "a census run replays its pixels.",
    ),
    _sw(
        "present-census-no-classification",
        *_within(
            _PRESENT_CENSUS,
            "        census_frame(plan, plan_count);\n",
            "        if (false) {\n            census_frame(plan, plan_count);\n        }\n",
        ),
        "the census frame is discarded unclassified.",
    ),
    _sw(
        "present-census-frames-not-counted",
        *_within(_PRESENT_CENSUS, "        state.stats.census_frames++;\n", ""),
        "the census frames are not counted.",
    ),
    _sw(
        "present-census-stream-kept",
        *_within(_PRESENT_CENSUS, "        d3d8_gpu_stream_discard(state.next);\n", ""),
        "the classified frame's stream is not discarded, so the next frame decodes it again.",
    ),
    _sw(
        "present-census-next-kept",
        *_within(_PRESENT_CENSUS, "        state.next = 0u;\n", ""),
        "the decode position is kept after the stream was discarded.",
    ),
    _sw(
        "present-census-switches-kept",
        *_within(_PRESENT_CENSUS, "        state.switch_count = 0u;\n", ""),
        "the classified frame's target switches carry into the next frame.",
    ),
    _sw(
        "present-census-model-kept",
        *_within(_PRESENT_CENSUS, "        (void)gpu_pgraph_begin_frame(state.pgraph);\n", ""),
        "the classified draws stay in the model's list, so every later frame classifies them again.",
    ),
    _sw(
        "present-census-empty-not-counted",
        *_within(_PRESENT_CENSUS, "        state.stats.frames_empty++;\n", ""),
        "a classified frame is not counted among the frames that replayed nothing.",
    ),
    _sw(
        "present-census-falls-through",
        *_within(_PRESENT_CENSUS, "        return;\n    }\n    for", "    }\n    for"),
        "the census frame is then replayed as well, with a dry provider.",
    ),
    _sw(
        "roll-carry-lost",
        "        state.texture_carry[state.texture_events[i].stage] = state.texture_events[i].binding;\n",
        "",
        "the next frame does not start with what the stages held.",
    ),
    _sw(
        "roll-known-lost",
        "        state.texture_carry_known[state.texture_events[i].stage] = true;\n",
        "",
        "the carried bindings are never marked known.",
    ),
    _sw(
        "roll-stage-zero",
        "        state.texture_carry[state.texture_events[i].stage] = state.texture_events[i].binding;",
        "        state.texture_carry[0] = state.texture_events[i].binding;",
        "every stage carries the binding of the last event.",
    ),
    _sw(
        "roll-events-kept",
        "    state.texture_event_count = 0u;\n    state.texture_overflow = false;\n}",
        "    state.texture_overflow = false;\n}",
        "the next frame replays this frame's bindings again.",
    ),
    _sw(
        "present-roll-dropped",
        "    provider.plan_count = 0u;\n    roll_texture_events();\n",
        "    provider.plan_count = 0u;\n    if (false) {\n        roll_texture_events();\n    }\n",
        "a frame never hands its bindings on to the next one.",
    ),
    _sw(
        "replay-pass-wiring",
        "    if (state.config.render_target_texture) {\n        backend.resolve_texture = provide_texture;",
        "    if (false) {\n        backend.resolve_texture = provide_texture;",
        "the replay never asks the provider, so every texture read is refused as a missing test texture.",
    ),
    _sw(
        "replay-pass-sampled-callback",
        "        backend.texture_sampled = texture_sampled;\n",
        "        backend.texture_sampled = state.config.render_target_texture_census ? texture_sampled : NULL;\n",
        "no sample is counted, tallied or reported as the BRIDGE inference.",
    ),
    _sw(
        "replay-pass-passes",
        "        provider.passes = passes;\n",
        "",
        "the provider reads the image of a stale pass list.",
    ),
    _sw(
        "replay-pass-current",
        "        provider.current = pass_count;\n",
        "        provider.current = 0u;\n",
        "every pass is judged as the first one.",
    ),
    _sw(
        "replay-pass-current-late",
        "        provider.current = pass_count;\n",
        "        provider.current = pass_count + 1u;\n",
        "the pass is judged one plan index late, so its own target is an earlier producer.",
    ),
    _sw(
        "replay-pass-bridge-dropped",
        "report.used_inferences | (provider.sampled ? GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE : 0u);",
        "report.used_inferences;",
        "a replay that sampled a render target does not report the bridge inference.",
    ),
    _sw(
        "replay-pass-bridge-always",
        "report.used_inferences | (provider.sampled ? GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE : 0u);",
        "report.used_inferences | GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE;",
        "every replayed pass reports the bridge inference, sampled or not.",
    ),
    _sw(
        "replay-pass-bridge-inverted",
        "report.used_inferences | (provider.sampled ? GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE : 0u);",
        "report.used_inferences | (provider.sampled ? 0u : GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE);",
        "the bridge inference is reported by the passes that sampled nothing.",
    ),
    # ================================================================== combiner
    _row(
        _COMBINER,
        "plan-named-refusal-mode",
        '            if (textures[stage].refusal != NULL) {\n                return refuse(&an, "the combiner reads texture register t%u: %s"',
        '            if (false) {\n                return refuse(&an, "the combiner reads texture register t%u: %s"',
        "a stage read in mode 0 is refused with the generic text instead of the named cause.",
        _COMBINER_TEST,
    ),
    _row(
        _COMBINER,
        "plan-named-refusal-stage",
        'return refuse(&an, "the combiner reads texture register t%u: %s", (unsigned)stage, textures[stage].refusal);',
        'return refuse(&an, "the combiner reads texture register t%u: %s", 0u, textures[stage].refusal);',
        "the named refusal of a mode 0 stage always says t0.",
        _COMBINER_TEST,
    ),
    _row(
        _COMBINER,
        "plan-named-refusal-sampled",
        "        if (texture->rgba == NULL && texture->refusal != NULL) {",
        "        if (false) {",
        "a sampled stage with no image is refused with the generic text instead of the named cause.",
        _COMBINER_TEST,
    ),
    _row(
        _COMBINER,
        "plan-named-refusal-either",
        "        if (texture->rgba == NULL && texture->refusal != NULL) {",
        "        if (texture->rgba == NULL || texture->refusal != NULL) {",
        "a stage with no image and no named cause prints a null refusal instead of the generic text.",
        _COMBINER_TEST,
    ),
    _row(
        _COMBINER,
        "plan-named-refusal-sampled-stage",
        'return refuse(&an, "the combiner reads texture register t%u: %s", (unsigned)stage, texture->refusal);',
        'return refuse(&an, "the combiner reads texture register t%u: %s", 0u, texture->refusal);',
        "the named refusal of a sampled stage always says t0.",
        _COMBINER_TEST,
    ),
    # ================================================================== texel
    _tx(
        "texel-null-words",
        "    if (words == NULL || size == NULL || out == NULL || out_count == NULL || word_count < 5u || words[0] != SPV_MAGIC ||",
        "    if (size == NULL || out == NULL || out_count == NULL || word_count < 5u || words[0] != SPV_MAGIC ||",
        "a NULL module is read.",
    ),
    _tx(
        "texel-null-size",
        "    if (words == NULL || size == NULL || out == NULL ||",
        "    if (words == NULL || out == NULL ||",
        "a NULL size table is read.",
    ),
    _tx(
        "texel-null-out",
        "size == NULL || out == NULL || out_count == NULL || word_count < 5u",
        "size == NULL || out_count == NULL || word_count < 5u",
        "a NULL output pointer is written through.",
    ),
    _tx(
        "texel-null-out-count",
        "size == NULL || out == NULL || out_count == NULL || word_count < 5u",
        "size == NULL || out == NULL || word_count < 5u",
        "a NULL output count is written through.",
    ),
    _tx(
        "texel-magic",
        "word_count < 5u || words[0] != SPV_MAGIC ||\n        words[3] == 0u || words[3] > SPV_MAX_IDS || stages",
        "word_count < 5u ||\n        words[3] == 0u || words[3] > SPV_MAX_IDS || stages",
        "a module without the SPIR-V magic is rewritten.",
    ),
    _tx(
        "texel-zero-bound",
        "words[3] == 0u || words[3] > SPV_MAX_IDS || stages == 0u",
        "words[3] > SPV_MAX_IDS || stages == 0u",
        "a module whose id bound is 0 is rewritten.",
    ),
    _tx(
        "texel-huge-bound",
        "words[3] == 0u || words[3] > SPV_MAX_IDS || stages == 0u",
        "words[3] == 0u || stages == 0u",
        "a module whose id bound is past the limit is rewritten.",
    ),
    _tx(
        "texel-stage-past-three",
        "stages == 0u || (stages & ~0xFu) != 0u) {",
        "stages == 0u) {",
        "a stage bit past 3 is accepted.",
    ),
    _tx(
        "texel-out-not-cleared",
        "    *out = NULL;\n    *out_count = 0u;\n    texel_stage found[4]",
        "    *out_count = 0u;\n    texel_stage found[4]",
        "a failed rewrite leaves the caller's pointer as it was.",
    ),
    _tx(
        "texel-count-not-cleared",
        "    *out = NULL;\n    *out_count = 0u;\n    texel_stage found[4]",
        "    *out = NULL;\n    texel_stage found[4]",
        "a failed rewrite leaves the caller's count as it was.",
    ),
    _tx(
        "texel-float-width",
        "words[at + 2u] == 32u && float_type == 0u) {",
        "words[at + 2u] == 16u && float_type == 0u) {",
        "a 16 bit float type is the sample's scalar.",
    ),
    _tx(
        "texel-float-length",
        "opcode == SPV_OP_TYPE_FLOAT && length >= 3u &&",
        "opcode == SPV_OP_TYPE_FLOAT && length >= 4u &&",
        "no float type is found.",
    ),
    _tx(
        "texel-first-function-last",
        "        if (opcode == SPV_OP_FUNCTION && first_function == 0u) {",
        "        if (opcode == SPV_OP_FUNCTION) {",
        "the last function is taken as the first.",
    ),
    _tx(
        "texel-truncated-instruction",
        "        const size_t length = words[at] >> 16;\n        if (length == 0u || word_count - at < length) {\n            return false;\n        }\n        if (opcode == SPV_OP_TYPE_FLOAT",
        "        const size_t length = words[at] >> 16;\n        if (length == 0u || word_count - at < length) {\n            return false;\n        }\n        if (opcode == SPV_OP_TYPE_FLOAT".replace(
            "at < length", "at <= length"
        ),
        "an instruction that ends exactly at the end of the module is truncated.",
    ),
    _tx(
        "texel-zero-length",
        "        const size_t length = words[at] >> 16;\n        if (length == 0u || word_count - at < length) {\n            return false;\n        }\n        if (opcode == SPV_OP_TYPE_FLOAT",
        "        const size_t length = words[at] >> 16;\n        if (length == 0u || word_count - at < length) {\n            return false;\n        }\n        if (opcode == SPV_OP_TYPE_FLOAT".replace(
            "length == 0u || ", ""
        ),
        "an instruction of length 0 loops forever.",
    ),
    _tx(
        "texel-vector-width",
        "words[at + 2u] == float_type && words[at + 3u] == 2u) {",
        "words[at + 2u] == float_type && words[at + 3u] == 3u) {",
        "a vec3 is the coordinate type.",
    ),
    _tx(
        "texel-stage-skip-inverted",
        "        if (((stages >> stage) & 1u) == 0u) {\n            continue;\n        }\n        uint32_t variable = 0u;",
        "        if (((stages >> stage) & 1u) != 0u) {\n            continue;\n        }\n        uint32_t variable = 0u;",
        "the stages asked for are skipped and the others rewritten.",
    ),
    _tx(
        "texel-binding-base",
        "words[at + 2u] == SPV_DECORATION_BINDING && words[at + 3u] == 2u + stage) {",
        "words[at + 2u] == SPV_DECORATION_BINDING && words[at + 3u] == 1u + stage) {",
        "the sampler of stage n is looked up at binding 1 + n.",
    ),
    _tx(
        "texel-decoration-length",
        "(words[at] >> 16) >= 4u &&\n                words[at + 2u] == SPV_DECORATION_BINDING",
        "(words[at] >> 16) >= 5u &&\n                words[at + 2u] == SPV_DECORATION_BINDING",
        "no binding decoration is long enough.",
    ),
    _tx(
        "texel-decoration-constant",
        "#define SPV_DECORATION_BINDING 33u",
        "#define SPV_DECORATION_BINDING 34u",
        "the descriptor set decoration is read as the binding.",
    ),
    _tx(
        "texel-storage-constant",
        "#define SPV_STORAGE_UNIFORM_CONSTANT 0u",
        "#define SPV_STORAGE_UNIFORM_CONSTANT 1u",
        "an Input variable is taken for the sampler.",
    ),
    _tx(
        "texel-load-opcode",
        "#define SPV_OP_LOAD 61u",
        "#define SPV_OP_LOAD 62u",
        "OpStore is taken for OpLoad.",
    ),
    _tx(
        "texel-function-opcode",
        "#define SPV_OP_FUNCTION 54u",
        "#define SPV_OP_FUNCTION 55u",
        "OpFunctionParameter is taken for OpFunction.",
    ),
    _tx(
        "texel-load-length",
        "if ((words[at] & 0xFFFFu) == SPV_OP_LOAD && (words[at] >> 16) >= 4u && words[at + 3u] == variable) {",
        "if ((words[at] & 0xFFFFu) == SPV_OP_LOAD && (words[at] >> 16) >= 5u && words[at + 3u] == variable) {",
        "no load is long enough.",
    ),
    _tx(
        "texel-load-operand",
        "(words[at] >> 16) >= 4u && words[at + 3u] == variable) {",
        "(words[at] >> 16) >= 4u && words[at + 2u] == variable) {",
        "the load's result id is compared with the variable.",
    ),
    _tx(
        "texel-load-id",
        "                found[stage].load = words[at + 2u];",
        "                found[stage].load = words[at + 1u];",
        "the result type of the load is taken for its result id.",
    ),
    _tx(
        "texel-load-not-active",
        "                found[stage].active = true;\n",
        "",
        "a found load is never marked.",
    ),
    _tx(
        "texel-width-zero",
        "if (!found[stage].active || size[stage][0] < 1.0f || size[stage][1] < 1.0f) {",
        "if (!found[stage].active || size[stage][1] < 1.0f) {",
        "a zero width divides the coordinate by zero.",
    ),
    _tx(
        "texel-height-zero",
        "if (!found[stage].active || size[stage][0] < 1.0f || size[stage][1] < 1.0f) {",
        "if (!found[stage].active || size[stage][0] < 1.0f) {",
        "a zero height divides the coordinate by zero.",
    ),
    _tx(
        "texel-width-one",
        "size[stage][0] < 1.0f || size[stage][1] < 1.0f) {",
        "size[stage][0] <= 1.0f || size[stage][1] < 1.0f) {",
        "a one texel wide texture is refused.",
    ),
    _tx(
        "texel-height-one",
        "size[stage][0] < 1.0f || size[stage][1] < 1.0f) {",
        "size[stage][0] < 1.0f || size[stage][1] <= 1.0f) {",
        "a one texel high texture is refused.",
    ),
    _tx(
        "texel-use-else-dropped",
        "                } else if (!(opcode == SPV_OP_LOAD && operand == 2u)) {\n                    return false;\n                }",
        "                }",
        "a sampler used by anything but a sample is rewritten.",
    ),
    _tx(
        "texel-sample-opcode",
        "#define SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD 87u",
        "#define SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD 88u",
        "OpImageSampleExplicitLod is taken for the implicit one.",
    ),
    _tx(
        "texel-sample-operand",
        "if (opcode == SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD && operand == 3u && length >= 5u) {",
        "if (opcode == SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD && operand == 4u && length >= 5u) {",
        "the coordinate operand is taken for the sampled image.",
    ),
    _tx(
        "texel-sample-length",
        "if (opcode == SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD && operand == 3u && length >= 5u) {",
        "if (opcode == SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD && operand == 3u && length >= 6u) {",
        "a sample is never long enough.",
    ),
    _tx(
        "texel-coordinate-type",
        "if (!id_has_type(words, word_count, words[at + 4u], vector_type)) {",
        "if (id_has_type(words, word_count, words[at + 4u], vector_type)) {",
        "a coordinate that is a vec2 is rejected and any other accepted.",
    ),
    _tx(
        "texel-coordinate-operand",
        "if (!id_has_type(words, word_count, words[at + 4u], vector_type)) {",
        "if (!id_has_type(words, word_count, words[at + 3u], vector_type)) {",
        "the sampled image's type is checked instead of the coordinate's.",
    ),
    _tx(
        "texel-samples-not-counted",
        "                    samples++;\n",
        "",
        "the capacity and the no sample check never see a sample.",
    ),
    _tx(
        "texel-no-sample",
        "    if (samples == 0u) {\n        return false;\n    }",
        "",
        "a module that loads the sampler and never samples it is rewritten.",
    ),
    _tx(
        "texel-bound-start",
        "    uint32_t bound = words[3];\n    uint32_t *result = malloc((word_count + 13u",
        "    uint32_t bound = words[3] + 1u;\n    uint32_t *result = malloc((word_count + 13u",
        "the new ids start one above the module's bound.",
    ),
    _tx(
        "texel-head-copy",
        "    memcpy(result, words, first_function * sizeof *result);",
        "    memcpy(result, words, (first_function - 1u) * sizeof *result);",
        "the last word before the first function is lost.",
    ),
    _tx(
        "texel-inactive-skip",
        "    for (uint32_t stage = 0u; stage < 4u; stage++) {\n        if (!found[stage].active) {\n            continue;\n        }\n        uint32_t bits[2];",
        "    for (uint32_t stage = 0u; stage < 4u; stage++) {\n        if (found[stage].active) {\n            continue;\n        }\n        uint32_t bits[2];",
        "the stages asked for are skipped and the others get constants.",
    ),
    _tx(
        "texel-width-bits",
        "        memcpy(&bits[0], &size[stage][0], sizeof bits[0]);",
        "        memcpy(&bits[0], &size[stage][1], sizeof bits[0]);",
        "the width constant holds the height.",
    ),
    _tx(
        "texel-height-bits",
        "        memcpy(&bits[1], &size[stage][1], sizeof bits[1]);",
        "        memcpy(&bits[1], &size[stage][0], sizeof bits[1]);",
        "the height constant holds the width.",
    ),
    _tx(
        "texel-width-constant-length",
        *_within(
            _REWRITE_CONSTANTS,
            "(4u << 16) | SPV_OP_CONSTANT;\n        result[used++] = float_type;\n        result[used++] = width;",
            "(3u << 16) | SPV_OP_CONSTANT;\n        result[used++] = float_type;\n        result[used++] = width;",
        ),
        "the width constant claims one word fewer.",
    ),
    _tx(
        "texel-height-constant-length",
        *_within(
            _REWRITE_CONSTANTS,
            "(4u << 16) | SPV_OP_CONSTANT;\n        result[used++] = float_type;\n        result[used++] = height;",
            "(3u << 16) | SPV_OP_CONSTANT;\n        result[used++] = float_type;\n        result[used++] = height;",
        ),
        "the height constant claims one word fewer.",
    ),
    _tx(
        "texel-width-constant-type",
        *_within(
            _REWRITE_CONSTANTS,
            "        result[used++] = float_type;\n        result[used++] = width;",
            "        result[used++] = vector_type;\n        result[used++] = width;",
        ),
        "the width constant is typed as a vec2.",
    ),
    _tx(
        "texel-height-constant-type",
        *_within(
            _REWRITE_CONSTANTS,
            "        result[used++] = float_type;\n        result[used++] = height;",
            "        result[used++] = vector_type;\n        result[used++] = height;",
        ),
        "the height constant is typed as a vec2.",
    ),
    _tx(
        "texel-width-constant-value",
        *_within(
            _REWRITE_CONSTANTS,
            "        result[used++] = width;\n        result[used++] = bits[0];",
            "        result[used++] = width;\n        result[used++] = bits[1];",
        ),
        "the width constant holds the height's bits.",
    ),
    _tx(
        "texel-height-constant-value",
        *_within(
            _REWRITE_CONSTANTS,
            "        result[used++] = height;\n        result[used++] = bits[1];",
            "        result[used++] = height;\n        result[used++] = bits[0];",
        ),
        "the height constant holds the width's bits.",
    ),
    _tx(
        "texel-composite-length",
        *_within(
            _REWRITE_CONSTANTS,
            "(5u << 16) | SPV_OP_CONSTANT_COMPOSITE;",
            "(4u << 16) | SPV_OP_CONSTANT_COMPOSITE;",
        ),
        "the size composite claims one word fewer.",
    ),
    _tx(
        "texel-composite-opcode",
        *_within(
            _REWRITE_CONSTANTS,
            "(5u << 16) | SPV_OP_CONSTANT_COMPOSITE;",
            "(5u << 16) | SPV_OP_CONSTANT;",
        ),
        "the size composite is declared as a scalar constant.",
    ),
    _tx(
        "texel-composite-type",
        *_within(
            _REWRITE_CONSTANTS,
            "        result[used++] = vector_type;\n        result[used++] = found[stage].divisor;",
            "        result[used++] = float_type;\n        result[used++] = found[stage].divisor;",
        ),
        "the size composite is typed as a float.",
    ),
    _tx(
        "texel-composite-order",
        *_within(
            _REWRITE_CONSTANTS,
            "        result[used++] = width;\n        result[used++] = height;\n",
            "        result[used++] = height;\n        result[used++] = width;\n",
        ),
        "the size composite is (height, width).",
    ),
    _tx(
        "texel-rewrite-length",
        "        if (opcode == SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD && length >= 5u) {\n            for",
        "        if (opcode == SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD && length >= 6u) {\n            for",
        "no sample is rewritten.",
    ),
    _tx(
        "texel-rewrite-match-load",
        "if (found[stage].active && words[at + 3u] == found[stage].load) {",
        "if (found[stage].active && words[at + 4u] == found[stage].load) {",
        "the coordinate id is matched against the load.",
    ),
    _tx(
        "texel-divide-length",
        "        result[used++] = (5u << 16) | SPV_OP_FDIV;",
        "        result[used++] = (4u << 16) | SPV_OP_FDIV;",
        "the divide claims one word fewer.",
    ),
    _tx(
        "texel-divide-opcode",
        "#define SPV_OP_FDIV 136u",
        "#define SPV_OP_FDIV 137u",
        "OpUMod is emitted for OpFDiv.",
    ),
    _tx(
        "texel-divide-type",
        "        result[used++] = vector_type;\n        result[used++] = divided;",
        "        result[used++] = float_type;\n        result[used++] = divided;",
        "the divide is typed as a float.",
    ),
    _tx(
        "texel-divide-operands",
        "        result[used++] = words[at + 4u];\n        result[used++] = match->divisor;",
        "        result[used++] = match->divisor;\n        result[used++] = words[at + 4u];",
        "the size is divided by the coordinate.",
    ),
    _tx(
        "texel-divide-feeds-sample",
        "        result[used + 4u] = divided;",
        "        result[used + 3u] = divided;",
        "the divided coordinate replaces the sampled image instead of the coordinate.",
    ),
    _tx(
        "texel-divide-ids",
        "        const uint32_t divided = bound++;",
        "        const uint32_t divided = bound;",
        "every divide reuses the id the bound ends at.",
    ),
    _tx(
        "texel-bound-written",
        "        result[used + 4u] = divided;\n        used += length;\n        at += length;\n    }\n    result[3] = bound;",
        "        result[used + 4u] = divided;\n        used += length;\n        at += length;\n    }\n    result[3] = words[3];",
        "the id bound is left as the module's, below the new ids.",
    ),
    _tx(
        "texel-out-count",
        "        used += length;\n        at += length;\n    }\n    result[3] = bound;\n    *out = result;\n    *out_count = used;",
        "        used += length;\n        at += length;\n    }\n    result[3] = bound;\n    *out = result;\n    *out_count = used - 1u;",
        "the count is one word short.",
    ),
    # ================================================================== replay
    _rp(
        "replay-resolve-guard",
        "        if (backend->resolve_texture != NULL) {",
        "        if (false) {",
        "the provider is never called, so every texture read is refused.",
    ),
    _rp(
        "replay-resolve-draw-index",
        "backend->resolve_texture(backend->texture_context, index, gpu_pgraph_snapshot(pgraph, draw->snapshot), stage,",
        "backend->resolve_texture(backend->texture_context, 0u, gpu_pgraph_snapshot(pgraph, draw->snapshot), stage,",
        "every draw is resolved against the binding at draw 0.",
    ),
    _rp(
        "replay-resolve-stage",
        "                                         &draw_backend.test_textures[stage]);",
        "                                         &draw_backend.test_textures[0]);",
        "every stage's answer lands in stage 0.",
    ),
    _rp(
        "replay-resolve-fragment-backend",
        "result = gpu_pgraph_resolve_fragment(gpu_pgraph_snapshot(pgraph, draw->snapshot), &draw_backend,",
        "result = gpu_pgraph_resolve_fragment(gpu_pgraph_snapshot(pgraph, draw->snapshot), backend,",
        "the combiner is planned against the caller's backend, without the resolved textures.",
    ),
    _rp(
        "replay-composite-backend",
        "result = composite(device, &draw_backend, &program, backend->combiner ? &combiner : NULL,",
        "result = composite(device, backend, &program, backend->combiner ? &combiner : NULL,",
        "the draw is rendered with the caller's backend, without the resolved textures.",
    ),
    _rp(
        "replay-sampled-needs-callback",
        "backend->combiner && backend->texture_sampled != NULL &&",
        "backend->combiner &&",
        "a backend with no sampled callback calls a null pointer.",
    ),
    _rp(
        "replay-sampled-stage-bit",
        "                    if ((combiner.plan.texture_stages >> stage) & 1u) {\n                        backend->texture_sampled(",
        "                    if (!((combiner.plan.texture_stages >> stage) & 1u)) {\n                        backend->texture_sampled(",
        "the stages the combiner does not read are reported as sampled.",
    ),
    _rp(
        "render-linear-flag",
        "                fragment.textures[stage].linear = backend->test_textures[stage].linear;",
        "                fragment.textures[stage].linear = false;",
        "the sampler is nearest although the provider asked for bilinear.",
    ),
    _rp(
        "render-unnormalised-inverted",
        "                if (backend->test_textures[stage].unnormalised) {",
        "                if (!backend->test_textures[stage].unnormalised) {",
        "the coordinate of a normalised texture is divided and of an unnormalised one is not.",
    ),
    _rp(
        "render-texel-size-width",
        "                    texel_size[stage][0] = (float)backend->test_textures[stage].width;",
        "                    texel_size[stage][0] = (float)backend->test_textures[stage].height;",
        "the coordinate is divided by the height in x.",
    ),
    _rp(
        "render-texel-size-height",
        "                    texel_size[stage][1] = (float)backend->test_textures[stage].height;",
        "                    texel_size[stage][1] = (float)backend->test_textures[stage].width;",
        "the coordinate is divided by the width in y.",
    ),
    _rp(
        "render-texel-guard",
        "        if (texel_stages != 0u) {",
        "        if (false) {",
        "the module is never rewritten, so the texel coordinate samples the clamped edge.",
    ),
    _rp(
        "render-texel-words",
        "            fragment.words = texeled;\n",
        "",
        "the rewritten module is built and the original is drawn.",
    ),
    _rp(
        "render-texel-word-count",
        "            fragment.word_count = texeled_count;\n",
        "",
        "the rewritten words are drawn with the original's length.",
    ),
    _row(
        _VSH,
        "sampler-mag-filter",
        "        .magFilter = source->linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,",
        "        .magFilter = VK_FILTER_NEAREST,",
        "a bilinear texture is magnified nearest.",
        _SWAP_TEST,
    ),
    _row(
        _VSH,
        "sampler-mag-inverted",
        "        .magFilter = source->linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,",
        "        .magFilter = source->linear ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,",
        "the filter is the opposite of the flag.",
        _SWAP_TEST,
    ),
    # ================================================================== hostopt
    _ho(
        "hostopt-default-on",
        "    out->gpu_replay_rt_texture = false;",
        "    out->gpu_replay_rt_texture = true;",
        "the render target texture is on in every parse.",
    ),
    _ho(
        "hostopt-census-default-on",
        "    out->gpu_replay_rt_texture_census = false;",
        "    out->gpu_replay_rt_texture_census = true;",
        "the census is on in every parse.",
    ),
    _ho(
        "hostopt-flag-sets-nothing",
        '"--gpu-replay-rt-texture") == 0) {\n            out->gpu_replay_rt_texture = true;',
        '"--gpu-replay-rt-texture") == 0) {\n            out->gpu_replay_rt_texture = false;',
        "--gpu-replay-rt-texture is accepted and does nothing.",
    ),
    _ho(
        "hostopt-census-not-implied",
        '"--gpu-replay-rt-texture-census") == 0) {\n            out->gpu_replay_rt_texture = true;',
        '"--gpu-replay-rt-texture-census") == 0) {\n            out->gpu_replay_rt_texture = false;',
        "the census flag alone does not imply the option, so it is refused or does nothing.",
    ),
    _ho(
        "hostopt-census-flag-sets-nothing",
        "            out->gpu_replay_rt_texture_census = true;",
        "            out->gpu_replay_rt_texture_census = false;",
        "--gpu-replay-rt-texture-census replays pixels.",
    ),
    _ho(
        "hostopt-requirement-always",
        *_within(_REQUIREMENT, "(!out->gpu_replay_rt_texture ||", "(false ||"),
        "every parse demands the combiner, the TEXTURE group and a directory, with or without the option.",
    ),
    _ho(
        "hostopt-requirement-never",
        *_within(_REQUIREMENT, "(!out->gpu_replay_rt_texture ||", "(true ||"),
        "the option is accepted without any of its requirements.",
    ),
    _ho(
        "hostopt-needs-combiner",
        *_within(_REQUIREMENT, "(out->gpu_replay_combiner && ", "(true && "),
        "the option is accepted without --gpu-replay-combiner.",
    ),
    _ho(
        "hostopt-needs-output-state",
        *_within(_REQUIREMENT, "out->gpu_replay_output_state && ", "true && "),
        "the option is accepted without --gpu-replay-output-state, so the TEXTURE group words are never decoded.",
    ),
    _ho(
        "hostopt-standin-exclusive",
        *_within(_REQUIREMENT, "!out->gpu_replay_standin && ", "true && "),
        "the option is accepted with --gpu-replay-standin-texture.",
    ),
    _ho(
        "hostopt-flip-y-exclusive",
        *_within(_REQUIREMENT, "!out->gpu_replay_flip_y &&", "true &&"),
        "the option is accepted with --gpu-replay-flip-y.",
    ),
]

# Filled from a run with the Vulkan loader hidden (58 of 310 exit 77 there, see the header).
_NEEDS_DEVICE: frozenset[str] = frozenset(
    {
        "gpu-rt-texture-binding-data-mask-lost",
        "gpu-rt-texture-address-message-written",
        "gpu-rt-texture-filter-message-written",
        "gpu-rt-texture-address-register-text",
        "gpu-rt-texture-filter-register-text",
        "gpu-rt-texture-resolve-rgba-first-pass",
        "gpu-rt-texture-resolve-width-height-swapped",
        "gpu-rt-texture-resolve-not-linear",
        "gpu-rt-texture-resolve-normalised",
        "gpu-rt-texture-resolve-source-size-swapped",
        "gpu-rt-texture-resolve-source-target-header",
        "gpu-rt-texture-resolve-source-text",
        "gpu-rt-texture-tally-line-text",
        "gpu-rt-texture-tally-no-dedupe",
        "gpu-rt-texture-tally-dedupe-first-only",
        "gpu-rt-texture-tally-no-increment",
        "gpu-rt-texture-tally-full-early",
        "gpu-rt-texture-tally-full-never",
        "gpu-rt-texture-tally-first-draws",
        "gpu-rt-texture-sampled-flag-lost",
        "gpu-rt-texture-sampled-not-counted",
        "gpu-rt-texture-sampled-not-tallied",
        "gpu-rt-texture-census-at-bound",
        "gpu-rt-texture-summary-census-branch",
        "gpu-rt-texture-summary-census-only-text",
        "gpu-rt-texture-summary-census-counts-swapped",
        "gpu-rt-texture-summary-census-bindings",
        "gpu-rt-texture-summary-never-white",
        "gpu-rt-texture-summary-inferred",
        "gpu-rt-texture-summary-counts-swapped",
        "gpu-rt-texture-summary-draws",
        "gpu-rt-texture-segment-bound-size",
        "gpu-rt-texture-present-census-stream-kept",
        "gpu-rt-texture-present-census-empty-not-counted",
        "gpu-rt-texture-replay-pass-wiring",
        "gpu-rt-texture-replay-pass-sampled-callback",
        "gpu-rt-texture-replay-pass-passes",
        "gpu-rt-texture-replay-pass-current",
        "gpu-rt-texture-replay-pass-current-late",
        "gpu-rt-texture-replay-pass-bridge-dropped",
        "gpu-rt-texture-replay-pass-bridge-always",
        "gpu-rt-texture-replay-pass-bridge-inverted",
        "gpu-rt-texture-replay-resolve-guard",
        "gpu-rt-texture-replay-resolve-draw-index",
        "gpu-rt-texture-replay-resolve-stage",
        "gpu-rt-texture-replay-resolve-fragment-backend",
        "gpu-rt-texture-replay-composite-backend",
        "gpu-rt-texture-replay-sampled-needs-callback",
        "gpu-rt-texture-replay-sampled-stage-bit",
        "gpu-rt-texture-render-linear-flag",
        "gpu-rt-texture-render-unnormalised-inverted",
        "gpu-rt-texture-render-texel-size-width",
        "gpu-rt-texture-render-texel-size-height",
        "gpu-rt-texture-render-texel-guard",
        "gpu-rt-texture-render-texel-words",
        "gpu-rt-texture-render-texel-word-count",
        "gpu-rt-texture-sampler-mag-filter",
        "gpu-rt-texture-sampler-mag-inverted",
    }
)

for _mutation in MUTATIONS:
    _mutation["needs_device"] = _mutation["id"] in _NEEDS_DEVICE
