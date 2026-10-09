"""Mutation for the original-grounded state 0x89 entry registration (T1066)."""

SOURCE = "src/gpu/d3d8_state.c"
SUITE = "test_d3d8_state_89_registered"

MUTATIONS: list[dict] = [
    {
        "id": "t1066-state-89-registered-at-wrong-title-address",
        "file": SOURCE,
        "old": "{0x003D74B0u, handler_89},",
        "new": "{0x003D74B4u, handler_89},",
        "targets": [SUITE],
        "why": (
            "the XBE exposes this render-state 0x89 helper at 0x003D74B0; a neighbouring "
            "registration leaves the title entry unhandled."
        ),
    },
]
