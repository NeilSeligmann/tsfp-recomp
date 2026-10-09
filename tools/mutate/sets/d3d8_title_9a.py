# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for T598, the title's own 0x003D81F0 (`d3d8_state_set_9a`) running the ported recompute.

Targets: the handler registered for the title's call (src/gpu/d3d8_state.c), covered by the unit test
`test_d3d8_state` (the dispatch through the D3D8 table, the announcement count) and by the title tests
of `tests/test_d3d8_scaled_viewport_oracle.py` (the registered handler dispatched through the table over
the ORIGINAL's state, every dword compared). Mutants of the shared piece itself are in
`d3d8_scaled_viewport.py` (T533).

CONVENTION. `if (cond && false)` rather than `if (false)`, because `-Wunused-parameter -Werror` turns
the latter into NOT-A-MUTANT.
"""

STATE = "src/gpu/d3d8_state.c"
ORACLE = "pytest:tests/test_d3d8_scaled_viewport_oracle.py"
TITLE = f'{ORACLE} -k "title"'

HANDLER = "void d3d8_state_set_9a(uint32_t value)\n{\n    d3d8_state_library_set_9a(value);\n}"


def mutation(identifier: str, file: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"t598-{identifier}",
        "file": file,
        "old": old,
        "new": new,
        "targets": ["test_d3d8_state", TITLE],
        "why": why,
    }


MUTATIONS: list[dict] = [
    mutation(
        "title-handler-store-only",
        STATE,
        HANDLER,
        "void d3d8_state_set_9a(uint32_t value)\n{\n    store_state(D3D8_STATE_9A, value);\n}",
        "the title's call at the first back buffer runs the recompute and writes its packets (the pre-T598 handler stored and announced).",
    ),
    mutation(
        "title-handler-announces",
        STATE,
        HANDLER,
        'void d3d8_state_set_9a(uint32_t value)\n{\n    d3d8_state_library_set_9a(value);\n    d3d8_hle_note_unmodelled(0x003D81F0u, "state 0x9A");\n}',
        "nothing of 0x003D81F0 is unmodelled any more, so it announces nothing (the announcement count stays 0).",
    ),
    mutation(
        "title-handler-state-9b",
        STATE,
        HANDLER,
        "void d3d8_state_set_9a(uint32_t value)\n{\n    d3d8_state_set_9b(value);\n}",
        "the title's call runs state 0x9A (stores 0x003E3F28, recomputes AT the first back buffer), not 0x9B.",
    ),
    mutation(
        "title-handler-other-state",
        STATE,
        HANDLER,
        "void d3d8_state_set_9a(uint32_t value)\n{\n    d3d8_state_library_set_9a(value + 1u);\n}",
        "the value the title passes is the one stored and used, not a neighbour.",
    ),
    mutation(
        "title-handler-bound-to-9b",
        STATE,
        "return direct_scaled_mode(context, 0x003D81F0u, D3D8_STATE_9A, true);",
        "return direct_scaled_mode(context, 0x003D81F0u, D3D8_STATE_9B, false);",
        "the handler registered for 0x003D81F0 is the 0x9A one.",
    ),
    mutation(
        "title-handler-registered-elsewhere",
        STATE,
        "        {0x003D81F0u, handler_9a},",
        "        {0x003D81F4u, handler_9a},",
        "0x003D81F0 itself is registered for the title's call.",
    ),
]


# T1174 executes these direct-dispatch defects at O0/O3 against the original,
# including the new raw return and post-refill reads; retain existing targets.
for item in MUTATIONS:
    if item["id"] in {
        "t598-title-handler-bound-to-9b",
        "t598-title-handler-registered-elsewhere",
    }:
        item["targets"].append("pytest:tests/test_t1174_scaled_mode.py")
