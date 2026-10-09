# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for the T870 CreateDevice title declaration header (0x003E2C68, flags 0x10 at 0x003E2C6C).

TARGET. `test_d3d8_shader` (the poisoned span checked right after `d3d8_shader_create`).
"""

SHADER = "src/gpu/d3d8_shader.c"


def mutation(identifier: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"t870-{identifier}",
        "file": SHADER,
        "old": old,
        "new": new,
        "targets": ["test_d3d8_shader"],
        "why": why,
    }


MUTATIONS: list[dict] = [
    mutation(
        "flags-value",
        "    d3d8_guest_store32(0x003E2C6Cu, 0x10u);\n",
        "    d3d8_guest_store32(0x003E2C6Cu, 0x11u);\n",
        "the header flags are 0x10 (program class), not 0x11.",
    ),
    mutation(
        "flags-dropped",
        "    d3d8_guest_store32(0x003E2C6Cu, 0x10u);\n",
        "    d3d8_guest_store32(0x003E2C6Cu, 0u);\n",
        "without flags 0x10 the first select never writes the program mode.",
    ),
    mutation(
        "flags-address",
        "    d3d8_guest_store32(0x003E2C6Cu, 0x10u);\n",
        "    d3d8_guest_store32(0x003E2C70u, 0x10u);\n",
        "the flags are header word 1.",
    ),
    mutation(
        "zero-count-short",
        "    for (uint32_t offset = 0u; offset < 0x46u * 4u; offset += 4u) {\n",
        "    for (uint32_t offset = 0u; offset < 0x45u * 4u; offset += 4u) {\n",
        "the original clears all 0x46 dwords (the last cache word included).",
    ),
    mutation(
        "zero-count-long",
        "    for (uint32_t offset = 0u; offset < 0x46u * 4u; offset += 4u) {\n",
        "    for (uint32_t offset = 0u; offset < 0x47u * 4u; offset += 4u) {\n",
        "the clear stops before the built-in declaration header at 0x003E2D80.",
    ),
    mutation(
        "zero-start",
        "        d3d8_guest_store32(0x003E2C68u + offset, 0u);\n",
        "        d3d8_guest_store32(0x003E2C6Cu + offset, 0u);\n",
        "the clear starts at the header, 0x003E2C68.",
    ),
]
