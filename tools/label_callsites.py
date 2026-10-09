# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Find every function that passes a localized label id to a text getter (T1507). Read only.

The front end shows each string through `0x3D7B0(id)` (`game_text_string_get_by_id_from_bank`, see tools.frontend_labels). This
tool disassembles every game function of the function table (export + overrides + additions), finds the calls to that getter and to its
sibling getters (wrappers that forward their own argument to it, found by shape), recovers the id argument and attributes it to the
enclosing function:

* a constant push (`push imm`) directly before the call,
* a register-passed id (`push reg`) resolved by a short backward slice (`mov reg, imm`, `xor reg, reg`, `push imm; pop reg`)
  that must not cross a jump, a jump target or another write of the register,
* anything else is reported as unresolved with a reason (`branch`, `computed`, `memory`, `window`).

Each resolved id is looked up with tools.frontend_labels (the owner's disc, read only) and classified into a text kind with
`classify`. Only label ids and one or two word titles are written, never the text. Outputs go to `generated/` (gitignored).

    python -m tools.label_callsites --iso discs/tsfp-xbox.iso --out generated/t1507
    python -m tools.label_callsites --show 0x001a3610 --iso discs/tsfp-xbox.iso
"""

from __future__ import annotations

import argparse
import csv
import json
from collections import Counter, defaultdict
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG

TEXT_GETTER = 0x3D7B0
MAX_LABEL = 0x1DBC  # ids below this index one of the two banks
MAX_ARGS = 16  # arguments a function is assumed to take at most
WINDOW = 24  # instructions searched backwards for an argument
STORY_NOISE = (
    range(0x1D5, 0x214),
    range(0x1004, 0x1005),
)  # t1468: item or event ids that also resolve to dialogue lines

# Text kind by label id range. Boundaries are INFERRED from reading the first words of the English bank (docs/t1507-label-id-naming.md).
KIND_RANGES: tuple[tuple[int, int, str], ...] = (
    (0x0000, 0x0140, "system"),  # online status, confirmation and DNAS error screens
    (0x0140, 0x0235, "dialogue"),
    (0x0235, 0x02A8, "menu"),  # map browser, upload and online status popups
    (0x02A8, 0x0348, "biography"),
    (0x0348, 0x06B0, "menu"),  # includes the credits names 0x4d8..0x690
    (0x06B0, 0x06E5, "hud"),  # counters and round words
    (0x06E5, 0x073A, "scoreboard"),  # awards and results
    (0x073A, 0x09D8, "script_name"),  # level script object names (not shown text)
    (0x09D8, 0x0AC8, "mapmaker"),
    (0x0AC8, 0x0D98, "mapmaker_help"),
    (0x0D98, 0x0E38, "character"),
    (0x0E38, 0x0E90, "menu"),
    (0x0E90, 0x0E9C, "hud"),  # result and time warning banners
    (0x0E9C, 0x0EC5, "menu"),  # memory card and save prompts
    (0x0EC5, 0x1063, "dialogue"),
    (0x1063, 0x1068, "mission"),
    (0x1068, 0x1084, "dialogue"),
    (0x1084, 0x10B0, "terminal"),  # security terminal screen words
    (0x10B0, 0x10C9, "dialogue"),
    (0x10C9, 0x10CD, "mission"),
    (0x10CD, 0x111F, "dialogue"),
    (0x111F, 0x112B, "mission"),
    (0x112B, 0x117B, "dialogue"),
    (0x117B, 0x1190, "terminal"),  # email and railbot control terminal words
    (0x1190, 0x1286, "dialogue"),
    (0x1286, 0x12D0, "terminal"),  # lab terminal documents, memos, login and status words
    (0x12D0, 0x12D4, "mission"),
    (0x12D4, 0x12D9, "dialogue"),
    (0x12D9, 0x12DF, "terminal"),
    (0x12DF, 0x15F0, "dialogue"),
    (0x15F0, 0x15FB, "mission"),  # objective words
    (0x15FB, 0x1666, "hud"),  # prompts, time and score words
    (0x1666, 0x166C, "scoreboard"),  # medal words
    (0x166C, 0x16B2, "hint"),  # loading tips and unlock hints
    (0x16B2, 0x1720, "menu"),  # game modes, teams, level titles
    (0x1720, 0x177E, "item"),  # weapon, ammo and pickup names
    (0x177E, 0x17DB, "menu"),  # pause, game over and control names
    (0x17DB, 0x17EB, "tutorial"),  # control tutorial lines
    (0x17EB, 0x180F, "menu"),
    (0x180F, 0x1811, "tutorial"),
    (0x1811, 0x1817, "menu"),
    (0x1817, 0x182C, "hud"),  # pause, result and objective banners
    (0x182C, 0x1842, "menu"),
    (0x1842, 0x184F, "network"),
    (0x184F, 0x18D6, "scoreboard"),  # statistics and awards
    (0x18D6, 0x18F7, "menu"),  # stage and difficulty names
    (0x18F7, 0x1918, "hud"),  # checkpoint, bag, door and vehicle gear messages
    (0x1918, 0x1970, "menu"),  # music and stage lists, control mapping glyphs
    (0x1970, 0x19EB, "storage"),  # memory unit, profile, save and controller messages
    (0x19EB, 0x1A68, "menu"),  # mission titles and misc
    (0x1A68, 0x1CE8, "network"),
    (0x1CE8, 0x1DBC, "dialogue"),
)


def classify(label: int) -> str:
    """Text kind of a label id (id range table above)."""
    for low, high, kind in KIND_RANGES:
        if low <= label < high:
            return kind
    return "other"


def is_noise(label: int) -> bool:
    return any(label in span for span in STORY_NOISE)


@dataclass
class Site:
    """One text getter call in a function."""

    function: int
    call_va: int
    getter: int
    status: str  # constant, slice or unresolved
    label: int | None = None
    reason: str = ""


@dataclass
class FunctionSummary:
    entry: int
    labels: list[int] = field(default_factory=list)
    unresolved: list[str] = field(default_factory=list)


def decode(code: bytes, base: int) -> list:
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    return list(md.disasm(code, base))


def jump_targets(insns: list) -> set[int]:
    """Addresses inside the function that a jump or conditional jump can reach."""
    targets: set[int] = set()
    for ins in insns:
        if ins.mnemonic.startswith("j") and ins.operands and ins.operands[0].type == X86_OP_IMM:
            targets.add(ins.operands[0].imm)
    return targets


def call_target(ins: Any) -> int | None:
    if ins.mnemonic == "call" and ins.operands and ins.operands[0].type == X86_OP_IMM:
        return ins.operands[0].imm
    return None


def _define(
    insns: list, start: int, reg: int, targets: set[int], stdcall: dict[int, int]
) -> tuple[int | None, str]:
    """Backward slice for the constant held by `reg` before insns[start]; `arg<N>` when it is the function's own argument N."""
    low = max(0, start - WINDOW)
    if insns[start].address in targets:
        return None, "branch"  # the push itself is a merge point
    for index in range(start - 1, low - 1, -1):
        ins = insns[index]
        if ins.address in targets:
            # a jump can land here: the value may come from another path
            return None, "branch"
        if ins.mnemonic.startswith("j") or ins.mnemonic in ("ret", "retn"):
            return None, "branch"
        if ins.mnemonic == "call":
            if ins.reg_name(reg) in ("eax", "ecx", "edx"):
                return None, "computed"  # caller-saved registers are clobbered by a call
            continue
        ops = ins.operands
        if not ops:
            continue
        written = (
            ops[0].type == X86_OP_REG
            and ops[0].reg == reg
            and ins.mnemonic not in ("cmp", "test", "push")
        )
        if ins.mnemonic == "pop" and ops[0].type == X86_OP_REG and ops[0].reg == reg:
            if (
                index > 0
                and insns[index - 1].mnemonic == "push"
                and insns[index - 1].operands[0].type == X86_OP_IMM
            ):
                return insns[index - 1].operands[0].imm & 0xFFFFFFFF, ""
            return None, "computed"
        if written:
            if ins.mnemonic == "mov" and ops[1].type == X86_OP_IMM:
                return ops[1].imm & 0xFFFFFFFF, ""
            if ins.mnemonic == "xor" and ops[1].type == X86_OP_REG and ops[1].reg == reg:
                return 0, ""
            if ins.mnemonic == "mov" and ops[1].type == X86_OP_MEM:
                arg = own_argument(insns, index, ops[1].mem, stdcall)
                if arg is not None:
                    return None, f"arg{arg}"
            return None, "computed"
    return None, "window"


def find_argument(
    insns: list, call_index: int, position: int, targets: set[int], stdcall: dict[int, int]
) -> tuple[int | None, str]:
    """Value of argument `position` (0 = first, pushed last) of the call at call_index, or (None, reason)."""
    seen = 0
    low = max(0, call_index - WINDOW)
    for index in range(call_index - 1, low - 1, -1):
        ins = insns[index]
        if ins.mnemonic == "push":
            if seen == position:
                op = ins.operands[0]
                if op.type == X86_OP_IMM:
                    return op.imm & 0xFFFFFFFF, "constant"
                if op.type == X86_OP_REG:
                    value, reason = _define(insns, index, op.reg, targets, stdcall)
                    return (value, "slice") if value is not None else (None, reason)
                if op.type == X86_OP_MEM:
                    arg = own_argument(insns, index, op.mem, stdcall)
                    if arg is not None:
                        return None, f"arg{arg}"
                return None, "memory"
            seen += 1
        elif ins.mnemonic.startswith("j") or ins.mnemonic == "call" or ins.address in targets:
            return None, "branch" if ins.mnemonic != "call" else "computed"
    return None, "window"


def stack_delta(insns: list, upto: int, stdcall: dict[int, int]) -> int:
    """Bytes the stack pointer moved before insns[upto] (pushes, pops, sub/add esp, stdcall callees), straight line approximation."""
    delta = 0
    for ins in insns[:upto]:
        ops = ins.operands
        if ins.mnemonic == "push":
            delta += 4
        elif ins.mnemonic == "pop":
            delta -= 4
        elif (
            ins.mnemonic in ("sub", "add")
            and ops[0].type == X86_OP_REG
            and ins.reg_name(ops[0].reg) == "esp"
            and ops[1].type == X86_OP_IMM
        ):
            delta += ops[1].imm if ins.mnemonic == "sub" else -ops[1].imm
        elif ins.mnemonic == "call":
            delta -= stdcall.get(call_target(ins) or 0, 0)
    return delta


def own_argument(insns: list, index: int, mem: Any, stdcall: dict[int, int]) -> int | None:
    """Index of the function's own argument that an [esp+N] or [ebp+N] operand at insns[index] reads, else None."""
    base = insns[index].reg_name(mem.base) if mem.base else ""
    if mem.index != 0:
        return None
    if base == "ebp" and mem.disp >= 8:
        return (mem.disp - 8) // 4 if mem.disp < 8 + 4 * MAX_ARGS else None
    if base == "esp":
        disp = mem.disp - 4 - stack_delta(insns, index, stdcall)
        return disp // 4 if 0 <= disp < 4 * MAX_ARGS and disp % 4 == 0 else None
    return None


def stdcall_map(read_code: Callable[[int, int], bytes], table: dict[int, int]) -> dict[int, int]:
    """Callee-cleans functions (last instruction `ret N`, N a dword multiple): entry -> N."""
    out: dict[int, int] = {}
    for entry, size in table.items():
        if size < 3:
            continue
        code = read_code(entry, size)
        while code and code[-1] in (0xCC, 0x90):
            code = code[:-1]
        if (
            len(code) >= 3
            and code[-3] == 0xC2
            and code[-1] == 0
            and 0 < code[-2] <= 0x80
            and code[-2] % 4 == 0
        ):
            out[entry] = code[-2]
    return out


def scan_code(
    entry: int, code: bytes, getters: dict[int, int], stdcall: dict[int, int] | None = None
) -> tuple[list[Site], list]:
    """Sites of one function body. `getters` maps a getter entry to the position of its label argument."""
    stdcall = stdcall or {}
    insns = decode(code, entry)
    targets = jump_targets(insns)
    sites: list[Site] = []
    for index, ins in enumerate(insns):
        target = call_target(ins)
        if target not in getters:
            continue
        value, how = find_argument(insns, index, getters[target], targets, stdcall)
        if value is None and how.startswith("arg"):
            sites.append(Site(entry, ins.address, target, "argument", None, how))
        elif value is None:
            sites.append(Site(entry, ins.address, target, "unresolved", None, how))
        elif 0 <= value < MAX_LABEL:
            sites.append(Site(entry, ins.address, target, how, value))
        else:
            sites.append(Site(entry, ins.address, target, "unresolved", None, "not_a_label"))
    return sites, insns


def calls_any(code: bytes, base: int, getters: dict[int, int]) -> bool:
    """Cheap byte level test: does the body contain a relative call to one of the getters."""
    at = code.find(b"\xe8")
    while at != -1:
        if at + 5 <= len(code):
            rel = int.from_bytes(code[at + 1 : at + 5], "little", signed=True)
            if base + at + 5 + rel in getters:
                return True
        at = code.find(b"\xe8", at + 1)
    return False


def find_wrappers(
    read_code: Callable[[int, int], bytes],
    table: dict[int, int],
    seed: dict[int, int],
    stdcall: dict[int, int] | None = None,
    max_size: int = 2048,
) -> dict[int, int]:
    """Functions that forward exactly one own argument to a known getter (same role by shape), iterated to a fixed point."""
    getters = dict(seed)
    changed = True
    while changed:
        changed = False
        for entry, size in table.items():
            if entry in getters or size > max_size:
                continue
            code = read_code(entry, size)
            if not calls_any(code, entry, getters):
                continue
            found, _ = scan_code(entry, code, getters, stdcall)
            forwarded = {site.reason for site in found if site.status == "argument"}
            if len(forwarded) == 1:
                getters[entry] = int(next(iter(forwarded))[3:])
                changed = True
    return getters


def constant_arguments(insns: list, targets: set[int]) -> list[tuple[int, list[int | None]]]:
    """For each direct call: (callee, pushed argument values by position, None when not an immediate)."""
    found: list[tuple[int, list[int | None]]] = []
    for index, ins in enumerate(insns):
        callee = call_target(ins)
        if callee is None:
            continue
        values: list[int | None] = []
        for back in range(index - 1, max(-1, index - 40), -1):
            prior = insns[back]
            if (
                prior.mnemonic.startswith("j")
                or prior.mnemonic == "call"
                or prior.address in targets
            ):
                break
            if prior.mnemonic == "push":
                op = prior.operands[0]
                values.append(op.imm & 0xFFFFFFFF if op.type == X86_OP_IMM else None)
                if len(values) == 10:
                    break
        found.append((callee, values))
    return found


def sibling_candidates(
    sites: Counter[tuple[int, int]],
    labelled: Counter[tuple[int, int]],
    distinct: dict[tuple[int, int], set[int]],
) -> list[tuple[int, int, int, int, int]]:
    """Calls whose argument position is a label range constant at most sites: (callee, position, sites, labelled, distinct)."""
    out = []
    for key, total in sites.items():
        if (
            total >= 3
            and labelled[key] >= 3
            and labelled[key] * 10 >= total * 7
            and len(distinct[key]) >= 3
        ):
            out.append((key[0], key[1], total, labelled[key], len(distinct[key])))
    return sorted(out, key=lambda row: -row[3])


def summarize(sites: list[Site]) -> dict[int, FunctionSummary]:
    out: dict[int, FunctionSummary] = {}
    for site in sites:
        summary = out.setdefault(site.function, FunctionSummary(site.function))
        if site.label is not None:
            summary.labels.append(site.label)
        else:
            summary.unresolved.append(site.reason)
    return out


def title(text: str, words: int = 2) -> str:
    return " ".join(text.replace("\n", " ").split()[:words])


def read_siblings(path: Path) -> dict[int, int]:
    """Vetted storing builders (they keep an id argument in a record that is shown later): entry -> argument position."""
    if not path.exists():
        return {}
    with path.open(newline="") as handle:
        return {int(row["entry_va"], 16): int(row["position"]) for row in csv.DictReader(handle)}


def vet_wrappers(
    getters: dict[int, int], sites: list[Site], out: Path
) -> tuple[dict[int, int], list[Site]]:
    """Drop sibling getters whose callers mostly pass non-label constants (the argument was not an id)."""
    good: Counter[int] = Counter()
    bad: Counter[int] = Counter()
    for site in sites:
        if site.getter == TEXT_GETTER:
            continue
        if site.label is not None:
            good[site.getter] += 1
        elif site.reason == "not_a_label":
            bad[site.getter] += 1
    rejected = {g for g in getters if g != TEXT_GETTER and bad[g] > good[g]}
    out.mkdir(parents=True, exist_ok=True)
    with (out / "wrappers.csv").open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["getter", "argument", "label_sites", "non_label_sites", "accepted"])
        for getter, position in sorted(getters.items()):
            accepted = getter not in rejected
            writer.writerow([f"{getter:#x}", position, good[getter], bad[getter], accepted])
    kept = {g: p for g, p in getters.items() if g not in rejected}
    return kept, [site for site in sites if site.getter not in rejected]


def write_candidates(world: Any, table: dict[int, int], labels: Any, out: Path) -> None:
    """Sibling builder candidates by callers: functions that take label ids as constant arguments."""
    totals: Counter[tuple[int, int]] = Counter()
    labelled: Counter[tuple[int, int]] = Counter()
    distinct: dict[tuple[int, int], set[int]] = defaultdict(set)
    for entry, size in table.items():
        insns = decode(world.read(entry, size), entry)
        for callee, values in constant_arguments(insns, jump_targets(insns)):
            for position, value in enumerate(values):
                totals[(callee, position)] += 1
                if value is not None and 0 < value < MAX_LABEL:
                    labelled[(callee, position)] += 1
                    distinct[(callee, position)].add(value)
    with (out / "sibling_candidates.csv").open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["callee", "name", "position", "sites", "labelled", "distinct", "sample"])
        for callee, position, total, hit, many in sibling_candidates(totals, labelled, distinct):
            sample = " / ".join(
                f"{v:#x} {title(labels.text(v), 1)}"
                for v in sorted(distinct[(callee, position)])[:4]
            )
            writer.writerow(
                [f"{callee:#x}", world.names.get(callee, ""), position, total, hit, many, sample]
            )


def main() -> None:
    from tools.frontend_labels import load
    from tools.name_additions import World

    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--iso", type=Path, default=Path("discs/tsfp-xbox.iso"))
    parser.add_argument("--out", type=Path, default=Path("generated/t1507"))
    parser.add_argument(
        "--show", nargs="*", default=[], help="print the sites of these function entries"
    )
    parser.add_argument(
        "--siblings",
        type=Path,
        default=Path("tools/data/label_siblings.csv"),
        help="vetted storing builders: va,position,evidence",
    )
    parser.add_argument(
        "--candidates", action="store_true", help="also write sibling_candidates.csv for review"
    )
    args = parser.parse_args()
    world = World(Path("."))
    labels = load(args.iso)
    table = {va: size for va, size in world.size.items() if va not in world.library}
    stdcall = stdcall_map(world.read, table)
    seeds = {TEXT_GETTER: 0, **read_siblings(args.siblings)}
    getters = find_wrappers(world.read, table, seeds, stdcall)
    sites: list[Site] = []
    for entry, size in table.items():
        found, _ = scan_code(entry, world.read(entry, size), getters, stdcall)
        sites.extend(found)
    getters, sites = vet_wrappers(getters, sites, args.out)
    if args.candidates:
        write_candidates(world, table, labels, args.out)
    summary = summarize(sites)
    args.out.mkdir(parents=True, exist_ok=True)
    with (args.out / "callsites.csv").open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(
            [
                "function",
                "name",
                "call",
                "getter",
                "status",
                "label",
                "kind",
                "title_words",
                "reason",
            ]
        )
        for site in sites:
            name = world.names.get(site.function, "")
            label_text = labels.text(site.label) if site.label is not None else ""
            writer.writerow(
                [
                    f"{site.function:#010x}",
                    name,
                    f"{site.call_va:#010x}",
                    f"{site.getter:#x}",
                    site.status,
                    "" if site.label is None else f"{site.label:#x}",
                    "" if site.label is None else classify(site.label),
                    title(label_text),
                    site.reason,
                ]
            )
    with (args.out / "functions.csv").open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["function", "name", "distinct_ids", "kinds", "first_titles", "unresolved"])
        for entry, item in sorted(summary.items()):
            distinct = sorted(set(item.labels))
            kinds = Counter(classify(label) for label in distinct)
            firsts = " / ".join(f"{label:#x} {title(labels.text(label))}" for label in distinct[:4])
            writer.writerow(
                [
                    f"{entry:#010x}",
                    world.names.get(entry, ""),
                    len(distinct),
                    " ".join(f"{k}={v}" for k, v in sorted(kinds.items())),
                    firsts,
                    " ".join(sorted(set(item.unresolved))),
                ]
            )
    (args.out / "getters.json").write_text(
        json.dumps({f"{k:#x}": v for k, v in sorted(getters.items())})
    )
    by_kind: dict[str, set[int]] = defaultdict(set)
    for entry, item in summary.items():
        for label in set(item.labels):
            by_kind[classify(label)].add(entry)
    print(f"getters {len(getters)} sites {len(sites)} functions {len(summary)}")
    for kind, funcs in sorted(by_kind.items()):
        print(f"{kind}: {len(funcs)} functions")
    for text in args.show:
        entry = int(text, 0)
        for site in (s for s in sites if s.function == entry):
            shown = (
                "" if site.label is None else f" {site.label:#x} {title(labels.text(site.label))}"
            )
            print(f"{site.call_va:#x} {site.status}{shown} {site.reason}")


if __name__ == "__main__":
    main()
