"""Mutation for the original-grounded EdgeAntiAlias entry registration (T1066)."""

SOURCE = "src/gpu/d3d8_state.c"
SUITE = "test_d3d8_state_97_registered"

MUTATIONS: list[dict] = [
    {
        "id": "t1066-state-97-registered-at-wrong-title-address",
        "file": SOURCE,
        "old": "{0x003D6F90u, handler_97},",
        "new": "{0x003D6F94u, handler_97},",
        "targets": [SUITE],
        "why": (
            "the XBE exposes this stdcall setter at 0x003D6F90; a neighbouring registration "
            "leaves the title entry unhandled."
        ),
    },
]
