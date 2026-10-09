"""Mutations for the CALLEE ABI entry point in `src/host/xdk_thunk.c`.

The point of that entry point is what it does NOT gate on (a site quorum) and what it refuses
INSTEAD (a conflict between the callee's own returns), so both directions are mutated.
"""

MUTATIONS: list[dict] = [
    {
        "id": "xdk-callee-conflict-accepted",
        "file": "src/host/xdk_thunk.c",
        "old": "    if (!unanimous) {\n"
        '        xdk_thunk_log()("xdk: REFUSING a callee ABI for 0x%08X -- its %u terminator(s) "',
        "new": "    if (!unanimous && false) {\n"
        '        xdk_thunk_log()("xdk: REFUSING a callee ABI for 0x%08X -- its %u terminator(s) "',
        "targets": ["test_xdk_dispatch"],
        "why": "two disagreeing returns mean the walk crossed a function boundary, and a wrong "
        "extent is not biased in a knowable direction. Accepting it launders an extent bug into "
        "an arity.",
    },
    {
        "id": "xdk-callee-zero-terminators-accepted",
        "file": "src/host/xdk_thunk.c",
        "old": "    if (terminators == 0u) {",
        "new": "    if (terminators == 0u && false) {",
        "targets": ["test_xdk_dispatch"],
        "why": "nothing read is not a zero-argument function; accepting it hands a plausible "
        "stack_args of 0 to a function nobody read.",
    },
]
