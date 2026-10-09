"""Mutations for tests/c/shader_compiler_probe.c, judged by tests/test_shader_compiler_native.py.

Each target is the probe binary plus the pytest selection that must notice. They need the
private XBE and the installed alias chunk, so a fresh clone cannot run them (the harness reports
UNBUILT for the probe rather than a pass). The probe is a test instrument, so these check that a
broken instrument cannot silently pass the acceptance tests built on it.
"""

NATIVE = "tests/test_shader_compiler_native.py"

MUTATIONS: list[dict] = [
    {
        "id": "shaderprobe-injection-never-fires",
        "file": "tests/c/shader_compiler_probe.c",
        "old": "tls.alloc_count == tls.fail_at &&",
        "new": "tls.alloc_count == tls.fail_at + 1000000u &&",
        "targets": ["tsfp_shader_probe", f"pytest:{NATIVE} -k failed_title_allocation"],
        "why": "an allocation failure that is never injected makes every out of memory class "
        "collapse into the baseline, and the acceptance tests would be checking nothing.",
    },
    {
        "id": "shaderprobe-frees-not-counted",
        "file": "tests/c/shader_compiler_probe.c",
        "old": "    tls.free_count++;\n    for (uint32_t i = 0u; i < tls.live_count; i++) {",
        "new": "    for (uint32_t i = 0u; i < tls.live_count; i++) {",
        "targets": ["tsfp_shader_probe", f"pytest:{NATIVE} -k boot_reaches"],
        "why": "without the free count the heap balance claims (306 of 308 freed, two left) are "
        "unobserved.",
    },
    {
        "id": "shaderprobe-live-set-not-reduced",
        "file": "tests/c/shader_compiler_probe.c",
        "old": "            tls.live[i] = tls.live[--tls.live_count];\n            break;",
        "new": "            break;",
        "targets": ["tsfp_shader_probe", f"pytest:{NATIVE} -k boot_reaches"],
        "why": "if frees never reduce the live set every call looks like it leaks all its "
        "allocations, or a real leak would hide among them.",
    },
    {
        "id": "shaderprobe-repeat-does-not-restore-stack",
        "file": "tests/c/shader_compiler_probe.c",
        "old": "        g_esp = entry_esp;\n        g_ebx = before.ebx;",
        "new": "        g_ebx = before.ebx;",
        "targets": ["tsfp_shader_probe", f"pytest:{NATIVE} -k repeated_use"],
        "why": "a repeat that runs on the previous call's leftover stack proves nothing about "
        "repeat use of the compiler.",
    },
    {
        "id": "shaderprobe-repeat-output-not-read",
        "file": "tests/c/shader_compiler_probe.c",
        "old": "const output_copy repeat = read_output(slot, again.eax);",
        "new": "const output_copy repeat = read_output(slot, 1u);",
        "targets": ["tsfp_shader_probe", f"pytest:{NATIVE} -k repeated_use"],
        "why": "repeats whose output is never read cannot be compared with the first call.",
    },
    {
        "id": "shaderprobe-concurrent-rounds-use-one-source",
        "file": "tests/c/shader_compiler_probe.c",
        "old": "        const uint32_t index = (round + thread_id) % reference_count;",
        "new": "        const uint32_t index = 0u;",
        "targets": ["tsfp_shader_probe", f"pytest:{NATIVE} -k route_serialises"],
        "why": "two threads compiling the same source every round would miss cross talk between "
        "different compiles, and the test demands three distinct outputs.",
    },
]
