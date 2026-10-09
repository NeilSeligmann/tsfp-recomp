"""Mutation for the original-grounded PSTextureModes entry registration (T1066)."""

SOURCE = "src/gpu/d3d8_state.c"
SUITE = "test_d3d8_state_88_registered"

MUTATIONS: list[dict] = [
    {
        "id": "t1066-state-88-registered-at-wrong-title-address",
        "file": SOURCE,
        "old": "{0x003D6C60u, handler_88},",
        "new": "{0x003D6C64u, handler_88},",
        "targets": [SUITE],
        "why": (
            "the XBE exposes this stdcall setter at 0x003D6C60; a neighbouring registration "
            "leaves the title entry unhandled."
        ),
    },
]
