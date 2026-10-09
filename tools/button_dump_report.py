#!/usr/bin/env python3
# ruff: noqa: E501
"""T1629: report which guest-memory bytes change around each button press or release.

Input: a directory written by the host flag `--dump-on-button` (see the T1629 spec in docs): `buttons.jsonl` (JSON Lines
manifest: one `start` line, one `event` line per button event, `dropped` lines, an optional `end` line) and the dump
files `buttons/guestdump.<label>` (the text format of src/host/guest_dump.c: `indirect`, `pointer .. final ..`,
`range ADDR LEN` and `ADDR: bb bb ..` rows, several ranges per file).

For every non-idle event the BEFORE dump is compared with each AFTER dump over every selected range. By default that
is both the player extension block `*0x7356D8+0` (the weapon inventory) and the player record `*0x7B0C48+0`, one report
section each (ext first), a default range only when the dumps contain it. --range / --range-index (repeatable) select
others, --window narrows every range. All offsets are relative to the start of the range (record offsets such as
+0x58C). Changes are grouped to the 4 byte aligned DWORD (little endian, as on x86) with an optional per byte, 16 bit
word and float32 view. Known offsets of the ext block and the record are NAMED (INFERRED names, see the annotator
below and docs/t-player-inventory.md) and get cautious hints that end in `?`.

NOISE: fields that move without any input (positions, animation timers) would drown the signal, but a field that every
event of ONE button changes (the current weapon after each switch) is exactly the signal for that button. So the
frequency rule is PER BUTTON GROUP (the key of the per button tables, for example `press X`): a dword is noise for a
group when it also changed in at least --noise-fraction (default 0.30) of the events OUTSIDE that group (idle control
events included), given at least --noise-min-events (default 5) events outside it; with fewer outside events the rule
does not fire for that group. Two rules are global: a dword that changed in at least --idle-noise-min idle control
events (given at least 2 idle events, see --dump-button-idle-every of the host), and the dwords listed in
--noise-offsets. The frequency table is always printed, with a `noise for` column (the groups, or idle / listed). Noise
is hidden from the per button tables, the summary (an offset stays when it is signal for at least one group, listed
with those groups) and the event lines (offsets that are noise for that event's group) unless --show-noise.
--no-noise-filter switches the classification off.

Partial dumps: a dump whose selected range holds fewer bytes than the range (or the window) needs, because of an
`unreadable` row, a gap or a torn row, is EXCLUDED from the diffs, the event gets a warning and the section header
counts it under `dumps with partial data`. A dump that is unreadable only beyond the --window is still used.

Which after dumps count: --after first (default) uses the first usable after dump of each event for the noise
frequencies, --after all uses the union of all after dumps, --after K only the after dump at offset K frames (and the
tables then show only that one). The tables otherwise show every after dump.

Streaming: dump files are parsed one at a time (only the selected ranges are kept), so a 2000 dump set stays small.
Evidence class: xemu-level MEASURED observation of the unmodified game; a changed field is a CANDIDATE for the
button's effect, the meaning is INFERRED.

Exit codes: 0 ok, 1 --validate found manifest problems, 2 usage error or missing manifest, 3 every dump unreadable.

Usage: python -m tools.button_dump_report DIR [--range SPEC | --range-index N] [--window OFF:LEN] [--list-ranges]
       python -m tools.button_dump_report DIR --write      (writes DIR/button_report.md)
       python -m tools.button_dump_report DIR --validate   (checks buttons.jsonl against the spec)
       python -m tools.button_dump_report DIR --timeline   (adds the cross-event timeline, see tools.button_dump_timeline)
"""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import math
import re
import struct
import sys
from collections import Counter, defaultdict
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path
from typing import IO, Any

BUTTON_NAMES: tuple[str, ...] = (
    "UP",
    "DOWN",
    "LEFT",
    "RIGHT",
    "START",
    "BACK",
    "LTHUMB",
    "RTHUMB",
    "A",
    "B",
    "X",
    "Y",
    "BLACK",
    "WHITE",
    "LT",
    "RT",
)
GAME_BITS: dict[str, int] = {
    "UP": 0x04,
    "DOWN": 0x08,
    "LEFT": 0x01,
    "RIGHT": 0x02,
    "START": 0x200,
    "BACK": 0x100,
    "LTHUMB": 0x1000,
    "RTHUMB": 0x2000,
    "A": 0x80,
    "B": 0x20,
    "X": 0x10,
    "Y": 0x40,
    "BLACK": 0x4000,
    "WHITE": 0x8000,
    "LT": 0x400,
    "RT": 0x800,
}
DEFAULT_RANGE = "*0x7B0C48+0"
MANIFEST_NAME = "buttons.jsonl"
DUMP_SUBDIR = "buttons"
REPORT_NAME = "button_report.md"
VIEWS = ("dword", "byte", "float", "word")
EXIT_OK = 0
EXIT_INVALID = 1
EXIT_USAGE = 2
EXIT_UNREADABLE = 3
MAX_WARNINGS_SHOWN = 30
MAX_LISTED_EDGES = 64
MIN_NORMAL_FLOAT = 1.1754943508222875e-38

LABEL_RE = re.compile(r"^[0-9]{4,}_(press|release|mixed|idle)_[A-Za-z0-9-]+_(before|after[0-9]+)$")
MASK_RE = re.compile(r"^0x[0-9A-Fa-f]{4}$")
WALL_MS_RE = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3}Z$")
WALL_RE = re.compile(r"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?Z$")
INDIRECT_RE = re.compile(
    r"^indirect 0x([0-9A-Fa-f]+) \+ 0x([0-9A-Fa-f]+) length 0x([0-9A-Fa-f]+)\s*$"
)
INDIRECT2_RE = re.compile(
    r"^indirect2 0x([0-9A-Fa-f]+) \+ 0x([0-9A-Fa-f]+) \+ 0x([0-9A-Fa-f]+) length 0x([0-9A-Fa-f]+)\s*$"
)
POINTER_RE = re.compile(
    r"^pointer 0x([0-9A-Fa-f]+)(?: pointer2 0x([0-9A-Fa-f]+))?(?: final 0x([0-9A-Fa-f]+))?\s*$"
)
RANGE_LINE_RE = re.compile(r"^range 0x([0-9A-Fa-f]+) 0x([0-9A-Fa-f]+)\s*$")
HEX_TOKEN_RE = re.compile(r"^[0-9A-Fa-f]{2}$")


# --------------------------------------------------------------------------------------------------------------------
# Dump file parsing (streaming, only the selected range keeps its bytes)
# --------------------------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class RangeSpec:
    """`*0xADDR+OFF` (indirect: pointer dword at ADDR, final = pointer + OFF) or `0xADDR` (direct range start)."""

    indirect: bool
    address: int
    offset: int = 0

    def __str__(self) -> str:
        if self.indirect:
            return f"*0x{self.address:X}+{self.offset}"
        return f"0x{self.address:X}"


def parse_range_spec(text: str) -> RangeSpec:
    body = text.strip()
    indirect = body.startswith("*")
    if indirect:
        body = body[1:]
    address_text, plus, offset_text = body.partition("+")
    try:
        address = int(address_text, 0)
        offset = int(offset_text, 0) if plus else 0
    except ValueError:
        raise ValueError(
            f"bad range spec {text!r}: use *0xADDR+OFF (indirect) or 0xADDR (direct)"
        ) from None
    if address < 0 or offset < 0 or (plus and not indirect):
        raise ValueError(f"bad range spec {text!r}: use *0xADDR+OFF (indirect) or 0xADDR (direct)")
    return RangeSpec(indirect, address, offset)


@dataclass
class RangeInfo:
    index: int
    indirect: bool
    spec_address: int
    spec_offset: int
    length: int
    pointer: int | None = None
    address: int | None = None
    offset2: int | None = None  # T1759 two level `**A+OFF1+OFF2`: set, spec_offset is OFF1
    pointer2: int | None = None
    readable: bool = True
    reason: str = ""
    data: bytearray | None = None
    collect: bool = False

    def spec_text(self) -> str:
        if self.offset2 is not None:
            return f"**0x{self.spec_address:X}+{self.spec_offset}+{self.offset2}"
        if self.indirect:
            return f"*0x{self.spec_address:X}+{self.spec_offset}"
        return f"0x{self.spec_address:X}"


@dataclass
class ParsedDump:
    ranges: list[RangeInfo] = field(default_factory=list)
    selected: RangeInfo | None = None  # the first collected range
    collected: list[RangeInfo] = field(default_factory=list)


@dataclass(frozen=True)
class RangeSelector:
    spec: RangeSpec | None = None
    index: int | None = None

    def matches(self, info: RangeInfo) -> bool:
        if self.index is not None:
            return info.index == self.index
        assert self.spec is not None
        if self.spec.indirect:
            return (
                info.indirect
                and info.offset2 is None
                and info.spec_address == self.spec.address
                and info.spec_offset == self.spec.offset
            )
        return not info.indirect and info.spec_address == self.spec.address

    def describe(self) -> str:
        return f"index {self.index}" if self.index is not None else str(self.spec)


def open_dump_file(path: Path) -> IO[str]:
    """The only place a dump file is opened (tests count concurrent opens through this hook)."""
    return open(path, encoding="utf-8", errors="replace")  # noqa: SIM115 - used as a context manager by the caller


def _hex(text: str) -> int:
    return int(text, 16)


def parse_dump(path: Path, wanted: Callable[[RangeInfo], bool] | None = None) -> ParsedDump:
    """Parse headers of all ranges and the bytes of every range for which wanted(info) is true."""
    parsed = ParsedDump()
    current: RangeInfo | None = None
    with open_dump_file(path) as handle:
        for raw in handle:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("indirect "):
                match = INDIRECT_RE.match(line)
                if not match:
                    continue
                current = RangeInfo(
                    len(parsed.ranges),
                    True,
                    _hex(match.group(1)),
                    _hex(match.group(2)),
                    _hex(match.group(3)),
                )
                parsed.ranges.append(current)
                continue
            if line.startswith("indirect2 "):
                match2 = INDIRECT2_RE.match(line)
                if not match2:
                    continue
                current = RangeInfo(
                    len(parsed.ranges),
                    True,
                    _hex(match2.group(1)),
                    _hex(match2.group(2)),
                    _hex(match2.group(4)),
                    offset2=_hex(match2.group(3)),
                )
                parsed.ranges.append(current)
                continue
            if line.startswith("pointer "):
                match = POINTER_RE.match(line)
                if match and current is not None and current.indirect:
                    current.pointer = _hex(match.group(1))
                    if match.group(2) is not None:
                        current.pointer2 = _hex(match.group(2))
                continue
            if line.startswith("unreadable pointer"):
                if current is not None:
                    current.readable = False
                    current.reason = line
                continue
            if line.startswith("unreadable"):
                if current is not None:
                    current.readable = False
                    current.reason = line
                continue
            if line.startswith("range "):
                match = RANGE_LINE_RE.match(line)
                if not match:
                    continue
                address, length = _hex(match.group(1)), _hex(match.group(2))
                if (
                    current is not None
                    and current.indirect
                    and current.address is None
                    and current.readable
                ):
                    current.address = address
                else:
                    current = RangeInfo(len(parsed.ranges), False, address, 0, length)
                    current.address = address
                    parsed.ranges.append(current)
                if wanted is not None and wanted(current):
                    if parsed.selected is None:
                        parsed.selected = current
                    parsed.collected.append(current)
                    current.collect = True
                    current.data = bytearray()
                continue
            if (
                current is None
                or not current.collect
                or current.data is None
                or current.address is None
            ):
                continue
            _add_row(current, line)
    # an indirect range whose pointer was unreadable never gets a `range` line: it can still be the selection target
    return parsed


def _add_row(info: RangeInfo, line: str) -> None:
    assert info.data is not None and info.address is not None
    prefix, colon, rest = line.partition(":")
    if not colon:
        return
    try:
        address = int(prefix, 16)
    except ValueError:
        info.readable = False
        info.collect = False
        return
    tokens = rest.split()
    if any(HEX_TOKEN_RE.match(token) is None for token in tokens):
        # a torn last row: keep the complete leading bytes only
        good: list[str] = []
        for token in tokens:
            if HEX_TOKEN_RE.match(token) is None:
                break
            good.append(token)
        tokens = good
        info.readable = False
        info.collect = False
    offset = address - info.address
    if offset != len(info.data):
        info.readable = False
        info.collect = False
        return
    room = info.length - len(info.data)
    info.data.extend(bytes.fromhex("".join(tokens))[:room])


# --------------------------------------------------------------------------------------------------------------------
# Diff views
# --------------------------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class DwordChange:
    offset: int
    old: int
    new: int


def diff_dwords(
    before: bytes | bytearray, after: bytes | bytearray, start: int, end: int
) -> list[DwordChange]:
    """Changed 4 byte aligned dwords whose bytes inside [start, end) differ. end is clipped to both buffers."""
    end = min(end, len(before), len(after))
    if start >= end or before[start:end] == after[start:end]:
        return []
    changes: list[DwordChange] = []
    for offset in range(start - start % 4, end, 4):
        low = max(offset, start)
        high = min(offset + 4, end)
        if before[low:high] != after[low:high]:
            changes.append(
                DwordChange(
                    offset,
                    int.from_bytes(before[offset : offset + 4], "little"),
                    int.from_bytes(after[offset : offset + 4], "little"),
                )
            )
    return changes


def changed_bytes(change: DwordChange, start: int, end: int) -> list[tuple[int, int, int]]:
    result = []
    for index in range(4):
        position = change.offset + index
        if position < start or position >= end:
            continue
        old = (change.old >> (8 * index)) & 0xFF
        new = (change.new >> (8 * index)) & 0xFF
        if old != new:
            result.append((position, old, new))
    return result


def changed_words(change: DwordChange, start: int, end: int) -> list[tuple[int, int, int]]:
    result = []
    for index in range(2):
        position = change.offset + 2 * index
        if position + 2 <= start or position >= end:
            continue
        old = (change.old >> (16 * index)) & 0xFFFF
        new = (change.new >> (16 * index)) & 0xFFFF
        if old != new:
            result.append((position, old, new))
    return result


def as_float(value: int) -> float:
    return float(struct.unpack("<f", (value & 0xFFFFFFFF).to_bytes(4, "little"))[0])


def float_pair(old: int, new: int) -> tuple[float, float] | None:
    """Both float32 views, or None when either is NaN, infinite or a denormal (small integers are not floats)."""
    first, second = as_float(old), as_float(new)
    if all(
        math.isfinite(item) and (item == 0.0 or abs(item) >= MIN_NORMAL_FLOAT)
        for item in (first, second)
    ):
        return first, second
    return None


def format_float(value: float) -> str:
    return f"{value:.6g}"


def format_change(change: DwordChange, views: tuple[str, ...], start: int, end: int) -> str:
    parts: list[str] = []
    if "dword" in views:
        parts.append(f"0x{change.old:X}->0x{change.new:X}")
    if "word" in views:
        for position, old, new in changed_words(change, start, end):
            parts.append(f"w+0x{position:X}:{old:04X}>{new:04X}")
    if "byte" in views:
        for position, old, new in changed_bytes(change, start, end):
            parts.append(f"b+0x{position:X}:{old:02X}>{new:02X}")
    if "float" in views:
        pair = float_pair(change.old, change.new)
        if pair is not None:
            parts.append(f"f:{format_float(pair[0])}->{format_float(pair[1])}")
    return " ".join(parts)


def format_transition(offset: int, old: int, new: int, views: tuple[str, ...]) -> str:
    return format_change(DwordChange(offset, old, new), views, offset, offset + 4)


# --------------------------------------------------------------------------------------------------------------------
# Manifest loading (tolerant)
# --------------------------------------------------------------------------------------------------------------------


@dataclass
class Event:
    index: int
    edge: str
    button: str
    buttons: list[str]
    poll: int | None
    complete: bool
    dumps: list[dict[str, Any]]
    ok_false: bool = False

    @property
    def idle(self) -> bool:
        return self.edge == "idle"

    @property
    def name(self) -> str:
        if self.idle:
            return "idle"
        return "-".join(self.buttons) if self.buttons else self.button

    @property
    def group(self) -> str:
        return "idle" if self.idle else f"{self.edge} {self.name}"


@dataclass
class Manifest:
    start: dict[str, Any] | None = None
    end: dict[str, Any] | None = None
    events: list[Event] = field(default_factory=list)
    dropped: list[dict[str, Any]] = field(default_factory=list)
    unknown_lines: int = 0
    warnings: list[str] = field(default_factory=list)


def _event_from(obj: dict[str, Any]) -> Event | None:
    index = obj.get("index")
    if not isinstance(index, int) or isinstance(index, bool):
        return None
    dumps = obj.get("dumps")
    entries = [item for item in dumps if isinstance(item, dict)] if isinstance(dumps, list) else []
    buttons = obj.get("buttons")
    names = [str(item) for item in buttons] if isinstance(buttons, list) else []
    poll = obj.get("poll")
    edge = str(obj.get("edge", "")) or "unknown"
    kinds = [str(entry.get("kind", "")) for entry in entries]
    if edge == "unknown" and kinds and all(kind.startswith("idle") for kind in kinds):
        edge = "idle"
    return Event(
        index=index,
        edge=edge,
        button=str(obj.get("button", "")),
        buttons=names,
        poll=poll if isinstance(poll, int) and not isinstance(poll, bool) else None,
        complete=obj.get("complete") is not False,
        dumps=entries,
        ok_false=any(entry.get("ok") is False for entry in entries),
    )


def load_manifest(path: Path) -> Manifest:
    manifest = Manifest()
    lines = [
        (number, text.strip())
        for number, text in enumerate(
            path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
        )
    ]
    lines = [(number, text) for number, text in lines if text]
    by_index: dict[int, Event] = {}
    for position, (number, text) in enumerate(lines):
        try:
            obj = json.loads(text)
        except json.JSONDecodeError:
            if position == len(lines) - 1:
                manifest.warnings.append(f"manifest line {number}: truncated last line ignored")
            else:
                manifest.warnings.append(f"manifest line {number}: invalid JSON skipped")
            continue
        if not isinstance(obj, dict):
            manifest.warnings.append(f"manifest line {number}: not a JSON object, skipped")
            continue
        kind = obj.get("type")
        if kind == "start":
            if manifest.start is None:
                manifest.start = obj
        elif kind == "event":
            event = _event_from(obj)
            if event is None:
                manifest.warnings.append(
                    f"manifest line {number}: event without integer index skipped"
                )
                continue
            if event.index in by_index:
                manifest.warnings.append(
                    f"manifest line {number}: duplicate event index {event.index}, last one wins"
                )
            by_index[event.index] = event
        elif kind == "dropped":
            manifest.dropped.append(obj)
        elif kind == "end":
            manifest.end = obj
        else:
            manifest.unknown_lines += 1
    manifest.events = [by_index[index] for index in sorted(by_index)]
    if manifest.start is None:
        manifest.warnings.append("manifest has no start line")
    if manifest.end is None:
        manifest.warnings.append("manifest has no end line (run was killed or is still running)")
    return manifest


# --------------------------------------------------------------------------------------------------------------------
# Per event analysis
# --------------------------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class Window:
    start: int = 0
    length: int | None = None

    @property
    def end(self) -> int:
        return (1 << 62) if self.length is None else self.start + self.length


@dataclass
class AfterDiff:
    offset: int
    tag: str
    label: str
    status: str  # ok, missing, unreadable
    reason: str = ""
    changes: list[DwordChange] = field(default_factory=list)


@dataclass
class EventResult:
    event: Event
    before_status: str = "ok"
    before_reason: str = ""
    afters: list[AfterDiff] = field(default_factory=list)
    window_end: int = 0
    identity: RangeSpec | None = None

    def usable_afters(self) -> list[AfterDiff]:
        if self.before_status != "ok":
            return []
        return [after for after in self.afters if after.status == "ok"]


@dataclass
class DumpCounters:
    attempted: int = 0
    usable: int = 0
    missing: int = 0
    unreadable: int = 0
    partial: int = 0
    ok_false: int = 0


def _dump_path(directory: Path, entry: dict[str, Any]) -> Path | None:
    name = entry.get("file")
    if not isinstance(name, str) or not name:
        label = entry.get("label")
        if not isinstance(label, str) or not label:
            return None
        name = f"{DUMP_SUBDIR}/guestdump.{label}"
    relative = Path(name)
    if relative.is_absolute() or ".." in relative.parts:
        return None
    return directory / relative


LoadedRange = tuple[bytes | None, str, str, RangeSpec | None]


def load_dump(
    path: Path | None, selectors: list[RangeSelector], window: Window
) -> list[LoadedRange]:
    """Parse one dump file ONCE and return (data, status, reason, identity) per selector.

    status is ok / missing / unreadable. Only the selected ranges keep their bytes."""
    count = len(selectors)
    if path is None:
        return [(None, "missing", "no usable file name in the manifest", None)] * count
    try:
        parsed = parse_dump(path, lambda info: any(item.matches(info) for item in selectors))
    except FileNotFoundError:
        return [(None, "missing", f"{path.name} not on disk", None)] * count
    except OSError as error:
        return [(None, "missing", f"{path.name}: {error.strerror or error}", None)] * count
    loaded: list[LoadedRange] = []
    for selector in selectors:
        chosen = next(
            (item for item in parsed.collected if selector.matches(item) and item.data is not None),
            None,
        )
        if chosen is None or chosen.data is None:
            unreadable = next((item for item in parsed.ranges if selector.matches(item)), None)
            if unreadable is not None and not unreadable.readable:
                reason = (
                    f"{path.name}: range {selector.describe()} unreadable ({unreadable.reason})"
                )
            else:
                reason = f"{path.name}: range {selector.describe()} not in the dump"
            loaded.append((None, "unreadable", reason, None))
            continue
        identity = RangeSpec(chosen.indirect, chosen.spec_address, chosen.spec_offset)
        needed = min(chosen.length, window.end)
        if len(chosen.data) < needed:
            # a mid-range unreadable row, a torn row or a gap: the dump is NOT diffed (documented in the module docstring)
            why = f" ({chosen.reason})" if chosen.reason else ""
            reason = (
                f"{path.name}: range {selector.describe()} partial data, {len(chosen.data)} of "
                f"{needed} bytes readable{why}: excluded from the diff"
            )
            loaded.append((None, "partial", reason, identity))
            continue
        if len(chosen.data) <= window.start:
            reason = f"{path.name}: range holds {len(chosen.data)} bytes, window starts at {window.start}"
            loaded.append((None, "unreadable", reason, identity))
            continue
        loaded.append((bytes(chosen.data), "ok", "", identity))
    return loaded


def _tag_offset(entry: dict[str, Any]) -> int:
    offset = entry.get("offset")
    if isinstance(offset, int) and not isinstance(offset, bool):
        return offset
    match = re.search(r"([0-9]+)$", str(entry.get("tag", "")))
    return int(match.group(1)) if match else 0


def analyze_event_multi(
    event: Event,
    directory: Path,
    selectors: list[RangeSelector],
    window: Window,
    only_offset: int | None,
    counters: list[DumpCounters],
    warnings: list[list[str]],
) -> list[EventResult]:
    """Diff one event over every selected range; each dump file is parsed once for all of them."""
    results = [EventResult(event) for _ in selectors]
    before_entry = next(
        (
            item
            for item in event.dumps
            if str(item.get("kind", "")).endswith("before") or item.get("tag") == "before"
        ),
        None,
    )
    after_entries = sorted(
        (item for item in event.dumps if item is not before_entry),
        key=lambda item: _tag_offset(item),
    )
    if only_offset is not None:
        after_entries = [item for item in after_entries if _tag_offset(item) == only_offset]
    if before_entry is None:
        for index, result in enumerate(results):
            result.before_status, result.before_reason = "missing", "no before dump listed"
            warnings[index].append(f"event {event.index}: no before dump listed")
        return results
    befores: list[bytes | None] = []
    for index, (data, status, reason, identity) in enumerate(
        load_dump(_dump_path(directory, before_entry), selectors, window)
    ):
        counters[index].attempted += 1
        if before_entry.get("ok") is False:
            counters[index].ok_false += 1
        results[index].identity = identity
        befores.append(data)
        if data is None:
            _count_failure(counters[index], status)
            results[index].before_status, results[index].before_reason = status, reason
            warnings[index].append(f"event {event.index} before: {reason}")
        else:
            counters[index].usable += 1
    if all(data is None for data in befores):
        return results
    for entry in after_entries:
        loaded = load_dump(_dump_path(directory, entry), selectors, window)
        for index, (after, status, reason, _) in enumerate(loaded):
            before = befores[index]
            if before is None:
                continue
            counters[index].attempted += 1
            if entry.get("ok") is False:
                counters[index].ok_false += 1
            diff = AfterDiff(
                _tag_offset(entry),
                str(entry.get("tag", "")),
                str(entry.get("label", "")),
                status,
                reason,
            )
            if after is None:
                _count_failure(counters[index], status)
                warnings[index].append(f"event {event.index} {diff.tag}: {reason}")
            else:
                counters[index].usable += 1
                diff.changes = diff_dwords(before, after, window.start, window.end)
                results[index].window_end = max(
                    results[index].window_end, min(window.end, len(before), len(after))
                )
            results[index].afters.append(diff)
    for index, before in enumerate(befores):
        if before is not None and results[index].window_end == 0:
            results[index].window_end = min(window.end, len(before))
    return results


def analyze_event(
    event: Event,
    directory: Path,
    selector: RangeSelector,
    window: Window,
    only_offset: int | None,
    counters: DumpCounters,
    warnings: list[str],
) -> EventResult:
    """Single range form of analyze_event_multi."""
    return analyze_event_multi(
        event, directory, [selector], window, only_offset, [counters], [warnings]
    )[0]


def _count_failure(counters: DumpCounters, status: str) -> None:
    if status == "missing":
        counters.missing += 1
    elif status == "partial":
        counters.partial += 1
    else:
        counters.unreadable += 1


# --------------------------------------------------------------------------------------------------------------------
# Noise
# --------------------------------------------------------------------------------------------------------------------


def parse_offset_items(text: str) -> list[tuple[int, int]]:
    """Comma list of `0x58C` or `0x10-0x40` (inclusive byte ranges); raises ValueError on bad input."""
    items: list[tuple[int, int]] = []
    for part in text.replace("\n", ",").split(","):
        part = part.split("#", 1)[0].strip()
        if not part:
            continue
        for token in part.split():
            low_text, dash, high_text = token.partition("-")
            try:
                low = int(low_text, 0)
                high = int(high_text, 0) if dash else low
            except ValueError:
                raise ValueError(f"bad noise offset {token!r}: use 0x58C or 0x10-0x40") from None
            if low < 0 or high < low:
                raise ValueError(f"bad noise offset range {token!r}")
            items.append((low, high))
    return items


def load_noise_offsets(spec: str) -> list[tuple[int, int]]:
    if spec.startswith("@"):
        try:
            return parse_offset_items(Path(spec[1:]).read_text(encoding="utf-8"))
        except OSError as error:
            raise ValueError(
                f"cannot read noise offsets file {spec[1:]!r}: {error.strerror or error}"
            ) from None
    return parse_offset_items(spec)


@dataclass
class NoiseConfig:
    fraction: Fraction = Fraction(3, 10)
    min_events: int = 5
    idle_min: int = 1
    listed: list[tuple[int, int]] = field(default_factory=list)
    enabled: bool = True


@dataclass
class FrequencyStats:
    """Change counts over the usable events: all events, per group (a button group or `idle`) and idle."""

    total: int = 0
    counts: Counter[int] = field(default_factory=Counter)
    idle_total: int = 0
    idle_counts: Counter[int] = field(default_factory=Counter)
    group_total: Counter[str] = field(default_factory=Counter)
    group_counts: dict[str, Counter[int]] = field(default_factory=lambda: defaultdict(Counter))


def stat_afters(result: EventResult, mode: str) -> list[AfterDiff]:
    usable = sorted(result.usable_afters(), key=lambda after: after.offset)
    if not usable:
        return []
    if mode == "first":
        return usable[:1]
    return usable


def compute_frequencies(results: list[EventResult], mode: str) -> FrequencyStats:
    stats = FrequencyStats()
    for result in results:
        chosen = stat_afters(result, mode)
        if not chosen:
            continue
        offsets = {change.offset for after in chosen for change in after.changes}
        stats.total += 1
        stats.counts.update(offsets)
        group = result.event.group
        stats.group_total[group] += 1
        stats.group_counts[group].update(offsets)
        if result.event.idle:
            stats.idle_total += 1
            stats.idle_counts.update(offsets)
    return stats


def frequency_noise(offset: int, group: str, stats: FrequencyStats, config: NoiseConfig) -> bool:
    """Noise by frequency FOR a group: the offset also changed in enough events OUTSIDE the group.

    Idle events count as outside. With fewer than --noise-min-events outside events the rule does not fire,
    so a field that every switch changes is signal for the switch group while it is noise for the others."""
    outside_total = stats.total - stats.group_total[group]
    if outside_total < config.min_events:
        return False
    outside = stats.counts[offset] - stats.group_counts[group][offset]
    return Fraction(outside, outside_total) >= config.fraction


def noise_reasons(
    offset: int, stats: FrequencyStats, config: NoiseConfig, group: str | None = None
) -> list[str]:
    """Why an offset is noise: with a group frequency, idle and listed; without one only idle and listed."""
    if not config.enabled:
        return []
    reasons = []
    if group is not None and frequency_noise(offset, group, stats, config):
        reasons.append("frequency")
    if stats.idle_total >= 2 and stats.idle_counts[offset] >= config.idle_min:
        reasons.append("idle")
    if any(low <= offset + 3 and offset <= high for low, high in config.listed):
        reasons.append("listed")
    return reasons


def noise_groups(offset: int, stats: FrequencyStats, config: NoiseConfig) -> list[str]:
    """The button groups that changed the offset and for which it is frequency noise."""
    if not config.enabled:
        return []
    return sorted(
        group
        for group in stats.group_total
        if group != "idle"
        and stats.group_counts[group][offset] > 0
        and frequency_noise(offset, group, stats, config)
    )


# --------------------------------------------------------------------------------------------------------------------
# Field annotator (table driven, every name INFERRED from docs/t-player-inventory.md)
# --------------------------------------------------------------------------------------------------------------------

EXT_SPEC = "*0x7356D8+0"
DEFAULT_RANGES = (EXT_SPEC, DEFAULT_RANGE)
EXT_SIZE = 0xEA0
RECORD_SIZE = 0x1584
AMMO_TYPES = 44
OWNED_ENTRIES = 0x46
ENTRIES_CSV = (
    Path(__file__).resolve().parents[1] / "docs" / "data" / "t-player-inventory-entries.csv"
)
LEGEND = (
    "Field names (ext.* = player extension block *0x7356D8+slot*0xEA0, rec.* = player record "
    "*0x7B0C48+slot*0x1584) come from docs/t-player-inventory.md and are INFERRED from the disassembly "
    "unless that document marks them MEASURED. Hints after '=>' end in '?': they are guesses from the "
    "direction of a change, never claims. Entry role, group and record come from "
    "docs/data/t-player-inventory-entries.csv (XBE table bytes, MEASURED) when it is available."
)


@dataclass(frozen=True)
class FieldRule:
    """One named field or array of fields of a block: count elements of size bytes from start.

    label and short are format templates that receive the element number as {i}. value says how numbers
    print: hex, dec, sdec (signed), entry (weapon entry number) or record (signed record index)."""

    start: int
    count: int
    size: int
    label: str
    short: str
    family: str
    value: str = "hex"
    index_kind: str = ""

    @property
    def end(self) -> int:
        return self.start + self.count * self.size


def _hand_rules(side: str, base: int) -> list[FieldRule]:
    """A hand struct of the ext block (0x130 bytes): record index, loaded rounds and cap words per ammo type."""
    return [
        FieldRule(
            base + 0x30,
            1,
            4,
            f"ext.{side}_hand.record_index",
            f"{side}_record",
            "hand_record",
            "record",
        ),
        FieldRule(
            base + 0x38,
            AMMO_TYPES,
            2,
            f"ext.{side}_hand.loaded[type {{i}}]",
            f"{side}_loaded[type {{i}}]",
            "loaded",
            "dec",
            "type",
        ),
        FieldRule(
            base + 0x90,
            AMMO_TYPES,
            2,
            f"ext.{side}_hand.cap[type {{i}}]",
            f"{side}_cap[type {{i}}]",
            "cap",
            "dec",
            "type",
        ),
    ]


def _slot_rules(name: str, base: int) -> list[FieldRule]:
    """A weapon slot struct of the player record (0x3D8 bytes): held record index, dual flag, state."""
    return [
        FieldRule(
            base + 0x30, 1, 4, f"rec.{name}.record_index", f"{name}_record", "slot_record", "record"
        ),
        FieldRule(base + 0x40, 1, 4, f"rec.{name}.dual_wield", f"{name}_dual", "dual", "dec"),
        FieldRule(base + 0x44, 1, 4, f"rec.{name}.state", f"{name}_state", "slot_state", "dec"),
    ]


def _entry_array(start: int, count: int, name: str, short: str, family: str) -> FieldRule:
    return FieldRule(
        start,
        count,
        1,
        f"ext.{name}[entry 0x{{i:02X}}]",
        f"{short}[0x{{i:02X}}]",
        family,
        "dec",
        "entry",
    )


EXT_RULES: list[FieldRule] = sorted(
    [
        FieldRule(0x00, 1, 4, "ext.actor_ptr", "actor_ptr", "pointer"),
        FieldRule(0x08, 1, 4, "ext.slot", "slot", "slot", "dec"),
        FieldRule(0x28, 1, 4, "ext.flags", "flags", "flags"),
        FieldRule(0x80, 1, 4, "ext.chain_flag", "chain_flag", "chain_flag", "dec"),
        FieldRule(0x94, 1, 4, "ext.current_weapon", "current_weapon", "current", "entry"),
        FieldRule(0x98, 1, 4, "ext.previous_weapon", "previous_weapon", "previous", "entry"),
        FieldRule(0x9C, 1, 4, "ext.secondary_weapon", "secondary_weapon", "secondary", "entry"),
        FieldRule(0xA0, 1, 4, "ext.default_throwable", "default_throwable", "throwable", "entry"),
        *_hand_rules("left", 0xA4),
        *_hand_rules("main", 0x1D4),
        _entry_array(0x54C, OWNED_ENTRIES, "owned", "owned", "owned"),
        _entry_array(0x594, 0x44, "entry_array2", "array2", "array2"),
        _entry_array(0x5D8, OWNED_ENTRIES, "left_hand_byte", "lefthand", "left_hand_byte"),
        FieldRule(
            0x61E,
            AMMO_TYPES,
            2,
            "ext.reserve_ammo[type {i}]",
            "reserve_ammo[type {i}]",
            "reserve",
            "dec",
            "type",
        ),
        FieldRule(0x678, 1, 4, "ext.slot_valid", "slot_valid", "slot_valid", "dec"),
    ],
    key=lambda rule: rule.start,
)
REC_RULES: list[FieldRule] = sorted(
    [
        FieldRule(0x00, 1, 4, "rec.kind", "kind", "kind", "dec"),
        FieldRule(0x04, 1, 4, "rec.player_id", "player_id", "player_id", "sdec"),
        FieldRule(0x14, 1, 4, "rec.actor_ptr", "actor_ptr", "pointer"),
        *_slot_rules("left_hand", 0x184),
        *_slot_rules("main_hand", 0x55C),
        *_slot_rules("third_hand", 0x934),
        FieldRule(
            0xE3C,
            OWNED_ENTRIES,
            1,
            "rec.group_order[group {i}]",
            "group_order[{i}]",
            "group_order",
            "entry",
            "group",
        ),
        FieldRule(0x14F8, 1, 2, "rec.weapon_class", "weapon_class", "weapon_class"),
    ],
    key=lambda rule: rule.start,
)


@dataclass(frozen=True)
class Block:
    name: str
    address: int
    size: int
    rules: tuple[FieldRule, ...]
    starts: tuple[int, ...]

    def lookup(self, position: int) -> tuple[FieldRule, int] | None:
        """The rule and element number that cover a block offset, or None."""
        found = bisect.bisect_right(self.starts, position) - 1
        if found < 0:
            return None
        rule = self.rules[found]
        if position >= rule.end:
            return None
        return rule, (position - rule.start) // rule.size


def _block(name: str, address: int, size: int, rules: list[FieldRule]) -> Block:
    return Block(name, address, size, tuple(rules), tuple(rule.start for rule in rules))


BLOCKS: dict[str, Block] = {
    "ext": _block("ext", 0x7356D8, EXT_SIZE, EXT_RULES),
    "rec": _block("rec", 0x7B0C48, RECORD_SIZE, REC_RULES),
}


def block_for_spec(spec: RangeSpec | None) -> Block | None:
    if spec is None or not spec.indirect:
        return None
    return next((block for block in BLOCKS.values() if block.address == spec.address), None)


@dataclass(frozen=True)
class EntryInfo:
    group: str
    role: str
    record: str


def parse_entry_number(text: str) -> int:
    return int(str(text).strip(), 0)


def load_entries_csv(path: Path) -> dict[int, EntryInfo]:
    """entry -> role / group / record from the T1628 entry table. Raises ValueError on a malformed file."""
    entries: dict[int, EntryInfo] = {}
    try:
        with open(path, newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                entries[parse_entry_number(row["entry"])] = EntryInfo(
                    row["group"].strip(), row["role"].strip(), row["record_0x10"].strip()
                )
    except (KeyError, ValueError, AttributeError) as error:
        raise ValueError(f"cannot parse entries csv {path}: {error!r}") from None
    except OSError as error:
        raise ValueError(f"cannot read entries csv {path}: {error.strerror or error}") from None
    return entries


def load_entry_names(path: Path) -> dict[int, str]:
    """JSON object {"0x36": "Name", "54": "Name"} -> entry number -> name."""
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(raw, dict):
            raise ValueError("not a JSON object")
        return {parse_entry_number(key): str(value) for key, value in raw.items()}
    except OSError as error:
        raise ValueError(f"cannot read entry names {path}: {error.strerror or error}") from None
    except ValueError as error:
        raise ValueError(f"bad entry names file {path}: {error}") from None


def format_value(value: int, rule: FieldRule) -> str:
    if rule.value == "dec":
        return str(value)
    if rule.value in ("sdec", "record"):
        bits = 8 * rule.size
        return str(value - (1 << bits) if value >= 1 << (bits - 1) else value)
    if rule.value == "entry":
        return f"0x{value:02X}"
    return f"0x{value:X}"


@dataclass
class FieldChange:
    rule: FieldRule
    index: int
    slot: int
    old: int
    new: int
    hint: str = ""

    def _name(self, template: str, short: bool = False) -> str:
        text = template.format(i=self.index)
        if not self.slot:
            return text
        if short:
            return f"{text}@slot{self.slot}"
        prefix, dot, rest = text.partition(".")
        return f"{prefix}[{self.slot}]{dot}{rest}"

    @property
    def label(self) -> str:
        return self._name(self.rule.label)

    @property
    def short(self) -> str:
        return self._name(self.rule.short, True)

    def transition(self) -> str:
        return f"{format_value(self.old, self.rule)}->{format_value(self.new, self.rule)}"

    def hint_text(self) -> str:
        """The compact semantic line: `owned[0x36] 0->1 (pickup?)`."""
        return f"{self.short} {self.transition()} ({self.hint})" if self.hint else ""


def _hint_word(change: FieldChange, reloads: set[int]) -> str:
    family, old, new = change.rule.family, change.old, change.new
    if family == "owned":
        if old == 0 and new != 0:
            return "pickup?"
        if old != 0 and new == 0:
            return "release?"
    elif family in ("current", "previous"):
        return "switch?"
    elif family == "reserve":
        if change.index in reloads:
            return "reload?"
        return "fire?" if new < old else "ammo gain?"
    elif family == "loaded":
        if change.index in reloads:
            return "reload?"
        return "fire?" if new < old else "load?"
    elif family in ("hand_record", "slot_record"):
        return "weapon change?"
    elif family == "dual":
        return "dual-wield on?" if new > old else "dual-wield off?"
    elif family == "group_order":
        return "group order change?"
    return ""


class Annotator:
    """Names changed offsets of one block and derives the cautious semantic hints."""

    def __init__(
        self,
        block: Block,
        spec_offset: int = 0,
        entries: dict[int, EntryInfo] | None = None,
        names: dict[int, str] | None = None,
    ) -> None:
        self.block = block
        self.spec_offset = spec_offset
        self.entries = entries or {}
        self.names = names or {}

    def _locate(self, position: int) -> tuple[FieldRule, int, int] | None:
        absolute = position + self.spec_offset
        slot, inside = divmod(absolute, self.block.size)
        hit = self.block.lookup(inside)
        return None if hit is None else (hit[0], hit[1], slot)

    def fields(self, change: DwordChange, start: int, end: int) -> list[FieldChange]:
        """The named elements whose bytes changed inside this dword, without hints."""
        result: list[FieldChange] = []
        seen: set[tuple[int, int, int]] = set()
        for position, _, _ in changed_bytes(change, start, end):
            located = self._locate(position)
            if located is None:
                continue
            rule, index, slot = located
            key = (rule.start, index, slot)
            if key in seen:
                continue
            seen.add(key)
            element = rule.start + index * rule.size + slot * self.block.size - self.spec_offset
            shift = max(0, (element - change.offset) * 8)
            mask = (1 << (8 * rule.size)) - 1
            result.append(
                FieldChange(
                    rule, index, slot, (change.old >> shift) & mask, (change.new >> shift) & mask
                )
            )
        return result

    def annotate(
        self, changes: Iterable[DwordChange], start: int, end: int
    ) -> dict[int, list[FieldChange]]:
        """Fields per dword offset with hints (a reload needs the reserve and loaded change together)."""
        per_offset = {change.offset: self.fields(change, start, end) for change in changes}
        every = [item for items in per_offset.values() for item in items]
        loaded_up = {
            item.index for item in every if item.rule.family == "loaded" and item.new > item.old
        }
        reserve_down = {
            item.index for item in every if item.rule.family == "reserve" and item.new < item.old
        }
        reloads = loaded_up & reserve_down
        for item in every:
            item.hint = _hint_word(item, reloads)
        return per_offset

    def offset_labels(self, offset: int) -> list[str]:
        """Names of every element a dword overlaps, for tables that have no old/new values."""
        labels: list[str] = []
        for position in range(offset, offset + 4):
            located = self._locate(position)
            if located is None:
                continue
            rule, index, slot = located
            label = FieldChange(rule, index, slot, 0, 0).label
            if label not in labels:
                labels.append(label)
        return labels

    def entry_text(self, entry: int) -> str:
        """` "Name" (role gN rec R)` for a weapon entry, empty when nothing is known."""
        parts = ""
        if entry in self.names:
            parts += f' "{self.names[entry]}"'
        info = self.entries.get(entry)
        if info is not None:
            parts += f" ({info.role} g{info.group} rec {info.record})"
        return parts

    def describe(self, item: FieldChange) -> str:
        """`ext.owned[entry 0x36] (primary g5 rec 37) 0->1`, with the new entry's info for entry values."""
        text = item.label
        if item.rule.index_kind == "entry":
            text += self.entry_text(item.index)
        text += f" {item.transition()}"
        if item.rule.value == "entry" and item.rule.index_kind == "":
            text += self.entry_text(item.new)
        return text

    def labels(self, items: Iterable[FieldChange]) -> list[str]:
        result: list[str] = []
        for item in items:
            text = item.label + (
                self.entry_text(item.index) if item.rule.index_kind == "entry" else ""
            )
            if text not in result:
                result.append(text)
        return result


# --------------------------------------------------------------------------------------------------------------------
# Report model
# --------------------------------------------------------------------------------------------------------------------


@dataclass
class Table:
    title: str
    headers: list[str]
    rows: list[list[str]]
    note: str = ""


@dataclass
class ReportOptions:
    views: tuple[str, ...] = ("dword", "byte", "float")
    show_noise: bool = False
    top: int = 40
    max_event_lines: int = 300
    max_changes_per_line: int = 24
    max_group_rows: int = 60


@dataclass
class Report:
    meta: dict[str, Any]
    warnings: list[str]
    frequency: Table
    groups: list[Table]
    summary: Table
    event_lines: list[str]
    data: dict[str, Any]
    name: str = ""
    annotated: bool = False


def _hex_offset(offset: int) -> str:
    return f"0x{offset:X}"


def build_report(
    results: list[EventResult],
    stats: FrequencyStats,
    config: NoiseConfig,
    options: ReportOptions,
    window: Window,
    meta: dict[str, Any],
    warnings: list[str],
    annotator: Annotator | None = None,
    name: str = "",
) -> Report:
    noise_cache: dict[tuple[int, str | None], list[str]] = {}
    groups_cache: dict[int, list[str]] = {}

    def reasons_for(offset: int, group: str | None = None) -> list[str]:
        if (offset, group) not in noise_cache:
            noise_cache[(offset, group)] = noise_reasons(offset, stats, config, group)
        return noise_cache[(offset, group)]

    def groups_for(offset: int) -> list[str]:
        if offset not in groups_cache:
            groups_cache[offset] = noise_groups(offset, stats, config)
        return groups_cache[offset]

    def is_noise(offset: int, group: str) -> bool:
        return bool(reasons_for(offset, group))

    def visible(offset: int, group: str) -> bool:
        return options.show_noise or not is_noise(offset, group)

    def mark(offset: int, group: str) -> str:
        return _hex_offset(offset) + ("~" if is_noise(offset, group) else "")

    def noise_for(offset: int) -> str:
        """The `noise for` column: listed / idle (hidden everywhere), else the groups it is frequency noise for."""
        everywhere = reasons_for(offset)
        if everywhere:
            return "+".join(everywhere)
        return ", ".join(groups_for(offset)) or "-"

    def change_text(
        change: DwordChange,
        window_end: int,
        annotated: dict[int, list[FieldChange]],
        group: str,
    ) -> str:
        text = f"{mark(change.offset, group)} {format_change(change, options.views, window.start, window_end)}"
        items = annotated.get(change.offset, [])
        if annotator is not None and items:
            text += " <" + "; ".join(annotator.describe(item) for item in items) + ">"
        return text

    def fields_text(offset: int, old: int, new: int) -> str:
        if annotator is None:
            return ""
        change = DwordChange(offset, old, new)
        items = annotator.annotate([change], window.start, window.end)[offset]
        return ", ".join(annotator.labels(items)) or "-"

    # frequency table (always printed)
    ranked = sorted(stats.counts, key=lambda offset: (-stats.counts[offset], offset))
    frequency_rows = []
    for offset in ranked[: options.top]:
        count = stats.counts[offset]
        row = [
            _hex_offset(offset),
            f"{count} of {stats.total}",
            f"{count / stats.total:.3f}",
            f"{stats.idle_counts[offset]} of {stats.idle_total}",
            noise_for(offset),
        ]
        if annotator is not None:
            row.append(", ".join(annotator.offset_labels(offset)) or "-")
        frequency_rows.append(row)
    shown = len(frequency_rows)
    frequency = Table(
        "Change frequency per dword (events, most frequent first)",
        ["offset", "changed in", "fraction", "idle events", "noise for"]
        + (["fields"] if annotator is not None else []),
        frequency_rows,
        f"{shown} of {len(ranked)} changed offsets shown (--top {options.top}); "
        f"noise for a button group: changed in >= {float(config.fraction):g} of the events outside the group "
        f"(needs >= {config.min_events} outside events); noise everywhere: idle >= {config.idle_min} "
        "(needs >= 2 idle events) or listed"
        + ("" if config.enabled else "; noise filter DISABLED"),
    )

    # per event data (non-idle, usable afters)
    group_events: dict[str, list[EventResult]] = defaultdict(list)
    for result in results:
        if not result.event.idle and result.usable_afters():
            group_events[result.event.group].append(result)

    offset_events: dict[int, set[int]] = defaultdict(set)
    offset_groups: dict[int, set[str]] = defaultdict(set)
    offset_signal: dict[int, set[str]] = defaultdict(set)
    offset_transitions: dict[int, Counter[tuple[int, int]]] = defaultdict(Counter)
    offset_hints: dict[int, Counter[str]] = defaultdict(Counter)
    event_lines: list[str] = []
    json_events: list[dict[str, Any]] = []
    group_tables: list[Table] = []
    json_groups: dict[str, Any] = {}

    for group in sorted(group_events):
        events = group_events[group]
        counts: Counter[int] = Counter()
        transitions: dict[int, Counter[tuple[int, int]]] = defaultdict(Counter)
        first_event: dict[int, int] = {}
        for result in events:
            per_event: dict[int, set[tuple[int, int]]] = defaultdict(set)
            per_event_hints: dict[int, set[str]] = defaultdict(set)
            for after in result.usable_afters():
                annotated = (
                    annotator.annotate(after.changes, window.start, result.window_end)
                    if annotator is not None
                    else {}
                )
                for change in after.changes:
                    if visible(change.offset, group):
                        per_event[change.offset].add((change.old, change.new))
                        for item in annotated.get(change.offset, []):
                            if item.hint:
                                per_event_hints[change.offset].add(item.hint_text())
            for offset, texts in per_event_hints.items():
                offset_hints[offset].update(texts)
            for offset, pairs in per_event.items():
                counts[offset] += 1
                first_event.setdefault(offset, result.event.index)
                offset_events[offset].add(result.event.index)
                offset_groups[offset].add(group)
                if not is_noise(offset, group):
                    offset_signal[offset].add(group)
                for pair in pairs:
                    transitions[offset][pair] += 1
                    offset_transitions[offset][pair] += 1
        rows = []
        for offset in sorted(counts, key=lambda item: (-counts[item], item)):
            (old, new), _ = transitions[offset].most_common(1)[0]
            row = [
                group,
                mark(offset, group),
                f"{counts[offset]} of {len(events)}",
                format_transition(offset, old, new, options.views),
            ]
            if annotator is not None:
                row.append(fields_text(offset, old, new))
            rows.append([*row, f"#{first_event[offset]:04d}"])
        omitted = max(0, len(rows) - options.max_group_rows)
        group_tables.append(
            Table(
                f"{group} ({len(events)} events)",
                ["button", "offset", "events changed", "typical old->new"]
                + (["fields"] if annotator is not None else [])
                + ["first event"],
                rows[: options.max_group_rows],
                f"{omitted} more offsets omitted (--max-group-rows)" if omitted else "",
            )
        )
        json_groups[group] = {
            "events": [result.event.index for result in events],
            "offsets": {
                _hex_offset(offset): {
                    "events_changed": counts[offset],
                    "transitions": [
                        {"old": f"0x{old:X}", "new": f"0x{new:X}", "events": count}
                        for (old, new), count in transitions[offset].most_common()
                    ],
                }
                for offset in sorted(counts)
            },
        }

    for result in sorted(
        (r for r in results if not r.event.idle), key=lambda item: item.event.index
    ):
        for after in result.usable_afters():
            group = result.event.group
            shown_changes = [change for change in after.changes if visible(change.offset, group)]
            hidden = len(after.changes) - len(shown_changes)
            head = f"#{result.event.index:04d} {result.event.group} poll {result.event.poll} +{after.offset}:"
            annotated = (
                annotator.annotate(after.changes, window.start, result.window_end)
                if annotator is not None
                else {}
            )

            body = " | ".join(
                change_text(change, result.window_end, annotated, group)
                for change in shown_changes[: options.max_changes_per_line]
            )
            if len(shown_changes) > options.max_changes_per_line:
                body += f" | ... +{len(shown_changes) - options.max_changes_per_line} more"
            if not shown_changes:
                body = "(no signal changes)"
            if hidden:
                body += f" (+{hidden} noise hidden)"
            hints = [
                item.hint_text()
                for change in shown_changes
                for item in annotated.get(change.offset, [])
                if item.hint
            ]
            if hints:
                body += " => " + "; ".join(hints[:8]) + (" ..." if len(hints) > 8 else "")
            event_lines.append(f"{head} {body}")
            json_events.append(
                {
                    "index": result.event.index,
                    "group": result.event.group,
                    "poll": result.event.poll,
                    "after": after.offset,
                    "changes": [
                        {
                            "offset": _hex_offset(change.offset),
                            "old": f"0x{change.old:X}",
                            "new": f"0x{change.new:X}",
                            "noise": is_noise(change.offset, group),
                            **(
                                {
                                    "fields": [
                                        {
                                            "label": item.label,
                                            "old": format_value(item.old, item.rule),
                                            "new": format_value(item.new, item.rule),
                                            "hint": item.hint,
                                        }
                                        for item in annotated.get(change.offset, [])
                                    ]
                                }
                                if annotator is not None
                                else {}
                            ),
                        }
                        for change in after.changes
                    ],
                }
            )

    summary_rows = []
    json_offsets = {}
    for offset in sorted(offset_events):
        pairs = offset_transitions[offset].most_common(5)
        transition_text = "; ".join(
            f"{format_transition(offset, old, new, options.views)} x{count}"
            for (old, new), count in pairs
        )
        extra = len(offset_transitions[offset]) - len(pairs)
        if extra > 0:
            transition_text += f"; +{extra} more"
        indexes = sorted(offset_events[offset])
        sample = ",".join(f"#{index:04d}" for index in indexes[:6]) + (
            ",..." if len(indexes) > 6 else ""
        )
        top_old, top_new = offset_transitions[offset].most_common(1)[0][0]
        hint_pairs = offset_hints[offset].most_common(3)
        hint_text = "; ".join(f"{text} x{count}" for text, count in hint_pairs)
        extra_columns = (
            [fields_text(offset, top_old, top_new), hint_text or "-"]
            if annotator is not None
            else []
        )
        summary_rows.append(
            [
                _hex_offset(offset) + ("" if offset_signal[offset] else "~"),
                str(len(indexes)),
                ", ".join(sorted(offset_groups[offset])),
                transition_text,
                *extra_columns,
                sample,
            ]
        )
        json_offsets[_hex_offset(offset)] = {
            "events": indexes,
            "buttons": sorted(offset_groups[offset]),
            "noise": not offset_signal[offset],
            "signal_groups": sorted(offset_signal[offset]),
            "transitions": [
                {"old": f"0x{old:X}", "new": f"0x{new:X}", "events": count}
                for (old, new), count in offset_transitions[offset].most_common()
            ],
            **(
                {
                    "fields": fields_text(offset, top_old, top_new).split(", "),
                    "hints": [
                        {"text": text, "events": count}
                        for text, count in offset_hints[offset].most_common()
                    ],
                }
                if annotator is not None
                else {}
            ),
        }
    summary = Table(
        "Summary: offset -> events -> buttons",
        ["offset", "events", "buttons", "value transitions"]
        + (["fields", "hints"] if annotator is not None else [])
        + ["event ids"],
        summary_rows,
    )
    data = {
        "range": name,
        "annotated": annotator is not None,
        "meta": meta,
        "warnings": warnings,
        "frequency": [
            {
                "offset": _hex_offset(offset),
                "changed": stats.counts[offset],
                "events": stats.total,
                "idle_changed": stats.idle_counts[offset],
                "idle_events": stats.idle_total,
                "noise": (["frequency"] if groups_for(offset) else []) + reasons_for(offset),
                "noise_groups": groups_for(offset),
            }
            for offset in ranked
        ],
        "buttons": json_groups,
        "offsets": json_offsets,
        "events": json_events,
    }
    return Report(
        meta,
        warnings,
        frequency,
        group_tables,
        summary,
        event_lines,
        data,
        name,
        annotator is not None,
    )


# --------------------------------------------------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------------------------------------------------


def render_table_text(table: Table) -> list[str]:
    lines = [f"== {table.title} =="]
    if not table.rows:
        lines.append("(none)")
    else:
        widths = [len(header) for header in table.headers]
        for row in table.rows:
            widths = [max(width, len(cell)) for width, cell in zip(widths, row, strict=True)]
        lines.append(
            "  ".join(
                header.ljust(width) for header, width in zip(table.headers, widths, strict=True)
            ).rstrip()
        )
        for row in table.rows:
            lines.append(
                "  ".join(
                    cell.ljust(width) for cell, width in zip(row, widths, strict=True)
                ).rstrip()
            )
    if table.note:
        lines.append(table.note)
    return lines


def render_table_markdown(table: Table, level: int = 2) -> list[str]:
    lines = [f"{'#' * level} {table.title}", ""]
    if not table.rows:
        lines.append("(none)")
    else:
        lines.append("| " + " | ".join(table.headers) + " |")
        lines.append("|" + "|".join("---" for _ in table.headers) + "|")
        for row in table.rows:
            lines.append("| " + " | ".join(cell.replace("|", "\\|") for cell in row) + " |")
    if table.note:
        lines.extend(["", table.note])
    lines.append("")
    return lines


def meta_lines(meta: dict[str, Any]) -> list[str]:
    return [f"{key}: {'n/a' if value is None else value}" for key, value in meta.items()]


FORCED_TEXT = (
    "FABRICATED-STATE: the session ran with --forced-state (guarded pokes, e.g. the Map Maker editor "
    "weapon-set slots were poked before the preview). The pickup, switch, fire and reload transitions in "
    "these dumps are unmodified game code acting on a poked weapon set; field meanings stay INFERRED."
)
PASSIVE_TEXT = "passive observation, no pokes"
POKE_FILES = ("FORCED_STATE", "guestpoke.log")


def poke_files_in(directory: Path) -> list[str]:
    """Names of the poke marker files that exist in the dump directory."""
    return [name for name in POKE_FILES if (directory / name).exists()]


def evidence_lines(forced: bool | None, files: list[str]) -> list[str]:
    """The evidence class of the session: poked (FABRICATED-STATE), passive, or unknown for older manifests."""
    lines: list[str] = []
    if forced is True:
        lines.append(FORCED_TEXT)
    elif forced is False:
        lines.append(PASSIVE_TEXT)
    if files:
        lines.append(f"poke files present in the dump directory: {', '.join(files)}")
        if forced is None:
            lines.append(
                "the manifest does not say whether --forced-state was used: treat the session as possibly "
                "FABRICATED-STATE (guarded pokes)"
            )
        elif forced is False:
            lines.append(
                "contradiction: the manifest says no pokes but poke files exist, check the session"
            )
    return lines


def warning_lines(warnings: list[str]) -> list[str]:
    shown = warnings[:MAX_WARNINGS_SHOWN]
    lines = [f"warning: {text}" for text in shown]
    if len(warnings) > len(shown):
        lines.append(f"warning: ... {len(warnings) - len(shown)} more")
    return lines


@dataclass
class MultiReport:
    """One section per analysed range plus the settings and warnings they share."""

    meta: dict[str, Any]
    warnings: list[str]
    sections: list[Report]
    evidence: list[str] = field(default_factory=list)

    @property
    def annotated(self) -> bool:
        return any(section.annotated for section in self.sections)

    @property
    def data(self) -> dict[str, Any]:
        result: dict[str, Any] = {"meta": self.meta, "warnings": self.warnings}
        if self.evidence:
            result["evidence"] = self.evidence
        if self.annotated:
            result["legend"] = LEGEND
        result["ranges"] = [section.data for section in self.sections]
        return result


def render_text(report: MultiReport) -> str:
    lines = [
        "== Button dump report (xemu-level MEASURED observation; meaning of a field INFERRED) =="
    ]
    lines += report.evidence
    lines += meta_lines(report.meta)
    if report.annotated:
        lines.append(f"legend: {LEGEND}")
    lines += warning_lines(report.warnings)
    for section in report.sections:
        lines += ["", f"######## Range {section.name} ########"]
        lines += meta_lines(section.meta)
        lines += warning_lines(section.warnings)
        lines.append("")
        lines += render_table_text(section.frequency)
        for table in section.groups:
            lines.append("")
            lines += render_table_text(table)
        lines.append("")
        lines += render_table_text(section.summary)
        lines.append("")
        lines.append("== Events (marked ~ = noise offset, shown with --show-noise) ==")
        lines += section.event_lines or ["(none)"]
    return "\n".join(lines) + "\n"


def legend_rows() -> list[list[str]]:
    rows = []
    for block in BLOCKS.values():
        for rule in block.rules:
            size = rule.count * rule.size
            rows.append(
                [
                    block.name,
                    f"+0x{rule.start:X}",
                    f"0x{size:X}",
                    rule.label.replace("{i:02X}", "XX").replace("{i}", "N"),
                ]
            )
    return rows


def render_markdown(report: MultiReport) -> str:
    lines = [
        "# Button dump report",
        "",
        "Evidence: xemu-level MEASURED observation of the unmodified game; the meaning of a field is INFERRED.",
        "",
    ]
    for text in report.evidence:
        lines += [f"**{text}**" if text.startswith("FABRICATED-STATE") else text, ""]
    lines += [f"- {text}" for text in meta_lines(report.meta)]
    if report.warnings:
        lines += ["", "## Warnings", ""]
        lines += [f"- {text}" for text in report.warnings[:MAX_WARNINGS_SHOWN]]
        if len(report.warnings) > MAX_WARNINGS_SHOWN:
            lines.append(f"- ... {len(report.warnings) - MAX_WARNINGS_SHOWN} more")
    lines.append("")
    if report.annotated:
        lines += ["## Legend", "", LEGEND, ""]
        lines += render_table_markdown(
            Table(
                "Field names used (all INFERRED)",
                ["block", "offset", "bytes", "name"],
                legend_rows(),
            ),
            3,
        )
    for section in report.sections:
        lines += [f"## Range {section.name}", ""]
        lines += [f"- {text}" for text in meta_lines(section.meta)]
        lines += [f"- warning: {text}" for text in section.warnings[:MAX_WARNINGS_SHOWN]]
        lines.append("")
        lines += render_table_markdown(section.frequency, 3)
        lines += ["### Per button", ""]
        for table in section.groups:
            lines += render_table_markdown(table, 4)
        lines += render_table_markdown(section.summary, 3)
        lines += ["### Events", "", "```"]
        lines += section.event_lines or ["(none)"]
        lines += ["```", ""]
    return "\n".join(lines)


# --------------------------------------------------------------------------------------------------------------------
# Manifest validator (the spec as code)
# --------------------------------------------------------------------------------------------------------------------

START_KEYS = [
    "type",
    "version",
    "wall_start",
    "after_frames",
    "threshold",
    "coalesce_frames",
    "start_poll",
    "max_dumps",
    "max_bytes",
    "max_pending",
    "idle_every",
    "ranges",
    "bytes_per_dump",
    "button_names",
    "game_bits",
    "dump_dir",
]
START_OPTIONAL_KEYS = ("max_idle", "after_replay", "forced_state")
EVENT_KEYS = [
    "type",
    "index",
    "edge",
    "button",
    "buttons",
    "poll",
    "present",
    "mono_ms",
    "wall_time",
    "mask_before",
    "mask_after",
    "mask_names_before",
    "mask_names_after",
    "raw_before",
    "raw_after",
    "edges",
    "coalesced",
    "dumps",
    "complete",
]
DROPPED_KEYS = ["type", "poll", "reason", "edges", "mono_ms"]
END_KEYS = [
    "type",
    "events",
    "idle_events",
    "dumps",
    "bytes",
    "dropped",
    "coalesced_edges",
    "reason",
    "wall_time",
]
DUMP_KEYS = ["kind", "tag", "offset", "poll", "present", "label", "file", "bytes", "ok"]
EDGE_KEYS = ["poll", "edge", "button"]
DROP_REASONS = ("max_pending", "max_dumps", "max_bytes", "queue_full")
SUPPRESSED = "suppressed"
MAX_DROPPED_LINES = 200


def _is_int(value: object, low: int | None = None, high: int | None = None) -> bool:
    if not isinstance(value, int) or isinstance(value, bool):
        return False
    return (low is None or value >= low) and (high is None or value <= high)


def _keys_ok(obj: dict[str, Any], expected: list[str], where: str, problems: list[str]) -> bool:
    keys = list(obj)
    if keys == expected:
        return True
    missing = [key for key in expected if key not in obj]
    extra = [key for key in keys if key not in expected]
    if missing or extra:
        problems.append(f"{where}: missing keys {missing}, unexpected keys {extra}")
    else:
        problems.append(f"{where}: keys out of order {keys}")
    return False


def _mask_value(text: object) -> int | None:
    if isinstance(text, str) and MASK_RE.match(text):
        return int(text, 16)
    return None


def _expected_names(mask: int) -> list[str]:
    return [name for bit, name in enumerate(BUTTON_NAMES) if (mask >> bit) & 1]


def _check_edges(
    obj: dict[str, Any], where: str, coalesce: int | None, problems: list[str]
) -> None:
    edges = obj["edges"]
    if not isinstance(edges, list):
        problems.append(f"{where}: edges is not a list")
        return
    clean = True
    for position, edge in enumerate(edges):
        label = f"{where} edges[{position}]"
        if not isinstance(edge, dict) or not _keys_ok(edge, EDGE_KEYS, label, problems):
            clean = False
            continue
        if not _is_int(edge["poll"], 0):
            problems.append(f"{label}: poll must be a non-negative integer")
            clean = False
        if edge["edge"] not in ("press", "release"):
            problems.append(f"{label}: edge must be press or release")
            clean = False
        if edge["button"] not in BUTTON_NAMES:
            problems.append(f"{label}: button {edge['button']!r} is not one of the 16 names")
            clean = False
    if obj["edge"] == "idle":
        if edges:
            problems.append(f"{where}: idle event must have no edges")
        if obj["button"] != "none" or obj["buttons"] != []:
            problems.append(f"{where}: idle event must have button 'none' and buttons []")
        if obj["coalesced"] != 0:
            problems.append(f"{where}: idle event coalesced must be 0")
        return
    if not edges:
        problems.append(f"{where}: non-idle event needs at least one edge")
        return
    if not clean:
        return
    kinds = {edge["edge"] for edge in edges}
    expected_edge = kinds.pop() if len(kinds) == 1 else "mixed"
    if obj["edge"] != expected_edge:
        problems.append(f"{where}: edge {obj['edge']!r} but the edges say {expected_edge!r}")
    if obj["button"] != edges[0]["button"]:
        problems.append(f"{where}: button must be the first edge's button {edges[0]['button']!r}")
    names = list(dict.fromkeys(edge["button"] for edge in edges))
    if obj["buttons"] != names:
        problems.append(
            f"{where}: buttons {obj['buttons']!r} must be the unique edge buttons in order {names!r}"
        )
    if obj["poll"] != edges[0]["poll"]:
        problems.append(f"{where}: poll must equal the first edge's poll")
    polls = [edge["poll"] for edge in edges]
    if polls != sorted(polls):
        problems.append(f"{where}: edge polls must not decrease")
    if coalesce is not None and polls[-1] - polls[0] > coalesce:
        problems.append(
            f"{where}: an edge is more than coalesce_frames={coalesce} polls after the first"
        )
    later = sum(1 for poll in polls if poll != polls[0])
    # the host lists at most MAX_LISTED_EDGES edges per event and counts the rest in coalesced only
    capped = len(edges) >= MAX_LISTED_EDGES
    if obj["coalesced"] < later or (obj["coalesced"] != later and not capped):
        problems.append(
            f"{where}: coalesced is {obj['coalesced']!r} but {later} edge(s) are in later polls than the first"
        )


def _check_raw(raw: object, where: str, problems: list[str]) -> None:
    if not isinstance(raw, dict) or not _keys_ok(raw, ["digital", "analog"], where, problems):
        if not isinstance(raw, dict):
            problems.append(f"{where}: not an object")
        return
    if _mask_value(raw["digital"]) is None:
        problems.append(f"{where}: digital must be a 0xHHHH string")
    analog = raw["analog"]
    if not (
        isinstance(analog, list)
        and len(analog) == 8
        and all(_is_int(item, 0, 255) for item in analog)
    ):
        problems.append(f"{where}: analog must be 8 integers 0..255")


def _check_dumps(
    obj: dict[str, Any], where: str, after_frames: list[int] | None, problems: list[str]
) -> int:
    """Check the dumps list, return the total of the bytes fields."""
    dumps = obj["dumps"]
    if not isinstance(dumps, list):
        problems.append(f"{where}: dumps is not a list")
        return 0
    idle = obj["edge"] == "idle"
    before_kind, after_kind = ("idle-before", "idle-after") if idle else ("before", "after")
    total_bytes = 0
    offsets: list[int] = []
    previous_present: int | None = None
    for position, dump in enumerate(dumps):
        label = f"{where} dumps[{position}]"
        if not isinstance(dump, dict) or not _keys_ok(dump, DUMP_KEYS, label, problems):
            continue
        if _is_int(dump["bytes"], 0):
            total_bytes += dump["bytes"]
        else:
            problems.append(f"{label}: bytes must be a non-negative integer")
        if not isinstance(dump["ok"], bool):
            problems.append(f"{label}: ok must be a boolean")
        for key in ("poll", "present"):
            if not _is_int(dump[key], 0):
                problems.append(f"{label}: {key} must be a non-negative integer")
        offset = dump["offset"]
        is_before = dump["kind"] == before_kind
        if dump["kind"] not in (before_kind, after_kind):
            problems.append(f"{label}: kind {dump['kind']!r} must be {before_kind} or {after_kind}")
        if not _is_int(offset, 0):
            problems.append(f"{label}: offset must be a non-negative integer")
            continue
        offsets.append(offset)
        expected_tag = "before" if is_before else f"after{offset}"
        if dump["tag"] != expected_tag:
            problems.append(f"{label}: tag {dump['tag']!r} must be {expected_tag!r}")
        if is_before and offset != 0:
            problems.append(f"{label}: the before dump has offset 0")
        if not is_before and offset == 0:
            problems.append(f"{label}: an after dump needs a positive offset")
        if after_frames is not None and not is_before and offset not in after_frames:
            problems.append(
                f"{label}: offset {offset} is not in the start line after_frames {after_frames}"
            )
        names = obj["buttons"]
        name = "none" if idle else "-".join(str(item) for item in names)
        expected_label = f"{obj['index']:04d}_{obj['edge']}_{name}_{dump['tag']}"
        if not isinstance(dump["label"], str) or not LABEL_RE.match(dump["label"]):
            problems.append(f"{label}: label {dump['label']!r} does not match the label format")
        elif dump["label"] != expected_label:
            problems.append(f"{label}: label {dump['label']!r} must be {expected_label!r}")
        if dump["file"] != f"{DUMP_SUBDIR}/guestdump.{dump['label']}":
            problems.append(f"{label}: file must be {DUMP_SUBDIR}/guestdump.<label>")
        if _is_int(dump["poll"]) and _is_int(obj["poll"]):
            if is_before and dump["poll"] != obj["poll"]:
                problems.append(f"{label}: before poll must equal the event poll")
            if not is_before and dump["poll"] < obj["poll"] + offset:
                problems.append(f"{label}: after poll must be at least event poll + offset")
        if (
            _is_int(dump["present"])
            and previous_present is not None
            and dump["present"] < previous_present
        ):
            problems.append(f"{label}: present must not decrease between dumps")
        if _is_int(dump["present"]):
            previous_present = dump["present"]
        if is_before and _is_int(dump["present"]) and dump["present"] != obj["present"]:
            problems.append(f"{label}: before present must equal the event present")
    if offsets != sorted(set(offsets)):
        problems.append(f"{where}: dump offsets {offsets} must be strictly increasing")
    if obj["complete"] is True and after_frames is not None and offsets != [0, *after_frames]:
        problems.append(
            f"{where}: a complete event has dumps at offsets {[0, *after_frames]}, found {offsets}"
        )
    return total_bytes


def _check_event(
    obj: dict[str, Any], start: dict[str, Any] | None, where: str, problems: list[str]
) -> int:
    """Return the sum of the dump bytes of the event (for the end line check)."""
    if not _keys_ok(obj, EVENT_KEYS, where, problems):
        return 0
    if not _is_int(obj["index"], 1):
        problems.append(f"{where}: index must be an integer >= 1")
        return 0
    if obj["edge"] not in ("press", "release", "mixed", "idle"):
        problems.append(f"{where}: edge must be press, release, mixed or idle")
        return 0
    if not isinstance(obj["button"], str) or not isinstance(obj["buttons"], list):
        problems.append(f"{where}: button must be a string and buttons a list")
        return 0
    for key in ("poll", "present", "mono_ms"):
        if not _is_int(obj[key], 0):
            problems.append(f"{where}: {key} must be a non-negative integer")
            return 0
    if not isinstance(obj["wall_time"], str) or not WALL_MS_RE.match(obj["wall_time"]):
        problems.append(f"{where}: wall_time must be UTC ISO8601 with milliseconds")
    for side in ("before", "after"):
        mask = _mask_value(obj[f"mask_{side}"])
        names = obj[f"mask_names_{side}"]
        if mask is None:
            problems.append(f"{where}: mask_{side} must be a 0xHHHH string")
        elif names != _expected_names(mask):
            problems.append(
                f"{where}: mask_names_{side} {names!r} do not match mask_{side} {obj[f'mask_{side}']}"
            )
        _check_raw(obj[f"raw_{side}"], f"{where} raw_{side}", problems)
    if not _is_int(obj["coalesced"], 0):
        problems.append(f"{where}: coalesced must be a non-negative integer")
        return 0
    if not isinstance(obj["complete"], bool):
        problems.append(f"{where}: complete must be a boolean")
        return 0
    coalesce = (
        start["coalesce_frames"]
        if start is not None and _is_int(start.get("coalesce_frames"))
        else None
    )
    _check_edges(obj, where, coalesce, problems)
    frames = start.get("after_frames") if start is not None else None
    after_frames = (
        frames if isinstance(frames, list) and all(_is_int(item) for item in frames) else None
    )
    return _check_dumps(obj, where, after_frames, problems)


def _check_start(obj: dict[str, Any], problems: list[str]) -> None:
    where = "start line"
    # the host appends max_idle, after_replay and forced_state (T1629 H, T1637); optional for older or hand made manifests
    core = {key: value for key, value in obj.items() if key not in START_OPTIONAL_KEYS}
    if not _keys_ok(core, START_KEYS, where, problems):
        return
    if "max_idle" in obj and not _is_int(obj["max_idle"], 0):
        problems.append(f"{where}: max_idle must be a non-negative integer")
    if "after_replay" in obj and not isinstance(obj["after_replay"], bool):
        problems.append(f"{where}: after_replay must be a boolean")
    if "forced_state" in obj and not isinstance(obj["forced_state"], bool):
        problems.append(f"{where}: forced_state must be a boolean")
    trailing = list(obj)[len(core) :]
    if trailing != [key for key in START_OPTIONAL_KEYS if key in obj]:
        problems.append(
            f"{where}: optional keys must be the last keys, in the order {', '.join(START_OPTIONAL_KEYS)}"
        )
    if obj["version"] != 1 or not _is_int(obj["version"]):
        problems.append(f"{where}: version must be 1")
    if not isinstance(obj["wall_start"], str) or not WALL_RE.match(obj["wall_start"]):
        problems.append(f"{where}: wall_start must be UTC ISO8601")
    frames = obj["after_frames"]
    if not (
        isinstance(frames, list)
        and 1 <= len(frames) <= 4
        and all(_is_int(item, 1, 3600) for item in frames)
        and all(left < right for left, right in zip(frames, frames[1:], strict=False))
    ):
        problems.append(f"{where}: after_frames must be 1..4 strictly increasing integers 1..3600")
    bounds = {
        "threshold": (1, 254),
        "coalesce_frames": (0, 60),
        "start_poll": (0, None),
        "max_dumps": (1, 100000),
        "max_bytes": (1, None),
        "max_pending": (1, 64),
        "idle_every": (0, None),
        "ranges": (1, None),
        "bytes_per_dump": (0, None),
    }
    for key, (low, high) in bounds.items():
        if not _is_int(obj[key], low, high):
            problems.append(f"{where}: {key} must be an integer in [{low}, {high}]")
    if obj["button_names"] != list(BUTTON_NAMES):
        problems.append(f"{where}: button_names must be the 16 names in bit order")
    if obj["game_bits"] != GAME_BITS or list(obj["game_bits"]) != list(BUTTON_NAMES):
        problems.append(
            f"{where}: game_bits must map the 16 names, in bit order, to the title's bits"
        )
    if not isinstance(obj["dump_dir"], str) or not obj["dump_dir"]:
        problems.append(f"{where}: dump_dir must be a non-empty string")


def _check_dropped(obj: dict[str, Any], where: str, problems: list[str]) -> None:
    if not _keys_ok(obj, DROPPED_KEYS, where, problems):
        return
    if not _is_int(obj["poll"], 0) or not _is_int(obj["mono_ms"], 0):
        problems.append(f"{where}: poll and mono_ms must be non-negative integers")
    if obj["reason"] not in (*DROP_REASONS, SUPPRESSED):
        problems.append(f"{where}: reason must be one of {[*DROP_REASONS, SUPPRESSED]}")
    edges = obj["edges"]
    if obj["reason"] == SUPPRESSED:
        if edges != []:
            problems.append(f"{where}: a suppressed marker has empty edges")
        return
    if not isinstance(edges, list) or not edges:
        problems.append(f"{where}: edges must be a non-empty list")
        return
    for position, edge in enumerate(edges):
        label = f"{where} edges[{position}]"
        if not isinstance(edge, dict) or not _keys_ok(edge, EDGE_KEYS, label, problems):
            continue
        if (
            not _is_int(edge["poll"], 0)
            or edge["edge"] not in ("press", "release")
            or edge["button"] not in BUTTON_NAMES
        ):
            problems.append(f"{label}: bad poll, edge or button")


def _check_end(
    obj: dict[str, Any], totals: dict[str, int], indexes: set[int], where: str, problems: list[str]
) -> None:
    if not _keys_ok(obj, END_KEYS, where, problems):
        return
    if obj["reason"] not in ("exit", "signal"):
        problems.append(f"{where}: reason must be exit or signal")
    if not isinstance(obj["wall_time"], str) or not WALL_RE.match(obj["wall_time"]):
        problems.append(f"{where}: wall_time must be UTC ISO8601")
    for key, name in (
        ("events", "events"),
        ("idle_events", "idle_events"),
        ("dumps", "dumps"),
        ("bytes", "bytes"),
        ("dropped", "dropped"),
        ("coalesced_edges", "coalesced_edges"),
    ):
        if not _is_int(obj[key], 0):
            problems.append(f"{where}: {key} must be a non-negative integer")
        elif key == "dropped":
            # the end line holds the TOTAL of dropped edge groups: more than the listed lines only after a suppressed marker
            if obj[key] < totals[name] or (obj[key] != totals[name] and not totals["suppressed"]):
                problems.append(
                    f"{where}: {key} is {obj[key]} but the manifest has {totals[name]} dropped line(s)"
                    + ("" if totals["suppressed"] else " and no suppressed marker")
                )
        elif obj[key] != totals[name]:
            problems.append(f"{where}: {key} is {obj[key]} but the manifest has {totals[name]}")
    if indexes != set(range(1, len(indexes) + 1)):
        problems.append(f"{where}: event indices must be 1..{len(indexes)} without gaps")


def validate_manifest(lines: Iterable[str]) -> list[str]:
    """Check buttons.jsonl text lines against the T1629 spec. Returns the list of problems (empty = valid).

    A missing end line is NOT a problem (a hard kill leaves none). Everything else the spec states is checked: the
    start line first with its exact keys in order, event key order and types, label and file format, mask strings
    against mask_names, edge consistency, dump offsets against the start after_frames, unique indices and, when an
    end line exists, its counters.
    """
    problems: list[str] = []
    start: dict[str, Any] | None = None
    totals = {
        "events": 0,
        "idle_events": 0,
        "dumps": 0,
        "bytes": 0,
        "dropped": 0,
        "suppressed": 0,
        "coalesced_edges": 0,
    }
    indexes: set[int] = set()
    end_object: dict[str, Any] | None = None
    end_line = 0
    count = 0
    for number, text in enumerate(lines, 1):
        count = number
        where = f"line {number}"
        text = text.rstrip("\r\n")
        if not text.strip():
            problems.append(f"{where}: blank line")
            continue
        try:
            obj = json.loads(text)
        except json.JSONDecodeError as error:
            problems.append(f"{where}: invalid JSON ({error.msg})")
            continue
        if not isinstance(obj, dict):
            problems.append(f"{where}: not a JSON object")
            continue
        if end_object is not None:
            problems.append(f"{where}: line after the end line")
        kind = obj.get("type")
        if number == 1 and kind != "start":
            problems.append(f"{where}: the first line must be the start line")
        if kind == "start":
            if number != 1:
                problems.append(f"{where}: a second start line")
            else:
                start = obj
                _check_start(obj, problems)
        elif kind == "event":
            total_bytes = _check_event(obj, start, where, problems)
            index = obj.get("index")
            if _is_int(index, 1):
                if index in indexes:
                    problems.append(f"{where}: duplicate event index {index}")
                indexes.add(index)
            if obj.get("edge") == "idle":
                totals["idle_events"] += 1
            else:
                totals["events"] += 1
            dumps = obj.get("dumps")
            totals["dumps"] += len(dumps) if isinstance(dumps, list) else 0
            totals["bytes"] += total_bytes
            if _is_int(obj.get("coalesced"), 0):
                totals["coalesced_edges"] += obj["coalesced"]
        elif kind == "dropped":
            _check_dropped(obj, where, problems)
            if obj.get("reason") == SUPPRESSED:
                if totals["suppressed"]:
                    problems.append(f"{where}: a second suppressed marker")
                totals["suppressed"] += 1
            else:
                if totals["suppressed"]:
                    problems.append(f"{where}: a dropped line after the suppressed marker")
                totals["dropped"] += 1
                if totals["dropped"] == MAX_DROPPED_LINES + 1:
                    problems.append(f"{where}: more than {MAX_DROPPED_LINES} dropped lines")
        elif kind == "end":
            end_object, end_line = obj, number
        else:
            problems.append(f"{where}: unknown line type {kind!r}")
    if count == 0:
        problems.append("manifest is empty")
    if end_object is not None:
        _check_end(end_object, totals, indexes, f"line {end_line} end", problems)
    return problems


def check_dump_files(directory: Path, manifest: Manifest) -> list[str]:
    """Problems about dump files the manifest lists but the disk lacks (used by --validate)."""
    problems = []
    for event in manifest.events:
        for entry in event.dumps:
            path = _dump_path(directory, entry)
            if path is None or not path.is_file():
                problems.append(f"event {event.index}: dump file {entry.get('file')!r} is missing")
    return problems


# --------------------------------------------------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------------------------------------------------


def _int_arg(low: int) -> Callable[[str], int]:
    def parse(text: str) -> int:
        try:
            value = int(text, 0)
        except ValueError:
            raise argparse.ArgumentTypeError(f"not an integer: {text!r}") from None
        if value < low:
            raise argparse.ArgumentTypeError(f"{value} is below {low}")
        return value

    return parse


def _range_arg(text: str) -> RangeSpec:
    try:
        return parse_range_spec(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError(str(error)) from None


def _window_arg(text: str) -> Window:
    start_text, colon, length_text = text.partition(":")
    try:
        start = int(start_text, 0)
        length = int(length_text, 0) if colon else None
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"bad window {text!r}: use OFF:LEN, for example 0x580:0x40"
        ) from None
    if start < 0 or (length is not None and length <= 0):
        raise argparse.ArgumentTypeError(f"bad window {text!r}: OFF >= 0 and LEN > 0")
    return Window(start, length)


def _views_arg(text: str) -> tuple[str, ...]:
    views = tuple(dict.fromkeys(item.strip() for item in text.split(",") if item.strip()))
    if not views or any(view not in VIEWS for view in views):
        raise argparse.ArgumentTypeError(f"--views takes a comma list of {','.join(VIEWS)}")
    return views


def _after_arg(text: str) -> str:
    if text in ("first", "all"):
        return text
    try:
        value = int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(
            "--after takes first, all or an after offset in frames such as 2"
        ) from None
    if value < 1:
        raise argparse.ArgumentTypeError("--after offset must be >= 1")
    return str(value)


def _fraction_arg(text: str) -> Fraction:
    try:
        value = Fraction(text)
    except (ValueError, ZeroDivisionError):
        raise argparse.ArgumentTypeError(f"not a number: {text!r}") from None
    if value < 0:
        raise argparse.ArgumentTypeError("must be >= 0")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.button_dump_report",
        description="T1629: diff the BEFORE and AFTER guest dumps of every button press/release (host flag "
        "--dump-on-button) over one range and report the changed fields per button, minus the noise.",
    )
    parser.add_argument(
        "directory", metavar="DIR", help="the --dump-guest-dir with buttons.jsonl and buttons/"
    )
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument(
        "--range",
        type=_range_arg,
        action="append",
        default=None,
        metavar="SPEC",
        help="range to diff: *0xADDR+OFF (indirect) or 0xADDR (direct), repeatable, one report section "
        f"each. Default: both {EXT_SPEC} (player extension block, weapon inventory) and {DEFAULT_RANGE} "
        "(player record), each only when the dumps contain it",
    )
    selection.add_argument(
        "--range-index",
        type=_int_arg(0),
        action="append",
        default=None,
        metavar="N",
        help="range by position in the dump, repeatable",
    )
    parser.add_argument(
        "--window",
        type=_window_arg,
        default=Window(),
        metavar="OFF:LEN",
        help="byte window inside the range",
    )
    parser.add_argument(
        "--list-ranges",
        action="store_true",
        help="list the ranges of the first readable dump and exit",
    )
    parser.add_argument(
        "--views",
        type=_views_arg,
        default=("dword", "byte", "float"),
        help="comma list of dword,byte,float,word",
    )
    parser.add_argument(
        "--after",
        type=_after_arg,
        default="first",
        help="first (default), all, or an offset K in frames",
    )
    parser.add_argument(
        "--noise-fraction",
        type=_fraction_arg,
        default=Fraction(3, 10),
        metavar="F",
        help="a dword is noise for a button group when it changed in at least this fraction of the events "
        "outside the group (default 0.30)",
    )
    parser.add_argument(
        "--noise-min-events",
        type=_int_arg(1),
        default=5,
        metavar="N",
        help="events outside the group needed before the frequency rule can fire for it (default 5)",
    )
    parser.add_argument(
        "--idle-noise-min",
        type=_int_arg(1),
        default=1,
        metavar="N",
        help="idle events that must change it, default 1",
    )
    parser.add_argument(
        "--noise-offsets",
        default="",
        metavar="LIST|@FILE",
        help="always noise: 0x58C,0x10-0x40 or @file",
    )
    parser.add_argument("--no-noise-filter", action="store_true", help="classify nothing as noise")
    parser.add_argument(
        "--show-noise", action="store_true", help="print noise offsets too (marked ~)"
    )
    parser.add_argument(
        "--top", type=_int_arg(1), default=40, metavar="N", help="frequency table rows, default 40"
    )
    parser.add_argument(
        "--max-events",
        type=_int_arg(1),
        default=None,
        metavar="N",
        help="analyse only the first N events by index",
    )
    parser.add_argument(
        "--max-event-lines",
        type=_int_arg(0),
        default=300,
        metavar="N",
        help="event lines printed, 0 = all",
    )
    parser.add_argument("--max-changes-per-line", type=_int_arg(1), default=24, metavar="N")
    parser.add_argument("--max-group-rows", type=_int_arg(1), default=60, metavar="N")
    parser.add_argument(
        "--markdown", default=None, metavar="FILE", help="write the Markdown report"
    )
    parser.add_argument(
        "--write", action="store_true", help=f"write DIR/{REPORT_NAME} (unless --markdown is given)"
    )
    parser.add_argument(
        "--json", default=None, metavar="FILE", help="write the machine readable summary"
    )
    parser.add_argument(
        "--force", action="store_true", help="allow overwriting an existing file outside DIR"
    )
    parser.add_argument(
        "--timeline",
        action="store_true",
        help="after the report, run the cross-event timeline (tools.button_dump_timeline: ownership flips, ammo trace, "
        "weapon state between events) and, with --write, also write DIR/button_timeline.md",
    )
    parser.add_argument(
        "--no-annotate",
        action="store_true",
        help="do not name offsets of the ext and record blocks",
    )
    parser.add_argument(
        "--entries-csv",
        default=None,
        metavar="FILE",
        help="weapon entry table (default docs/data/t-player-inventory-entries.csv when present)",
    )
    parser.add_argument(
        "--entry-names",
        default=None,
        metavar="FILE",
        help='JSON {"0x36": "name"} naming weapon entries in the labels',
    )
    parser.add_argument(
        "--validate",
        action="store_true",
        help="check buttons.jsonl against the T1629 spec and exit",
    )
    return parser


def _fail(message: str, code: int = EXIT_USAGE) -> int:
    print(f"error: {message}", file=sys.stderr)
    return code


def _inside(directory: Path, path: Path) -> bool:
    try:
        path.resolve().relative_to(directory.resolve())
    except ValueError:
        return False
    return True


def _write_output(directory: Path, target: Path, text: str, force: bool) -> str | None:
    """Write text; return an error message instead of overwriting a foreign file."""
    if not _inside(directory, target) and target.exists() and not force:
        return f"{target} exists outside {directory}: refusing to overwrite (use --force)"
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")
    except OSError as error:
        return f"cannot write {target}: {error.strerror or error}"
    return None


def _list_ranges(directory: Path, manifest: Manifest) -> int:
    for event in manifest.events:
        for entry in event.dumps:
            path = _dump_path(directory, entry)
            if path is None or not path.is_file():
                continue
            parsed = parse_dump(path, None)
            print(f"ranges of {path.relative_to(directory)}")
            for info in parsed.ranges:
                pointer = f" pointer 0x{info.pointer:08X}" if info.pointer is not None else ""
                final = f" final 0x{info.address:08X}" if info.address is not None else ""
                state = "readable" if info.readable and info.address is not None else "UNREADABLE"
                print(
                    f"  [{info.index}] {info.spec_text()}{pointer}{final} length 0x{info.length:X} {state}"
                )
            return EXIT_OK
    return _fail("no readable dump file found to list ranges from", EXIT_UNREADABLE)


def run(args: argparse.Namespace) -> int:
    directory = Path(args.directory)
    manifest_path = directory / MANIFEST_NAME
    if not directory.is_dir():
        return _fail(f"{directory} is not a directory")
    if not manifest_path.is_file():
        return _fail(f"{manifest_path} not found (was the run started with --dump-on-button?)")
    try:
        if args.validate:
            text_lines = manifest_path.read_text(encoding="utf-8", errors="replace").splitlines()
            problems = validate_manifest(text_lines) + check_dump_files(
                directory, load_manifest(manifest_path)
            )
            if problems:
                for problem in problems:
                    print(f"problem: {problem}")
                print(f"{len(problems)} manifest problem(s) in {manifest_path}")
                return EXIT_INVALID
            print(f"manifest OK: {len(text_lines)} line(s) in {manifest_path}")
            return EXIT_OK
        manifest = load_manifest(manifest_path)
        if args.list_ranges:
            return _list_ranges(directory, manifest)
        code = analyze(args, directory, manifest)
        if code == EXIT_OK and args.timeline:
            # lazy: the timeline module imports this one
            from tools.button_dump_timeline import run_timeline

            return run_timeline(
                directory,
                manifest,
                entries_csv=args.entries_csv,
                entry_names=args.entry_names,
                write=args.write,
                force=args.force,
            )
        return code
    except OSError as error:
        return _fail(f"{error.filename or directory}: {error.strerror or error}")


def _selectors(args: argparse.Namespace) -> tuple[list[RangeSelector], bool]:
    """(selectors, auto): the default is both the ext block and the record, skipped when absent."""
    if args.range_index:
        return [RangeSelector(index=item) for item in args.range_index], False
    if args.range:
        return [RangeSelector(spec=item) for item in args.range], False
    return [RangeSelector(spec=parse_range_spec(item)) for item in DEFAULT_RANGES], True


def _annotation_sources(
    args: argparse.Namespace, notes: list[str]
) -> tuple[dict[int, EntryInfo], dict[int, str]]:
    """Entry table and optional entry names; an explicit file that cannot be read is an error."""
    entries: dict[int, EntryInfo] = {}
    names: dict[int, str] = {}
    if args.entries_csv:
        entries = load_entries_csv(Path(args.entries_csv))
    elif ENTRIES_CSV.is_file():
        try:
            entries = load_entries_csv(ENTRIES_CSV)
        except ValueError as error:
            notes.append(f"{error}; entry roles are not shown")
    if args.entry_names:
        names = load_entry_names(Path(args.entry_names))
    return entries, names


def analyze(args: argparse.Namespace, directory: Path, manifest: Manifest) -> int:
    try:
        listed = load_noise_offsets(args.noise_offsets) if args.noise_offsets else []
        notes: list[str] = []
        entries, names = _annotation_sources(args, notes)
    except ValueError as error:
        return _fail(str(error))
    selectors, auto = _selectors(args)
    window: Window = args.window
    only_offset = None if args.after in ("first", "all") else int(args.after)
    events = manifest.events
    truncated = args.max_events is not None and len(events) > args.max_events
    if args.max_events is not None:
        events = events[: args.max_events]
    warnings = list(manifest.warnings) + notes
    counters = [DumpCounters() for _ in selectors]
    section_warnings: list[list[str]] = [[] for _ in selectors]
    results: list[list[EventResult]] = [[] for _ in selectors]
    for event in events:
        per_range = analyze_event_multi(
            event, directory, selectors, window, only_offset, counters, section_warnings
        )
        for index, result in enumerate(per_range):
            results[index].append(result)
    attempted = max(item.attempted for item in counters)
    if attempted and not any(item.usable for item in counters):
        for text in warning_lines(warnings + [w for part in section_warnings for w in part]):
            print(text, file=sys.stderr)
        described = ", ".join(selector.describe() for selector in selectors)
        return _fail(
            f"every dump ({attempted}) was unreadable for range {described}", EXIT_UNREADABLE
        )
    ok_false = max(item.ok_false for item in counters)
    if ok_false:
        warnings.append(
            f"{ok_false} dump(s) are marked ok:false in the manifest (partly unreadable ranges)"
        )
    if manifest.unknown_lines:
        warnings.append(f"{manifest.unknown_lines} manifest line(s) of an unknown type ignored")
    if any(not event.complete for event in events):
        warnings.append(
            f"{sum(1 for event in events if not event.complete)} event(s) are complete:false (flushed at shutdown), "
            "analysed with the dumps that exist"
        )
    if truncated:
        warnings.append(f"--max-events {args.max_events}: later events not analysed")
    listed_drops = [item for item in manifest.dropped if item.get("reason") != SUPPRESSED]
    suppressed = len(listed_drops) != len(manifest.dropped)
    total_drops = manifest.end.get("dropped") if manifest.end is not None else None
    if listed_drops or suppressed:
        reasons = Counter(str(item.get("reason", "?")) for item in listed_drops)
        text = f"{len(listed_drops)} edge group(s) were dropped by the host: {dict(sorted(reasons.items()))}"
        if suppressed:
            text += "; further drops were suppressed"
            if isinstance(total_drops, int) and not isinstance(total_drops, bool):
                text += f" (end line total {total_drops})"
        warnings.append(text)
    config = NoiseConfig(
        args.noise_fraction,
        args.noise_min_events,
        args.idle_noise_min,
        listed,
        not args.no_noise_filter,
    )
    options = ReportOptions(
        views=args.views,
        show_noise=args.show_noise,
        top=args.top,
        max_event_lines=args.max_event_lines,
        max_changes_per_line=args.max_changes_per_line,
        max_group_rows=args.max_group_rows,
    )
    sections: list[Report] = []
    for index, selector in enumerate(selectors):
        if auto and not counters[index].usable:
            warnings.append(
                f"default range {selector.describe()} has no usable data in any dump: section omitted"
            )
            continue
        sections.append(
            _section(
                args,
                selector,
                results[index],
                counters[index],
                section_warnings[index],
                config,
                options,
                entries,
                names,
            )
        )
    meta = {
        "directory": str(directory),
        "window": f"0x{window.start:X}:{'whole range' if window.length is None else f'0x{window.length:X}'}",
        "views": ",".join(options.views),
        "after": args.after,
        "events in manifest": len(manifest.events),
        "noise filter": "off" if args.no_noise_filter else "on",
    }
    start = manifest.start or {}
    forced = start.get("forced_state") if isinstance(start.get("forced_state"), bool) else None
    files = poke_files_in(directory)
    meta["forced_state"] = forced
    meta["poke_files_present"] = bool(files)
    report = MultiReport(meta, warnings, sections, evidence_lines(forced, files))
    print(render_text(report), end="")
    markdown_target = (
        Path(args.markdown) if args.markdown else (directory / REPORT_NAME if args.write else None)
    )
    for target, text in (
        (markdown_target, None),
        (Path(args.json) if args.json else None, json.dumps(report.data, indent=1) + "\n"),
    ):
        if target is None:
            continue
        content = render_markdown(report) if text is None else text
        error = _write_output(directory, target, content, args.force)
        if error:
            return _fail(error)
        print(f"wrote {target}")
    return EXIT_OK


def _section(
    args: argparse.Namespace,
    selector: RangeSelector,
    results: list[EventResult],
    counters: DumpCounters,
    warnings: list[str],
    config: NoiseConfig,
    options: ReportOptions,
    entries: dict[int, EntryInfo],
    names: dict[int, str],
) -> Report:
    window: Window = args.window
    stats = compute_frequencies(results, "all" if args.after == "all" else "first")
    identity = next((item.identity for item in results if item.identity is not None), None)
    block = None if args.no_annotate else block_for_spec(identity or selector.spec)
    annotator = (
        Annotator(block, identity.offset if identity is not None else 0, entries, names)
        if block is not None
        else None
    )
    name = identity.__str__() if identity is not None else selector.describe()
    if block is not None:
        name += f" [{block.name}]"
    meta = {
        "range": name,
        "events analysed": sum(1 for result in results if result.usable_afters()),
        "idle control events": stats.idle_total,
        "events used for noise statistics": stats.total,
        "dumps usable/attempted": f"{counters.usable}/{counters.attempted} (missing {counters.missing}, unreadable {counters.unreadable})",
        "dumps with partial data": counters.partial,
        "field names": "yes (INFERRED)" if annotator is not None else "no",
    }
    report = build_report(results, stats, config, options, window, meta, warnings, annotator, name)
    if options.max_event_lines and len(report.event_lines) > options.max_event_lines:
        omitted = len(report.event_lines) - options.max_event_lines
        report.event_lines = report.event_lines[: options.max_event_lines] + [
            f"... {omitted} more event lines (--max-event-lines)"
        ]
    return report


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
