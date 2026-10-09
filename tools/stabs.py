# SPDX-License-Identifier: GPL-3.0-or-later
"""Parse `stdump` output from a symbol-bearing PlayStation 2 executable.

TimeSplitters 2's US OPM Demo 53 build (`TS2/SLUS_999.99`, 2001-10-05) shipped
unstripped, with a 7.3 MB `.mdebug` stabs section and an 8,099-entry `.symtab`.
Running `stdump` (from CCC) against it recovers the Free Radical engine's real
type definitions and function signatures.

Measured from our own extraction:

| | |
|---|---|
| struct/union definitions | 394 distinct |
| offset-annotated fields | 3,672, only 1.06% placeholder-named |
| typedefs / enums | 441 / 14 |
| functions with address and size | 3,976 |
| …with named, typed parameters | 2,483 |
| Free Radical source files | 120 across 34 subsystem directories |

**This is a donor, not the target.** TS2 is the previous game; TSFP is ours. Use
the field *names and order* as hypotheses and re-derive the byte offsets against
TSFP, because the donor is MIPS EE and the target is x86, and array sizes are
version-stamped. See the spec's symbol strategy.

Generate the input with:

    stdump types     SLUS_999.99 > types.log
    stdump functions SLUS_999.99 > functions.cpp
"""

from __future__ import annotations

import re
from collections.abc import Iterable, Iterator
from dataclasses import dataclass, field

# struct prop_s { // 0x280
RE_STRUCT_OPEN = re.compile(r"^(struct|union)\s+(\w+)\s*\{(?:\s*//\s*(0x[0-9a-fA-F]+))?")
# /* 0x044 */ float pos[3];
RE_FIELD = re.compile(r"^\s*/\*\s*(0x[0-9a-fA-F]+)\s*\*/\s*(.+?)\s*(\w+)\s*(\[[^\]]*\])?\s*;")
# // FILE -- /home/hzala/game/boss/boss.c
RE_FILE = re.compile(r"^//\s*FILE\s*--\s*(.+?)\s*$")
# /* 002000c8 0000007c */ int bossCheckParm(/* s1 17 */ char *check) {
RE_FUNCTION = re.compile(r"^/\*\s*([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})\s*\*/\s*(.+?)\s*\{?\s*$")
# a parameter annotated with its storage: /* a0 4 */ int psm
RE_PARAM_STORAGE = re.compile(r"/\*\s*[a-z0-9()+\-]+\s+\d+\s*\*/")

PLACEHOLDER_FIELD = re.compile(r"^(field_|unk|pad$|_pad|unknown)", re.IGNORECASE)


@dataclass(frozen=True)
class StructField:
    offset: int
    type_name: str
    name: str
    array_suffix: str = ""

    @property
    def is_placeholder(self) -> bool:
        """True for names carrying no information beyond their offset."""
        return bool(PLACEHOLDER_FIELD.match(self.name))


@dataclass
class StructDef:
    kind: str
    name: str
    size: int | None = None
    fields: list[StructField] = field(default_factory=list)


@dataclass(frozen=True)
class FunctionDef:
    address: int
    size: int
    signature: str
    source_file: str | None
    has_named_params: bool

    @property
    def name(self) -> str | None:
        """The identifier immediately before the parameter list, if parsable."""
        match = re.search(r"([A-Za-z_]\w*)\s*\(", self.signature)
        return match.group(1) if match else None


def parse_types(lines: Iterable[str]) -> list[StructDef]:
    """Parse `stdump types` output into struct definitions.

    Only top-level aggregates are collected. Nested anonymous members appear as
    ordinary fields, which is what a consumer applying layouts wants.
    """
    structs: list[StructDef] = []
    current: StructDef | None = None
    depth = 0
    for raw in lines:
        line = raw.rstrip("\n")
        if current is None:
            match = RE_STRUCT_OPEN.match(line)
            if match:
                size = int(match.group(3), 16) if match.group(3) else None
                current = StructDef(kind=match.group(1), name=match.group(2), size=size)
                depth = 1
            continue

        depth += line.count("{") - line.count("}")
        field_match = RE_FIELD.match(line)
        if field_match:
            current.fields.append(
                StructField(
                    offset=int(field_match.group(1), 16),
                    type_name=field_match.group(2).strip(),
                    name=field_match.group(3),
                    array_suffix=field_match.group(4) or "",
                )
            )
        if depth <= 0:
            structs.append(current)
            current = None
    return structs


def parse_functions(lines: Iterable[str]) -> list[FunctionDef]:
    """Parse `stdump functions` output, tracking the current source file."""
    functions: list[FunctionDef] = []
    source: str | None = None
    for raw in lines:
        line = raw.rstrip("\n")
        file_match = RE_FILE.match(line)
        if file_match:
            source = file_match.group(1)
            continue
        fn_match = RE_FUNCTION.match(line)
        if fn_match:
            signature = fn_match.group(3)
            functions.append(
                FunctionDef(
                    address=int(fn_match.group(1), 16),
                    size=int(fn_match.group(2), 16),
                    signature=signature,
                    source_file=source,
                    has_named_params=bool(RE_PARAM_STORAGE.search(signature)),
                )
            )
    return functions


def engine_subsystems(functions: Iterable[FunctionDef]) -> dict[str, int]:
    """Map engine subsystem directory to file count, from `/game/<subsystem>/`.

    Recovers the Free Radical module decomposition, which is the part that
    transfers to TSFP even though addresses and offsets do not.
    """
    per_dir: dict[str, set[str]] = {}
    for function in functions:
        if not function.source_file or "/game/" not in function.source_file:
            continue
        match = re.search(r"/game/([^/]+)/", function.source_file)
        if match:
            per_dir.setdefault(match.group(1), set()).add(function.source_file)
    return {name: len(files) for name, files in sorted(per_dir.items())}


def iter_named_fields(structs: Iterable[StructDef]) -> Iterator[tuple[str, StructField]]:
    """Yield (struct name, field) for every field carrying a real name."""
    for struct in structs:
        for item in struct.fields:
            if not item.is_placeholder:
                yield struct.name, item
