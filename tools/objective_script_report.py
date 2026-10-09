#!/usr/bin/env python3
# ruff: noqa: E501
"""T1759: diff the mission script arrays between a start and an end guest dump of the first Story objective.

Input: guest dumps (text format of src/host/guest_dump.c) taken with
  --dump-guest-range "*0x6F32F0+0:0x160,*0x765E44+0:0x1960"
range index 0 = the cached mode record M (`[0x6F32F0]`, docs/t-weaponslot-poke.md), index 1 = the script context
C = `[0x765E44]`, size 0x1960 (docs/t-mission-script-engine.md, T1757). The layout is the T1757 static one (INFERRED purpose,
MEASURED access widths): C+0 packed group array (stride 0x25E), C+4+4*i group states, C+0x7C used groups, C+0x80/0x84 condition
count/base (stride 0x28, callback-visible E = slot+4, state at E+0x18 = slot+0x1C), C+0x88/0x8C action count/base (stride 0x24,
state at slot+0x18), C+0xAC/0xB0 point record count/base (stride 0x58, visitation words +0x30..). A sub-array whose base lies
inside the dumped context range is decoded, one outside it is named and skipped.
T1759 follow-up: the 0x220 rule records (base M+0x138, count M+0x134) and the 0xB4 setup entries (base M+0x130, count M+0x12C)
are dumped through the host two level form `**ADDR+OFF1+OFF2:LEN` (`**0x6F32F0+0x138+0:0x2200`, `**0x6F32F0+0x130+0:0x1680`:
the first RULE_DUMP_COUNT rules and SETUP_DUMP_COUNT entries) and diffed dword by dword with the T1757 field names where known.
A null or unmapped pointer is recorded by the host as `unreadable` and reported here as a note, never an error. A changed
group is the runtime image of a rule (groups are built only for rules with +8 < 0xFDE8 and +0x21C == 0, in rule order, so the
group index is not the rule index). Nothing here is hardware evidence, an observed change is a candidate only.

Usage: python -m tools.objective_script_report START END
       python -m tools.objective_script_report --session DIR   (START = guestdump.ingame-idle, END = guestdump.ingame-objective,
                                                              else guestdump.exit; button dumps in DIR/buttons are summarised)
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass, field
from pathlib import Path

from tools import button_dump_report as dumps

MODE_POINTER = 0x6F32F0
CONTEXT_POINTER = 0x765E44
CONTEXT_SIZE = 0x1960
GROUP_STRIDE = 0x25E
COND_STRIDE = 0x28
ACTION_STRIDE = 0x24
POINT_STRIDE = 0x58
RULE_STRIDE = 0x220
SETUP_STRIDE = 0xB4
MODE_SETUP_BASE = 0x130
MODE_RULE_BASE = 0x138
RULE_DUMP_COUNT = 16
SETUP_DUMP_COUNT = 32
RULE_SPEC = f"**0x{MODE_POINTER:X}+0x{MODE_RULE_BASE:X}+0:0x{RULE_DUMP_COUNT * RULE_STRIDE:X}"
SETUP_SPEC = f"**0x{MODE_POINTER:X}+0x{MODE_SETUP_BASE:X}+0:0x{SETUP_DUMP_COUNT * SETUP_STRIDE:X}"
RULE_FIELDS = {
    0x4: "kind",
    0x8: "group_threshold",
    0x50: "cond_count",
    0x88: "action_count",
    0x21C: "event_rule",
}
SETUP_FIELDS = {
    0x0: "list_kind",
    0x4: "key",
    0x8: "subtype",
    0xC: "object_index",
    0xA4: "rule_index",
    0xAC: "identifier",
    0xB0: "flags",
}
MAX_ROWS = 40
EXIT_OK = 0
EXIT_USAGE = 2
EXIT_UNREADABLE = 3


def dword(data: bytes | bytearray, offset: int) -> int | None:
    if offset < 0 or offset + 4 > len(data):
        return None
    return struct.unpack_from("<I", data, offset)[0]


def word(data: bytes | bytearray, offset: int) -> int | None:
    if offset < 0 or offset + 2 > len(data):
        return None
    return struct.unpack_from("<H", data, offset)[0]


@dataclass
class Snapshot:
    """The context and mode record bytes of one dump, and where they were read."""

    context: bytes | None = None
    context_address: int = 0
    mode: bytes | None = None
    mode_address: int = 0
    rules: bytes | None = None
    rules_address: int = 0
    setups: bytes | None = None
    setups_address: int = 0
    notes: list[str] = field(default_factory=list)
    problems: list[str] = field(default_factory=list)


def load_snapshot(path: Path) -> Snapshot:
    parsed = dumps.parse_dump(path, lambda info: True)
    snap = Snapshot()
    for info in parsed.ranges:
        if not info.indirect:
            continue
        if info.offset2 is not None:
            load_second_level(snap, info, path)
            continue
        which = (
            "context"
            if info.spec_address == CONTEXT_POINTER
            else "mode"
            if info.spec_address == MODE_POINTER
            else None
        )
        if which is None:
            continue
        if not info.readable or info.data is None or info.address is None:
            snap.problems.append(
                f"{which} range unreadable in {path.name}: {info.reason or 'no data'}"
            )
            continue
        if len(info.data) < info.length:
            snap.problems.append(
                f"{which} range partial in {path.name}: {len(info.data)} of {info.length} bytes"
            )
        if which == "context":
            snap.context, snap.context_address = bytes(info.data), info.address
        else:
            snap.mode, snap.mode_address = bytes(info.data), info.address
    return snap


def load_second_level(snap: Snapshot, info: dumps.RangeInfo, path: Path) -> None:
    """A `**0x6F32F0+0x138+0` (rules) or `+0x130+0` (setup entries) range; unreadable is a note, not an error."""
    if info.spec_address != MODE_POINTER or info.offset2 != 0:
        return
    which = {MODE_RULE_BASE: "rules", MODE_SETUP_BASE: "setups"}.get(info.spec_offset)
    if which is None:
        return
    if not info.readable or info.data is None or info.address is None:
        snap.notes.append(
            f"{which} range unreadable in {path.name} (null or unmapped pointer, host marker): {info.reason or 'no data'}"
        )
        return
    if len(info.data) < info.length:
        snap.notes.append(
            f"{which} range partial in {path.name}: {len(info.data)} of {info.length} bytes"
        )
        return
    if which == "rules":
        snap.rules, snap.rules_address = bytes(info.data), info.address
    else:
        snap.setups, snap.setups_address = bytes(info.data), info.address


@dataclass(frozen=True)
class Change:
    """One changed value: where (family, index, field) and old/new."""

    family: str
    index: int
    name: str
    old: int
    new: int


@dataclass
class Report:
    changes: list[Change] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)
    counts: dict[str, tuple[int | None, int | None]] = field(default_factory=dict)
    raw_context_dwords: int = 0
    raw_mode_dwords: int = 0
    mode_pointer_changed: bool = False


def sub_array(
    data: bytes,
    base_address: int,
    header_base: int,
    count: int,
    stride: int,
    family: str,
    notes: list[str],
) -> int | None:
    """Offset of a sub-array inside the context dump, or None (with a note) when it is not covered."""
    if count == 0:
        return None
    offset = header_base - base_address
    if offset < 0 or offset + count * stride > len(data):
        notes.append(
            f"{family}: base 0x{header_base:X} count {count} is outside the dumped context (0x{base_address:X}+0x{len(data):X}), not decoded"
        )
        return None
    return offset


def raw_dword_changes(a: bytes, b: bytes) -> int:
    size = min(len(a), len(b))
    return sum(1 for off in range(0, size - 3, 4) if a[off : off + 4] != b[off : off + 4])


def diff_context(start: Snapshot, end: Snapshot, report: Report) -> None:
    if start.context is None or end.context is None:
        report.notes.append("context range missing in one dump, nothing to diff")
        return
    a, b = start.context, end.context
    if start.context_address != end.context_address:
        report.notes.append(
            f"context moved 0x{start.context_address:X} -> 0x{end.context_address:X} (a new script context was allocated): array contents are not comparable index by index"
        )
    report.raw_context_dwords = raw_dword_changes(a, b)
    group_count_a, group_count_b = dword(a, 0x7C), dword(b, 0x7C)
    report.counts["groups used"] = (group_count_a, group_count_b)
    for index in range(min(group_count_a or 0, group_count_b or 0, 0x40)):
        old, new = dword(a, 4 + 4 * index), dword(b, 4 + 4 * index)
        if old is not None and new is not None and old != new:
            report.changes.append(Change("group", index, "state", old, new))
    group_base = dword(b, 0)
    if group_base is not None and dword(a, 0) == group_base and group_count_a == group_count_b:
        offset = sub_array(
            b,
            end.context_address,
            group_base,
            group_count_b or 0,
            GROUP_STRIDE,
            "groups",
            report.notes,
        )
        if offset is not None:
            diff_groups(a, b, offset, group_count_b or 0, report)
    for family, count_at, base_at, stride, fields in (
        ("condition", 0x80, 0x84, COND_STRIDE, (("state", 0x1C), ("aux", 0x24), ("flag", 0x10))),
        ("action", 0x88, 0x8C, ACTION_STRIDE, (("state", 0x18), ("aux", 0x20))),
    ):
        diff_entries(a, b, end.context_address, family, count_at, base_at, stride, fields, report)
    diff_points(a, b, end.context_address, report)


def diff_groups(a: bytes, b: bytes, offset: int, count: int, report: Report) -> None:
    for index in range(count):
        base = offset + index * GROUP_STRIDE
        conds_a, conds_b = word(a, base), word(b, base)
        if conds_a != conds_b:
            report.changes.append(
                Change("group", index, "condition_count", conds_a or 0, conds_b or 0)
            )
        for slot in range(min(conds_b or 0, 50)):
            old, new = word(a, base + 0xCA + 2 * slot), word(b, base + 0xCA + 2 * slot)
            if old is not None and new is not None and old != new:
                report.changes.append(Change("group", index, f"member_state[{slot}]", old, new))
        old_flag, new_flag = a[base + 0x25C : base + 0x25D], b[base + 0x25C : base + 0x25D]
        if old_flag and new_flag and old_flag != new_flag:
            report.changes.append(Change("group", index, "action_flags", old_flag[0], new_flag[0]))


def diff_entries(
    a: bytes,
    b: bytes,
    address: int,
    family: str,
    count_at: int,
    base_at: int,
    stride: int,
    fields: tuple[tuple[str, int], ...],
    report: Report,
) -> None:
    count_a, count_b = dword(a, count_at), dword(b, count_at)
    report.counts[f"{family}s"] = (count_a, count_b)
    base_a, base_b = dword(a, base_at), dword(b, base_at)
    if count_a != count_b or base_a != base_b or base_b is None:
        report.notes.append(
            f"{family}: count/base differ between the dumps ({count_a}, {base_a}) vs ({count_b}, {base_b}), not compared"
        )
        return
    offset = sub_array(b, address, base_b, count_b or 0, stride, family, report.notes)
    if offset is None:
        return
    for index in range(count_b or 0):
        for name, delta in fields:
            old, new = (
                dword(a, offset + index * stride + delta),
                dword(b, offset + index * stride + delta),
            )
            if old is not None and new is not None and old != new:
                report.changes.append(Change(family, index, name, old, new))


def diff_points(a: bytes, b: bytes, address: int, report: Report) -> None:
    count_a, count_b = dword(a, 0xAC), dword(b, 0xAC)
    report.counts["point records"] = (count_a, count_b)
    base_a, base_b = dword(a, 0xB0), dword(b, 0xB0)
    if count_a != count_b or base_a != base_b or base_b is None:
        report.notes.append(
            f"point records: count/base differ ({count_a}, {base_a}) vs ({count_b}, {base_b}), not compared"
        )
        return
    offset = sub_array(
        b, address, base_b, count_b or 0, POINT_STRIDE, "point records", report.notes
    )
    if offset is None:
        return
    for index in range(count_b or 0):
        for slot in range(10):
            where = offset + index * POINT_STRIDE + 0x30 + 4 * slot
            old, new = dword(a, where), dword(b, where)
            if old is not None and new is not None and old != new:
                report.changes.append(Change("point", index, f"visited[{slot}]", old, new))


def diff_records(
    family: str,
    a: bytes | None,
    b: bytes | None,
    address_a: int,
    address_b: int,
    count: tuple[int | None, int | None],
    stride: int,
    dumped: int,
    names: dict[int, str],
    report: Report,
) -> None:
    """Dword diff of the first min(count, dumped) records of a second level array (rules or setup entries)."""
    if a is None or b is None:
        report.notes.append(
            f"{family}: not dumped (or unreadable) in one of the dumps, not compared"
        )
        return
    if address_a != address_b or count[0] != count[1]:
        report.notes.append(
            f"{family}: array moved 0x{address_a:X} -> 0x{address_b:X} or count {count[0]} -> {count[1]}, not compared index by index"
        )
        return
    records = min(count[1] or 0, dumped, len(b) // stride, len(a) // stride)
    if (count[1] or 0) > dumped:
        report.notes.append(
            f"{family}: count {count[1]} exceeds the {dumped} dumped, the rest is not compared"
        )
    for index in range(records):
        for offset in range(0, stride - 3, 4):
            old, new = dword(a, index * stride + offset), dword(b, index * stride + offset)
            if old is not None and new is not None and old != new:
                report.changes.append(
                    Change(family, index, names.get(offset, f"dword@0x{offset:X}"), old, new)
                )


def diff_mode(start: Snapshot, end: Snapshot, report: Report) -> None:
    if start.mode is None or end.mode is None:
        report.notes.append("mode record range missing in one dump")
        return
    report.mode_pointer_changed = start.mode_address != end.mode_address
    report.raw_mode_dwords = raw_dword_changes(start.mode, end.mode)
    for name, offset in (
        ("rule count M+0x134", 0x134),
        ("b4 count M+0x12C", 0x12C),
        ("setup count M+0x124", 0x124),
    ):
        report.counts[name] = (dword(start.mode, offset), dword(end.mode, offset))


def compare(start: Snapshot, end: Snapshot) -> Report:
    report = Report()
    report.notes.extend(start.problems + end.problems)
    report.notes.extend(start.notes + end.notes)
    diff_mode(start, end, report)
    diff_context(start, end, report)
    diff_records(
        "rule", start.rules, end.rules, start.rules_address, end.rules_address,
        report.counts.get("rule count M+0x134", (None, None)), RULE_STRIDE, RULE_DUMP_COUNT, RULE_FIELDS, report,
    )  # fmt: skip
    diff_records(
        "setup", start.setups, end.setups, start.setups_address, end.setups_address,
        report.counts.get("b4 count M+0x12C", (None, None)), SETUP_STRIDE, SETUP_DUMP_COUNT, SETUP_FIELDS, report,
    )  # fmt: skip
    return report


def format_report(report: Report, start_name: str, end_name: str) -> list[str]:
    lines = [
        f"objective script array diff: {start_name} -> {end_name}",
        "(layout T1757, purposes INFERRED, an observed change is a candidate)",
    ]
    for name, (old, new) in report.counts.items():
        lines.append(f"  {name}: {old} -> {new}")
    if report.mode_pointer_changed:
        lines.append("  mode record pointer moved between the dumps")
    lines.append(
        f"  raw changed dwords: context {report.raw_context_dwords}, mode record {report.raw_mode_dwords}"
    )
    families: dict[str, list[Change]] = {}
    for change in report.changes:
        families.setdefault(change.family, []).append(change)
    if not report.changes:
        lines.append("  no decoded script record changed")
    for family, items in families.items():
        records = sorted({item.index for item in items})
        lines.append(
            f"  {family}: {len(records)} changed record(s), {len(items)} field change(s): indices {records[:MAX_ROWS]}"
        )
        for item in items[:MAX_ROWS]:
            lines.append(
                f"    {family}[{item.index}] {item.name}: 0x{item.old:X} -> 0x{item.new:X}"
            )
        if len(items) > MAX_ROWS:
            lines.append(f"    ... {len(items) - MAX_ROWS} more")
    lines.extend(f"  NOTE: {note}" for note in report.notes)
    return lines


def pick_session_dumps(session: Path) -> tuple[Path, Path] | None:
    start = session / "guestdump.ingame-idle"
    end = session / "guestdump.ingame-objective"
    if not end.is_file():
        end = session / "guestdump.exit"
    if not start.is_file() or not end.is_file():
        return None
    return start, end


def button_summary(session: Path) -> list[str]:
    """One line per button event with a before and an after dump: how many decoded records changed."""
    folder = session / "buttons"
    if not folder.is_dir():
        return []
    lines: list[str] = []
    for before in sorted(folder.glob("guestdump.*_before")):
        stem = before.name[: -len("_before")]
        afters = sorted(folder.glob(stem + "_after*"))
        if not afters:
            continue
        report = compare(load_snapshot(before), load_snapshot(afters[0]))
        label = stem[len("guestdump.") :]
        lines.append(
            f"  {label}: {len(report.changes)} decoded field change(s), raw context dwords {report.raw_context_dwords}"
        )
    return lines


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("dumps", nargs="*", type=Path, help="START END guest dump files")
    parser.add_argument("--session", type=Path, help="census session folder of run_objective.fish")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.session:
        pair = pick_session_dumps(args.session)
        if pair is None:
            print(
                f"{args.session}: need guestdump.ingame-idle and guestdump.ingame-objective (or guestdump.exit)",
                file=sys.stderr,
            )
            return EXIT_USAGE
    elif len(args.dumps) == 2:
        pair = (args.dumps[0], args.dumps[1])
    else:
        print("give START END or --session DIR", file=sys.stderr)
        return EXIT_USAGE
    start, end = (load_snapshot(path) for path in pair)
    if start.context is None or end.context is None:
        message = "\n".join(start.problems + end.problems)
        print(
            message or "no context range in the dumps (was --dump-guest-range given?)",
            file=sys.stderr,
        )
        return EXIT_UNREADABLE
    print("\n".join(format_report(compare(start, end), pair[0].name, pair[1].name)))
    if args.session:
        summary = button_summary(args.session)
        if summary:
            print("button events (before vs first after):")
            print("\n".join(summary))
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
