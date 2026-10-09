# SPDX-License-Identifier: GPL-3.0-or-later
"""The DSOUND call surface and each address's stack-argument count, read from the image itself.

The surface is the table compiled into `src/audio/dsound_hle.c`
(committed, so a fresh clone has it).
Argument counts come from the callee's own `ret imm16`, following the five-byte `jmp rel32` thunks
(the same evidence `tools/xdk_abi.py` uses), and are checked against the generated ABI table when it
exists. The two disagree on `ecx`: see `register_argument_claims`.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from tools.dsoundscan.image import Image

REPO = Path(__file__).resolve().parents[2]
HLE_SOURCE = REPO / "src" / "audio" / "dsound_hle.c"
ABI_TABLE = REPO / "src" / "xbox" / "xdk_abi.inc"

_ROW = re.compile(r'\{(0x[0-9a-fA-F]+),\s*(NULL|"[^"]*"),\s*(\d+)\}')
_ABI = re.compile(
    r"\{(0x[0-9A-Fa-f]+)u,\s*XDK_CC_(\w+),\s*(\d+)u,\s*(\d+)u,\s*XDK_ABI_FROM_(\w+),\s*(\d+)u\}"
)


@dataclass(frozen=True)
class SurfaceRow:
    address: int
    name: str | None
    sites: int


def read_surface(source: Path = HLE_SOURCE) -> list[SurfaceRow]:
    """The DSOUND rows compiled into dsound_hle.c, in declared order."""
    rows = []
    for match in _ROW.finditer(source.read_text()):
        name = None if match.group(2) == "NULL" else match.group(2).strip('"')
        rows.append(SurfaceRow(int(match.group(1), 16), name, int(match.group(3))))
    return rows


def stack_arguments(image: Image, address: int) -> int | None:
    """Stack-argument count from the callee's own `ret imm16`, or None when none/ambiguous.

    A bare `ret` leaves the count unsettled (cdecl-with-arguments and no-argument callee-cleanup
    both end in one), and so does a function whose returns disagree, so both give None.
    """
    for _hop in range(4):
        first = next(iter(image.decode(address, address + 5).instructions), None)
        if first is not None and first.mnemonic == "jmp" and first.size == 5:
            address = first.operands[0].imm & 0xFFFFFFFF
            continue
        break
    counts = set()
    bare = False
    for item in image.function_extent(address).values():
        if item.mnemonic != "ret":
            continue
        if item.operands:
            counts.add(item.operands[0].imm // 4)
        else:
            bare = True
    if len(counts) == 1 and not bare:
        return counts.pop()
    return None


def generated_abi(path: Path = ABI_TABLE) -> dict[int, tuple[str, int, int]]:
    """address -> (convention, stack arguments, register arguments) from the generated table."""
    if not path.exists():
        return {}
    return {
        int(match.group(1), 16): (match.group(2), int(match.group(3)), int(match.group(4)))
        for match in _ABI.finditer(path.read_text())
    }
