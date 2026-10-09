# SPDX-License-Identifier: GPL-3.0-or-later
"""Exact scratch-only observation insertion; execution capability remains zero."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path

EXECUTION_CAPABILITY = 0
CONTRACT = (
    Path(__file__).resolve().parents[2] / "docs/data/t1508-boundary-instrumentation-contract.json"
)


def instrument_boundary(
    source: str,
    original: bytes,
    *,
    flags_symbol: str | None = None,
    raw_state_symbol: str | None = None,
) -> str:
    """Require complete provider symbols; their actual correctness remains unvalidated."""
    contract = json.loads(CONTRACT.read_text())
    if original.hex() != contract["closure_bytes"]:
        raise ValueError("unauthenticated original bytes")
    if source.count(contract["body"]) != 1:
        raise ValueError("missing, duplicated or changed exact body")
    body = contract["body"]
    if hashlib.sha256(body.encode()).hexdigest() != contract["body_sha256"]:
        raise ValueError("changed contract body")
    for symbol in (flags_symbol, raw_state_symbol):
        if symbol is None or re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol) is None:
            raise ValueError("complete flags/raw-state providers required")
    write = "    MEM32(eax) = 0;\n"
    if body.count(write) != 1:
        raise ValueError("ambiguous write boundary")
    capture = (
        "    t1508_observe_boundary(0x003BB658u, eax, ecx, edx, ebx, esp, ebp, esi, edi, "
        f"{flags_symbol}, &{raw_state_symbol}); /* observation only, capability0 */\n"
    )
    return source.replace(body, body.replace(write, write + capture), 1)
