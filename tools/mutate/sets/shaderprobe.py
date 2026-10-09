# ruff: noqa: E501
"""Mutations of the T51 dispatch probe in src/host/xdk_thunk.c (suite: test_xdk_probe)."""

MUTATIONS: list[dict] = [
    {
        "id": "probe-never-called",
        "file": "src/host/xdk_thunk.c",
        "old": "    probe_dispatch(address);\n    /* Retained originals",
        "new": "    if (address == 0u) {\n        probe_dispatch(address);\n    }\n    /* Retained originals",
        "targets": ["test_xdk_probe"],
        "why": "a probe that is never invoked logs nothing, which reads as 'the game never "
        "made that call'.",
    },
    {
        "id": "probe-matches-the-wrong-address",
        "file": "src/host/xdk_thunk.c",
        "old": "        if (probe_sites[i].address == address) {\n            site = &probe_sites[i];",
        "new": "        if (probe_sites[i].address != address) {\n            site = &probe_sites[i];",
        "targets": ["test_xdk_probe"],
        "why": "logging every address EXCEPT the named ones puts unrelated calls and their "
        "arguments into a key census.",
    },
    {
        "id": "probe-arguments-off-by-one-word",
        "file": "src/host/xdk_thunk.c",
        "old": "(kernel_guest_ptr)(g_esp + 4u + 4u * i), &args[i]);",
        "new": "(kernel_guest_ptr)(g_esp + 4u * i), &args[i]);",
        "targets": ["test_xdk_probe"],
        "why": "the first logged argument would be the return address, so every program "
        "pointer and key argument is read one slot early.",
    },
    {
        "id": "probe-caller-from-the-wrong-slot",
        "file": "src/host/xdk_thunk.c",
        "old": "    (void)kernel_guest_read_u32((kernel_guest_ptr)g_esp, &return_address);\n    for (uint32_t i = 0; i < PROBE_ARGS",
        "new": "    (void)kernel_guest_read_u32((kernel_guest_ptr)(g_esp + 4u), &return_address);\n    for (uint32_t i = 0; i < PROBE_ARGS",
        "targets": ["test_xdk_probe"],
        "why": "the caller attributes a draw or an assembler call to a call site; a shifted "
        "slot attributes it to the wrong one.",
    },
    {
        "id": "probe-hash-mixing-reordered",
        "file": "src/host/xdk_thunk.c",
        "old": "hash = (hash ^ byte) * 1099511628211ull;",
        "new": "hash = (hash * 1099511628211ull) ^ byte;",
        "targets": ["test_xdk_probe"],
        "why": "the Python side recomputes FNV-1a 64 to match a logged hash to a builder "
        "source; FNV-1 instead of FNV-1a would make every match fail, or worse, "
        "match nothing silently.",
    },
    {
        "id": "probe-hash-length-from-the-pointer-argument",
        "file": "src/host/xdk_thunk.c",
        "old": "const uint32_t length = args[site->hash_len_arg];",
        "new": "const uint32_t length = args[site->hash_ptr_arg];",
        "targets": ["test_xdk_probe"],
        "why": "hashing a pointer-sized number of bytes from the buffer, or the wrong buffer, "
        "yields a hash that matches no builder output.",
    },
    {
        "id": "probe-bad-number-accepted",
        "file": "src/host/xdk_thunk.c",
        "old": "    if (end == text || value > 0xFFFFFFFFul) {",
        "new": "    if (end == text && value > 0xFFFFFFFFul) {",
        "targets": ["test_xdk_probe"],
        "why": "a typo in an address would silently watch address 0 instead of the intended "
        "one and log nothing.",
    },
    {
        "id": "probe-bad-spec-does-not-exit",
        "file": "src/host/xdk_thunk.c",
        "old": '    fprintf(stderr, "TSFP_XDK_PROBE: %s near \\"%s\\"\\n", why, text);\n    exit(2);',
        "new": '    fprintf(stderr, "TSFP_XDK_PROBE: %s near \\"%s\\"\\n", why, text);\n    return;',
        "targets": ["test_xdk_probe"],
        "why": "a malformed spec that only prints would leave the probe half-configured and "
        "a boot would report nothing for the missing address.",
    },
    {
        "id": "probe-word-limit-unenforced",
        "file": "src/host/xdk_thunk.c",
        "old": "} else if (probe_word_count < PROBE_WORDS_MAX) {",
        "new": "} else if (probe_word_count < PROBE_WORDS_MAX + 1u) {",
        "targets": ["test_xdk_probe"],
        "why": "an off-by-one on the word table overruns it (the line buffer is sized for "
        "PROBE_WORDS_MAX).",
    },
]
