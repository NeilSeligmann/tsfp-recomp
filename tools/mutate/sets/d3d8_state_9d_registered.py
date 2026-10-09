"""Mutation for the original-grounded state 0x9D entry registration (T1066)."""

SOURCE = "src/gpu/d3d8_state.c"
SUITE = "test_d3d8_state_9d_registered"

MUTATIONS: list[dict] = [
    {
        "id": "t1066-state-9d-registered-at-wrong-title-address",
        "file": SOURCE,
        "old": "{0x003D71B0u, handler_9d},",
        "new": "{0x003D71B4u, handler_9d},",
        "targets": [SUITE],
        "why": (
            "the XBE exposes this state-0x9D leaf at 0x003D71B0; a neighbouring registration "
            "leaves the title entry unhandled."
        ),
    },
]
