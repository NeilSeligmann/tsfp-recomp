# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded original-byte Map Maker builder catalogue (T1714), stdout only."""

from __future__ import annotations

import argparse
import csv
import hashlib
import struct
from pathlib import Path

import capstone

from tools.frontend_labels import load
from tools.xppscan.image import Image

HEADER = 0x75B70
LABEL = 0x3D7B0


def catalogue(root: Path, xbe: Path, iso: Path) -> str:
    image = Image(xbe)
    labels = load(iso)
    with (root / "tools/data/function_names.csv").open() as stream:
        rows = list(csv.DictReader(stream))
    names = {int(r["entry_va"], 0): r for r in rows}
    with (root / "tools/data/function_additions.csv").open() as stream:
        for row in csv.DictReader(stream):
            names.setdefault(int(row["entry_va"], 0), {"name": "unnamed_original_addition"})
    addresses = sorted(names)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    bodies = {}
    for index, start in enumerate(addresses[:-1]):
        if not (
            names[start]["name"].startswith("game_mapedit_")
            or 0x312000 <= start < 0x338200
            or start in (0x2BF830, 0x2C1830)
        ):
            continue
        # Never decode into the next named function, including branch-local returns.
        body = []
        for insn in decoder.disasm(image.read(start, addresses[index + 1] - start), start):
            if insn.mnemonic == "int3":
                break
            body.append(insn)
        bodies[start] = body
    candidates = {
        start
        for start, body in bodies.items()
        if (
            names[start]["name"].startswith("game_mapedit_")
            and any(i.mnemonic == "call" and i.op_str == hex(HEADER) for i in body)
        )
        or (
            (
                names[start]["name"].startswith(
                    (
                        "game_mapedit_build_",
                        "game_menu_build_",
                        "game_ui_build_",
                        "game_save_screen_build_",
                    )
                )
                or start == 0x315720
            )
            and any(
                word in names[start]["name"]
                for word in (
                    "widget",
                    "page",
                    "popup",
                    "item_group",
                    "choice_list",
                    "item_text",
                    "scroll_panel",
                )
            )
        )
    }
    descriptors = {}
    # A descriptor candidate must have a selected builder first and all three hooks
    # zero or known function entries. Report candidates, never assume reachability.
    for section in image.xbe.sections:
        if section.name not in (".data", ".rdata"):
            continue
        data = image.read(section.virtual_addr, section.raw_size)
        for offset in range(0, len(data) - 15, 4):
            words = struct.unpack_from("<4I", data, offset)
            if words[0] in candidates and all(v == 0 or v in names for v in words[1:]):
                descriptors.setdefault(words[0], []).append(
                    (section.virtual_addr + offset, words[1:])
                )
    lines = [
        "# T1714: Editor page and widget builder catalogue",
        "",
        "2026-10-09, codex-w2. Map Maker UI (T1690). Read with "
        "[UI framework](t-ui-framework.md), [editor state](t1712-editor-state.md), "
        "[mode contracts](t1715-mode-contract.md) and [decompilation tree](decomp-tree.md).",
        "",
        "## Evidence and scope",
        "",
        f"Original XBE SHA-256 `{hashlib.sha256(image.raw).hexdigest()}`. "
        "Source baseline `fd30bd596`; project Capstone " + capstone.__version__ + ". "
        "Original instruction operands and English bank strings are MEASURED static evidence. "
        "Existing function names and page meanings remain INFERRED. "
        "No host execution, xemu reference, replacement or proof is claimed.",
        "",
        f"The bounded census contains **{len(candidates)} records**: every named "
        "`game_mapedit_*` original caller of widget header `0x75B70`, plus named "
        "`build_*` page/widget/popup/item-group/appearance-row helpers and wrappers, "
        "including menu/UI/save-named builders in the editor region 0x312000..0x3381FF; "
        "shared Bot/Weapon Set builders 0x2BF830/0x2C1830 are also included. "
        "String/launch-structure builders are excluded because they do not construct widgets. "
        "The scope is original named functions, not a claim that every runtime variant was opened. "
        "Each body ends before the next named function or first INT3; returns do not truncate "
        "branch variants. Descriptor candidates require four aligned dwords in `.data`/`.rdata`, "
        "with builder first and zero or named entries for the three hooks. "
        "Candidates establish stored contracts, not runtime reachability.",
        "",
        "Widget header `0x75B70` and row helper `0x79D60` use cdecl pushed arguments "
        "(see framework). Descriptor `{builder, gate, draw, thunk}` is copied into the "
        "0x23C-byte page at `0x6FEBE8`; row nodes are 0x10C bytes via `[0x7BB1DC]`. "
        "Rows hold relocated string pointers. Label IDs differ from row IDs and option IDs. "
        "Settings pointers below refer to `[0x7844A8]`, not the page allocation.",
        "",
        "For each direct `0x3D7B0` call, the nearest preceding push before another call "
        "is reported; immediate operands give IDs, registers/memory remain computed. "
        "This is a local instruction witness, not path-sensitive symbolic evaluation. "
        "Function-entry immediates pushed in the body are callback candidates (constants "
        "can collide with function VAs, for example flag 0x28000), not proof "
        "of their parameter role. Descriptor hooks are separately exact dwords. "
        "Direct builder callees supply shared/table-generated rows. "
        "The existing `frontend_screens` linear stack decoder misses branch joins, "
        "register labels and the option/slider/text-edit helper families; its output alone "
        "was rejected as an exhaustive editor catalogue.",
        "",
        "## Builder records",
        "",
    ]
    label_count = 0
    for start in sorted(candidates):
        body = bodies[start]
        name = names[start]["name"].removeprefix("game_mapedit_")
        lines += [f"### 0x{start:06X}: {name}", ""]
        desc = descriptors.get(start, [])
        lines.append(
            "Descriptor candidates (gate / draw / thunk): "
            + (
                "; ".join(
                    f"`0x{va:06X}`: " + " / ".join(f"`0x{x:06X}`" for x in hooks)
                    for va, hooks in desc
                )
                if desc
                else "none found by the four-dword contract."
            )
        )
        header_sites = [i.address for i in body if i.mnemonic == "call" and i.op_str == hex(HEADER)]
        lines.append(
            "Header witnesses: "
            + (
                ", ".join(f"`0x{x:06X}`" for x in header_sites)
                if header_sites
                else "shared helper/wrapper; no direct header."
            )
        )
        witnesses = []
        for index, insn in enumerate(body):
            if insn.mnemonic != "call" or insn.op_str != hex(LABEL):
                continue
            arg = "?"
            for previous in reversed(body[:index]):
                if previous.mnemonic == "call":
                    break
                if previous.mnemonic == "push":
                    arg = previous.op_str
                    break
            try:
                identifier = int(arg, 0)
                value = (
                    labels.text(identifier)
                    .replace("\r", " ")
                    .replace("\n", " / ")
                    .replace("|", "\\|")
                )
                if identifier == 0:
                    value = "special ID 0 (not a normal bank lookup)"
                witnesses.append(f"`{identifier:#x}` {value!r} @`0x{insn.address:06X}`")
                label_count += 1
            except (ValueError, KeyError):
                witnesses.append(f"computed `{arg}` @`0x{insn.address:06X}`")
        lines.append(
            "Label witnesses: "
            + (
                "; ".join(witnesses)
                if witnesses
                else "no direct lookup; text inherited from helper/table or caller."
            )
        )
        callbacks = sorted(
            {
                int(i.op_str, 0)
                for i in body
                if i.mnemonic == "push" and i.op_str.startswith("0x") and int(i.op_str, 0) in names
            }
        )
        lines.append(
            "Pushed handler/drawer candidates: "
            + (
                ", ".join(
                    f"`0x{x:06X}` ({names[x]['name'].removeprefix('game_mapedit_')})"
                    for x in callbacks
                )
                if callbacks
                else "none immediate."
            )
        )
        callees = sorted(
            {
                int(i.op_str, 0)
                for i in body
                if i.mnemonic in ("call", "jmp")
                and i.op_str.startswith("0x")
                and int(i.op_str, 0) in candidates
                and int(i.op_str, 0) != start
            }
        )
        lines += [
            "Shared builder callees: "
            + (", ".join(f"`0x{x:06X}`" for x in callees) if callees else "none direct."),
            "",
        ]
    lines += [
        "## Reproduction and acceptance",
        "",
        "Run from an owned checkout using the existing project venv; absolute private input "
        "paths avoid copying disc data or generating lifted trees:",
        "",
        "```sh",
        "/workspace/.venv/bin/python -m tools.editor_pages --root . "
        "--xbe /workspace/build/default.xbe --iso /workspace/discs/tsfp-xbox.iso",
        "/workspace/.venv/bin/python -m tools.frontend_screens "
        "--xbe /workspace/build/default.xbe --iso /workspace/discs/tsfp-xbox.iso "
        "0x313010 0x3133e0 0x312270",
        "```",
        "",
        f"Census: {len(candidates)} records, {sum(bool(descriptors.get(s)) for s in candidates)} "
        f"builders with descriptor candidates, {label_count} immediate label-call witnesses. "
        "The catalogue is generated to stdout only; private bytes are never written. "
        "Validation and dynamic-table supplements follow in "
        "[T1714 contracts and limits](t1714-editor-page-contracts.md).",
    ]
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("."))
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--iso", type=Path, required=True)
    args = parser.parse_args()
    print(catalogue(args.root, args.xbe, args.iso), end="")


if __name__ == "__main__":
    main()
