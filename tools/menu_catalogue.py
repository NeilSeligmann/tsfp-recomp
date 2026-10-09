# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproduce the bounded T1706 widget-builder catalogue; read owner files only."""

from __future__ import annotations

import argparse
import csv
import re
from collections import Counter
from pathlib import Path

import capstone

from tools.codediff.boundaries import Function, load_function_table
from tools.frontend_labels import Labels, load
from tools.frontend_screens import WIDGET, decode, label_id
from tools.xppscan.image import Image

BEGIN = "<!-- T1706 records begin -->"
END = "<!-- T1706 records end -->"
BUILDER = re.compile(r"game_(?:menu|save_menu|save_screen|ingame_menu|results|scoreboard|ui)_")


def cell(text: str) -> str:
    return text.replace("|", "&#124;").replace("\r", " ").replace("\n", " ")


def text_for(labels: Labels, identifier: int) -> str:
    if identifier == 0:
        return "special pointer 0x4766D8 (not bank entry zero)"
    try:
        return repr(labels.text(identifier))
    except (KeyError, ValueError):
        return "unresolved/outside bank"


def catalogue(xbe: Path, iso: Path, functions: Path) -> tuple[str, dict[str, int]]:
    image, labels = Image(xbe), load(iso)
    table = load_function_table(functions)
    by_va = {f.entry_va: f for f in table}
    with Path("tools/data/function_names.csv").open() as stream:
        names = {int(r["entry_va"], 16): r["name"] for r in csv.DictReader(stream)}
    nav = {}
    for line_number, line in enumerate(Path("tools/data/menu_nav.txt").read_text().splitlines(), 1):
        match = re.match(r"menu (\S+) page=builder:(0x[0-9a-fA-F]+).*? conf=(\w+)", line)
        if match:
            nav[int(match[2], 16)] = (match[1], match[3], line_number)
    historical = {
        int(m[1], 16)
        for m in re.finditer(
            r"^\| [0-9a-f]{6} \| [^|]+ \| ([0-9a-f]{6}) \|",
            Path("docs/t1468-menu-page-descriptors.md").read_text(),
            re.M,
        )
    }
    disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    bodies, headers = {}, {}
    for f in table:
        body = list(disassembler.disasm(image.read(f.entry_va, f.size_bytes), f.entry_va))
        hits = [i.address for i in body if i.mnemonic == "call" and i.op_str == hex(WIDGET)]
        name = names.get(f.entry_va, f.name)
        named_builder = bool(BUILDER.match(name) and "build_" in name)
        if hits or named_builder or f.entry_va in nav or f.entry_va in historical:
            bodies[f.entry_va], headers[f.entry_va] = body, hits
    missing = (set(nav) | historical) - set(bodies)
    # Historical descriptor roots can lie outside the current table. Keep them
    # as explicit records, bounded at first INT3 or next known entry, not additions.
    for address in sorted(missing):
        stop = min(
            min((a for a in by_va if a > address), default=address + 0x1800), address + 0x1800
        )
        body = []
        for instruction in disassembler.disasm(image.read(address, stop - address), address):
            if instruction.mnemonic == "int3":
                break
            body.append(instruction)
        size = body[-1].address + body[-1].size - address if body else 0
        by_va[address] = Function(
            address, size, "historical_descriptor_builder_unlisted", False, address + size - 1
        )
        bodies[address] = body
        headers[address] = [
            i.address for i in body if i.mnemonic == "call" and i.op_str == hex(WIDGET)
        ]
    descriptors = {a: [] for a in bodies}
    for section in image.xbe.sections:
        if section.name not in (".data", ".rdata"):
            continue
        for address in range(section.virtual_addr, section.virtual_addr + section.raw_size - 15, 4):
            builder = image.u32(address)
            if builder not in bodies:
                continue
            hooks = tuple(image.u32(address + 4 * k) for k in range(1, 4))
            if all(h == 0 or h in by_va for h in hooks):
                descriptors[builder].append((address, hooks))
    counts = Counter(
        functions=len(table),
        builders=len(bodies),
        header_builders=sum(bool(h) for h in headers.values()),
        historical_unlisted=len(missing),
        descriptor_candidates=sum(len(d) for d in descriptors.values()),
    )
    lines = [BEGIN]
    for address, body in sorted(bodies.items()):
        title_candidates = []
        for event in decode(image, address, by_va[address].size_bytes):
            if event.kind != "widget":
                continue
            arg = event.args[1] if len(event.args) > 1 else "?"
            identifier = label_id(arg)
            title = (
                f"{identifier:#x} {text_for(labels, identifier)}"
                if identifier is not None
                else f"unresolved/dynamic `{arg}`"
            )
            title_candidates.append(f"`{event.address:#x}`: {title}")
        counts["title_associations"] += len(title_candidates)
        witnesses = []
        for index, instruction in enumerate(body):
            if instruction.mnemonic != "call" or instruction.op_str != "0x3d7b0":
                continue
            operand = "?"
            for previous in reversed(body[:index]):
                if previous.mnemonic == "call":
                    break
                if previous.mnemonic == "push":
                    operand = previous.op_str
                    break
            # An immediate push is an operand witness, not a CFG argument proof.
            if re.fullmatch(r"0x[0-9a-f]+|\d+", operand):
                identifier = int(operand, 0)
                witnesses.append(f"`{identifier:#x}@{instruction.address:#x}`")
                counts["immediate_label_witnesses"] += 1
            else:
                witnesses.append(f"`{operand}@{instruction.address:#x}`")
                counts["computed_label_witnesses"] += 1
        desc = (
            "; ".join(
                f"`{a:#x}`: " + "/".join(f"`{h:#x}`" for h in hooks)
                for a, hooks in descriptors[address]
            )
            or "none under contract"
        )
        header = "; ".join(title_candidates) or (
            "calls "
            + ", ".join(f"`{h:#x}`" for h in headers[address])
            + "; decoder has no title event"
            if headers[address]
            else "shared/wrapper; no direct header"
        )
        if address in nav:
            identifier, confidence, line_number = nav[address]
            measured = f"**{confidence}** `{identifier}` line {line_number}"
            counts["nav_" + confidence.lower()] += 1
        else:
            measured = "INFERRED; no menu_nav row"
        lines.extend(
            [
                f"### {address:#08x}: {names.get(address, by_va[address].name)}",
                "",
                f"Descriptor candidates (gate / draw / thunk): {cell(desc)}.",
                "",
                f"Header calls and INFERRED title association: {cell(header)}.",
                "",
                "Direct label-call witnesses (static): "
                + cell("; ".join(witnesses) or "none; inherited/computed elsewhere")
                + ".",
                "",
                f"menu_nav identity evidence: {measured}.",
                "",
            ]
        )
    lines.append(END)
    return "\n".join(lines), dict(counts)


def marked_records(doc: str) -> str:
    """Extract records; explanatory prose may mention the end marker first."""
    start = doc.index(BEGIN)
    return doc[start : doc.index(END, start) + len(END)]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--functions", type=Path, default=Path("generated/retail/functions.csv"))
    parser.add_argument(
        "--check", type=Path, help="compare only the marked records in an existing doc"
    )
    args = parser.parse_args()
    result, counts = catalogue(args.xbe, args.iso, args.functions)
    if args.check:
        doc = args.check.read_text()
        existing = marked_records(doc)
        if result != existing:
            raise SystemExit("catalogue differs; regenerate and review the input changes")
        print(f"T1706 catalogue matches: {counts}")
    else:
        print(result)
        print(f"\nCounts: {counts}")


if __name__ == "__main__":
    main()
