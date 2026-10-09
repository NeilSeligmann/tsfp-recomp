# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for T847 (on-demand shader modules for the live renderer, frame loop failure counter).

TARGETS. `test_gpu_pgraph` (program miss goes to the maker), `test_gpu_combiner` (combiner miss), `test_d3d8_swap_replay` (the module set
growth seam `d3d8_swap_replay_set_live_module_maker`), `test_live_module_maker` (name rules, subprocess, remembered failures, files left),
`test_host_options` (`--gpu-live-translate`) and `test_live_vk_frame` (a refused draw is not a loop failure; needs
`SDL_VIDEODRIVER=x11 TSFP_TEST_GPU_WINDOW_REQUIRED=1 xvfb-run -a`, ctest SKIPS it without a window, `needs_device`).
Python (`tools/nv2a/live_modules.py`, `tests/test_live_modules.py`): three mutants done by hand because the harness refuses a Python-only
row behind an unchanged ctest binary (INVALID, stale binary): `if made.stem != expect` made always false, `if expect not in keys` always false,
`if len(block) != BLOCK_BYTES` always false, all killed by `tests/test_live_modules.py`.
Left out and why: the 120 s subprocess timeout and the SIGKILL branch of `run` (a test would wait 120 s), the `status == 0 &&` part of
the success test in `live_module_maker_make` (a translator that exits non zero after writing the module is not a case the python tool
produces), and the `device_ok = false` assignment of `live_vk_frame_run_targets` (a failing queue submit cannot be made on llvmpipe).
"""

PGRAPH = "src/gpu/gpu_pgraph_replay.c"
SWAP = "src/gpu/d3d8_swap_replay.c"
MAKER = "src/gpu/live_module_maker.c"
FRAME = "src/gpu/live_vk_frame.c"
OPTIONS = "src/host/host_options.c"


def row(
    identifier: str, file: str, old: str, new: str, targets: list[str], why: str, **more: bool
) -> dict:
    return {
        "id": f"t847-{identifier}",
        "file": file,
        "old": old,
        "new": new,
        "targets": targets,
        "why": why,
        **more,
    }


PROGRAM_TESTS = ["test_gpu_pgraph"]
MAKER_TESTS = ["test_live_module_maker"]

MUTATIONS: list[dict] = [
    row(
        "program-maker-never-asked",
        PGRAPH,
        "        if (!found && backend->make_module != NULL &&\n",
        "        if (!found && backend->make_module != NULL && false &&\n",
        PROGRAM_TESTS,
        "a program in no table must reach the maker, that is the whole feature.",
    ),
    row(
        "program-maker-asked-on-a-hit",
        PGRAPH,
        "        if (!found && backend->make_module != NULL &&\n",
        "        if (backend->make_module != NULL &&\n",
        PROGRAM_TESTS,
        "a program already in the table must not be translated again.",
    ),
    row(
        "program-retry-not-checked",
        PGRAPH,
        "            gpu_vsh_lookup_name(backend->table, name, &out->module)) {\n            made_error[0]",
        "            true) {\n            made_error[0]",
        PROGRAM_TESTS,
        "a maker that says yes without a module in the table must still refuse the draw, never use an undefined module index.",
    ),
    row(
        "program-reason-dropped",
        PGRAPH,
        'made_error[0] != \'\\0\' ? ", and translating it failed: " : "",\n                        made_error);\n        }',
        'made_error[0] != \'\\0\' ? "" : "",\n                        "");\n        }',
        PROGRAM_TESTS,
        "the refusal must carry the translator's own message, the census is the user's only view.",
    ),
    row(
        "program-asked-as-fragment",
        PGRAPH,
        "backend->make_module(backend->make_module_context, false, name, bytes, 4u + count * 16u,",
        "backend->make_module(backend->make_module_context, true, name, bytes, 4u + count * 16u,",
        PROGRAM_TESTS,
        "the kind tells the maker which translator to run.",
    ),
    row(
        "program-bytes-without-header",
        PGRAPH,
        "name, bytes, 4u + count * 16u, made_error",
        "name, bytes + 4u, count * 16u, made_error",
        PROGRAM_TESTS,
        "the translator and the digest need the program exactly as the replay digests it, header included.",
    ),
    row(
        "fragment-block-words-zero",
        PGRAPH,
        "put_le32(block + word * 4u, state->combiner[word]);",
        "put_le32(block + word * 4u, 0u);",
        ["test_gpu_combiner"],
        "the definition block must be the draw's own 57 state words.",
    ),
    row(
        "fragment-block-short",
        PGRAPH,
        "word < GPU_PGRAPH_COMBINER_WORDS; word++) {\n                put_le32(block",
        "word < GPU_PGRAPH_COMBINER_WORDS - 1u; word++) {\n                put_le32(block",
        ["test_gpu_combiner"],
        "the last state word is part of the definition.",
    ),
    row(
        "fragment-maker-never-asked",
        PGRAPH,
        "        if (backend->make_module != NULL && state->combiner_captured) {",
        "        if (backend->make_module != NULL && state->combiner_captured && false) {",
        ["test_gpu_combiner"],
        "a combiner configuration in no table must reach the maker.",
    ),
    row(
        "fragment-asked-as-vertex",
        PGRAPH,
        "made = backend->make_module(backend->make_module_context, true, out->plan.name, block,",
        "made = backend->make_module(backend->make_module_context, false, out->plan.name, block,",
        ["test_gpu_combiner"],
        "a combiner module comes from the combiner translator.",
    ),
    row(
        "fragment-retry-not-checked",
        PGRAPH,
        "                   gpu_vsh_lookup_name(backend->fragment_table, out->plan.name, &out->module);\n        }\n        if (!made) {",
        "                   true;\n        }\n        if (!made) {",
        ["test_gpu_combiner"],
        "the retry lookup decides, not the maker's answer.",
    ),
    row(
        "swap-maker-not-installed",
        SWAP,
        "    if (live_module_maker != NULL) {\n        out->make_module = swap_make_module;",
        "    if (live_module_maker != NULL && false) {\n        out->make_module = swap_make_module;",
        ["test_d3d8_swap_replay"],
        "the live backend carries the maker only when one is set.",
    ),
    row(
        "swap-module-not-added",
        SWAP,
        "    if (gpu_vsh_lookup_name(&set->table, name, &existing) || append_module(set, name)) {",
        "    if (gpu_vsh_lookup_name(&set->table, name, &existing) || (append_module(set, name) && false)) {",
        ["test_d3d8_swap_replay"],
        "a made module must be added to the table or the retry misses.",
    ),
    row(
        "swap-wrong-set",
        SWAP,
        "    module_set *set = fragment ? &state.fragment : &state.vertex;\n    uint32_t existing",
        "    module_set *set = fragment ? &state.vertex : &state.fragment;\n    uint32_t existing",
        ["test_d3d8_swap_replay"],
        "a combiner module belongs to the fragment table.",
    ),
    row(
        "swap-count-not-updated",
        SWAP,
        "    set->table.module_count = set->count;\n",
        "",
        ["test_d3d8_swap_replay"],
        "the table the lookups use must see the new module.",
    ),
    row(
        "swap-maker-failure-ignored",
        SWAP,
        "    if (!live_module_maker(live_module_maker_context, fragment, name, bytes, byte_count, error, error_bytes)) {\n        return false;\n    }",
        "    (void)live_module_maker(live_module_maker_context, fragment, name, bytes, byte_count, error, error_bytes);",
        ["test_d3d8_swap_replay"],
        "a failed translation must not add a module.",
    ),
    row(
        "swap-word-slot-not-cleared",
        SWAP,
        "    set->words[set->count] = NULL;\n",
        "",
        ["test_d3d8_swap_replay"],
        "a new module's words are not loaded yet, an uninitialised slot would be read as loaded.",
    ),
    row(
        "maker-prefix-unchecked",
        MAKER,
        "    if (strncmp(name, prefix, prefix_length) != 0 || (strlen(name) != prefix_length + 64u && !alpha)) {",
        "    if (false || (strlen(name) != prefix_length + 64u && !alpha)) {",
        MAKER_TESTS,
        "a combiner name asked as a vertex module (and the reverse) must be refused.",
    ),
    row(
        "maker-length-unchecked",
        MAKER,
        "    if (strncmp(name, prefix, prefix_length) != 0 || (strlen(name) != prefix_length + 64u && !alpha)) {",
        "    if (strncmp(name, prefix, prefix_length) != 0 || (strlen(name) < prefix_length + 64u && !alpha)) {",
        MAKER_TESTS,
        "the name becomes a file name: a longer one must be refused.",
    ),
    row(
        "maker-hex-too-wide",
        MAKER,
        "if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {",
        "if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z'))) {",
        MAKER_TESTS,
        "only lowercase hex digits are a digest, anything else could carry a path.",
    ),
    row(
        "maker-uppercase-allowed",
        MAKER,
        "if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {",
        "if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {",
        MAKER_TESTS,
        "the translator names its modules in lowercase, an uppercase request would never be found.",
    ),
    row(
        "maker-existing-file-ignored",
        MAKER,
        "    if (exists(target)) {\n        maker->stats.reused++;",
        "    if (false && exists(target)) {\n        maker->stats.reused++;",
        MAKER_TESTS,
        "a module already in the directory must not be translated again (the persistent cache).",
    ),
    row(
        "maker-failure-forgotten",
        MAKER,
        "    if (known != NULL) {\n        maker->stats.refused_again++;",
        "    if (false && known != NULL) {\n        maker->stats.refused_again++;",
        MAKER_TESTS,
        "a program the translator refuses must not start a subprocess for every draw that binds it.",
    ),
    row(
        "maker-failure-not-recorded",
        MAKER,
        "    maker->stats.failed++;\n    if (maker->failure_count < MAX_REMEMBERED) {",
        "    if (maker->failure_count < MAX_REMEMBERED) {",
        MAKER_TESTS,
        "the stop report counts the names that failed.",
    ),
    row(
        "maker-input-left-behind",
        MAKER,
        "    unlink(input);\n    pthread_mutex_lock(&maker->lock);\n    maker->stats.seconds += spent;",
        "    pthread_mutex_lock(&maker->lock);\n    maker->stats.seconds += spent;",
        MAKER_TESTS,
        "the translator's input file must not pile up in the module directory.",
    ),
    row(
        "maker-made-uncounted",
        MAKER,
        "        maker->stats.made++;\n",
        "",
        MAKER_TESTS,
        "the stop report counts the modules made.",
    ),
    row(
        "maker-time-uncounted",
        MAKER,
        "    maker->stats.seconds += spent;\n",
        "    maker->stats.seconds += 0.0 * spent;\n",
        MAKER_TESTS,
        "the stop report says how long the guest was blocked by translation.",
    ),
    row(
        "maker-kind-swapped",
        MAKER,
        'fragment ? "combiner" : "vertex",',
        'fragment ? "vertex" : "combiner",',
        MAKER_TESTS,
        "the kind picks the translator.",
    ),
    row(
        "maker-message-lost",
        MAKER,
        '        snprintf(message, sizeof message, "%s", line[0] != \'\\0\' ? line : "the translator wrote no module");',
        '        snprintf(message, sizeof message, "%s", "the translator wrote no module");',
        MAKER_TESTS,
        "the refusal carries the translator's own last line.",
    ),
    row(
        "maker-failure-returns-true",
        MAKER,
        '    remember_failure(maker, name, message);\n    pthread_mutex_unlock(&maker->lock);\n    say(error, error_bytes, "%s%s", message, "");\n    return false;',
        '    remember_failure(maker, name, message);\n    pthread_mutex_unlock(&maker->lock);\n    say(error, error_bytes, "%s%s", message, "");\n    return true;',
        MAKER_TESTS,
        "a failed translation is a refusal.",
    ),
    row(
        "options-translate-not-set",
        OPTIONS,
        "            out->gpu_live_translate = true;\n",
        "            out->gpu_live_translate = false;\n",
        ["test_host_options"],
        "the flag must set its field.",
    ),
    row(
        "options-translate-without-live",
        OPTIONS,
        "        (!out->gpu_live_translate || out->gpu_live) &&\n",
        "",
        ["test_host_options"],
        "translation belongs to the live renderer.",
    ),
    row(
        "frame-refusal-is-failure",
        FRAME,
        "    (void)run_model(frame, model, &native, &device_ok);\n    return device_ok;",
        "    return run_model(frame, model, &native, &device_ok);",
        ["test_live_vk_frame"],
        "the T838 counter reported a loop failure for every frame with a refused draw (1062 of 1062 on the retail intro).",
        needs_device=True,
    ),
    row(
        "frame-refused-uncounted",
        FRAME,
        "    if (!all_drawn) {\n        frame->stats.frames_refused++;\n    }",
        "    if (!all_drawn) { }",
        ["test_live_vk_frame"],
        "the stop report counts the frames with a refused draw.",
        needs_device=True,
    ),
]
