# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the first native movie loop (T394): the overlay consumption policy in
src/gpu/d3d8_overlay.c, the substitution spec validation in src/host/xmv_original.c and the option
guards in src/host/host_options.c. The synchronous stream Flush and the unaligned completion words are in
dsound_movie_stream.py. All of these are reached through ctest binaries (test_d3d8_overlay,
test_xmv_original, test_host_options).

NOT HERE: the mutants of the code only a real boot runs (the substitution rewrite, the frame dump and
its CRC and picture, the GetNextFrame result trace in xmv_original.c). `tsfp_host` is not a ctest binary,
so this harness cannot rebuild it, and a pytest target without a rebuilt `tsfp_host` would test a stale
binary. Those ten were applied by hand in a scratch worktree, each rebuilt and run against
tests/test_movie_loop_boot.py, results in docs/tasks.md (T394).
"""

MUTATIONS: list[dict] = [
    {
        "id": "overlay-consume-ignores-policy",
        "file": "src/gpu/d3d8_overlay.c",
        "old": "    if (!consume_policy || shadow[pending_index] == 0u) {",
        "new": "    if (shadow[pending_index] == 0u) {",
        "targets": ["test_d3d8_overlay"],
        "why": "a vblank consumption that runs with the policy off turns the named refusal into a silent "
        "fabrication (T540: the policy retires a buffer only at a completed modeled vblank).",
    },
    {
        "id": "overlay-consume-never-applies",
        "file": "src/gpu/d3d8_overlay.c",
        "old": "    if (!consume_policy || shadow[pending_index] == 0u) {",
        "new": "    if (true || !consume_policy || shadow[pending_index] == 0u) {",
        "targets": ["test_d3d8_overlay"],
        "why": "an enabled policy that never consumes at the vblank leaves the end of every movie at the refusal.",
    },
    {
        # T540 removed the stop request 0x8704: the consumption is the modeled vblank retiring the
        # buffer, and the register write log stays exactly the title's. This replaces the T394 pair
        # overlay-consume-no-stop-request and overlay-consume-stop-request-value, whose code is gone.
        "id": "overlay-consume-counts-a-register-write",
        "file": "src/gpu/d3d8_overlay.c",
        "old": "    shadow[pending_index] = 0u;\n    consumed_buffers++;",
        "new": "    descriptor.write_count++;\n    shadow[pending_index] = 0u;\n    consumed_buffers++;",
        "targets": ["test_d3d8_overlay"],
        "why": "the vblank consumption is not a guest write: a counted register write would put a write in "
        "the trace that the title never made.",
    },
    {
        "id": "overlay-consume-does-not-clear",
        "file": "src/gpu/d3d8_overlay.c",
        "old": "    shadow[pending_index] = 0u;\n",
        "new": "",
        "targets": ["test_d3d8_overlay"],
        "why": "a buffer the hardware never retires would make the next EnableOverlay spin again.",
    },
    {
        "id": "overlay-consume-clears-wrong-register",
        "file": "src/gpu/d3d8_overlay.c",
        "old": "    shadow[pending_index] = 0u;\n",
        "new": "    shadow[(REG_PENDING_KICK - D3D8_OVERLAY_FIRST_REGISTER) / 4u] = 0u;\n",
        "targets": ["test_d3d8_overlay"],
        "why": "the hardware clears the pending flag 0x8700, not the stop request.",
    },
    {
        "id": "overlay-consume-not-counted",
        "file": "src/gpu/d3d8_overlay.c",
        "old": "    consumed_buffers++;\n",
        "new": "",
        "targets": ["test_d3d8_overlay"],
        "why": "the host reports the number of buffers the fabrication retired, it must be exact.",
    },
    {
        "id": "overlay-consume-count-survives-reset",
        "file": "src/gpu/d3d8_overlay.c",
        "old": "    memset(&descriptor, 0, sizeof(descriptor));\n    consumed_buffers = 0u;",
        "new": "    memset(&descriptor, 0, sizeof(descriptor));",
        "targets": ["test_d3d8_overlay"],
        "why": "reset clears every counter of the module.",
    },
    {
        "id": "overlay-consume-policy-lost-on-reset",
        "file": "src/gpu/d3d8_overlay.c",
        "old": "    consumed_buffers = 0u;\n}",
        "new": "    consumed_buffers = 0u;\n    consume_policy = false;\n}",
        "targets": ["test_d3d8_overlay"],
        "why": "the policy is configuration and survives the reset between runs.",
    },
    {
        "id": "options-overlay-consume-needs-no-xmv",
        "file": "src/host/host_options.c",
        "old": "(!out->overlay_consume || (out->native_xmv && out->couple_vblank_effects)) &&",
        "new": "",
        "targets": ["test_host_options"],
        "why": "the overlay policy only exists for the movie, without XMV it would be a stray "
        "fabrication.",
    },
    {
        "id": "options-overlay-consume-needs-coupled-effects",
        "file": "src/host/host_options.c",
        "old": "(!out->overlay_consume || (out->native_xmv && out->couple_vblank_effects)) &&",
        "new": "(!out->overlay_consume || out->native_xmv) &&",
        "targets": ["test_host_options"],
        "why": "T540: the policy retires a buffer at a completed modeled vblank, which only the coupled "
        "vblank effects run; without them the option would silently never consume.",
    },
    {
        "id": "options-dump-frames-needs-no-trace",
        "file": "src/host/host_options.c",
        "old": "(out->dump_xmv_frames == NULL || out->trace_xmv) &&",
        "new": "",
        "targets": ["test_host_options"],
        "why": "the frame dump is hung off the trace, a silent file writer is not allowed.",
    },
    {
        "id": "options-substitute-needs-no-xmv",
        "file": "src/host/host_options.c",
        "old": "(out->xmv_substitute == NULL || out->native_xmv) &&",
        "new": "",
        "targets": ["test_host_options"],
        "why": "the substitution rewrites the title's path, only the retained decoder reads it.",
    },
    {
        "id": "options-dump-max-accepts-trailing-text",
        "file": "src/host/host_options.c",
        "old": "if (end == argv[i] || *end != '\\0' || limit > 1000000u ||",
        "new": "if (end == argv[i] || limit > 1000000u ||",
        "targets": ["test_host_options"],
        "why": "a number with trailing characters is a typo, not a limit.",
    },
    {
        "id": "xmvorig-substitute-name-allows-dot",
        "file": "src/host/xmv_original.c",
        "old": "(c >= '0' && c <= '9') || c == '_')) return false;",
        "new": "(c >= '0' && c <= '9') || c == '_' || c == '.')) return false;",
        "targets": ["test_xmv_original"],
        "why": "a dot in a movie name lets the substitution point outside the movie directory and "
        "extension.",
    },
    {
        "id": "xmvorig-substitute-name-rejects-underscore",
        "file": "src/host/xmv_original.c",
        "old": "(c >= '0' && c <= '9') || c == '_')) return false;",
        "new": "(c >= '0' && c <= '9'))) return false;",
        "targets": ["test_xmv_original"],
        "why": "eag_e has an underscore, the spec must accept the disc's own names.",
    },
    {
        "id": "xmvorig-substitute-name-length-unbounded",
        "file": "src/host/xmv_original.c",
        "old": "if (length == 0u || length >= SUBSTITUTE_NAME_BYTES) return false;",
        "new": "if (length == 0u) return false;",
        "targets": ["test_xmv_original"],
        "why": "an over long name overruns the 33 byte storage.",
    },
    {
        "id": "xmvorig-substitute-name-may-be-empty",
        "file": "src/host/xmv_original.c",
        "old": "if (length == 0u || length >= SUBSTITUTE_NAME_BYTES) return false;",
        "new": "if (length >= SUBSTITUTE_NAME_BYTES) return false;",
        "targets": ["test_xmv_original"],
        "why": "an empty name opens the movie directory itself.",
    },
]
