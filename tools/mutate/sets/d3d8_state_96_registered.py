"""Mutation for the original-grounded state 0x96 entry registration (T1066)."""

SOURCE = "src/gpu/d3d8_state.c"
SUITE = "test_d3d8_state_96_registered"

MUTATIONS: list[dict] = [
    {
        "id": "t1066-state-96-registered-at-wrong-title-address",
        "file": SOURCE,
        "old": "{0x003D7320u, handler_96},",
        "new": "{0x003D7324u, handler_96},",
        "targets": [SUITE],
        "why": (
            "the XBE exposes this state-0x96 setter at 0x003D7320; a neighbouring registration "
            "leaves the title entry unhandled."
        ),
    },
]
