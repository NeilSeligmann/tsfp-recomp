"""Mutation for the original-grounded state 0xA5 entry registration (T1066)."""

SOURCE = "src/gpu/d3d8_state.c"
SUITE = "test_d3d8_state_a5_registered"

MUTATIONS: list[dict] = [
    {
        "id": "t1066-state-a5-registered-at-wrong-title-address",
        "file": SOURCE,
        "old": "{0x003D81D0u, handler_a5},",
        "new": "{0x003D81D4u, handler_a5},",
        "targets": [SUITE],
        "why": (
            "the XBE exposes the state-0xA5 helper at 0x003D81D0; a neighbouring registration "
            "leaves the title entry unhandled."
        ),
    },
]
