"""Mutations for the T200 compiler lock in src/host/xdk_original.c.

The retained shader compiler writes static data, so two guest threads inside it at once hang,
throw or fault (measured with tests/c/shader_compiler_probe.c, see docs/shader-original-routes.md).
`xdk_original_dispatch` serializes every profile entry except the read-only getter 0x003E6714.
Everything here is reachable through `test_xdk_original` (the fixture bodies stand in for the
compiled aliases, the real compiler is covered by tests/test_shader_compiler_native.py).
"""

MUTATIONS: list[dict] = [
    {
        "id": "xdkorig-serial-lock-never-taken",
        "file": "src/host/xdk_original.c",
        "old": "    const bool serialized = address != READ_ONLY_ENTRY;",
        "new": "    const bool serialized = address != READ_ONLY_ENTRY && address == 0u;",
        "targets": [
            "test_xdk_original",
            "tsfp_shader_probe",
            "pytest:tests/test_shader_compiler_native.py -k route_serialises",
        ],
        "why": "without the lock two guest threads run the compiler at once, which corrupts its "
        "static state and shows up as a hang, a C++ throw or a fault depending on timing.",
    },
    {
        "id": "xdkorig-serial-lock-takes-read-only-entry",
        "file": "src/host/xdk_original.c",
        "old": "    const bool serialized = address != READ_ONLY_ENTRY;",
        "new": "    const bool serialized = address != 0u;",
        "targets": ["test_xdk_original"],
        "why": "locking the read-only getter makes every buffer pointer query wait behind a "
        "running compile, which the title can do from another thread.",
    },
    {
        "id": "xdkorig-serial-lock-not-released-on-return",
        "file": "src/host/xdk_original.c",
        "old": ("        release_call(serialized);\n        return true;"),
        "new": ("        release_call(false);\n        return true;"),
        "targets": ["test_xdk_original"],
        "why": "a lock leaked on the normal return blocks every other guest thread's next compile "
        "forever, with no stop report.",
    },
    {
        "id": "xdkorig-serial-lock-not-released-on-stop",
        "file": "src/host/xdk_original.c",
        "old": "    release_call(serialized);\n    host_run_rethrow(host_run_result());",
        "new": "    release_call(false);\n    host_run_rethrow(host_run_result());",
        "targets": ["test_xdk_original"],
        "why": "a stop or fault inside the compiler that keeps the lock hangs the second guest "
        "thread instead of letting it report its own stop.",
    },
    {
        "id": "xdkorig-serial-lock-not-recursive",
        "file": "src/host/xdk_original.c",
        "old": (
            "    if (serial_depth == 0u) {\n        pthread_mutex_lock(&serial_lock);\n    }\n"
            "    serial_depth++;"
        ),
        "new": "    pthread_mutex_lock(&serial_lock);\n    serial_depth++;",
        "targets": ["test_xdk_original"],
        "why": "the compiler bodies call each other through the dispatcher, a non-recursive lock "
        "deadlocks the thread on its own nested call.",
    },
]
