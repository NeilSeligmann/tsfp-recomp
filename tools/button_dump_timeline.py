# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1642: cross-event TIMELINE of a button dump session (the state changes BETWEEN button events).

`tools.button_dump_report` is event centric: it diffs the BEFORE dump of one button event against its +2/+30 AFTER dumps.
A change that happens between two events (walking over a pickup, with no button edge) never shows up there. This tool
orders EVERY dump of the manifest by (poll, event index, tag), drops duplicate labels and compares each dump with the
previous one, over the player extension block `*0x7356D8+0` (slot 0, 0xEA0 bytes) and the player record `*0x7B0C48+0`
(0x1584 bytes). It prints three sections plus a header:

  a. Ownership flips: every owned byte (ext+0x54C+entry) that flips between two consecutive dumps, the interval, whether a
     manifest button edge lies inside it (`by button` or `no button edge in the interval`), and the other signals that
     changed in the same interval (current weapon, reserve ammo, ext+0x28 flags, cap words, "equipped once" bytes,
     group order bytes). An interval longer than --gap-polls is flagged GAP (it cannot be attributed to a moment).
  b. Ammo trace per ammo type that is ever nonzero: one row whenever (reserve, loaded) changes, classified from the deltas
     (shot, multi, reload, equip-fill, holster-return, pickup, other), the running total reserve+loaded, a conservation
     violation flag and the peak total (a ceiling hint).
  c. Weapon state timeline: one row whenever (ext+0x94, ext+0x98, ext+0x204, ext+0xD4, rec+0x58C, rec+0x5A0) changes,
     with the button edge that lies within --trigger-polls polls before the dump, if any.

Interval rule: a dump taken at poll q shows the state BEFORE the edge of poll q is processed (that is what a BEFORE dump is),
so the edges that can explain a change between the dumps at polls p and q are those with p <= poll < q.

A different range pointer (a level reload) prints a RELOAD marker and restarts the baseline of that range. A dump that lacks a
range is skipped for that range (the next comparison then spans it). Evidence class: xemu-level MEASURED observation of the
unmodified game, the meaning of the fields is INFERRED (docs/t-player-inventory.md).

Exit codes (as the report): 0 ok, 2 usage error or missing manifest, 3 every dump unreadable.

Usage: python -m tools.button_dump_timeline DIR [--write] [--json] [--gap-polls N] [--trigger-polls N]
       python -m tools.button_dump_report DIR --timeline [--write]
"""

from __future__ import annotations

import argparse
import bisect
import csv
import itertools
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from tools import button_dump_report as report
from tools.button_dump_report import (
    AMMO_TYPES,
    EXIT_OK,
    EXIT_UNREADABLE,
    OWNED_ENTRIES,
    EntryInfo,
    Manifest,
    RangeInfo,
    RangeSelector,
    RangeSpec,
    _dump_path,
    _tag_offset,
    evidence_lines,
    load_entries_csv,
    load_entry_names,
    load_manifest,
    parse_dump,
    poke_files_in,
)
from tools.t1628_inventory_entries import (
    AMMO_OFFSET,
    CAP_A_OFFSET,
    CAP_B_OFFSET,
    EXT_POINTER,
    FLAG_HIDDEN_BY_X2,
    FLAG_LEFT,
    FLAG_NOT_CYCLE,
    FLAG_X2,
    ORDER_OFFSET,
    OWNED_OFFSET,
    RECORD_POINTER,
    STRIDE,
    TABLE,
)

TIMELINE_NAME = "button_timeline.md"
DEFAULT_GAP_POLLS = 60
DEFAULT_TRIGGER_POLLS = 40

# Layout of the ext block (slot 0) and the record. MEASURED from the real session (T1642), meanings INFERRED.
HAND_A = 0xA4  # hand struct A
HAND_B = 0x1D4  # hand struct B (the main hand)
HAND_RECORD_INDEX = 0x30  # i32 record index inside a hand struct
HAND_LOADED = 0x38  # u16 loaded rounds per ammo type inside a hand struct
EXT_FLAGS = 0x28
EXT_THROWABLE = 0xA0  # default throwable entry (u32)
EXT_CURRENT = 0x94  # current weapon entry, selected immediately
EXT_FOLLOW = 0x98  # follows +0x94 after the switch animation, equal at rest
EQUIPPED_ONCE_OFFSET = OWNED_OFFSET + OWNED_ENTRIES  # ext+0x592+entry: "was equipped once"
REC_MAIN_RECORD = 0x58C  # i32 main hand record index
REC_DUAL = 0x59C  # dual flag
REC_MAIN_STATE = 0x5A0  # u32: 1 ready, 2 switching out, 6 drawing, 7 reloading
REC_ANIMATION = 0x644  # u32 animation id
EXT_NEED = AMMO_OFFSET + 2 * AMMO_TYPES  # bytes of the ext block the analysis reads
REC_NEED = ORDER_OFFSET + OWNED_ENTRIES
TABLE_FLAGS = 0x00  # entry table field offsets (the live table at 0x4FA128, stride 0x3C)
TABLE_PRIORITY = 0x08  # lower is better
TABLE_LEFT_RECORD = 0x0C
TABLE_RECORD = 0x10
TABLE_GROUP = 0x38
TABLE_NEED = OWNED_ENTRIES * STRIDE
FLAG_NEVER_SELECTABLE = 0x02
EMPTY_PRIORITY = 100  # 0x64: priority of an entry that is not in the table
MODES = (0, 1, 2, 3, 4)  # pickup auto-switch policy of 0x1BA7B0
MAX_PERMUTED = 6  # candidate entries permuted at most per ownership interval
STATE_FIELDS = (
    "ext.cur94",
    "ext.cur98",
    "ext.handB_rec",
    "ext.handA_rec",
    "rec.main_rec",
    "rec.main_state",
    "ext.throwable",
)
MAX_WARNINGS = 30

TABLE_SELECTOR = RangeSelector(spec=RangeSpec(False, TABLE, 0))
EXT_SELECTOR = RangeSelector(spec=RangeSpec(True, EXT_POINTER, 0))
REC_SELECTOR = RangeSelector(spec=RangeSpec(True, RECORD_POINTER, 0))

EVIDENCE_TEXT = (
    "evidence: xemu-level MEASURED observation of the unmodified game (no Xbox hardware capture exists), the meaning of the "
    "fields is INFERRED (static analysis of T1628, docs/t-player-inventory.md). A flip or a class below says what changed "
    "between two dumps, never why."
)


def u16(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 2], "little")


def u32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "little")


def i32(data: bytes, offset: int) -> int:
    return int.from_bytes(data[offset : offset + 4], "little", signed=True)


def words(data: bytes, offset: int, count: int) -> tuple[int, ...]:
    return tuple(u16(data, offset + 2 * index) for index in range(count))


@dataclass(frozen=True)
class DumpRef:
    """One dump file of the manifest, placed on the poll clock."""

    poll: int
    event: int
    tag_offset: int
    label: str
    path: Path | None

    @property
    def key(self) -> tuple[int, int, int, str]:
        return (self.poll, self.event, self.tag_offset, self.label)


@dataclass(frozen=True)
class Edge:
    poll: int
    event: int
    edge: str
    button: str

    def text(self) -> str:
        return f"{self.edge} {self.button}@{self.poll} (event {self.event})"

    def as_dict(self) -> dict[str, Any]:
        return {"poll": self.poll, "event": self.event, "edge": self.edge, "button": self.button}


@dataclass(frozen=True)
class ExtState:
    ref: DumpRef
    pointer: int
    owned: bytes
    once: bytes
    reserve: tuple[int, ...]
    loaded: tuple[int, ...]  # hand A + hand B per ammo type
    loaded_a: tuple[int, ...]
    loaded_b: tuple[int, ...]
    caps_a: tuple[int, ...]
    caps_b: tuple[int, ...]
    flags: int
    current: int
    follow: int
    record_a: int
    record_b: int
    throwable: int


@dataclass(frozen=True)
class RecState:
    ref: DumpRef
    pointer: int
    order: bytes
    main_record: int
    main_state: int
    animation: int
    dual: int


def decode_ext(ref: DumpRef, pointer: int, data: bytes) -> ExtState:
    loaded_a = words(data, HAND_A + HAND_LOADED, AMMO_TYPES)
    loaded_b = words(data, HAND_B + HAND_LOADED, AMMO_TYPES)
    return ExtState(
        ref=ref,
        pointer=pointer,
        owned=data[OWNED_OFFSET : OWNED_OFFSET + OWNED_ENTRIES],
        once=data[EQUIPPED_ONCE_OFFSET : EQUIPPED_ONCE_OFFSET + OWNED_ENTRIES],
        reserve=words(data, AMMO_OFFSET, AMMO_TYPES),
        loaded=tuple(a + b for a, b in zip(loaded_a, loaded_b, strict=True)),
        loaded_a=loaded_a,
        loaded_b=loaded_b,
        caps_a=words(data, CAP_A_OFFSET, AMMO_TYPES),
        caps_b=words(data, CAP_B_OFFSET, AMMO_TYPES),
        flags=u32(data, EXT_FLAGS),
        current=u32(data, EXT_CURRENT),
        follow=u32(data, EXT_FOLLOW),
        record_a=i32(data, HAND_A + HAND_RECORD_INDEX),
        record_b=i32(data, HAND_B + HAND_RECORD_INDEX),
        throwable=u32(data, EXT_THROWABLE),
    )


def decode_rec(ref: DumpRef, pointer: int, data: bytes) -> RecState:
    return RecState(
        ref=ref,
        pointer=pointer,
        order=data[ORDER_OFFSET : ORDER_OFFSET + OWNED_ENTRIES],
        main_record=i32(data, REC_MAIN_RECORD),
        main_state=u32(data, REC_MAIN_STATE),
        animation=u32(data, REC_ANIMATION),
        dual=u32(data, REC_DUAL),
    )


# --------------------------------------------------------------------------------------------------------------------
# The weapon entry table (live range 0x4FA128 of the dump, or the T1628 CSV)
# --------------------------------------------------------------------------------------------------------------------


@dataclass(frozen=True)
class TableEntry:
    flags: int
    priority: int
    left_record: int
    record: int
    group: int


@dataclass(frozen=True)
class EntryTable:
    entries: dict[int, TableEntry]
    source: str

    def role(self, entry: int) -> str:
        flags = self.entries[entry].flags
        return "x2" if flags & FLAG_X2 else "left_hand" if flags & FLAG_LEFT else "primary"

    def group_of(self, entry: int) -> int | None:
        row = self.entries.get(entry)
        return None if row is None else row.group

    def members(self, entry: int) -> list[int]:
        group = self.entries[entry].group
        return [member for member, row in sorted(self.entries.items()) if row.group == group]

    def grant(self, entry: int) -> set[int]:
        """Entries owned after granting `entry` (0x1BFAA0): an x2 owns the whole group, any other entry the members that are not x2."""
        members = self.members(entry)
        if self.role(entry) == "x2":
            return set(members)
        return {member for member in members if self.role(member) != "x2"}

    def priority(self, entry: int) -> int:
        row = self.entries.get(entry)
        return EMPTY_PRIORITY if row is None else row.priority

    def selectable(self, entry: int, owned: set[int]) -> bool:
        """The selectable predicate (0x1BA410) WITHOUT the ammo test: the record ammo types are not in the dump."""
        row = self.entries.get(entry)
        if row is None or entry not in owned:
            return False
        if row.flags & (FLAG_NOT_CYCLE | FLAG_NEVER_SELECTABLE):
            return False
        if row.flags & FLAG_HIDDEN_BY_X2:
            return not any(
                member in owned and self.role(member) == "x2" for member in self.members(entry)
            )
        return True

    def switches(self, mode: int, new: int, current: int, owned: set[int], was_owned: bool) -> bool:
        """Policy of 0x1BA7B0 for one grant of `new` while `current` is held (`owned` already includes the grant)."""
        row, held = self.entries.get(new), self.entries.get(current)
        if row is None:
            return False
        if held is not None and row.record == held.record and row.left_record == -1:
            return False  # early rule: same record as the current weapon and no left hand
        selectable = self.selectable(new, owned)
        better = row.priority <= self.priority(current)
        if mode == 0:
            return selectable
        if mode == 1:
            return False
        if mode == 2:
            return selectable and better
        if mode == 3:
            return selectable and not was_owned
        return selectable and better and not was_owned

    def final_current(
        self, mode: int, order: tuple[int, ...], owned_before: set[int], current: int
    ) -> int:
        owned = set(owned_before)
        for entry in order:
            was_owned = entry in owned
            owned |= self.grant(entry)
            if self.switches(mode, entry, current, owned, was_owned):
                current = entry
        return current


def decode_table_bytes(data: bytes) -> EntryTable:
    entries = {}
    for entry in range(OWNED_ENTRIES):
        base = entry * STRIDE
        entries[entry] = TableEntry(
            flags=u32(data, base + TABLE_FLAGS),
            priority=i32(data, base + TABLE_PRIORITY),
            left_record=i32(data, base + TABLE_LEFT_RECORD),
            record=i32(data, base + TABLE_RECORD),
            group=i32(data, base + TABLE_GROUP),
        )
    return EntryTable(entries, f"live dump range 0x{TABLE:X}")


def load_csv_table(path: Path) -> EntryTable:
    """The T1628 entries CSV as a table (columns entry, group, flags, priority_0x08, left_record_0x0C, record_0x10)."""
    entries: dict[int, TableEntry] = {}
    try:
        with open(path, newline="", encoding="utf-8") as handle:
            for row in csv.DictReader(handle):
                entries[int(row["entry"], 0)] = TableEntry(
                    flags=int(row["flags"], 0),
                    priority=int(row["priority_0x08"], 0),
                    left_record=int(row["left_record_0x0C"], 0),
                    record=int(row["record_0x10"], 0),
                    group=int(row["group"], 0),
                )
    except (KeyError, ValueError, OSError) as error:
        raise ValueError(f"cannot use entries csv {path} as an entry table: {error!r}") from None
    if not entries:
        raise ValueError(f"entries csv {path} has no rows")
    return EntryTable(entries, f"entries csv {path.name}")


# --------------------------------------------------------------------------------------------------------------------
# Ordering the dumps and the edges
# --------------------------------------------------------------------------------------------------------------------


def collect_dumps(directory: Path, manifest: Manifest, warnings: list[str]) -> list[DumpRef]:
    """Every dump of every event ordered by (poll, event index, tag), duplicate labels removed."""
    refs: dict[str, DumpRef] = {}
    skipped = 0
    for event in manifest.events:
        for entry in event.dumps:
            kind = str(entry.get("kind", ""))
            label = entry.get("label")
            if not (kind in ("before", "after") or kind.startswith("idle")):
                continue
            if not isinstance(label, str) or not label or label in refs:
                continue
            tag_offset = _tag_offset(entry)
            poll = entry.get("poll")
            if not isinstance(poll, int) or isinstance(poll, bool):
                if event.poll is None:
                    skipped += 1
                    continue
                poll = event.poll + tag_offset
            refs[label] = DumpRef(
                poll, event.index, tag_offset, label, _dump_path(directory, entry)
            )
    if skipped:
        warnings.append(f"{skipped} dump(s) without any usable poll were skipped")
    return sorted(refs.values(), key=lambda ref: ref.key)


def collect_edges(raw_events: list[dict[str, Any]]) -> list[Edge]:
    edges: list[Edge] = []
    for event in raw_events:
        index = event.get("index")
        listed = event.get("edges")
        if not isinstance(index, int) or not isinstance(listed, list):
            continue
        for item in listed:
            poll = item.get("poll") if isinstance(item, dict) else None
            if isinstance(poll, int) and not isinstance(poll, bool):
                edges.append(
                    Edge(poll, index, str(item.get("edge", "?")), str(item.get("button", "?")))
                )
    edges.sort(key=lambda edge: (edge.poll, edge.event, edge.button))
    return edges


def read_raw_events(path: Path) -> list[dict[str, Any]]:
    """The raw event objects of the manifest (the report's Event keeps no edge list)."""
    events: dict[int, dict[str, Any]] = {}
    for text in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            obj = json.loads(text)
        except json.JSONDecodeError:
            continue
        if (
            isinstance(obj, dict)
            and obj.get("type") == "event"
            and isinstance(obj.get("index"), int)
        ):
            events[obj["index"]] = obj
    return [events[index] for index in sorted(events)]


def edges_between(edges: list[Edge], start: int, end: int) -> list[Edge]:
    """Edges with start <= poll < end."""
    low = bisect.bisect_left(edges, start, key=lambda edge: edge.poll)
    high = bisect.bisect_left(edges, end, key=lambda edge: edge.poll)
    return edges[low:high]


# --------------------------------------------------------------------------------------------------------------------
# Loading one dump
# --------------------------------------------------------------------------------------------------------------------


def _pick(
    parsed_ranges: list[RangeInfo], selector: RangeSelector, need: int
) -> tuple[RangeInfo | None, str]:
    chosen = next(
        (item for item in parsed_ranges if selector.matches(item) and item.data is not None),
        None,
    )
    if chosen is None or chosen.data is None:
        return None, "range not readable"
    if len(chosen.data) < need:
        return None, f"only {len(chosen.data)} of {need} bytes readable"
    return chosen, ""


def load_states(
    ref: DumpRef, want_table: bool = False
) -> tuple[ExtState | None, RecState | None, bytes | None, list[str]]:
    """Parse the dump once; each state is None when that range is missing or too short. The notes say why.
    The third item is the live entry table (range 0x4FA128) when wanted and complete."""
    notes: list[str] = []
    if ref.path is None:
        return None, None, None, [f"{ref.label}: no usable file name in the manifest"]
    try:
        parsed = parse_dump(
            ref.path,
            lambda info: (
                EXT_SELECTOR.matches(info)
                or REC_SELECTOR.matches(info)
                or (want_table and TABLE_SELECTOR.matches(info))
            ),
        )
    except OSError as error:
        return None, None, None, [f"{ref.label}: {error.strerror or error}"]
    ext: ExtState | None = None
    rec: RecState | None = None
    picked, why = _pick(parsed.collected, EXT_SELECTOR, EXT_NEED)
    if picked is not None and picked.data is not None:
        pointer = picked.pointer if picked.pointer is not None else (picked.address or 0)
        ext = decode_ext(ref, pointer, bytes(picked.data))
    else:
        notes.append(f"{ref.label}: ext block {why}")
    picked, why = _pick(parsed.collected, REC_SELECTOR, REC_NEED)
    if picked is not None and picked.data is not None:
        pointer = picked.pointer if picked.pointer is not None else (picked.address or 0)
        rec = decode_rec(ref, pointer, bytes(picked.data))
    else:
        notes.append(f"{ref.label}: record {why}")
    live: bytes | None = None
    if want_table:
        picked, why = _pick(parsed.collected, TABLE_SELECTOR, TABLE_NEED)
        if picked is not None and picked.data is not None:
            live = bytes(picked.data)
    return ext, rec, live, notes


# --------------------------------------------------------------------------------------------------------------------
# Analysis
# --------------------------------------------------------------------------------------------------------------------


def ref_dict(ref: DumpRef) -> dict[str, Any]:
    return {"label": ref.label, "poll": ref.poll, "event": ref.event}


def classify_ammo(
    old_reserve: int,
    old_loaded: int,
    new_reserve: int,
    new_loaded: int,
    weapon_changed: bool,
    gap: bool = False,
    edge_near: bool = False,
    hands_same: bool = True,
) -> tuple[str, bool]:
    """(class, conservation violation) of one (reserve, loaded) change of one ammo type.

    shot / multi: loaded fell, reserve unchanged. equip-fill: loaded 0 -> n from reserve with a weapon change. reload: loaded
    +n, reserve -n. holster-return: loaded -n, reserve +n, and the loaded rounds are gone or the weapon changed (a dual wield
    hand swap keeps the other hand loaded). pickup: the total grew with loaded unchanged. Anything else with an unchanged
    total is `other`. A changed total that mixes a reserve and a loaded change is a composite of two events: in a GAP interval
    that is `mixed` (expected, not attributable), otherwise `other` and a conservation violation.
    use (throw or place a grenade or mine): reserve fell, loaded is unchanged in both hands, the weapon did not change and a
    button edge lies within the trigger window before the dump. Without that edge the same drop stays a violation."""
    delta_reserve = new_reserve - old_reserve
    delta_loaded = new_loaded - old_loaded
    delta_total = delta_reserve + delta_loaded
    if delta_loaded == -1 and delta_reserve == 0:
        return "shot", False
    if delta_loaded < -1 and delta_reserve == 0:
        return "multi", False
    if old_loaded == 0 and delta_loaded > 0 and delta_reserve == -delta_loaded and weapon_changed:
        return "equip-fill", False
    if delta_loaded > 0 and delta_reserve == -delta_loaded:
        return "reload", False
    if delta_loaded < 0 and delta_reserve == -delta_loaded and (new_loaded == 0 or weapon_changed):
        return "holster-return", False
    if delta_total > 0 and delta_loaded == 0:
        return "pickup", False
    if delta_reserve < 0 and delta_loaded == 0 and hands_same and edge_near and not weapon_changed:
        return "use", False
    if delta_total == 0:
        return "other", False
    return ("mixed", False) if gap else ("other", True)


@dataclass
class Timeline:
    gap_polls: int
    trigger_polls: int
    edges: list[Edge]
    entries: dict[int, EntryInfo]
    names: dict[int, str]
    ownership: list[dict[str, Any]] = field(default_factory=list)
    ammo: dict[int, list[dict[str, Any]]] = field(default_factory=dict)
    ammo_peak: dict[int, int] = field(default_factory=dict)
    states: list[dict[str, Any]] = field(default_factory=list)
    reloads: list[dict[str, Any]] = field(default_factory=list)
    intervals: int = 0
    last_ext: ExtState | None = None
    last_ext_rec: RecState | None = None
    last_rec: RecState | None = None
    state_tuple: tuple[int | None, ...] | None = None
    ext_now: ExtState | None = None
    rec_now: RecState | None = None
    table: EntryTable | None = None
    table_live: bool = False
    auto_switch: list[dict[str, Any]] = field(default_factory=list)
    cur_interval: dict[str, Any] | None = None
    order_at_row: bytes | None = None

    def entry_info(self, entry: int) -> dict[str, Any]:
        info = self.entries.get(entry)
        name = self.names.get(entry)
        return {
            "entry": entry,
            "hex": f"0x{entry:02X}",
            "name": name,
            "role": info.role if info is not None else None,
            "group": info.group if info is not None else None,
        }

    def trigger(self, ref: DumpRef) -> Edge | None:
        near = edges_between(self.edges, ref.poll - self.trigger_polls, ref.poll)
        return near[-1] if near else None

    # -- feeding the dumps in order --------------------------------------------------------------------------------

    def feed(self, ext: ExtState | None, rec: RecState | None, ref: DumpRef) -> None:
        reload_ranges: list[str] = []
        self.cur_interval = None
        if ext is not None:
            if self.last_ext is not None and ext.pointer != self.last_ext.pointer:
                reload_ranges.append("ext")
                self.reloads.append(self.reload_row(ref, "ext", self.last_ext.pointer, ext.pointer))
                self.baseline_ammo(ext)
            elif self.last_ext is None:
                self.baseline_ammo(ext)
            else:
                self.compare_ext(self.last_ext, ext, self.last_ext_rec, rec)
            self.last_ext = ext
            self.last_ext_rec = rec
            self.ext_now = ext
        if rec is not None:
            if self.last_rec is not None and rec.pointer != self.last_rec.pointer:
                reload_ranges.append("rec")
                self.reloads.append(self.reload_row(ref, "rec", self.last_rec.pointer, rec.pointer))
            self.last_rec = rec
            self.rec_now = rec
        self.feed_state(ref, bool(reload_ranges), ext is not None or rec is not None)

    def reload_row(self, ref: DumpRef, which: str, old: int, new: int) -> dict[str, Any]:
        return {
            **ref_dict(ref),
            "range": which,
            "old_pointer": f"0x{old:08X}",
            "new_pointer": f"0x{new:08X}",
        }

    def interval(self, old: ExtState, new: ExtState) -> dict[str, Any]:
        span = new.ref.poll - old.ref.poll
        found = edges_between(self.edges, old.ref.poll, new.ref.poll)
        return {
            "from": ref_dict(old.ref),
            "to": ref_dict(new.ref),
            "polls": span,
            "gap": span > self.gap_polls,
            "edges": [edge.as_dict() for edge in found],
            "attribution": "by button" if found else "no button edge in the interval",
        }

    def compare_ext(
        self, old: ExtState, new: ExtState, old_rec: RecState | None, new_rec: RecState | None
    ) -> None:
        self.intervals += 1
        info = self.interval(old, new)
        flips = [
            {**self.entry_info(entry), "change": "0->1" if new.owned[entry] else "1->0"}
            for entry in range(OWNED_ENTRIES)
            if bool(old.owned[entry]) != bool(new.owned[entry])
        ]
        self.cur_interval = {"edges": info["edges"], "flips": bool(flips)}
        if flips:
            self.ownership.append(
                {**info, "flips": flips, "other": self.other_changes(old, new, old_rec, new_rec)}
            )
            evidence = self.auto_switch_row(info, old, new, flips)
            if evidence is not None:
                self.auto_switch.append(evidence)
        self.compare_ammo(old, new, info)

    # -- auto-switch evidence ----------------------------------------------------------------------------------------

    def auto_switch_row(
        self, info: dict[str, Any], old: ExtState, new: ExtState, flips: list[dict[str, Any]]
    ) -> dict[str, Any] | None:
        """Which pickup policies (modes 0..4 of 0x1BA7B0) can explain the current weapon of the interval."""
        gained = [item["entry"] for item in flips if item["change"] == "0->1"]
        row: dict[str, Any] = {
            "from": info["from"],
            "to": info["to"],
            "polls": info["polls"],
            "gap": info["gap"],
            "edges": info["edges"],
            "cur_before": old.current,
            "cur_after": new.current,
            "new_owned": [],
            "candidates": [],
        }
        table = self.table
        if table is None:
            if not gained:
                return None
            row["new_owned"] = [{"entry": entry, "hex": f"0x{entry:02X}"} for entry in gained]
            row["unavailable"] = "no entry table (live range 0x4FA128 or the entries CSV)"
            return row
        known = [entry for entry in gained if entry in table.entries]
        row["new_owned"] = [
            {
                "entry": entry,
                "hex": f"0x{entry:02X}",
                "role": table.role(entry),
                "priority": table.entries[entry].priority,
                "flags": table.entries[entry].flags,
            }
            for entry in known
        ]
        candidates = [entry for entry in known if table.role(entry) in ("primary", "x2")]
        if not candidates:
            return None
        row["candidates"] = candidates
        if len(candidates) > MAX_PERMUTED:
            row["unavailable"] = (
                f"{len(candidates)} candidate entries, more than {MAX_PERMUTED} to permute"
            )
            return row
        owned_before = {entry for entry in range(OWNED_ENTRIES) if old.owned[entry]}
        consistent = [
            mode
            for mode in MODES
            if any(
                table.final_current(mode, order, owned_before, old.current) == new.current
                for order in itertools.permutations(candidates)
            )
        ]
        row["consistent_modes"] = consistent
        row["inconsistent_modes"] = [mode for mode in MODES if mode not in consistent]
        return row

    def other_changes(
        self, old: ExtState, new: ExtState, old_rec: RecState | None, new_rec: RecState | None
    ) -> dict[str, Any]:
        other: dict[str, Any] = {}
        for name, before, after in (
            ("ext.cur94", old.current, new.current),
            ("ext.cur98", old.follow, new.follow),
            ("ext.flags", old.flags, new.flags),
            ("ext.throwable", old.throwable, new.throwable),
        ):
            if before != after:
                other[name] = [before, after]
        for name, before, after in (
            ("reserve", old.reserve, new.reserve),
            ("loaded", old.loaded, new.loaded),
            ("cap_a", old.caps_a, new.caps_a),
            ("cap_b", old.caps_b, new.caps_b),
        ):
            changed = {
                str(kind): [before[kind], after[kind]]
                for kind in range(AMMO_TYPES)
                if before[kind] != after[kind]
            }
            if changed:
                other[name] = changed
        once = {
            f"0x{entry:02X}": [old.once[entry], new.once[entry]]
            for entry in range(OWNED_ENTRIES)
            if old.once[entry] != new.once[entry]
        }
        if once:
            other["equipped_once"] = once
        if old_rec is not None and new_rec is not None and old_rec.pointer == new_rec.pointer:
            order = {
                str(group): [old_rec.order[group], new_rec.order[group]]
                for group in range(OWNED_ENTRIES)
                if old_rec.order[group] != new_rec.order[group]
            }
            if order:
                other["group_order"] = order
        return other

    # -- ammo --------------------------------------------------------------------------------------------------------

    def baseline_ammo(self, ext: ExtState) -> None:
        for kind in range(AMMO_TYPES):
            if ext.reserve[kind] or ext.loaded[kind]:
                self.add_ammo_row(kind, ext, "start", False, None, None)

    def add_ammo_row(
        self,
        kind: int,
        ext: ExtState,
        name: str,
        violation: bool,
        info: dict[str, Any] | None,
        old: ExtState | None,
    ) -> None:
        total = ext.reserve[kind] + ext.loaded[kind]
        self.ammo_peak[kind] = max(self.ammo_peak.get(kind, 0), total)
        row: dict[str, Any] = {
            **ref_dict(ext.ref),
            "class": name,
            "reserve": ext.reserve[kind],
            "loaded": ext.loaded[kind],
            "total": total,
            "cap_a": ext.caps_a[kind],
            "cap_b": ext.caps_b[kind],
            "weapon": ext.current,
            "violation": violation,
        }
        if info is not None and old is not None:
            row["from"] = info["from"]
            row["polls"] = info["polls"]
            row["gap"] = info["gap"]
            row["edges"] = info["edges"]
            row["delta_reserve"] = ext.reserve[kind] - old.reserve[kind]
            row["delta_loaded"] = ext.loaded[kind] - old.loaded[kind]
            row["cap_changed"] = (old.caps_a[kind], old.caps_b[kind]) != (
                ext.caps_a[kind],
                ext.caps_b[kind],
            )
        self.ammo.setdefault(kind, []).append(row)

    def compare_ammo(self, old: ExtState, new: ExtState, info: dict[str, Any]) -> None:
        """One row per ammo type whose (reserve, loaded) changed; a change of only the per hand saved rounds words
        (ext+0x134 / ext+0x264) of a type that holds ammo is a `saved-rounds` row."""
        edge_near = bool(edges_between(self.edges, new.ref.poll - self.trigger_polls, new.ref.poll))
        for kind in range(AMMO_TYPES):
            if (old.reserve[kind], old.loaded[kind]) == (new.reserve[kind], new.loaded[kind]):
                holds = old.reserve[kind] + old.loaded[kind] > 0
                saved = (old.caps_a[kind], old.caps_b[kind]) != (new.caps_a[kind], new.caps_b[kind])
                if holds and saved:
                    self.add_ammo_row(kind, new, "saved-rounds", False, info, old)
                continue
            name, violation = classify_ammo(
                old.reserve[kind],
                old.loaded[kind],
                new.reserve[kind],
                new.loaded[kind],
                (old.current, old.follow) != (new.current, new.follow),
                info["gap"],
                edge_near,
                (old.loaded_a[kind], old.loaded_b[kind])
                == (new.loaded_a[kind], new.loaded_b[kind]),
            )
            self.add_ammo_row(kind, new, name, violation, info, old)

    # -- weapon state ------------------------------------------------------------------------------------------------

    def current_tuple(self) -> tuple[int | None, ...]:
        ext, rec = self.ext_now, self.rec_now
        return (
            ext.current if ext else None,
            ext.follow if ext else None,
            ext.record_b if ext else None,
            ext.record_a if ext else None,
            rec.main_record if rec else None,
            rec.main_state if rec else None,
            ext.throwable if ext else None,
        )

    def order_changes(self, rec: RecState | None, baseline: bool) -> dict[str, list[int]]:
        """Group order bytes (rec+0xE3C+group) that changed since the previous state row."""
        if baseline or rec is None or self.order_at_row is None:
            return {}
        return {
            str(group): [self.order_at_row[group], rec.order[group]]
            for group in range(OWNED_ENTRIES)
            if self.order_at_row[group] != rec.order[group]
        }

    def state_label(
        self,
        previous: tuple[int | None, ...],
        current: tuple[int | None, ...],
        order: dict[str, list[int]],
    ) -> str:
        """start / auto-switch / alt-toggle / cycle / switch (current weapon changed), throwable-select, or state."""
        before, after = previous[0], current[0]
        if before != after:
            interval = self.cur_interval
            if interval is not None and interval["flips"] and not interval["edges"]:
                return "auto-switch"
            if self.table is None or before is None or after is None:
                return "switch"
            old_group, new_group = self.table.group_of(before), self.table.group_of(after)
            if old_group is None or new_group is None:
                return "switch"
            if old_group != new_group:
                return "cycle"
            moved = order.get(str(new_group))
            return "alt-toggle" if moved is not None and moved[1] == after else "switch"
        if previous[6] != current[6]:
            return "throwable-select"
        return "state"

    def feed_state(self, ref: DumpRef, reloaded: bool, any_data: bool) -> None:
        if not any_data:
            return
        current = self.current_tuple()
        first = self.state_tuple is None
        if not (first or reloaded or current != self.state_tuple):
            return
        previous = self.state_tuple
        changed = (
            [
                name
                for name, before, after in zip(STATE_FIELDS, previous, current, strict=True)
                if before != after
            ]
            if previous is not None
            else []
        )
        trigger = None if first or reloaded else self.trigger(ref)
        rec = self.rec_now
        baseline = first or reloaded
        order = self.order_changes(rec, baseline)
        label = (
            "start" if previous is None or baseline else self.state_label(previous, current, order)
        )
        self.states.append(
            {
                **ref_dict(ref),
                "kind": label,
                "values": dict(zip(STATE_FIELDS, current, strict=True)),
                "changed": changed,
                "order": order,
                "baseline": baseline,
                "trigger": trigger.as_dict() if trigger is not None else None,
                "animation": rec.animation if rec else None,
                "dual": rec.dual if rec else None,
            }
        )
        self.state_tuple = current
        if rec is not None:
            self.order_at_row = rec.order


def build_timeline(
    directory: Path,
    manifest: Manifest,
    manifest_path: Path,
    gap_polls: int,
    trigger_polls: int,
    entries: dict[int, EntryInfo],
    names: dict[int, str],
    csv_table: EntryTable | None = None,
) -> tuple[dict[str, Any], int]:
    """Run the analysis; returns (document, attempted dumps that had at least one usable range)."""
    warnings: list[str] = list(manifest.warnings)
    refs = collect_dumps(directory, manifest, warnings)
    edges = collect_edges(read_raw_events(manifest_path))
    timeline = Timeline(gap_polls, trigger_polls, edges, entries, names, table=csv_table)
    usable = 0
    with_ext = 0
    with_rec = 0
    notes: list[str] = []
    for ref in refs:
        ext, rec, live, why = load_states(ref, want_table=not timeline.table_live)
        notes.extend(why)
        if live is not None:
            timeline.table = decode_table_bytes(live)
            timeline.table_live = True
        usable += ext is not None or rec is not None
        with_ext += ext is not None
        with_rec += rec is not None
        timeline.feed(ext, rec, ref)
    warnings.extend(notes[:MAX_WARNINGS])
    if len(notes) > MAX_WARNINGS:
        warnings.append(f"... {len(notes) - MAX_WARNINGS} more dumps with a missing range")
    forced = (manifest.start or {}).get("forced_state")
    forced = forced if isinstance(forced, bool) else None
    polls = [ref.poll for ref in refs]
    meta = {
        "directory": str(directory),
        "dumps": len(refs),
        "dumps with ext block": with_ext,
        "dumps with record": with_rec,
        "ext intervals": timeline.intervals,
        "poll range": [min(polls), max(polls)] if polls else None,
        "gap polls": gap_polls,
        "trigger polls": trigger_polls,
        "button edges in manifest": len(edges),
        "ownership intervals with flips": len(timeline.ownership),
        "gap intervals with flips": sum(1 for item in timeline.ownership if item["gap"]),
        "ammo types traced": len(timeline.ammo),
        "conservation violations": sum(
            1 for rows in timeline.ammo.values() for row in rows if row["violation"]
        ),
        "weapon state rows": len(timeline.states),
        "auto-switch intervals": len(timeline.auto_switch),
        "entry table": timeline.table.source if timeline.table is not None else "none",
        "reload markers": len(timeline.reloads),
        "forced_state": forced,
    }
    document = {
        "meta": meta,
        "evidence": [EVIDENCE_TEXT, *evidence_lines(forced, poke_files_in(directory))],
        "warnings": warnings,
        "reloads": timeline.reloads,
        "ownership": timeline.ownership,
        "ammo": {
            str(kind): {"peak_total": timeline.ammo_peak[kind], "rows": rows}
            for kind, rows in sorted(timeline.ammo.items())
        },
        "weapon_state": timeline.states,
        "auto_switch": timeline.auto_switch,
    }
    return document, usable


# --------------------------------------------------------------------------------------------------------------------
# Rendering
# --------------------------------------------------------------------------------------------------------------------


def table(headers: list[str], rows: list[list[str]]) -> list[str]:
    lines = ["| " + " | ".join(headers) + " |", "|" + "|".join("---" for _ in headers) + "|"]
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    return lines


def where(item: dict[str, Any]) -> str:
    return f"{item['label']} (poll {item['poll']})"


def edge_text(edges: list[dict[str, Any]]) -> str:
    if not edges:
        return "-"
    return ", ".join(f"{edge['edge']} {edge['button']}@{edge['poll']}" for edge in edges)


def entry_text(flip: dict[str, Any]) -> str:
    text = f"{flip['hex']} {flip['change']}"
    detail = [str(flip[key]) for key in ("name", "role") if flip.get(key)]
    if flip.get("group"):
        detail.append(f"group {flip['group']}")
    return text + (f" ({', '.join(detail)})" if detail else "")


def ammo_class_text(row: dict[str, Any]) -> str:
    if row["class"] == "multi":
        return f"multi ({-row['delta_loaded']} shots)"
    return str(row["class"])


AUTO_SWITCH_TEXT = (
    "Policy model of 0x1BA7B0, INFERRED: mode 0 switch if selectable, 1 never, 2 switch if selectable and priority(new) <= "
    "priority(current), 3 switch if selectable and the entry was not owned before, 4 like 2 but only if not owned before. "
    "Early rule: no switch when record(new) == record(current) and left(new) == -1. A mode is CONSISTENT when some order "
    "of the newly owned primary or x2 entries (an x1 grant owns its left-hand companion, an x2 grant owns the group) "
    "reproduces the observed current weapon (ext+0x94). LIMIT: selectable ignores ammo (the record ammo types are not in the "
    "dump, an owned entry is assumed selectable), so a grenade-like entry with flag 0x10 or 0x2 is the only never-selectable "
    "case. Several pickups in a gap, or a manual switch inside it, can also mislead the result.",
)


def auto_switch_line(item: dict[str, Any]) -> str:
    gained = ", ".join(
        f"{entry['hex']} {entry.get('role', '?')}"
        + (f" prio {entry['priority']}" if "priority" in entry else "")
        for entry in item["new_owned"]
    )
    head = f"- {item['from']['label']} -> {item['to']['label']} ({'GAP ' if item['gap'] else ''}{item['polls']} polls, edges: {edge_text(item['edges'])}): new {gained}; current {item['cur_before']} -> {item['cur_after']}"
    if "unavailable" in item:
        return f"{head}; unavailable: {item['unavailable']}"
    consistent = ",".join(str(mode) for mode in item["consistent_modes"]) or "none"
    inconsistent = ",".join(str(mode) for mode in item["inconsistent_modes"]) or "none"
    return f"{head}; modes consistent: {consistent}; inconsistent: {inconsistent}"


def other_text(other: dict[str, Any]) -> str:
    parts: list[str] = []
    for key, value in other.items():
        if isinstance(value, list):
            parts.append(f"{key} {value[0]}->{value[1]}")
        else:
            changes = ", ".join(f"{name} {pair[0]}->{pair[1]}" for name, pair in value.items())
            parts.append(f"{key}[{changes}]")
    return "; ".join(parts) if parts else "-"


def render_markdown(document: dict[str, Any]) -> str:
    out: list[str] = ["# Button dump timeline (T1642)", ""]
    out.extend(
        f"- {key}: {'n/a' if value is None else value}" for key, value in document["meta"].items()
    )
    out.append("")
    out.extend(document["evidence"])
    out.append("")
    out.extend(f"warning: {text}" for text in document["warnings"])
    if document["warnings"]:
        out.append("")
    out.append("## Reload markers")
    out.append("")
    if document["reloads"]:
        rows = [
            [where(item), item["range"], item["old_pointer"], item["new_pointer"]]
            for item in document["reloads"]
        ]
        out.extend(table(["dump", "range", "old pointer", "new pointer"], rows))
        out.append("")
        out.append("RELOAD: the baseline of that range restarts at the marked dump.")
    else:
        out.append("none (the range pointers never changed)")
    out.extend(["", "## a. Ownership flips", ""])
    if document["ownership"]:
        rows = []
        for item in document["ownership"]:
            flag = "GAP " if item["gap"] else ""
            rows.append(
                [
                    where(item["from"]),
                    where(item["to"]),
                    f"{flag}{item['polls']}",
                    "; ".join(entry_text(flip) for flip in item["flips"]),
                    item["attribution"]
                    + (f": {edge_text(item['edges'])}" if item["edges"] else ""),
                    other_text(item["other"]),
                ]
            )
        out.extend(table(["from", "to", "polls", "flips", "attribution", "other signals"], rows))
        out.append("")
        out.append(
            "GAP: the interval is longer than the gap threshold, the flip cannot be attributed to a moment."
        )
    else:
        out.append("no owned byte changed between two dumps")
    out.extend(["", "## b. Ammo trace", ""])
    if not document["ammo"]:
        out.append("no ammo type was ever nonzero")
    for kind, data in document["ammo"].items():
        out.extend([f"### ammo type {kind}: peak total {data['peak_total']} (ceiling hint)", ""])
        rows = []
        for row in data["rows"]:
            note = ("GAP " if row.get("gap") else "") + ("VIOLATION " if row["violation"] else "")
            rows.append(
                [
                    where(row),
                    f"{note}{ammo_class_text(row)}",
                    str(row["reserve"]),
                    str(row["loaded"]),
                    str(row["total"]),
                    f"{row['cap_a']}{'*' if row.get('cap_changed') else ''}",
                    f"{row['cap_b']}{'*' if row.get('cap_changed') else ''}",
                    str(row["weapon"]),
                    edge_text(row.get("edges", [])),
                ]
            )
        out.extend(
            table(
                [
                    "dump",
                    "class",
                    "reserve",
                    "loaded",
                    "total",
                    "cap A",
                    "cap B",
                    "weapon",
                    "edges in interval",
                ],
                rows,
            )
        )
        out.append("")
    out.extend(["## c. Weapon state timeline", ""])
    if document["weapon_state"]:
        headers = ["dump", *STATE_FIELDS, "anim", "kind", "changed", "order byte", "trigger"]
        rows = []
        for row in document["weapon_state"]:
            trigger = row["trigger"]
            rows.append(
                [
                    where(row),
                    *(
                        "-" if row["values"][name] is None else str(row["values"][name])
                        for name in STATE_FIELDS
                    ),
                    "-" if row["animation"] is None else str(row["animation"]),
                    row["kind"],
                    ",".join(row["changed"]) or "-",
                    ", ".join(
                        f"g{group} {pair[0]}->{pair[1]}" for group, pair in row["order"].items()
                    )
                    or "-",
                    "-"
                    if trigger is None
                    else f"{trigger['edge']} {trigger['button']}@{trigger['poll']}",
                ]
            )
        out.extend(table(headers, rows))
    else:
        out.append("no dump had a readable ext block or record")
    out.extend(["", "## d. Auto-switch evidence", ""])
    out.extend(AUTO_SWITCH_TEXT)
    out.append("")
    if document["auto_switch"]:
        out.extend(auto_switch_line(item) for item in document["auto_switch"])
    else:
        out.append("no interval in which a primary or x2 entry became owned")
    return "\n".join(out) + "\n"


# --------------------------------------------------------------------------------------------------------------------
# Command line
# --------------------------------------------------------------------------------------------------------------------


def _count(text: str) -> int:
    value = int(text, 0)
    if value < 0:
        raise argparse.ArgumentTypeError("must be >= 0")
    return value


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.button_dump_timeline",
        description="T1642: cross-event timeline of a button dump session: ownership flips, ammo trace and weapon "
        "state across ALL dumps, including the changes that happen between button events.",
    )
    parser.add_argument(
        "directory", metavar="DIR", help="the dump directory with buttons.jsonl and buttons/"
    )
    parser.add_argument("--write", action="store_true", help=f"write DIR/{TIMELINE_NAME}")
    parser.add_argument(
        "--json", action="store_true", help="print the JSON document instead of Markdown"
    )
    parser.add_argument(
        "--gap-polls",
        type=_count,
        default=DEFAULT_GAP_POLLS,
        metavar="N",
        help=f"an interval longer than N polls is flagged GAP (default {DEFAULT_GAP_POLLS})",
    )
    parser.add_argument(
        "--trigger-polls",
        type=_count,
        default=DEFAULT_TRIGGER_POLLS,
        metavar="N",
        help=f"a weapon state row names the button edge up to N polls before it (default {DEFAULT_TRIGGER_POLLS})",
    )
    parser.add_argument(
        "--entries-csv",
        default=None,
        metavar="FILE",
        help="entry roles (default docs/data/t-player-inventory-entries.csv)",
    )
    parser.add_argument("--entry-names", default=None, metavar="FILE", help="JSON {entry: name}")
    parser.add_argument(
        "--force", action="store_true", help="allow overwriting an existing file outside DIR"
    )
    return parser


def load_entry_sources(
    entries_csv: str | None, entry_names: str | None, warnings: list[str]
) -> tuple[dict[int, EntryInfo], dict[int, str], EntryTable | None]:
    """Entry roles and names, plus the CSV as a fallback entry table (the live dump table wins when the dumps carry it)."""
    entries: dict[int, EntryInfo] = {}
    names: dict[int, str] = {}
    csv_table: EntryTable | None = None
    path = Path(entries_csv) if entries_csv else report.ENTRIES_CSV
    if entries_csv:
        entries = load_entries_csv(path)
    elif path.is_file():
        try:
            entries = load_entries_csv(path)
        except ValueError as error:
            warnings.append(f"{error}; entry roles are not shown")
    if path.is_file():
        try:
            csv_table = load_csv_table(path)
        except ValueError as error:
            warnings.append(f"{error}; the dumps' live table is used when present")
    if entry_names:
        names = load_entry_names(Path(entry_names))
    return entries, names, csv_table


def run_timeline(
    directory: Path,
    manifest: Manifest,
    *,
    gap_polls: int = DEFAULT_GAP_POLLS,
    trigger_polls: int = DEFAULT_TRIGGER_POLLS,
    entries_csv: str | None = None,
    entry_names: str | None = None,
    write: bool = False,
    as_json: bool = False,
    force: bool = False,
) -> int:
    """Analyse, print (Markdown or JSON) and optionally write DIR/button_timeline.md."""
    sources: list[str] = []
    try:
        entries, names, csv_table = load_entry_sources(entries_csv, entry_names, sources)
    except ValueError as error:
        return report._fail(str(error))
    document, usable = build_timeline(
        directory,
        manifest,
        directory / report.MANIFEST_NAME,
        gap_polls,
        trigger_polls,
        entries,
        names,
        csv_table,
    )
    document["warnings"].extend(sources)
    if document["meta"]["dumps"] and not usable:
        return report._fail(
            f"every dump ({document['meta']['dumps']}) lacked both the ext block and the record",
            EXIT_UNREADABLE,
        )
    markdown = render_markdown(document)
    print(json.dumps(document, indent=1) if as_json else markdown, end="" if not as_json else "\n")
    if write:
        target = directory / TIMELINE_NAME
        error = report._write_output(directory, target, markdown, force)
        if error:
            return report._fail(error)
        print(f"wrote {target}", file=sys.stderr if as_json else sys.stdout)
    return EXIT_OK


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    directory = Path(args.directory)
    manifest_path = directory / report.MANIFEST_NAME
    if not directory.is_dir():
        return report._fail(f"{directory} is not a directory")
    if not manifest_path.is_file():
        return report._fail(
            f"{manifest_path} not found (was the run started with --dump-on-button?)"
        )
    try:
        return run_timeline(
            directory,
            load_manifest(manifest_path),
            gap_polls=args.gap_polls,
            trigger_polls=args.trigger_polls,
            entries_csv=args.entries_csv,
            entry_names=args.entry_names,
            write=args.write,
            as_json=args.json,
            force=args.force,
        )
    except OSError as error:
        return report._fail(f"{error.filename or directory}: {error.strerror or error}")


if __name__ == "__main__":
    sys.exit(main())
