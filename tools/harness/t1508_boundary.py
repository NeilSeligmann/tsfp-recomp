# SPDX-License-Identifier: GPL-3.0-or-later
"""Complete synthetic boundary packet validation; no execution capability."""

from __future__ import annotations

import json
import re
from collections.abc import Mapping
from dataclasses import dataclass
from types import MappingProxyType

EXECUTION_CAPABILITY = 0
GPRS = frozenset(("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"))
HASH_FIELDS = frozenset(
    ("root_sha256", "closure_sha256", "backend_sha256", "code_sha256", "memory_sha256")
)
FIELDS = HASH_FIELDS | frozenset(
    (
        "schema",
        "capability",
        "boundary_eip",
        "continuation",
        "gpr",
        "eflags",
        "eflags_valid_mask",
        "raw80",
        "tags",
        "cw",
        "sw",
        "xmm",
        "mxcsr",
        "memory_owner",
        "memory_epoch",
    )
)


def exact_keys(value: object, keys: frozenset[str]) -> dict:
    if not isinstance(value, dict) or set(value) != keys:
        raise ValueError("missing or unknown fields")
    return value


def unsigned(value: object, bits: int) -> int:
    if type(value) is not int or not 0 <= value < 1 << bits:
        raise ValueError(f"expected unsigned{bits}")
    return value


def hex_value(value: object, size: int) -> str:
    if not isinstance(value, str) or re.fullmatch(f"[0-9a-f]{{{size * 2}}}", value) is None:
        raise ValueError("invalid canonical hex width")
    return value


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate field: {key}")
        result[key] = value
    return result


@dataclass(frozen=True)
class BoundaryPacket:
    """Immutable validated observations, never authority to execute a guest tail."""

    provenance: Mapping[str, str]
    boundary_eip: int
    continuation: tuple[int, int]
    gpr: Mapping[str, int]
    eflags: int
    raw80: tuple[str, ...]
    tags: int
    cw: int
    sw: int
    xmm: tuple[str, ...]
    mxcsr: int
    memory_epoch: int


def parse_boundary(
    payload: str, *, provenance: Mapping[str, str], epoch: int, boundary_eip: int
) -> BoundaryPacket:
    """Validate against external authenticated pins, not self-asserted packet identity."""
    if set(provenance) != HASH_FIELDS:
        raise ValueError("complete external provenance required")
    expected = {key: hex_value(value, 32) for key, value in provenance.items()}
    unsigned(epoch, 64)
    unsigned(boundary_eip, 32)
    data = exact_keys(json.loads(payload, object_pairs_hook=unique_object), FIELDS)
    if type(data["schema"]) is not int or data["schema"] != 1:
        raise ValueError("unknown schema")
    if type(data["capability"]) is not int or data["capability"] != EXECUTION_CAPABILITY:
        raise ValueError("execution is unsupported")
    actual = {key: hex_value(data[key], 32) for key in HASH_FIELDS}
    if actual != expected:
        raise ValueError("stale provenance")
    if unsigned(data["memory_epoch"], 64) != epoch:
        raise ValueError("stale memory epoch")
    if unsigned(data["boundary_eip"], 32) != boundary_eip:
        raise ValueError("unexpected boundary")
    if data["memory_owner"] != "independent-tail":
        raise ValueError("unknown memory ownership")
    if unsigned(data["eflags_valid_mask"], 32) != 0xFFFFFFFF:
        raise ValueError("incomplete flags")
    gpr = exact_keys(data["gpr"], GPRS)
    registers = {key: unsigned(value, 32) for key, value in gpr.items()}
    continuation = exact_keys(data["continuation"], frozenset(("expected_target", "pre_ret_esp")))

    def slots(name: str, width: int) -> tuple[str, ...]:
        values = data[name]
        if not isinstance(values, list) or len(values) != 8:
            raise ValueError("all eight physical slots required")
        return tuple(hex_value(value, width) for value in values)

    return BoundaryPacket(
        MappingProxyType(actual),
        boundary_eip,
        (unsigned(continuation["expected_target"], 32), unsigned(continuation["pre_ret_esp"], 32)),
        MappingProxyType(registers),
        unsigned(data["eflags"], 32),
        slots("raw80", 10),
        unsigned(data["tags"], 16),
        unsigned(data["cw"], 16),
        unsigned(data["sw"], 16),
        slots("xmm", 16),
        unsigned(data["mxcsr"], 32),
        epoch,
    )
