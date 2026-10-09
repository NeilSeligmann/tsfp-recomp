# SPDX-License-Identifier: GPL-3.0-or-later
"""Compiled semantic mutations for T1068's bounded SetPosition record."""

# This set is run by py_mutants.py; c_suites still requires an empty native list.
MUTATIONS: list[dict] = []

PYTHON_MUTATIONS = [
    {
        "id": "t1068-position-y-copies-x",
        "file": "src/audio/dsound_buffer.c",
        "old": "        n->value.position_bits[1]=r->count;",
        "new": "        n->value.position_bits[1]=r->argument;",
        "targets": ["pytest: tests/test_t1068_set_position.py"],
        "why": "A component mix-up loses the second raw word.",
    },
    {
        "id": "t1068-position-apply-gate-admits-zero",
        "file": "src/audio/dsound_buffer.c",
        "old": "if(r->apply!=1u) {",
        "new": "if(r->apply!=0u) {",
        "targets": ["pytest: tests/test_t1068_set_position.py"],
        "why": "The direct title callers pass apply=1; apply=0 is outside the admitted domain.",
    },
    {
        "id": "t1068-position-third-caller-drift",
        "file": "src/audio/dsound_buffer.c",
        "old": (
            "(actual==POSITION_CALLER_1 || actual==POSITION_CALLER_2 || actual==POSITION_CALLER_3);"
        ),
        "new": (
            "(actual==POSITION_CALLER_1 || actual==POSITION_CALLER_2 || "
            "actual==POSITION_CALLER_3+1u);"
        ),
        "targets": ["pytest: tests/test_t1068_set_position.py"],
        "why": "Keep the third measured return site, with no neighboring address admitted.",
    },
    {
        "id": "t1068-position-policy-registration-removed",
        "file": "src/audio/dsound_buffer.c",
        "old": "    if(policy)count+=dsound_hle_register(SET_POSITION,position_handler)?1u:0u;",
        "new": "    if(false)count+=dsound_hle_register(SET_POSITION,position_handler)?1u:0u;",
        "targets": ["pytest: tests/test_t1068_set_position.py"],
        "why": "The opt-in policy must register the handler for the measured title frames.",
    },
]


# T1245: original global error precedes object/settings access for both setters.
PYTHON_MUTATIONS += [
    {
        "id": "t1245-buffer-global-reports-success",
        "file": "src/audio/dsound_buffer.c",
        "old": "if(failure!=0u)return failure;\n    }\n    access_request r={0};",
        "new": "if(failure!=0u)return 0u;\n    }\n    access_request r={0};",
        "targets": ["pytest: tests/test_t1068_set_position.py"],
        "why": "Global failure must return E_FAIL rather than successful acknowledgement.",
    },
    {
        "id": "t1245-buffer-global-preflight-skipped",
        "file": "src/audio/dsound_buffer.c",
        "old": "if(entry==SET_POSITION || entry==SET_LOOP_REGION || entry==MAX_DISTANCE) {",
        "new": "if(false) {",
        "targets": ["pytest: tests/test_t1068_set_position.py"],
        "why": "Original failure precedes identifying even a null or invalid buffer.",
    },
    {
        "id": "t1245-buffer-loop-global-preflight-skipped",
        "file": "src/audio/dsound_buffer.c",
        "old": "if(entry==SET_POSITION || entry==SET_LOOP_REGION || entry==MAX_DISTANCE) {",
        "new": "if(entry==SET_POSITION || entry==MAX_DISTANCE) {",
        "targets": ["pytest: tests/test_t1068_loop_regions.py"],
        "why": "Loop-region errors also precede reading the buffer and loop settings.",
    },
]

PYTHON_MUTATIONS.append(
    {
        "id": "t1245-buffer-distance-global-preflight-skipped",
        "file": "src/audio/dsound_buffer.c",
        "old": "if(entry==SET_POSITION || entry==SET_LOOP_REGION || entry==MAX_DISTANCE) {",
        "new": "if(entry==SET_POSITION || entry==SET_LOOP_REGION) {",
        "targets": ["pytest: tests/test_t1068_set_max_distance.py"],
        "why": "Distance also returns original E_FAIL before invalid buffer access.",
    }
)
