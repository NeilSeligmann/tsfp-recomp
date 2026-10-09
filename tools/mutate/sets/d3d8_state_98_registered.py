"""Mutation for the original-grounded state 0x98 entry registration (T1066)."""

SOURCE = "src/gpu/d3d8_state.c"
SUITE = "test_d3d8_state_98_registered"

MUTATIONS: list[dict] = [
    {
        "id": "t1066-state-98-registered-at-wrong-title-address",
        "file": SOURCE,
        "old": "{0x003D8250u, handler_98},",
        "new": "{0x003D8254u, handler_98},",
        "targets": [SUITE],
        "why": (
            "the XBE exposes the state-0x98 helper at 0x003D8250; a neighbouring registration "
            "leaves the title entry unhandled."
        ),
    },
]
