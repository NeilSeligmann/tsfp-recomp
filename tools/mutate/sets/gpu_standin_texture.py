# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the swap replay's stand-in texture option (T497).

T497 gave `d3d8_swap_replay` ONE stand-in texture for combiner texture stage 0 (`standin_texture`, `standin_width`,
`standin_height`, `standin_rgba`, host flag `--gpu-replay-standin-texture WxH:RRGGBBAA`), wired to
`gpu_pgraph_backend.test_textures[0]`, allowing exactly TEXTURE_SAMPLING, counting the draws that sampled it and
saying in so many words that it is NOT the title's texture. Grouped by what a survivor would let through:

    config     enable refusing the option without the combiner and a size outside 1..4096, the edges accepted
    mask       the inferences the host option allows (exactly TEXTURE_SAMPLING, never STAGE_PROGRAM, only when asked)
    texture    the texels allocated, filled with the colour, and handed to backend.test_textures[0] (stage 0, rgba,
               width, height)
    summary    the line that says it is NOT the title's texture, empty when the option is off, the draw counts
    count      `textured_draws` of the replay report and its fold into the stats
    hostopt    `--gpu-replay-standin-texture`: default off, the value's parse (edges 1..4096, exactly eight hex
               digits, either case, nothing before or after), width, height and colour not swapped, refused without
               `--gpu-replay-combiner`
    census     the frame profile's second census with a stand-in texel (src/gpu/d3d8_frame_profile.c)

The kills are by `test_d3d8_swap_replay` (no device for the configuration, the mask and the summary text, a device for
everything that is drawn), `test_host_options` and `test_d3d8_frame_profile`. DEVICE-DEPENDENT kills (exit 77 without a
Vulkan device) are reported SKIPPED by the harness, never killed: `_NEEDS_DEVICE` at the bottom records which ones, from a
run with the Vulkan loader hidden. T558 moves the two texel-fill faults to the actual
pattern writer; its focused pytest target kills them without a device, while the
swap binary remains a real compiler/fingerprint guard.

NOT MUTATED, and why: the `main.c` wiring (`replay.standin_*`, the fourth argument of
`d3d8_swap_replay_host_inferences`, the announcement and the end-of-run summary line) because `tsfp_host` is not a ctest
binary. It is covered by the real boot (`tools/steady_replay.py`, `tests/test_steady_replay.py`) and was mutated by hand, see
docs/tasks.md T497. EQUIVALENT: the `if (state.config.standin_texture)` guard around the `backend.test_textures[0]` wiring
(without the option `standin_pixels` is NULL, which the planner reads as "no test texture", the same refusal), the five
digit limit of the edge parse (a sixth digit is over 4096 anyway), the `digits == 0u` test of the same loop (an empty edge is
0, below 1) and the frame profile's `census_standin` NULL-state guard (the caller checked it).

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

_SWAP = "src/gpu/d3d8_swap_replay.c"
_SWAP_H = "src/gpu/d3d8_swap_replay.h"
_REPLAY = "src/gpu/gpu_pgraph_replay.c"
_OPTIONS = "src/host/host_options.c"
_PROFILE = "src/gpu/d3d8_frame_profile.c"
_PATTERN = "src/gpu/gpu_standin_pattern.h"

_SWAP_TEST = ["test_d3d8_swap_replay"]
_OPTION_TEST = ["test_host_options"]
_PROFILE_TEST = ["test_d3d8_frame_profile"]


def _row(file: str, mutation_id: str, old: str, new: str, why: str, targets: list[str]) -> dict:
    return {
        "id": f"gpu-standin-texture-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(targets),
        "why": why,
    }


def _sw(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_SWAP, mutation_id, old, new, why, _SWAP_TEST)


def _ho(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_OPTIONS, mutation_id, old, new, why, _OPTION_TEST)


def _pr(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_PROFILE, mutation_id, old, new, why, _PROFILE_TEST)


MUTATIONS: list[dict] = [
    # ================================================================== config
    _sw(
        "config-needs-combiner",
        "    if (config->standin_texture && !config->combiner) {",
        "    if (config->standin_texture && false) {",
        "a stand-in with no combiner is accepted: nothing reads it and the run says nothing.",
    ),
    _sw(
        "config-width-zero-accepted",
        "(config->standin_width < 1u || config->standin_width > D3D8_SWAP_REPLAY_STANDIN_MAX_EDGE ||",
        "(config->standin_width < 0u || config->standin_width > D3D8_SWAP_REPLAY_STANDIN_MAX_EDGE ||",
        "a zero width passes enable and fails later, in the planner, as a draw refusal.",
    ),
    _sw(
        "config-width-over-accepted",
        "(config->standin_width < 1u || config->standin_width > D3D8_SWAP_REPLAY_STANDIN_MAX_EDGE ||",
        "(config->standin_width < 1u || config->standin_width > D3D8_SWAP_REPLAY_STANDIN_MAX_EDGE + 1u ||",
        "a 4097 wide stand-in is accepted, then refused by the planner.",
    ),
    _sw(
        "config-height-zero-accepted",
        "         config->standin_height < 1u || config->standin_height > D3D8_SWAP_REPLAY_STANDIN_MAX_EDGE)) {",
        "         config->standin_height < 0u || config->standin_height > D3D8_SWAP_REPLAY_STANDIN_MAX_EDGE)) {",
        "a zero height passes enable.",
    ),
    _sw(
        "config-height-over-accepted",
        "         config->standin_height < 1u || config->standin_height > D3D8_SWAP_REPLAY_STANDIN_MAX_EDGE)) {",
        "         config->standin_height < 1u || config->standin_height > D3D8_SWAP_REPLAY_STANDIN_MAX_EDGE + 1u)) {",
        "a 4097 high stand-in is accepted.",
    ),
    _row(
        _SWAP_H,
        "config-max-edge",
        "#define D3D8_SWAP_REPLAY_STANDIN_MAX_EDGE 4096u",
        "#define D3D8_SWAP_REPLAY_STANDIN_MAX_EDGE 4095u",
        "the largest accepted edge is one short of what the planner takes.",
        _SWAP_TEST,
    ),
    # ================================================================== mask
    _sw(
        "mask-host-drops-standin",
        "        allowed |= D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE;",
        "        allowed |= 0u;",
        "`--gpu-replay-standin-texture` allows no inference: every draw that reads t0 is refused for TEXTURE_SAMPLING, and "
        "the main.c wiring that no ctest binary reaches cannot tell.",
    ),
    _sw(
        "mask-host-standin-always",
        "    if (standin_texture) {\n        allowed |= D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE;",
        "    (void)standin_texture;\n    if (true) {\n        allowed |= D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE;",
        "every replay allows the texture sampling inference, so the guess is made without anyone opting in.",
    ),
    _row(
        _SWAP_H,
        "mask-adds-stage-program",
        "#define D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING",
        "#define D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE (GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING | GPU_PGRAPH_INFER_COMBINER_STAGE_PROGRAM)",
        "the stand-in also stands in for a texture stage program the stream never wrote, which the title's stream does not need.",
        _SWAP_TEST,
    ),
    _row(
        _SWAP_H,
        "mask-without-sampling",
        "#define D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING",
        "#define D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE 0u",
        "the option allows nothing, so no stand-in draw is ever planned.",
        _SWAP_TEST,
    ),
    # ================================================================== texture
    _sw(
        "texture-not-allocated",
        "    if (ok && config->standin_texture) {\n        const size_t texels",
        "    if (ok && !config->standin_texture) {\n        const size_t texels",
        "the texels are allocated for the replay that does not want them and not for the one that does.",
    ),
    _row(
        _PATTERN,
        "texture-fill-first-texel-only",
        "memcpy(pixels + ((size_t)y * width + x) * 4u, colour, 4u);",
        "memcpy(pixels, colour, 4u);",
        "only the first texel holds the colour, the rest is uninitialised memory sampled wherever oT0 points.",
        ["test_d3d8_swap_replay", "pytest:tests/test_gpu_standin_pattern.py"],
    ),
    _row(
        _PATTERN,
        "texture-fill-wrong-order",
        "memcpy(pixels + ((size_t)y * width + x) * 4u, colour, 4u);",
        "memcpy(pixels + ((size_t)y * width + x) * 4u, colour, 3u);",
        "the alpha texel is not copied (it stays whatever malloc returned).",
        ["test_d3d8_swap_replay", "pytest:tests/test_gpu_standin_pattern.py"],
    ),
    _sw(
        "texture-wired-to-nothing",
        "        backend.test_textures[0].rgba = state.standin_pixels;",
        "        backend.test_textures[0].rgba = NULL;",
        "the stand-in is configured and never handed to the backend: every draw that reads t0 is still refused.",
    ),
    _sw(
        "texture-wired-to-stage-one",
        "        backend.test_textures[0].rgba = state.standin_pixels;",
        "        backend.test_textures[1].rgba = state.standin_pixels;",
        "the stand-in feeds texture stage 1: a read of t0 finds none.",
    ),
    _sw(
        "texture-width-not-passed",
        "        backend.test_textures[0].width = state.config.standin_width;",
        "        backend.test_textures[0].width = 0u;",
        "the planner is told a zero width and refuses the texture.",
    ),
    _sw(
        "texture-height-not-passed",
        "        backend.test_textures[0].height = state.config.standin_height;",
        "        backend.test_textures[0].height = 0u;",
        "the planner is told a zero height and refuses the texture.",
    ),
    # ================================================================== summary
    _sw(
        "summary-shown-when-off",
        "    if (!state.active || !state.config.standin_texture) {",
        "    if (!state.active) {",
        "a replay without the stand-in prints a stand-in summary naming a texture that was never given.",
    ),
    _sw(
        "summary-not-the-titles-texture",
        "\"replayed draw(s). It is NOT the title's texture (the title's textures are not decoded, T480): a frame \"",
        "\"replayed draw(s). It is the title's texture (the title's textures are not decoded, T480): a frame \"",
        "the summary calls the stand-in the title's texture.",
    ),
    _sw(
        "summary-counts-swapped",
        "pattern, (unsigned long long)state.stats.standin_draws, (unsigned long long)state.stats.draws,",
        "pattern, (unsigned long long)state.stats.draws, (unsigned long long)state.stats.standin_draws,",
        "the draws that sampled the stand-in and all draws trade places.",
    ),
    _sw(
        "summary-names-no-inference",
        '"(TEXTURE_SAMPLING). The stage program is the stream\'s own (word 54) and the coordinate is what each vertex "',
        '"(sampling). The stage program is the stream\'s own (word 54) and the coordinate is what each vertex "',
        "the inference the stand-in rests on is no longer named in the summary.",
    ),
    # ================================================================== count
    _sw(
        "count-folds-all-draws",
        "        state.stats.standin_draws += passes[i].textured_draws;",
        "        state.stats.standin_draws += passes[i].drawn;",
        "every drawn draw is counted as having sampled the stand-in.",
    ),
    _sw(
        "count-not-folded",
        "        state.stats.standin_draws += passes[i].textured_draws;",
        "        state.stats.standin_draws += 0u;",
        "the stand-in draw count stays 0 whatever was sampled.",
    ),
    _sw(
        "count-pass-forgets",
        "    pass->textured_draws = report.textured_draws;",
        "    pass->textured_draws = 0u;",
        "a pass does not carry the replay's textured draw count.",
    ),
    _row(
        _REPLAY,
        "count-report-always",
        "                report->textured_draws += backend->combiner && combiner.plan.texture_stages != 0u;",
        "                report->textured_draws += 1u;",
        "every drawn draw counts as textured, the pass-through ones included.",
        _SWAP_TEST,
    ),
    _row(
        _REPLAY,
        "count-report-inverted",
        "                report->textured_draws += backend->combiner && combiner.plan.texture_stages != 0u;",
        "                report->textured_draws += backend->combiner && combiner.plan.texture_stages == 0u;",
        "the draws that did NOT read a texture are the ones counted.",
        _SWAP_TEST,
    ),
    # ================================================================== hostopt
    _ho(
        "hostopt-default-on",
        "    out->gpu_replay_standin = false;",
        "    out->gpu_replay_standin = true;",
        "the stand-in texture is on in every parse, so every combiner draw samples a texture nobody gave.",
    ),
    _ho(
        "hostopt-width-default",
        "    out->gpu_replay_standin_width = 0u;",
        "    out->gpu_replay_standin_width = 1u;",
        "an unset stand-in has a width.",
    ),
    _ho(
        "hostopt-height-default",
        "    out->gpu_replay_standin_height = 0u;",
        "    out->gpu_replay_standin_height = 1u;",
        "an unset stand-in has a height.",
    ),
    _ho(
        "hostopt-colour-default",
        "    memset(out->gpu_replay_standin_rgba, 0, sizeof out->gpu_replay_standin_rgba);",
        "    memset(out->gpu_replay_standin_rgba, 0xFF, sizeof out->gpu_replay_standin_rgba);",
        "an unset stand-in has a colour (white).",
    ),
    _ho(
        "hostopt-flag-sets-nothing",
        "    out->gpu_replay_standin = true;\n",
        "    out->gpu_replay_standin = false;\n",
        "--gpu-replay-standin-texture is accepted and does nothing.",
    ),
    _ho(
        "hostopt-width-from-height",
        "    out->gpu_replay_standin_width = edges[0];",
        "    out->gpu_replay_standin_width = edges[1];",
        "2x3 is read as a 3 wide texture.",
    ),
    _ho(
        "hostopt-height-from-width",
        "    out->gpu_replay_standin_height = edges[1];",
        "    out->gpu_replay_standin_height = edges[0];",
        "2x3 is read as a 2 high texture.",
    ),
    _ho(
        "hostopt-colour-reversed",
        "        rgba[byte] = (uint8_t)value;",
        "        rgba[3u - byte] = (uint8_t)value;",
        "RRGGBBAA is read as AABBGGRR.",
    ),
    _ho(
        "hostopt-colour-not-copied",
        "    memcpy(out->gpu_replay_standin_rgba, rgba, sizeof rgba);",
        "    memcpy(out->gpu_replay_standin_rgba, rgba, 3u);",
        "the alpha byte is dropped.",
    ),
    _ho(
        "hostopt-hex-base",
        "            value = value * 16u + part;",
        "            value = value * 10u + part;",
        "the digits are read in the wrong base.",
    ),
    _ho(
        "hostopt-uppercase-refused",
        "                                  : digit >= 'A' && digit <= 'F' ? (unsigned)(digit - 'A') + 10u",
        "                                  : false ? (unsigned)(digit - 'A') + 10u",
        "uppercase hex digits are refused.",
    ),
    _ho(
        "hostopt-lowercase-refused",
        "                                  : digit >= 'a' && digit <= 'f' ? (unsigned)(digit - 'a') + 10u",
        "                                  : false ? (unsigned)(digit - 'a') + 10u",
        "lowercase hex digits are refused (or read as 16, the invalid marker).",
    ),
    _ho(
        "hostopt-zero-edge-accepted",
        "        if (digits == 0u || value < 1u || value > (unsigned long)GPU_REPLAY_MAX_EDGE ||",
        "        if (digits == 0u || value < 0u || value > (unsigned long)GPU_REPLAY_MAX_EDGE ||",
        "a zero edge is accepted and the replay refuses it later.",
    ),
    _ho(
        "hostopt-oversize-edge-accepted",
        "        if (digits == 0u || value < 1u || value > (unsigned long)GPU_REPLAY_MAX_EDGE ||",
        "        if (digits == 0u || value < 1u || value > (unsigned long)GPU_REPLAY_MAX_EDGE + 1u ||",
        "4097 is accepted as an edge.",
    ),
    _ho(
        "hostopt-edge-limit-off-by-one",
        "        if (digits == 0u || value < 1u || value > (unsigned long)GPU_REPLAY_MAX_EDGE ||",
        "        if (digits == 0u || value < 1u || value >= (unsigned long)GPU_REPLAY_MAX_EDGE ||",
        "4096, the largest edge, is refused.",
    ),
    _ho(
        "hostopt-separator-not-checked",
        "            text[digits] != (edge == 0u ? 'x' : ':')) {",
        "            false) {",
        "`1:FFFFFFFF` or `1x1FFFFFFFF` is read as a texture.",
    ),
    _ho(
        "hostopt-separators-swapped",
        "            text[digits] != (edge == 0u ? 'x' : ':')) {",
        "            text[digits] != (edge == 0u ? ':' : 'x')) {",
        "the value is spelled WxH:RRGGBBAA only with the separators swapped.",
    ),
    _ho(
        "hostopt-trailing-text-accepted",
        "    if (!checker && text[8] != '\\0') {",
        "    if (false) {",
        "`1x1:FFFFFFFFF` (nine digits) or `1x1:FFFFFFFF:` is read as the first eight digits.",
    ),
    _ho(
        "hostopt-malformed-accepted",
        "            if (++i >= argc || !parse_standin(argv[i], out)) {",
        "            if (++i >= argc || (!parse_standin(argv[i], out) && false)) {",
        "the parse result is dropped: a malformed value is accepted and a good one is never read.",
    ),
    _ho(
        "hostopt-value-optional",
        "            if (++i >= argc || !parse_standin(argv[i], out)) {",
        "            if (++i >= argc ? false : !parse_standin(argv[i], out)) {",
        "a flag with no value is accepted (the value is read past the end of argv otherwise, or skipped).",
    ),
    _ho(
        "hostopt-without-combiner-accepted",
        "        (!out->gpu_replay_standin || out->gpu_replay_combiner) &&",
        "        true &&",
        "--gpu-replay-standin-texture without --gpu-replay-combiner is accepted and silently does nothing.",
    ),
    # ================================================================== census
    _pr(
        "census-standin-not-called",
        "    census_standin(frame, draw, state);\n",
        "    (void)census_standin;\n",
        "the stand-in census never examines a draw.",
    ),
    _pr(
        "census-standin-no-texture",
        "const gpu_combiner_texture standin[GPU_COMBINER_TEXTURE_STAGES] = {{texel, 1u, 1u, false, false, NULL, false, false}};",
        "const gpu_combiner_texture standin[GPU_COMBINER_TEXTURE_STAGES] = {{texel, 0u, 0u, false, false, NULL, false, false}};",
        "the stand-in census is given an empty texture, so it plans nothing (the planner refuses a size outside 1..4096).",
    ),
    _pr(
        "census-standin-allows-no-inference",
        "GPU_COMBINER_INFER_ALL, standin, &plan,",
        "0u, standin, &plan,",
        "every plan that needs an inference is reported refused, which hides what the stand-in would let through.",
    ),
    _pr(
        "census-standin-planned-counted-refused",
        "        census->planned++;\n",
        "        census->refused++;\n",
        "plans are tallied as refusals.",
    ),
    _pr(
        "census-standin-merges-everything",
        "strcmp(label, outcome->text) == 0",
        "1",
        "every outcome is merged into the first, so the distinct reasons and modules are lost.",
    ),
    _pr(
        "census-standin-first-frame",
        "outcome->first_frame = frame_number;",
        "outcome->first_frame = frame_number - frame_number;",
        "the first frame of an outcome is not recorded.",
    ),
    _pr(
        "census-standin-first-draw",
        "outcome->first_draw = (uint32_t)draw_index;",
        "outcome->first_draw = (uint32_t)draw_index - (uint32_t)draw_index;",
        "the draw index of an outcome is not recorded.",
    ),
    _pr(
        "census-standin-words-not-kept",
        "        memcpy(outcome->words, state->combiner, sizeof(outcome->words));\n",
        "        (void)state;\n        memset(outcome->words, 0, sizeof(outcome->words));\n",
        "the planned outcome's definition is not kept, so the module the loop needs cannot be made from the report.",
    ),
    _pr(
        "census-standin-definition-not-printed",
        "        if (!outcome->planned) {\n            continue;\n        }\n        char name[GPU_COMBINER_NAME_BYTES];",
        "        if (outcome->planned) {\n            continue;\n        }\n        char name[GPU_COMBINER_NAME_BYTES];",
        "the definitions of the refused outcomes are printed instead of the planned ones.",
    ),
]

_NEEDS_DEVICE: frozenset[str] = frozenset(
    f"gpu-standin-texture-{name}"
    for name in (
        "texture-not-allocated",
        "texture-wired-to-nothing",
        "texture-wired-to-stage-one",
        "texture-width-not-passed",
        "texture-height-not-passed",
        "summary-counts-swapped",
        "count-folds-all-draws",
        "count-not-folded",
        "count-pass-forgets",
        "count-report-always",
        "count-report-inverted",
    )
)  # from a run with the Vulkan loader hidden (11 device-only of the 56 after the two writer targets move), see the header

for _mutation in MUTATIONS:
    _mutation["needs_device"] = _mutation["id"] in _NEEDS_DEVICE
