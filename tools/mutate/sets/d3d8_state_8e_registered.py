"""Mutation for the original-grounded state 0x8E entry registration (T1066)."""

SOURCE = "src/gpu/d3d8_state.c"
SUITE = "test_d3d8_state_8e_registered"

MUTATIONS: list[dict] = [
    {
        "id": "t1066-state-8e-registered-at-wrong-title-address",
        "file": SOURCE,
        "old": "{0x003D7110u, handler_8e},",
        "new": "{0x003D7114u, handler_8e},",
        "targets": [SUITE],
        "why": (
            "the XBE exposes this stdcall setter at 0x003D7110; a neighbouring registration "
            "leaves the title entry unhandled."
        ),
    },
]
