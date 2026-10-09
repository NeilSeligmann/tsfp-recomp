# SPDX-License-Identifier: GPL-3.0-or-later
"""Compiled defects for the T1066 original-backed method packet emitter."""

MUTATIONS = [
    {
        "id": "d3d8-method-packet-wrong-method-base",
        "file": "src/gpu/d3d8_method_packet.c",
        "old": "0x00041940u + method_index * 4u",
        "new": "0x00041944u + method_index * 4u",
        "targets": ["test_d3d8_method_packet"],
        "why": (
            "The packet header is the public contract consumed by NV2A. A method-base error "
            "emits a valid-looking but different register write; the caller-specific literal "
            "and arbitrary-index control both need to reject this mutant."
        ),
    },
    {
        "id": "d3d8-method-packet-wrong-byte-lane",
        "file": "src/gpu/d3d8_method_packet.c",
        "old": "((input & 0xFFu) << 16)",
        "new": "((input & 0xFFu) << 8)",
        "targets": ["test_d3d8_method_packet"],
        "why": (
            "The original shuffles the low input byte into output bits 16..23. Moving it to "
            "bits 8..15 corrupts the packet parameter while retaining every other byte, so "
            "several asymmetric inputs distinguish this from a passing identity/default case."
        ),
    },
]
