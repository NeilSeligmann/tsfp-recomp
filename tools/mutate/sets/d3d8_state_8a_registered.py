"""Mutations for the caller-authenticated D3D8 state 0x8A registration (T1066)."""

SOURCE = "src/gpu/d3d8_state.c"
SUITE = "test_d3d8_state_8a_registered"

MUTATIONS: list[dict] = [
    {
        "id": "t1066-state-8a-registered-at-wrong-title-address",
        "file": SOURCE,
        "old": "{0x003D7060u, handler_93},        {0x003D7010u, handler_8a},",
        "new": "{0x003D7060u, handler_93},        {0x003D7014u, handler_8a},",
        "targets": [SUITE],
        "why": (
            "the observed title calls 0x003D7010; registering the helper at a neighboring address "
            "leaves "
            "the measured call unhandled and emits no method packet."
        ),
    },
]
