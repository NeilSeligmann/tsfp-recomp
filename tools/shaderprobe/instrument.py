# SPDX-License-Identifier: GPL-3.0-or-later
"""Instrument a COPY of the lifted C so a boot logs the shader key as the title forms it.

The host binary links the lifted chunks. The key global is written by lifted game code
(no XDK boundary sees those stores), so the only way to watch every write is to log from
inside the lift. This module rewrites a copy of the lift directory, never the original:

  * at the entry of each listed function it adds one `fprintf(stderr, ...)` of the return
    address and the first stack arguments, so a call is attributed to its call site;
  * after every `MEM32(<key global>) = ...;` statement it logs the stored value together
    with the enclosing function and the nearest preceding label (the instruction address
    region of the store).

The lift writes plain `MEM32(0x...) = expr;` statements, so a line-based rewrite is exact
for that shape. Any OTHER mention of the key global that could be a write in a form this
rewrite does not log (a `MEMn(...)` store of any width, a compound assignment, an
address taken for a pointer store) is reported as `unhandled`, and the command line
refuses to continue while any exists, because an unlogged writer would make "the game
formed exactly these keys" false without any visible sign. Stores through a pointer held
in a register cannot be seen at all by a text rewrite. `tools.shaderscan xrefs` checks that
side, and the limit is recorded in docs/shader-inputs.md section 11.

Nothing here reads or prints a shader. The output is C text that logs addresses and dwords.
"""

from __future__ import annotations

import re
import shutil
from dataclasses import dataclass, field
from pathlib import Path

#: The chunk file pattern of a lift directory.
CHUNK_GLOB = "recomp_[0-9][0-9][0-9][0-9].c"
#: The include the added fprintf needs. Added once per patched chunk.
STDIO_INCLUDE = "#include <stdio.h>\n"
ANCHOR_INCLUDE = '#include "recomp_funcs.h"\n'
#: How many stack arguments an entry record shows.
ENTRY_ARGUMENTS = 5

_FUNCTION = re.compile(r"^void sub_([0-9A-F]{8})\(void\)\s*$")
_LABEL = re.compile(r"^loc_([0-9A-F]{8}): ;\s*$")
_STORE = re.compile(r"^(\s*)MEM32\((0x[0-9A-Fa-f]+)\) = (.*);\s*$")
#: Every `MEMn(address)` operand. Plain full-width reads are the only legal other mention.
_ANY_WIDTH_MENTION = re.compile(r"MEM(8|16|32)\((0x[0-9A-Fa-f]+)\)")


@dataclass
class PatchReport:
    """What one patch run did. Everything is counts and addresses."""

    entries: set[int] = field(default_factory=set)
    #: (function, label) of every logged store, in file order
    stores: list[tuple[int, int]] = field(default_factory=list)
    #: (function, line text stripped) of every other mention that is not a plain read
    unhandled: list[tuple[int, str]] = field(default_factory=list)


def entry_statement(address: int) -> str:
    """The C statement logged at the entry of the function at `address`."""
    arguments = ", ".join(f"MEM32(esp + {4 * (i + 1)})" for i in range(ENTRY_ARGUMENTS))
    shown = ",".join(["0x%X"] * ENTRY_ARGUMENTS)
    return (
        f'fprintf(stderr, "t51: enter fn=0x{address:08X} ret=0x%X a={shown}\\n", '
        f"MEM32(esp), {arguments});"
    )


def store_statement(function: int, label: int, key_global: int) -> str:
    """The C statement logged after a store to the key global."""
    return (
        f'fprintf(stderr, "t51: store fn=0x{function:08X} at=0x{label:08X} val=0x%X\\n", '
        f"MEM32(0x{key_global:X}));"
    )


def patch_chunk(text: str, entries: set[int], key_global: int) -> tuple[str, PatchReport]:
    """Rewrite one chunk's text. Pure, so it is testable on a stand-in chunk."""
    report = PatchReport()
    lines = text.splitlines(keepends=True)
    output: list[str] = []
    function: int | None = None
    pending_entry: int | None = None
    label: int | None = None
    for line in lines:
        output.append(line)
        match = _FUNCTION.match(line)
        if match:
            function = int(match.group(1), 16)
            label = None
            pending_entry = function if function in entries else None
            continue
        match = _LABEL.match(line)
        if match:
            label = int(match.group(1), 16)
            if pending_entry is not None and label == pending_entry:
                output.append("    " + entry_statement(pending_entry) + "\n")
                report.entries.add(pending_entry)
                pending_entry = None
            continue
        store = _STORE.match(line)
        if store and int(store.group(2), 16) == key_global:
            assert function is not None and label is not None, "store outside a function"
            output.append(store.group(1) + store_statement(function, label, key_global) + "\n")
            report.stores.append((function, label))
            continue
        for mention in _ANY_WIDTH_MENTION.finditer(line):
            if int(mention.group(2), 16) != key_global:
                continue
            tail = line[mention.end() :].lstrip()
            head = line[: mention.start()].rstrip()
            assigned = re.match(r"(\|=|&=|\^=|\+=|-=|<<=|>>=|\+\+|--|=[^=])", tail) is not None
            if assigned or head.endswith(("++", "--", "&")):
                assert function is not None
                report.unhandled.append((function, line.strip()))
    new_text = "".join(output)
    if report.entries or report.stores:
        assert ANCHOR_INCLUDE in new_text, "chunk lacks the recomp_funcs.h include anchor"
        new_text = new_text.replace(ANCHOR_INCLUDE, ANCHOR_INCLUDE + STDIO_INCLUDE, 1)
    return new_text, report


def instrument_directory(
    source: Path,
    destination: Path,
    entries: set[int],
    key_global: int,
    extra_files: list[Path] | None = None,
) -> PatchReport:
    """Copy `source` to `destination` and patch the chunks in the copy.

    Refuses an existing destination (an old copy is not trusted to be current) and
    refuses a result in which any requested entry was not found exactly once or no store
    was logged, because either would make an empty log mean nothing.
    """
    if destination.exists():
        raise FileExistsError(f"{destination} exists; remove it so the copy is current")
    if not any(source.glob(CHUNK_GLOB)):
        raise FileNotFoundError(f"no lifted chunks in {source}")
    shutil.copytree(source, destination)
    for extra in extra_files or []:
        shutil.copy2(extra, destination / extra.name)
    total = PatchReport()
    found_entries: list[int] = []
    for chunk in sorted(destination.glob(CHUNK_GLOB)):
        new_text, report = patch_chunk(chunk.read_text(), entries, key_global)
        if report.entries or report.stores:
            chunk.write_text(new_text)
        found_entries.extend(sorted(report.entries))
        total.entries |= report.entries
        total.stores.extend(report.stores)
        total.unhandled.extend(report.unhandled)
    duplicated = sorted({a for a in found_entries if found_entries.count(a) > 1})
    if duplicated:
        raise ValueError("entry defined in more than one chunk: " + ", ".join(map(hex, duplicated)))
    missing = sorted(entries - total.entries)
    if missing:
        raise ValueError("entry function(s) not found in the lift: " + ", ".join(map(hex, missing)))
    if not total.stores:
        raise ValueError(f"no store to {key_global:#x} found in the lift")
    return total
