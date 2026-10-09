"""Mutation for the original-grounded state 0x92 entry registration (T1066)."""

SOURCE = "src/gpu/d3d8_state.c"
SUITE = "test_d3d8_state_92_registered"

MUTATIONS: list[dict] = [
    {
        "id": "t1066-state-92-registered-at-wrong-title-address",
        "file": SOURCE,
        "old": "{0x003D70D0u, handler_92},",
        "new": "{0x003D70D4u, handler_92},",
        "targets": [SUITE],
        "why": (
            "the XBE exposes this stdcall setter at 0x003D70D0; a neighbouring registration "
            "leaves the title entry unhandled."
        ),
    },
]
