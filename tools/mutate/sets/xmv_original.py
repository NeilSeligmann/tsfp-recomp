"""Mutations for the opt-in retained XMV exports (T92/T234): src/host/xmv_original.c and
the --native-xmv option guards in src/host/host_options.c.

`tsfp_host` is not a ctest binary, so the generated chunk and main.c wiring are covered by
the pytest generator suite and the recorded boot measurement, not here. Everything below
is reachable through `test_xmv_original` and `test_host_options`. The Python generator
(tools/gen_xmv_original_bodies.py) cannot be mutated by this C harness, it needs a changed
binary. Its mutation sweep is recorded in docs/xmv-contracts.md and run by hand.
"""

MUTATIONS: list[dict] = [
    {
        "id": "xmvorig-dispatch-ignores-disabled",
        "file": "src/host/xmv_original.c",
        "old": "    if (!configured) {",
        "new": "    if (!configured && false) {",
        "targets": ["test_xmv_original"],
        "why": "a retained XMV body that runs while --native-xmv is off makes the default boot "
        "execute the original decoder, so the stop at 0x444A2D would silently disappear.",
    },
    {
        "id": "xmvorig-wrong-export-address",
        "file": "src/host/xmv_original.c",
        "old": "    0x00445241u, 0x00445252u, 0x0044525Du,",
        "new": "    0x00445241u, 0x00445252u, 0x0044525Eu,",
        "targets": ["test_xmv_original"],
        "why": "GetNextFrame at an interior address would route nothing and leave the real "
        "trampoline to stop, looking like a decoder boundary rather than a table typo.",
    },
    {
        "id": "xmvorig-lookup-miss-accepted",
        "file": "src/host/xmv_original.c",
        "old": (
            "        if (function == NULL) {\n            return false;\n        }\n"
            "        if (out != NULL) {"
        ),
        "new": (
            "        if (function == NULL && false) {\n            return false;\n        }\n"
            "        if (out != NULL) {"
        ),
        "targets": ["test_xmv_original"],
        "why": "a chunk missing one of the seven bodies would report ready and then call a NULL "
        "function pointer on the first export that is missing.",
    },
    {
        "id": "xmvorig-identity-not-checked",
        "file": "src/host/xmv_original.c",
        "old": '    if (identity == NULL || strcmp(identity, "xmv-original-v1") != 0) {',
        "new": "    if (identity == NULL) {",
        "targets": ["test_xmv_original"],
        "why": "the shader chunk or a future XMV profile with other semantics would be accepted "
        "as this one, so the manifest identity would stop meaning anything.",
    },
    {
        "id": "xmvorig-configure-while-active",
        "file": "src/host/xmv_original.c",
        "old": "    if (active_calls != 0u || (enabled && !collect(selected))) {",
        "new": "    if ((enabled && !collect(selected))) {",
        "targets": ["test_xmv_original"],
        "why": "reconfiguring while an original executes swaps the function table under a running "
        "decoder call, which only shows up as a crash in a long movie.",
    },
    {
        "id": "xmvorig-active-call-not-released",
        "file": "src/host/xmv_original.c",
        "old": (
            "        if (!host_run_scope_pop(&scope)) {\n            abort();\n        }\n"
            "        release_call();\n        return true;"
        ),
        "new": (
            "        if (!host_run_scope_pop(&scope)) {\n            abort();\n        }\n"
            "        return true;"
        ),
        "targets": ["test_xmv_original"],
        "why": "a leaked active count refuses every later configure, so teardown reports the "
        "originals as not cleaned and the exit status lies about a clean run.",
    },
    {
        "id": "xmvorig-stop-path-not-released",
        "file": "src/host/xmv_original.c",
        "old": (
            "    if (!host_run_scope_pop(&scope)) {\n        abort();\n    }\n"
            "    release_call();\n    host_run_rethrow(host_run_result());"
        ),
        "new": (
            "    if (!host_run_scope_pop(&scope)) {\n        abort();\n    }\n"
            "    host_run_rethrow(host_run_result());"
        ),
        "targets": ["test_xmv_original"],
        "why": "the expected end of a bring-up run is a stop inside an original (the audio stream "
        "boundary), so a leak on that path hits the one path every real run takes.",
    },
    {
        "id": "xmvopt-native-xmv-defaults-on",
        "file": "src/host/host_options.c",
        "old": "    out->native_xmv = false;\n    out->native_xmv_explicit",
        "new": "    out->native_xmv = true;\n    out->native_xmv_explicit",
        "targets": ["test_host_options"],
        "why": "without a disc nothing changes (T1093: with a disc the route is the default): "
        "the no-disc default must stay off.",
    },
    {
        "id": "xmvopt-native-xmv-disc-default",
        "file": "src/host/host_options.c",
        "old": "    } else if (!out->native_xmv && out->disc_path != NULL) {",
        "new": "    } else if (0 && !out->native_xmv && out->disc_path != NULL) {",
        "targets": ["test_host_options"],
        "why": "T1093: a mounted disc must serve the retained XMV exports without the opt-in flag.",
    },
    {
        "id": "xmvopt-native-xmv-without-disc",
        "file": "src/host/host_options.c",
        "old": (
            "        (!out->native_xmv || out->disc_path != NULL) && "
            "!(out->native_xmv_off && out->native_xmv_explicit) && "
            "(!out->trace_xmv || out->native_xmv) &&"
        ),
        "new": "        (!out->trace_xmv || out->native_xmv) &&",
        "targets": ["test_host_options"],
        "why": "without a disc the movie open fails and the title enters its own disc read "
        "failure screen, which would then be mistaken for a decoder result.",
    },
    {
        "id": "xmvopt-trace-without-native",
        "file": "src/host/host_options.c",
        "old": "(!out->trace_xmv || out->native_xmv) &&",
        "new": "true &&",
        "targets": ["test_host_options"],
        "why": "a trace flag that silently does nothing looks like a decoder that never ran.",
    },
]
