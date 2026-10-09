"""Mutation for the original-grounded state 0x9C entry registration (T1066)."""

SOURCE = "src/gpu/d3d8_state.c"
SUITE = "test_d3d8_state_9c_registered"

MUTATIONS: list[dict] = [
    {
        "id": "t1066-state-9c-registered-at-wrong-title-address",
        "file": SOURCE,
        "old": "{0x003D6FD0u, handler_9c},",
        "new": "{0x003D6FD4u, handler_9c},",
        "targets": [SUITE],
        "why": (
            "the XBE exposes this stdcall setter at 0x003D6FD0; a neighbouring registration "
            "leaves the title entry unhandled."
        ),
    },
]
