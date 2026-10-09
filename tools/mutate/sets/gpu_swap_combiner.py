# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the swap replay's combiner option and dump cadence (T478, T484).

T478 gave `d3d8_swap_replay` a `combiner` option: the model decodes the 57 combiner words, the directory's
`combiner_<sha256>.spv` modules become a fragment table beside the vertex one, and the backend replaces the
fixed fragment stage. T484 gave it `dump_every` and `dump_last`. Grouped by what a survivor would let through:

    modules    the directory split (a combiner module offered as a vertex program or the reverse), the
               refusal of the option with no combiner module, the loaders reading the wrong set
    optin      the model decoding the words only with the option, the backend replaying them only with it,
               the table and loader the backend is handed
    mask       the inferences the host option allows (exactly CONSTANT_BYTES and COLOUR_RANGE)
    cadence    which replayed frames are written, the last frame at the end, written once, and the
               offscreen passes following the frame's own decision
    hostopt    `--gpu-replay-combiner`, `--gpu-replay-dump-every N`, `--gpu-replay-dump-last`: defaults, what each
               flag sets, the count's validation and the refusals without the option each one refines
    census     the frame profile's per-draw combiner census (src/gpu/d3d8_frame_profile.c)

The kills are by `test_d3d8_swap_replay` (no device for the module split, the decode and the configuration,
a device for everything that is replayed or dumped), `test_host_options` and `test_d3d8_frame_profile`.
DEVICE-DEPENDENT kills (exit 77 without a Vulkan device) are reported SKIPPED by the harness, never killed:
`_NEEDS_DEVICE` at the bottom records which ones, from a run with the Vulkan loader hidden.

NOT MUTATED, and why: the `main.c` wiring (`replay.combiner`, `replay.allowed_inferences |=`, the
`dump_every` and `dump_last` hand-over, the `d3d8_swap_replay_dump_last` call at exit) because `tsfp_host` is
not a ctest binary. It is covered by the real boot (`tests/test_steady_replay.py`, `tools/steady_replay.py`) and
was mutated by hand, see docs/tasks.md T478. The frame profile's `first_draw` is killed by the second draw of
frame 22 in `test_d3d8_frame_profile`. EQUIVALENT: `dump_due`'s `dump_directory == NULL` guard (`dump_image`
checks it too), a census merge that ignores the planned flag (a plan and a refusal never share text), and the
`--gpu-replay-dump-every` / `--gpu-replay-dump-last` terms of the "refused without --gpu-replay" clause of
`parse_options` (a cadence needs a dump directory, which needs --gpu-replay, so the first clause never sees one
that the second has not refused: the terms were removed from the source rather than kept dead).

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

_SWAP = "src/gpu/d3d8_swap_replay.c"
_OPTIONS = "src/host/host_options.c"
_PROFILE = "src/gpu/d3d8_frame_profile.c"
_REPLAY_H = "src/gpu/gpu_pgraph_replay.h"

_SWAP_TEST = ["test_d3d8_swap_replay"]
_OPTION_TEST = ["test_host_options"]
_PROFILE_TEST = ["test_d3d8_frame_profile"]


def _row(file: str, mutation_id: str, old: str, new: str, why: str, targets: list[str]) -> dict:
    return {
        "id": f"gpu-swap-combiner-{mutation_id}",
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
    # ================================================================== modules
    _sw(
        "modules-combiner-flag-not-passed",
        "list_modules(state.spv_directory, config->combiner, error, error_size)",
        "list_modules(state.spv_directory, false, error, error_size)",
        "enable no longer refuses the option over a directory with no combiner module: the replay then "
        "starts and every draw is refused much later, one at a time, instead of the loud refusal at enable.",
    ),
    _sw(
        "modules-prefix-inverted",
        "strncmp(stem, COMBINER_MODULE_PREFIX, strlen(COMBINER_MODULE_PREFIX)) == 0",
        "strncmp(stem, COMBINER_MODULE_PREFIX, strlen(COMBINER_MODULE_PREFIX)) != 0",
        "the vertex programs are taken for fragment stages and the combiner modules for vertex programs.",
    ),
    _sw(
        "modules-count-check",
        "if (combiner && fragment_count == 0u) {",
        "if (combiner && fragment_count == 99u) {",
        "the refusal of a combiner option with no combiner module never fires.",
    ),
    _sw(
        "modules-fragment-loader-reads-vertex-set",
        "    return load_module_words(&state.fragment, module, words, word_count);",
        "    return load_module_words(&state.vertex, module, words, word_count);",
        "a combiner module index is resolved in the vertex set: the wrong SPIR-V (or none) reaches the draw.",
    ),
    _sw(
        "modules-vertex-loader-reads-fragment-set",
        "    return load_module_words(&state.vertex, module, words, word_count);",
        "    return load_module_words(&state.fragment, module, words, word_count);",
        "a vertex program index is resolved in the combiner set.",
    ),
    _sw(
        "modules-backend-vertex-table",
        "backend.table = &state.vertex.table;",
        "backend.table = &state.fragment.table;",
        "the vertex selector is built from the combiner modules.",
    ),
    # ================================================================== optin
    _sw(
        "optin-model-ignores-option",
        "gpu_pgraph_set_combiner(state.pgraph, state.config.combiner);",
        "gpu_pgraph_set_combiner(state.pgraph, false);",
        "the model never decodes the combiner words (also after a CreateDevice reset): they stay unhandled "
        "and strict mode refuses the first one, so the option does nothing.",
    ),
    _sw(
        "optin-model-always-on",
        "gpu_pgraph_set_combiner(state.pgraph, state.config.combiner);",
        "gpu_pgraph_set_combiner(state.pgraph, true);",
        "the words are decoded with the option OFF: strict mode stops refusing a stream whose combiner the "
        "replay then does not apply, and the default is no longer the old behaviour.",
    ),
    _sw(
        "optin-backend-flag",
        "backend.combiner = state.config.combiner;",
        "backend.combiner = false;",
        "the words are decoded but the fixed fragment stage still draws: the combiner has no effect on a pixel.",
    ),
    _sw(
        "optin-backend-always-on",
        "backend.combiner = state.config.combiner;",
        "backend.combiner = true;",
        "the combiner replaces the fixed stage with the option OFF: every default replay changes (or is refused).",
    ),
    _sw(
        "optin-backend-fragment-table",
        "backend.fragment_table = &state.fragment.table;",
        "backend.fragment_table = &state.vertex.table;",
        "the fragment module is looked up among the vertex programs: every combiner draw is refused by name.",
    ),
    _sw(
        "optin-backend-fragment-loader",
        "backend.load_fragment_module = load_fragment_module;",
        "backend.load_fragment_module = load_module; (void)load_fragment_module;",
        "the fragment module's words come from the vertex loader.",
    ),
    # ================================================================== mask
    _row(
        _REPLAY_H,
        "mask-adds-unwritten",
        "#define GPU_PGRAPH_INFER_COMBINER_REPLAY (GPU_COMBINER_INFER_CONSTANT_BYTES | GPU_COMBINER_INFER_COLOUR_RANGE)",
        "#define GPU_PGRAPH_INFER_COMBINER_REPLAY (GPU_COMBINER_INFER_CONSTANT_BYTES | GPU_COMBINER_INFER_COLOUR_RANGE | GPU_COMBINER_INFER_UNWRITTEN)",
        "the host option invents a combiner word the stream never wrote (reads as 0) without anyone saying so.",
        _SWAP_TEST,
    ),
    _row(
        _REPLAY_H,
        "mask-without-colour-range",
        "#define GPU_PGRAPH_INFER_COMBINER_REPLAY (GPU_COMBINER_INFER_CONSTANT_BYTES | GPU_COMBINER_INFER_COLOUR_RANGE)",
        "#define GPU_PGRAPH_INFER_COMBINER_REPLAY (GPU_COMBINER_INFER_CONSTANT_BYTES)",
        "every configuration reading oD0 is refused, which includes both draws of the title's loop that plan.",
        _SWAP_TEST,
    ),
    _sw(
        "mask-host-drops-combiner",
        "        allowed |= GPU_PGRAPH_INFER_COMBINER_REPLAY;",
        "        allowed |= 0u;",
        "`--gpu-replay-combiner` allows no inference: every combiner draw is refused, and the main.c wiring that "
        "no ctest binary reaches cannot tell.",
    ),
    _sw(
        "mask-host-combiner-always",
        "    if (combiner) {\n        allowed |= GPU_PGRAPH_INFER_COMBINER_REPLAY;",
        "    (void)combiner;\n    if (true) {\n        allowed |= GPU_PGRAPH_INFER_COMBINER_REPLAY;",
        "every replay allows the combiner inferences, so the guess is made without anyone opting in.",
    ),
    _sw(
        "mask-host-drops-program-mode",
        "        allowed |= GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN;",
        "        allowed |= 0u;",
        "`--gpu-replay-assume-program-mode` allows nothing (T441's wiring, now testable).",
    ),
    _sw(
        "mask-host-drops-output-state",
        "        allowed |= GPU_PGRAPH_INFER_OUTPUT_ALL;",
        "        allowed |= 0u;",
        "`--gpu-replay-output-state` allows none of the output inferences.",
    ),
    _sw(
        "mask-host-base-is-nothing",
        "    uint32_t allowed = GPU_PGRAPH_INFER_ALL | GPU_PGRAPH_INFER_CMP_PACKED; /* T1204 */",
        "    uint32_t allowed = 0u;",
        "the host stops allowing the base inferences (program header, viewport constants, component defaults).",
    ),
    # ================================================================== cadence
    _sw(
        "cadence-every-inverted",
        "return frame % state.config.dump_every == 0u;",
        "return frame % state.config.dump_every != 0u;",
        "the frames that are NOT multiples of N are written: the cadence keeps what it should skip.",
    ),
    _sw(
        "cadence-every-ignored",
        "if (state.config.dump_every != 0u) {",
        "if (false) {",
        "dump_every does nothing: every frame is written, the 7 GB a 6000 Swap replay writes.",
    ),
    _sw(
        "cadence-last-alone-writes-all",
        "return !state.config.dump_last;",
        "return true;",
        "dump_last alone also writes every frame as it is replayed, so it saves nothing.",
    ),
    _sw(
        "cadence-default-writes-nothing",
        "return !state.config.dump_last;",
        "return false;",
        "the default (no cadence option) writes no frame: the behaviour before T484 is lost.",
    ),
    _sw(
        "cadence-last-frame-number",
        "state.last_frame = frame_number;",
        "state.last_frame = 0u;",
        "the last frame is written under another number (frame_000000.png).",
    ),
    _sw(
        "cadence-last-flag-ignored",
        "if (!state.active || state.dump_directory == NULL || !state.config.dump_last) {",
        "if (!state.active || state.dump_directory == NULL) {",
        "the end-of-run flush writes the last frame without dump_last.",
    ),
    _sw(
        "cadence-last-dumped-forgotten",
        "state.last_dumped = due;",
        "state.last_dumped = false;",
        "a last frame the cadence already wrote is written again by the flush.",
    ),
    _sw(
        "cadence-last-written-twice",
        "if (state.last.pixels != NULL && !state.last_dumped) {",
        "if (state.last.pixels != NULL) {",
        "the flush writes the last frame even when it was written already.",
    ),
    _sw(
        "cadence-last-flag-not-set",
        "        state.last_dumped = true;\n",
        "",
        "a second flush writes the last frame again.",
    ),
    _sw(
        "cadence-offscreen-dumped-forgotten",
        "state.offscreen_dumped = due;",
        "state.offscreen_dumped = false;",
        "the offscreen passes of a frame the cadence wrote are written again by the flush.",
    ),
    _sw(
        "cadence-offscreen-written-twice",
        "if (state.offscreen_count != 0u && !state.offscreen_dumped) {",
        "if (state.offscreen_count != 0u) {",
        "the flush writes the offscreen passes even when they were written already.",
    ),
    _sw(
        "cadence-offscreen-flag-not-set",
        "        state.offscreen_dumped = true;\n",
        "",
        "a second flush writes the offscreen passes again.",
    ),
    _sw(
        "cadence-offscreen-frame-number",
        "state.offscreen_frame = frame_number;",
        "state.offscreen_frame = 0u;",
        "the offscreen passes the flush writes are named after frame 0.",
    ),
    _sw(
        "cadence-offscreen-follows-the-cadence",
        "        dump_offscreen(frame_number);\n",
        "        (void)frame_number;\n",
        "the offscreen passes of a due frame are never written at the present.",
    ),
    # ================================================================== hostopt
    _ho(
        "hostopt-combiner-default-on",
        "    out->gpu_replay_combiner = false;",
        "    out->gpu_replay_combiner = true;",
        "the combiner replaces the fixed fragment stage of every replay, and every default frame changes.",
    ),
    _ho(
        "hostopt-dump-every-default",
        "    out->gpu_replay_dump_every = 0u;",
        "    out->gpu_replay_dump_every = 1u;",
        "the unset cadence is spelled 1, which `--gpu-replay-dump-last` alone would then also write.",
    ),
    _ho(
        "hostopt-dump-last-default-on",
        "    out->gpu_replay_dump_last = false;",
        "    out->gpu_replay_dump_last = true;",
        "every dumped replay turns the per-frame writes off.",
    ),
    _ho(
        "hostopt-combiner-flag-sets-nothing",
        '        } else if (strcmp(arg, "--gpu-replay-combiner") == 0) {\n            out->gpu_replay_combiner = true;',
        '        } else if (strcmp(arg, "--gpu-replay-combiner") == 0) {\n            out->gpu_replay_combiner = false;',
        "--gpu-replay-combiner is accepted and does nothing.",
    ),
    _ho(
        "hostopt-dump-last-flag-sets-nothing",
        "            out->gpu_replay_dump_last = true;",
        "            out->gpu_replay_dump_last = false;",
        "--gpu-replay-dump-last is accepted and does nothing.",
    ),
    _ho(
        "hostopt-dump-every-zero-accepted",
        "every == 0u || every > UINT32_MAX ||",
        "every > UINT32_MAX ||",
        "N = 0 is accepted (a modulus of zero in the replay, or the unset value spelled as a choice).",
    ),
    _ho(
        "hostopt-dump-every-overflow-accepted",
        "every > UINT32_MAX ||",
        "",
        "4294967296 wraps to 0 in the unsigned field and silently means 'unset'.",
    ),
    _ho(
        "hostopt-dump-every-trailing-garbage",
        "end == argv[i] || *end != '\\0' || every == 0u",
        "end == argv[i] || every == 0u",
        "`5x` is read as 5.",
    ),
    _ho(
        "hostopt-dump-every-sign-accepted",
        "every > UINT32_MAX ||\n                argv[i][0] < '0' || argv[i][0] > '9'",
        "every > UINT32_MAX ||\n                false",
        "strtoull takes a leading space or plus: ` 5` and `+5` are accepted as 5. (T536: the sign check "
        "also exists for --vblank-owner-waits, so the anchor carries the `every` line above it.)",
    ),
    _ho(
        "hostopt-combiner-without-replay-accepted",
        "!out->gpu_replay_combiner && !out->gpu_replay_viewport_from_target))",
        "true && !out->gpu_replay_viewport_from_target))",
        "--gpu-replay-combiner without --gpu-replay is accepted and silently does nothing.",
    ),
    _ho(
        "hostopt-cadence-without-dump-directory-accepted",
        "(out->gpu_replay_dump != NULL ||",
        "(true ||",
        "a cadence with no --gpu-replay-dump directory is accepted: there is nothing to choose among.",
    ),
    # ================================================================== census
    _pr(
        "census-not-called",
        "            census_draw(record->number, draw, info);\n",
        "            (void)census_draw;\n",
        "the census never examines a draw, so the report says 0 draws and every refusal stays unseen.",
    ),
    _pr(
        "census-allows-no-inference",
        "GPU_COMBINER_INFER_ALL, no_textures, &plan,",
        "0u, no_textures, &plan,",
        "every plan that needs an inference is reported refused, which hides the draws the combiner can do.",
    ),
    _pr(
        "census-planned-counted-as-refused",
        "        summary.combiner_planned++;\n",
        "        summary.combiner_refused++;\n",
        "plans are tallied as refusals.",
    ),
    _pr(
        "census-merges-everything",
        "strcmp(outcome->text, text) == 0",
        "1",
        "every outcome is merged into the first, so the distinct reasons are lost.",
    ),
    _pr(
        "census-first-frame",
        "outcome->first_frame = frame;",
        "outcome->first_frame = frame - frame;",
        "the first frame of an outcome is not recorded.",
    ),
    _pr(
        "census-first-draw",
        "outcome->first_draw = (uint32_t)draw;",
        "outcome->first_draw = (uint32_t)draw - (uint32_t)draw;",
        "the draw index of an outcome is not recorded.",
    ),
]

_NEEDS_DEVICE: frozenset[str] = frozenset(
    f"gpu-swap-combiner-{name}"
    for name in (
        "modules-fragment-loader-reads-vertex-set",
        "modules-vertex-loader-reads-fragment-set",
        "modules-backend-vertex-table",
        "optin-backend-flag",
        "optin-backend-always-on",
        "optin-backend-fragment-table",
        "optin-backend-fragment-loader",
        "cadence-every-inverted",
        "cadence-every-ignored",
        "cadence-last-alone-writes-all",
        "cadence-default-writes-nothing",
        "cadence-last-frame-number",
        "cadence-last-flag-ignored",
        "cadence-last-dumped-forgotten",
        "cadence-last-written-twice",
        "cadence-last-flag-not-set",
        "cadence-offscreen-dumped-forgotten",
        "cadence-offscreen-written-twice",
        "cadence-offscreen-flag-not-set",
        "cadence-offscreen-frame-number",
        "cadence-offscreen-follows-the-cadence",
    )
)  # from a run with the Vulkan loader hidden (21 of the set), see the header

for _mutation in MUTATIONS:
    _mutation["needs_device"] = _mutation["id"] in _NEEDS_DEVICE
