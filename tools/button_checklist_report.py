#!/usr/bin/env python3
# ruff: noqa: E501
# SPDX-License-Identifier: GPL-3.0-or-later
"""T1736: per-step table of a checklist session folder (written by tools.button_checklist).

For every step it reads the before/after dump pair of the pad arrays (0x6B93C0 .. 0x6B95B8, docs/t-guest-input-chain.md), lists the
changed dwords with their field names, and compares them with the expected control of the step. Flags:

  NO-CHANGE        no dword of the pad arrays changed at all
  MISSING-EXPECTED something changed, but not the expected field (button words / axis dword of port 0)
  UNEXPECTED       a dword outside the expected set changed (other ports, other axes, button words on a stick step)
  NO-DUMPS         the step has no readable dump pair
  SKIPPED          the owner skipped it
  NO-HOST-EVENT    a digital step with no host button event (the host edge counter did not see the tap)
  HOST-EDGE        an analog step that the host counted as digital edges (an analog button threshold crossed?)

A trigger step may move the button words (the analog button threshold, 0x47DF40, INFERRED) without an UNEXPECTED flag.
Confidence of the tables: MEASURED reads of the guest pad arrays, the field names are INFERRED (docs/t-guest-input-chain.md).

    python -m tools.button_checklist_report DIR [--write] [--strict]
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path

from tools import button_dump_report as dump_report
from tools.button_checklist import PAD_BASE, PAD_LEN, SESSION_FILE

PHYSICAL = 0x6B93D0
EFFECTIVE = 0x6B94C8
STRIDE = 0x3C
PORTS = 4
FIELDS: dict[int, str] = {
    0x00: "deadzone",
    0x04: "connected",
    0x08: "previous",
    0x0C: "current",
    0x10: "pressed",
    0x14: "released",
    0x18: "neutral18",
    0x1C: "LX",
    0x20: "LY",
    0x24: "RX",
    0x28: "RY",
    0x2C: "LT",
    0x30: "RT",
    0x34: "rumble",
    0x38: "suppress",
}
BUTTON_FIELDS = (0x08, 0x0C, 0x10, 0x14)
AXIS_OFFSETS = {
    name: offset for offset, name in FIELDS.items() if name in ("LX", "LY", "RX", "RY", "LT", "RT")
}
FLAG_ORDER = (
    "NO-DUMPS",
    "SKIPPED",
    "NO-CHANGE",
    "MISSING-EXPECTED",
    "UNEXPECTED",
    "NO-HOST-EVENT",
    "HOST-EDGE",
)


@dataclass(frozen=True)
class Slot:
    """Where a dword offset (relative to PAD_BASE) lives."""

    array: str  # remap, physical, effective, gap or outside
    index: int  # port or slot
    field_offset: int

    def name(self) -> str:
        if self.array == "remap":
            return f"remap[{self.field_offset // 4}]"
        if self.array in ("gap", "outside"):
            return f"{self.array}+0x{self.field_offset:X}"
        prefix = "P" if self.array == "physical" else "E"
        return (
            f"{prefix}{self.index}+0x{self.field_offset:02X} {FIELDS.get(self.field_offset, '?')}"
        )


def locate(offset: int) -> Slot:
    """Classify a dword offset relative to PAD_BASE (physical records 0x6B93D0 + port*0x3C, effective 0x6B94C8 + slot*0x3C)."""
    address = PAD_BASE + offset
    if address < PHYSICAL:
        return Slot("remap", 0, address - PAD_BASE)
    for array, base in (("physical", PHYSICAL), ("effective", EFFECTIVE)):
        relative = address - base
        if 0 <= relative < PORTS * STRIDE:
            return Slot(array, relative // STRIDE, relative % STRIDE)
    if PHYSICAL + PORTS * STRIDE <= address < EFFECTIVE:
        return Slot("gap", 0, address - (PHYSICAL + PORTS * STRIDE))
    return Slot("outside", 0, offset)


@dataclass(frozen=True)
class Change:
    offset: int
    old: int
    new: int

    @property
    def slot(self) -> Slot:
        return locate(self.offset)

    def text(self) -> str:
        return f"{self.slot.name()} 0x{self.old:X}->0x{self.new:X}"


def pad_changes(before: bytes, after: bytes) -> list[Change]:
    return [
        Change(item.offset, item.old, item.new)
        for item in dump_report.diff_dwords(before, after, 0, PAD_LEN)
    ]


def expected_offsets(kind: str, expects: list[tuple[str, str]]) -> tuple[set[int], set[int]]:
    """(expected offsets, allowed offsets) relative to PAD_BASE. Port 0 physical and effective slot 0 are the expected records.

    digital: the four button words. analog: the axis dwords; a trigger also allows the button words (analog button threshold)."""
    expected: set[int] = set()
    allowed: set[int] = set()
    for base in (PHYSICAL - PAD_BASE, EFFECTIVE - PAD_BASE):
        if kind == "digital":
            expected.update(base + offset for offset in BUTTON_FIELDS)
            continue
        for _, name in expects:
            expected.add(base + AXIS_OFFSETS[name])
            if name in ("LT", "RT"):
                allowed.update(base + offset for offset in BUTTON_FIELDS)
    return expected, allowed


def parse_expect_text(text: str) -> list[tuple[str, str]]:
    """'button:A' -> [('button','A')], 'axis:LY:+:100,axis:LX:-:50' -> [('axis','LY'), ('axis','LX')]."""
    result = []
    for item in text.split(","):
        parts = item.split(":")
        if len(parts) >= 2:
            result.append((parts[0], parts[1]))
    return result


@dataclass
class StepResult:
    n: int
    step_id: str
    kind: str
    expect: str
    changes: list[Change] = field(default_factory=list)
    flags: list[str] = field(default_factory=list)
    host_events: int | None = None
    mask_set: int = 0
    mask_cleared: int = 0
    axis_values: dict[str, tuple[int, int]] = field(default_factory=dict)

    @property
    def ok(self) -> bool:
        return not self.flags


def evaluate(record: dict, changes: list[Change] | None, have_pair: bool) -> StepResult:
    """Pure: flags of one step record against its pad array changes (changes is None when the dumps could not be read)."""
    expects = parse_expect_text(str(record.get("expect", "")))
    result = StepResult(
        int(record.get("n", 0)),
        str(record.get("id", "")),
        str(record.get("kind", "")),
        str(record.get("expect", "")),
        host_events=record.get("host_events"),
    )
    flags: set[str] = set()
    if str(record.get("note", "")).startswith("skipped"):
        flags.add("SKIPPED")
    elif not have_pair or changes is None:
        flags.add("NO-DUMPS")
    else:
        result.changes = changes
        expected, allowed = expected_offsets(result.kind, expects)
        changed = {item.offset for item in changes}
        if not changed:
            flags.add("NO-CHANGE")
        else:
            if not changed & expected:
                flags.add("MISSING-EXPECTED")
            if changed - expected - allowed:
                flags.add("UNEXPECTED")
        phys_current = PHYSICAL - PAD_BASE + 0x0C
        for item in changes:
            if item.offset == phys_current:
                result.mask_set = item.new & ~item.old & 0xFFFFFFFF
                result.mask_cleared = item.old & ~item.new & 0xFFFFFFFF
            slot = item.slot
            if slot.array == "physical" and slot.index == 0 and slot.field_offset in FIELDS:
                name = FIELDS[slot.field_offset]
                if name in AXIS_OFFSETS:
                    result.axis_values[name] = (item.old & 0xFF, item.new & 0xFF)
        events = result.host_events
        if result.kind == "digital" and events is not None and events == 0:
            flags.add("NO-HOST-EVENT")
        if result.kind == "analog" and events:
            flags.add("HOST-EDGE")
    result.flags = [name for name in FLAG_ORDER if name in flags]
    return result


def load_pad(path: Path) -> bytes | None:
    """The pad array range of one dump file, or None when it is missing, unreadable or partial."""
    selector = dump_report.RangeSelector(spec=dump_report.RangeSpec(False, PAD_BASE, 0))
    data, status, _, _ = dump_report.load_dump(path, [selector], dump_report.Window(0, PAD_LEN))[0]
    return data if status == "ok" else None


def read_session(directory: Path) -> tuple[dict, list[dict]]:
    header: dict = {}
    steps: list[dict] = []
    path = directory / SESSION_FILE
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if obj.get("type") == "start":
            header = obj
        elif obj.get("type") == "step":
            steps.append(obj)
    return header, steps


def analyze_session(directory: Path) -> tuple[dict, list[StepResult]]:
    header, records = read_session(directory)
    results = []
    for record in records:
        changes: list[Change] | None = None
        pairs = record.get("pairs") or []
        if pairs:
            pair = pairs[0]
            before = load_pad(directory / str(pair.get("before", "")))
            after = load_pad(directory / str(pair.get("after", "")))
            if before is not None and after is not None:
                changes = pad_changes(before, after)
        results.append(evaluate(record, changes, bool(pairs)))
    return header, results


def compact(changes: list[Change], limit: int = 6) -> str:
    if not changes:
        return "-"
    shown = [item.text() for item in changes[:limit]]
    if len(changes) > limit:
        shown.append(f"+{len(changes) - limit} more")
    return "; ".join(shown)


def render(header: dict, results: list[StepResult]) -> str:
    lines = [
        f"# Button checklist report (T1736), mode {header.get('mode', '?')}, pad range {header.get('range', '?')}",
        "",
        "MEASURED reads of the guest pad arrays (physical Pn = 0x6B93D0 + n*0x3C, effective En = 0x6B94C8 + n*0x3C), field names INFERRED",
        "(docs/t-guest-input-chain.md). Expected control: digital = the button words of port 0, analog = the axis dword of port 0.",
        "",
        "| # | step | expected | status | observed pad array changes | host events | flags |",
        "|---|---|---|---|---|---|---|",
    ]
    for item in results:
        status = "OK" if item.ok else "FLAG"
        events = "-" if item.host_events is None else str(item.host_events)
        flags = " ".join(item.flags) or "-"
        lines.append(
            f"| {item.n} | {item.step_id} | {item.expect} | {status} | {compact(item.changes)} | {events} | {flags} |"
        )
    bad = [item for item in results if not item.ok]
    lines += ["", f"{len(results)} steps, {len(results) - len(bad)} OK, {len(bad)} flagged."]
    masks = [item for item in results if item.kind == "digital" and item.mask_set]
    if masks:
        lines += [
            "",
            "## Guest button mask per digital step (physical P0 +0xC, bits set by the press)",
            "",
        ]
        lines += ["| step | expected | mask set | mask cleared |", "|---|---|---|---|"]
        for item in masks:
            lines.append(
                f"| {item.step_id} | {item.expect} | 0x{item.mask_set:04X} | 0x{item.mask_cleared:04X} |"
            )
    axes = [item for item in results if item.kind == "analog" and item.axis_values]
    if axes:
        lines += ["", "## Axis response (physical P0 low byte, rest -> held)", ""]
        lines += ["| step | expected | axes |", "|---|---|---|"]
        for item in axes:
            values = ", ".join(
                f"{name} {old}->{new}" for name, (old, new) in item.axis_values.items()
            )
            lines.append(f"| {item.step_id} | {item.expect} | {values} |")
    return "\n".join(lines) + "\n"


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("directory", help="session folder holding checklist.jsonl")
    parser.add_argument("--write", action="store_true", help="also write DIR/checklist_report.md")
    parser.add_argument("--strict", action="store_true", help="exit 1 when any step is flagged")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    directory = Path(args.directory)
    if not (directory / SESSION_FILE).exists():
        print(f"no {SESSION_FILE} in {directory}", file=sys.stderr)
        return 2
    header, results = analyze_session(directory)
    text = render(header, results)
    print(text, end="")
    if args.write:
        (directory / "checklist_report.md").write_text(text)
    return 1 if args.strict and any(not item.ok for item in results) else 0


if __name__ == "__main__":
    sys.exit(main())
