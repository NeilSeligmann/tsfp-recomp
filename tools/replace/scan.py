# SPDX-License-Identifier: GPL-3.0-or-later
"""Find the `GAME_REPLACE(...)` registrations in src/game/*.c.

A source scan is used ONLY to decide which lifted symbols must be made weak before the
lifted chunks are compiled, which has to happen before any binary containing the
registry exists. It is never evidence that a replacement exists: that is the registry
dumped from a linked binary (`tools/replace/manifest.py`), and the two are cross-checked
so a registration hidden behind a comment or an `#if 0` shows up as a disagreement.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .input_abi import INPUT_SETS, validate_inputs

CONVENTIONS = ("cdecl", "stdcall", "thiscall", "fastcall")
RETURNS = ("u32", "void")

#: The eight hex digits are UPPER CASE because they are pasted into the lifter's symbol
#: name, which is upper case (`sub_0003C330`). Matching only upper case is deliberate: a
#: lower-case registration would compile, define a symbol nothing calls, and silently
#: replace nothing.
REGISTRATION = re.compile(
    r"\bGAME_REPLACE(?P<mode>_EXACT_INPUTS|_EXACT)?\(\s*(?P<va>[0-9A-F]{8})\s*,\s*(?P<cc>"
    + "|".join(CONVENTIONS)
    + r")\s*,\s*(?P<count>\d+)\s*,\s*(?P<ret>"
    + "|".join(RETURNS)
    + r")\s*,\s*(?:(?P<inputs>ecx_esi|ecx|esi)\s*,\s*)?(?P<fn>[A-Za-z_]\w*)\s*\)"
)
ANY_REGISTRATION = re.compile(r"\bGAME_REPLACE(?:_EXACT(?:_INPUTS)?)?\s*\(")
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT = re.compile(r"//[^\n]*")
#: The macro's own definition lives in game_replace.h and its usage example sits in a
#: comment, so only `.c` files are scanned and comments are removed first.
SOURCE_SUFFIX = ".c"
#: `game_manifest.c` and `game_registry.c` are infrastructure, never registrations.
INFRASTRUCTURE = frozenset({"game_manifest.c", "game_registry.c"})


class ScanError(ValueError):
    """A registration that looks like one but cannot be accepted."""


@dataclass(frozen=True)
class Registration:
    va: int
    convention: str
    stack_args: int
    returns: str
    function: str
    source: str
    exact_registers: bool = False
    register_inputs: tuple[str, ...] | None = None

    @property
    def symbol(self) -> str:
        return f"sub_{self.va:08X}"


def strip_comments(text: str) -> str:
    """Source with comments blanked, keeping line structure."""

    def blank(match: re.Match[str]) -> str:
        return re.sub(r"[^\n]", " ", match.group(0))

    return LINE_COMMENT.sub(blank, BLOCK_COMMENT.sub(blank, text))


def scan_text(text: str, source: str) -> list[Registration]:
    code = strip_comments(text)
    found = []
    for match in REGISTRATION.finditer(code):
        explicit = match["mode"] == "_EXACT_INPUTS"
        if explicit != (match["inputs"] is not None):
            raise ScanError(f"{source}: missing or conflicting explicit input metadata")
        inputs = INPUT_SETS[match["inputs"]] if explicit else None
        try:
            validate_inputs(inputs, match["cc"], int(match["count"]), ())
        except ValueError as error:
            raise ScanError(f"{source}: {error}") from error
        found.append(
            Registration(
                va=int(match["va"], 16),
                convention=match["cc"],
                stack_args=int(match["count"]),
                returns=match["ret"],
                function=match["fn"],
                source=source,
                exact_registers=match["mode"] is not None,
                register_inputs=inputs,
            )
        )
    uses = len(ANY_REGISTRATION.findall(code))
    if uses != len(found):
        raise ScanError(
            f"{source}: {uses} GAME_REPLACE use(s) but {len(found)} well-formed. A VA must be "
            "eight UPPER CASE hex digits without 0x, then a convention, a stack argument "
            "count, u32 or void, and the function name."
        )
    return found


def scan_directory(game_dir: Path) -> list[Registration]:
    """Every registration in `game_dir`, sorted by address, duplicates refused."""
    found: list[Registration] = []
    for path in sorted(game_dir.glob(f"*{SOURCE_SUFFIX}")):
        if path.name in INFRASTRUCTURE:
            continue
        found.extend(scan_text(path.read_text(encoding="utf-8"), path.name))
    # T1510: registrations in an `.inc` file are scanned too (x87_roots.inc is included by
    # x87_roots.c unconditionally, so it is part of every build's registry).
    for path in sorted(game_dir.glob("*.inc")):
        found.extend(scan_text(path.read_text(encoding="utf-8"), path.name))
    seen: dict[int, Registration] = {}
    for registration in found:
        if registration.va in seen:
            other = seen[registration.va]
            raise ScanError(
                f"{registration.va:#010x} is registered twice: {other.source} "
                f"({other.function}) and {registration.source} ({registration.function}). "
                "Two strong definitions of one symbol cannot both link."
            )
        seen[registration.va] = registration
    return sorted(found, key=lambda registration: registration.va)
