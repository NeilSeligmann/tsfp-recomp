# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1640: where the converter (`tools/route_nav.py`) gets the menu state at an activation poll from.

Three sources, tried in this order, each answers "which menu was active, where was its cursor, which item id and label were
under it" for the poll of an A press, or says why it cannot:

* `DumpSource`: the `--dump-on-button` snapshots of a REPLAY of the route (T1629, `docs/t-button-dumps.md`). The `before`
  snapshot of an A press is the guest memory just before that press reached the guest. With the ranges `suggest-dumps` prints
  (mode word, the widget table, the pointers the conditions use) the whole menu table, page widgets included, is evaluated
  against the snapshot in Python: the same selection the host's state machine does at run time (`page=builder:VA` finds the
  slot of the page, `@+OFF` reads inside it). This is the only source that works with the widget based menus of the real table
  without any new host feature.
* `NavLogSource`: `nav` lines of a `--route-event-log` written by the host recorder side (phase 2 of T1640):
  `t=<ms> poll=<N> nav menu=ID cursor=C count=K id=I ready=R` (the format is the one the orchestrator announced, NOT verified
  against a host that writes it, no such host exists yet).
* `SampleSource`: `mem` samples of `--route-log-mem` (T1633). Only menus whose conditions and cursor are plain memory references
  work, a page offset (`@+OFF`) cannot be sampled because the slot of the page moves.

Nothing here writes anything: sources are read only.
"""

from __future__ import annotations

import bisect
import json
import re
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Protocol

from tools import route_events
from tools.play import route_nav as grammar

NO_ROW = 0xFFFFFFFF  # a cursor with no row yet, and the item id of a row without one
PAGE_USED_OFFSET = 0x0  # dword non zero: the slot is in use
PAGE_BUILDER_OFFSET = 0x8  # dword: the builder VA of the page
PAGE_STATE_OFFSET = 0x1B0  # dword: the page state (3 = fully open)
MAX_ROWS = 512
MAX_LABEL = 63


# ---- result types ----


@dataclass
class MenuState:
    menu: str
    cursor: int
    count: int | None = None
    item_id: int | None = None
    label: bytes | None = None
    ready: bool | None = None
    ids_unique: bool | None = None  # no other row of the menu has this id (None = not known)
    label_unique: bool | None = None
    note: str = ""


@dataclass
class Lookup:
    state: MenuState | None
    reason: str = ""  # why there is no state
    source: str = ""


class StateSource(Protocol):
    name: str

    def lookup(self, poll: int, menus: dict[str, grammar.Menu]) -> Lookup: ...


def pick_menu(
    status: dict[str, str],
    menus: dict[str, grammar.Menu],
    ready: dict[str, bool | None] | None = None,
) -> tuple[str | None, str]:
    """(menu id, "") when exactly one menu is the answer, else (None, reason). `status` maps menu id to true/false/unknown.

    Several menus can be on screen at once (a container page with its list on top): the one with a cursor wins, then the one
    whose `ready` conditions hold."""
    true = [menu_id for menu_id, state in status.items() if state == "true"]
    if len(true) > 1:
        with_cursor = [menu_id for menu_id in true if menus[menu_id].has_cursor]
        true = with_cursor or true
    if len(true) > 1 and ready:
        not_refused = [menu_id for menu_id in true if ready.get(menu_id) is not False]
        true = not_refused or true
    if len(true) > 1:
        return None, f"ambiguous menu: {', '.join(true)} are all active"
    if true:
        return true[0], ""
    unknown = [menu_id for menu_id, state in status.items() if state == "unknown"]
    if unknown:
        return None, f"no data for the active condition of {', '.join(unknown)}"
    if not menus:
        return None, "the menu table is empty"
    return None, "activation outside any known menu (no active condition holds)"


# ---- source 1: mem samples of --route-log-mem ----


@dataclass
class MemTimeline:
    """Sampled values of `--route-log-mem` entries over the pad poll: key -> [(poll, value or None)] in log order."""

    series: dict[tuple[bool, int, int, int], list[tuple[int, int | None]]] = field(
        default_factory=dict
    )

    @classmethod
    def from_items(cls, items: list[tuple[str, object]]) -> MemTimeline:
        timeline = cls()
        for kind, payload in items:
            if kind != "mem":
                continue
            event: route_events.MemEvent = payload  # type: ignore[assignment]
            try:
                ref = grammar.parse_mem_ref(event.spec)
            except grammar.NavError:
                continue
            value = None if event.new == "unreadable" else int(event.new, 16)
            timeline.series.setdefault(ref.key, []).append((event.poll or 0, value))
        for samples in timeline.series.values():
            samples.sort(
                key=lambda sample: sample[0]
            )  # a log is in time order, stay safe for a merged one
        return timeline

    def sample_at(self, ref: grammar.MemRef, poll: int) -> tuple[bool, int | None]:
        """(known, value): the last sample logged at a poll <= `poll`. known False = no sample that early (or none at all)."""
        samples = self.series.get(ref.key)
        if not samples:
            return False, None
        index = bisect.bisect_right([sample[0] for sample in samples], poll)
        if index == 0:
            return False, None
        return True, samples[index - 1][1]

    def has(self, ref: grammar.MemRef) -> bool:
        return ref.key in self.series


def sample_status(menu: grammar.Menu, timeline: MemTimeline, poll: int) -> str:
    """true / false / unknown for the active conditions of a menu at a poll."""
    unknown = False
    for cond in menu.active:
        known, value = timeline.sample_at(grammar.cond_ref(cond), poll)
        if not known:
            unknown = True
        elif value is None or not grammar.eval_cond(cond, value):
            return "false"
    return "unknown" if unknown else "true"


class SampleSource:
    name = "mem samples (--route-log-mem)"

    def __init__(self, timeline: MemTimeline) -> None:
        self.timeline = timeline

    def lookup(self, poll: int, menus: dict[str, grammar.Menu]) -> Lookup:
        status: dict[str, str] = {}
        for menu_id, menu in menus.items():
            if menu.uses_page():
                status[menu_id] = "unsupported"  # a page offset cannot be sampled
            else:
                status[menu_id] = sample_status(menu, self.timeline, poll)
        picked, reason = pick_menu(status, menus)
        if picked is None:
            unsupported = [menu_id for menu_id, state in status.items() if state == "unsupported"]
            if unsupported and "outside any known menu" in reason:
                reason += f" among the menus a sample can tell ({len(unsupported)} widget based menus need nav lines or button dumps)"
            if "no data" in reason:
                reason = f"no mem samples before poll {poll} for the active condition of {reason.split(' of ')[-1]} (was --route-log-mem on for them? python -m tools.route_nav suggest-memlog)"
            elif reason.startswith(("ambiguous", "activation outside")):
                reason += f" at poll {poll}"
            return Lookup(None, reason, self.name)
        menu = menus[picked]
        if menu.cursor is None:
            return Lookup(
                None, f"menu {picked} has no cursor= (a screen that is only waited for)", self.name
            )
        known, cursor = self.timeline.sample_at(menu.cursor, poll)
        if not known or cursor is None:
            return Lookup(
                None,
                f"menu {picked} is active but its cursor {menu.cursor.spec()} has no readable sample before poll {poll}",
                self.name,
            )
        item_id = None
        if menu.itemid is not None and menu.itemid.samplable:
            id_known, raw = self.timeline.sample_at(menu.itemid, poll)
            item_id = menu.itemid.read(raw) if id_known and raw is not None else None
        return Lookup(MenuState(picked, menu.cursor.read(cursor), item_id=item_id), "", self.name)


# ---- source 2: nav lines of the event log ----


@dataclass
class NavSample:
    poll: int
    menu: str | None
    cursor: int | None
    count: int | None
    item_id: int | None
    ready: bool | None


def _number(text: str | None) -> int | None:
    if text is None or text in ("-", "?", "none", "unreadable"):
        return None
    return int(text, 0) if re.fullmatch(r"-?(?:0[xX][0-9a-fA-F]+|[0-9]+)", text) else None


@dataclass
class HostNavLine:
    """`nav-line at=P to=Q edge=E menu=ID select=id:I activate`: a nav line the host recorder added to the record."""

    poll: int  # the poll the line was logged at
    edge: int  # the select edge poll
    step: grammar.NavStep


@dataclass
class HostNavSkip:
    """`nav-skip menu=ID reason=R [row=C flags=0xF] [at=P to=Q] ...`: a select press the host recorder did NOT turn into a line."""

    poll: int
    menu: str | None
    reason: str
    fields: dict[str, str]


@dataclass
class NavEvents:
    samples: list[NavSample] = field(default_factory=list)
    lines: list[HostNavLine] = field(default_factory=list)
    skips: list[HostNavSkip] = field(default_factory=list)
    bad_lines: list[str] = field(default_factory=list)  # nav-line lines the grammar refuses


_EVENT_PREFIX = re.compile(r"t=(\d+) poll=(\d+) (nav-line|nav-skip|nav) ?(.*)")


def read_nav_events(path: Path) -> NavEvents:
    """Every host recorder line of an event log: `nav ...` states, `nav-line ...` additions, `nav-skip ...` refusals."""
    events = NavEvents()
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = _EVENT_PREFIX.fullmatch(raw.strip())
        if not match:
            continue
        poll, kind, rest = int(match.group(2)), match.group(3), match.group(4)
        fields = dict(part.partition("=")[::2] for part in rest.split() if "=" in part)
        if kind == "nav":
            events.samples.append(nav_sample(poll, fields))
        elif kind == "nav-skip":
            menu = fields.get("menu")
            events.skips.append(
                HostNavSkip(
                    poll,
                    None if menu in (None, "-", "none") else menu,
                    fields.get("reason", "?"),
                    fields,
                )
            )
        else:
            text = f"at={fields.get('at')} to={fields.get('to')} menu={fields.get('menu')} select={fields.get('select')}"
            if "activate" in rest.split():
                text += " activate"
            try:
                step = grammar.parse_nav_text(text)
                edge = _number(fields.get("edge"))
            except grammar.NavError as error:
                events.bad_lines.append(f"poll {poll}: {raw.strip()}: {error}")
                continue
            events.lines.append(HostNavLine(poll, step.at if edge is None else edge, step))
    events.samples.sort(key=lambda sample: sample.poll)
    return events


def nav_sample(poll: int, fields: dict[str, str]) -> NavSample:
    menu = fields.get("menu")
    ready = _number(fields.get("ready"))
    return NavSample(
        poll,
        None if menu in (None, "-", "none") else menu,
        _number(fields.get("cursor")),
        _number(fields.get("count")),
        _number(fields.get("id")),
        None if ready is None else bool(ready),
    )


def read_nav_log(path: Path) -> list[NavSample]:
    """The `nav menu=ID cursor=C count=K id=I ready=R` state lines (`?` unreadable, `-` no such field, `nav menu=none`)."""
    return read_nav_events(path).samples


class NavLogSource:
    name = "nav lines (host recorder log)"

    def __init__(self, samples: list[NavSample]) -> None:
        self.samples = samples
        self.polls = [sample.poll for sample in samples]
        # evidence about repeated ids: per menu the item ids seen under each cursor value
        self.ids: dict[str, dict[int, set[int]]] = {}
        for sample in samples:
            if sample.menu and sample.cursor is not None and sample.item_id is not None:
                self.ids.setdefault(sample.menu, {}).setdefault(sample.cursor, set()).add(
                    sample.item_id
                )

    def ids_unique(self, menu: str, cursor: int, item_id: int) -> bool | None:
        seen = self.ids.get(menu, {})
        others = [rows for key, rows in seen.items() if key != cursor and item_id in rows]
        if others:
            return False
        return None if len(seen) < 2 else True

    def lookup(self, poll: int, menus: dict[str, grammar.Menu]) -> Lookup:
        index = bisect.bisect_right(self.polls, poll)
        if index == 0:
            return Lookup(None, f"no nav line at or before poll {poll}", self.name)
        sample = self.samples[index - 1]
        if sample.menu is None:
            return Lookup(
                None, f"the last nav line (poll {sample.poll}) says no menu is active", self.name
            )
        if sample.menu not in menus:
            return Lookup(
                None,
                f"the nav line at poll {sample.poll} names menu {sample.menu}, which the table does not have",
                self.name,
            )
        if sample.cursor is None:
            return Lookup(
                None,
                f"menu {sample.menu} has no readable cursor at poll {poll} (nav line poll {sample.poll}: '?' or '-')",
                self.name,
            )
        if sample.cursor == NO_ROW:
            return Lookup(
                None,
                f"menu {sample.menu} has no cursor row yet at poll {poll} (nav line poll {sample.poll})",
                self.name,
            )
        item_id = sample.item_id
        return Lookup(
            MenuState(
                sample.menu,
                sample.cursor,
                sample.count,
                item_id,
                None,
                sample.ready,
                None if item_id is None else self.ids_unique(sample.menu, sample.cursor, item_id),
            ),
            "",
            self.name,
        )


# ---- source 3: --dump-on-button snapshots ----


class Snapshot:
    """The guest memory of one dump file (`# guest-dump 1`): `range 0xADDR 0xLEN` blocks of `ADDR: hex bytes` rows.

    A pointer range (`indirect 0xPTR + 0xOFF length 0xLEN`, `pointer P final Q`) is followed by an ordinary `range` of the
    pointee at its real address, so a plain address map covers both."""

    def __init__(self) -> None:
        self.segments: list[tuple[int, bytes]] = []

    @classmethod
    def from_text(cls, text: str) -> Snapshot:
        snapshot = cls()
        base: int | None = None
        length = 0
        data = bytearray()

        def close() -> None:
            nonlocal base
            if base is not None and data:
                snapshot.segments.append((base, bytes(data[:length])))
            base = None
            data.clear()

        for line in text.splitlines():
            if line.startswith("range "):
                close()
                parts = line.split()
                base, length = int(parts[1], 16), int(parts[2], 16)
            elif base is not None:
                match = re.fullmatch(r"([0-9A-Fa-f]{8}): ((?:[0-9a-fA-F]{2} ?)+)", line.strip())
                if match:
                    offset = int(match.group(1), 16) - base
                    chunk = bytes.fromhex(match.group(2).replace(" ", ""))
                    if offset == len(data):
                        data.extend(chunk)
        close()
        snapshot.segments.sort()
        return snapshot

    @classmethod
    def load(cls, path: Path) -> Snapshot:
        return cls.from_text(path.read_text(encoding="utf-8", errors="replace"))

    def read_bytes(self, address: int, length: int) -> bytes | None:
        """The bytes at `address` from ANY segment that holds the whole range (the smallest one when several do: a small pointer
        range can lie inside the widget table range, and must not hide it)."""
        best: tuple[int, bytes, int] | None = None
        for start, data in self.segments:
            if start > address:
                break  # sorted by start
            if address + length <= start + len(data) and (best is None or len(data) < len(best[1])):
                best = (start, data, len(data))
        if best is None:
            return None
        start, data, _ = best
        return data[address - start : address - start + length]

    def read(self, address: int, width: int = 4) -> int | None:
        raw = self.read_bytes(address, width)
        if raw is None:
            return None
        return struct.unpack({1: "<B", 2: "<H", 4: "<I"}[width], raw)[0]

    def read_string(self, address: int) -> bytes | None:
        """A NUL terminated 8 bit string of at most 63 bytes, None when the memory is not in the snapshot."""
        out = bytearray()
        for offset in range(MAX_LABEL + 1):
            byte = self.read_bytes(address + offset, 1)
            if byte is None:
                return None
            if byte == b"\0":
                return bytes(out) or None
            out += byte
        return bytes(out)


def locate_page(snapshot: Snapshot, widgets: grammar.Widgets, builder: int) -> int | None:
    """The base address of the page widget of `builder`: among the used slots (dword +0 non zero) whose builder dword (+8)
    equals the VA and whose state dword (+0x1B0) is not in `closing`, the highest state, then the highest slot."""
    best: tuple[int, int] | None = None
    for slot in range(widgets.count):
        base = widgets.table + slot * widgets.stride
        used = snapshot.read(base + PAGE_USED_OFFSET)
        found = snapshot.read(base + PAGE_BUILDER_OFFSET)
        state = snapshot.read(base + PAGE_STATE_OFFSET)
        if not used or found != builder or state is None or state in widgets.closing:
            continue
        if best is None or (state, slot) > best:
            best = (state, slot)
    return None if best is None else widgets.table + best[1] * widgets.stride


def read_ref(snapshot: Snapshot, ref: grammar.MemRef, page_base: int | None) -> int | None:
    """The masked value of a reference in a snapshot, None when its memory is not there."""
    if ref.page:
        if page_base is None:
            return None
        raw = snapshot.read(page_base + ref.address, ref.width)
    elif ref.indirect:
        pointer = snapshot.read(ref.address, 4)
        raw = snapshot.read(pointer + ref.offset, ref.width) if pointer else None
    else:
        raw = snapshot.read(ref.address, ref.width)
    return None if raw is None else ref.read(raw)


@dataclass
class Row:
    node: int
    item_id: int | None
    label: bytes | None
    skipped: bool = (
        False  # flags & skip=MASK: the cursor never rests on it, a name never matches it
    )


def walk_rows(snapshot: Snapshot, menu: grammar.Menu, page_base: int | None) -> list[Row] | None:
    """The cursor rows of a menu (`items=walk`): the k-th node whose type is one of the table's types is row k. None when the
    snapshot does not hold the nodes."""
    items = menu.items
    if items is None:
        return None
    node = read_ref(snapshot, items.head, page_base)
    rows: list[Row] = []
    visited = 0
    while node and visited < MAX_ROWS:
        visited += 1
        node_type = snapshot.read(node + items.type_off)
        following = snapshot.read(node + items.next_off)
        if node_type is None or following is None:
            return None
        if node_type in items.types:
            item_id = snapshot.read(node + items.id_off) if items.id_off is not None else None
            label = None
            if items.text_off is not None:
                text_pointer = snapshot.read(node + items.text_off)
                label = snapshot.read_string(text_pointer) if text_pointer else None
            skipped = False
            if items.flags_off is not None and items.skip_mask:
                flags = snapshot.read(node + items.flags_off)
                skipped = flags is not None and bool(flags & items.skip_mask)
            rows.append(Row(node, item_id, label, skipped))
        node = following
    return rows


def evaluate_snapshot(
    snapshot: Snapshot, menu: grammar.Menu, widgets: grammar.Widgets | None
) -> tuple[str, int | None, bool | None]:
    """(status true/false/unknown, page base, ready) of a menu in a snapshot."""
    page_base = None
    if menu.page_builder is not None:
        if widgets is None:
            return "unknown", None, None
        page_base = locate_page(snapshot, widgets, menu.page_builder)
        if page_base is None:
            return "false", None, None  # the page is not on screen
    status = "true"
    for cond in menu.active:
        value = read_ref(snapshot, cond.ref, page_base)
        if value is None:
            status = "unknown" if status == "true" else status
        elif not grammar.eval_cond(cond, value):
            return "false", page_base, None
    ready: bool | None = True
    for cond in menu.ready:
        value = read_ref(snapshot, cond.ref, page_base)
        if value is None:
            ready = None if ready else ready
        elif not grammar.eval_cond(cond, value):
            ready = False
    return status, page_base, ready


def state_from_snapshot(
    snapshot: Snapshot, menus: grammar.MenuTable, poll: int, source: str
) -> Lookup:
    status: dict[str, str] = {}
    bases: dict[str, int | None] = {}
    ready: dict[str, bool | None] = {}
    for menu_id, menu in menus.items():
        status[menu_id], bases[menu_id], ready[menu_id] = evaluate_snapshot(
            snapshot, menu, menus.widgets
        )
    picked, reason = pick_menu(status, menus, ready)
    if picked is None:
        return Lookup(None, f"{reason} at poll {poll}", source)
    menu = menus[picked]
    if menu.cursor is None:
        return Lookup(
            None, f"menu {picked} has no cursor= (a screen that is only waited for)", source
        )
    cursor = read_ref(snapshot, menu.cursor, bases[picked])
    if cursor is None:
        return Lookup(
            None, f"menu {picked} is active but its cursor is not in the snapshot", source
        )
    if cursor == NO_ROW:
        return Lookup(None, f"menu {picked} has no cursor row yet (0xFFFFFFFF)", source)
    count = read_ref(snapshot, menu.count, bases[picked]) if menu.count is not None else None
    item_id = read_ref(snapshot, menu.itemid, bases[picked]) if menu.itemid is not None else None
    label = None
    ids_unique = label_unique = None
    rows = walk_rows(snapshot, menu, bases[picked])
    if rows is not None and cursor < len(rows):
        row = rows[cursor]
        if item_id is None:
            item_id = row.item_id
        label = row.label
        if row.item_id is not None:
            ids_unique = (
                row.item_id != NO_ROW
                and sum(1 for other in rows if other.item_id == row.item_id) == 1
            )
        if row.label is not None:
            # skipped rows (titles, hints) do not take part in name matching, only the rows the cursor can rest on
            label_unique = (
                sum(1 for other in rows if other.label == row.label and not other.skipped) == 1
            )
    return Lookup(
        MenuState(picked, cursor, count, item_id, label, ready[picked], ids_unique, label_unique),
        "",
        source,
    )


@dataclass
class DumpEvent:
    poll: int  # the poll of the A press edge
    before: Path


def read_dump_manifest(directory: Path, button: str = "A") -> list[DumpEvent]:
    """The `before` dumps of the presses of `button` in `DIR/buttons.jsonl` (tolerates a truncated last line)."""
    events: list[DumpEvent] = []
    manifest = directory / "buttons.jsonl"
    for raw in manifest.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            record = json.loads(raw)
        except json.JSONDecodeError:
            continue
        if record.get("type") != "event":
            continue
        edges = [
            edge
            for edge in record.get("edges", [])
            if edge.get("button") == button and edge.get("edge") == "press"
        ]
        before = [
            dump
            for dump in record.get("dumps", [])
            if dump.get("kind") == "before" and dump.get("ok", True)
        ]
        if edges and before:
            events.append(DumpEvent(int(edges[0]["poll"]), directory / before[0]["file"]))
    return events


DEFAULT_STALL_SLACK = (
    100_000  # the most polls the host may have stalled at marks in total (auto alignment)
)


class DumpSource:
    """The `before` snapshot of each A press of a replay. The dump manifest names HOST polls (`xinput_source_port_poll_count`),
    the route names RECORD polls. They are equal until the first mark that stalls (a `min=` / event wait holds the replay with the
    pad at rest, the host poll runs on, the record position does not): from then on host poll = record poll + the polls stalled
    at the marks before it. `prepare` maps the A edges of the record to dump polls:

    * `host_offset` / `stalls` ({mark ordinal: stalled polls}) given: exact, host = record + offset + the stalls of the marks at or
      before that record poll, the dump must be at exactly that poll;
    * otherwise AUTO: the offset is constant between two marks (a stall happens only AT a mark), so per mark segment the offset is
      the one (>= the previous segment's, at most `slack` more) that lines up the most A edges with dumped presses, ties go to the
      smallest. A segment whose only dump is missing is mapped to the next press's dump: that cannot be detected, pass `stalls`."""

    name = "button dumps (--dump-on-button)"

    def __init__(
        self,
        events: list[DumpEvent],
        host_offset: int | None = None,
        stalls: dict[int, int] | None = None,
        slack: int = DEFAULT_STALL_SLACK,
    ) -> None:
        self.by_poll = {event.poll: event for event in events}
        self.host_offset = host_offset
        self.stalls = stalls
        self.slack = slack
        self.mapping: dict[int, int] | None = None

    @property
    def exact(self) -> bool:
        return self.host_offset is not None or self.stalls is not None

    def prepare(self, edges: list[int], marks: list[int]) -> None:
        """Map the record polls of the A edges to dump polls (see the class text)."""
        self.mapping = {}
        if self.exact:
            for edge in edges:
                shift = (self.host_offset or 0) + sum(
                    polls
                    for ordinal, polls in (self.stalls or {}).items()
                    if 0 < ordinal <= len(marks) and marks[ordinal - 1] <= edge
                )
                self.mapping[edge] = edge + shift
            return
        bounds = [0, *marks, max(edges, default=0) + 1]
        offset = 0
        dumped = sorted(self.by_poll)
        for low, high in zip(bounds, bounds[1:], strict=False):
            segment = [edge for edge in edges if low <= edge < high] if high > low else []
            if not segment:
                continue
            candidates = sorted(
                {
                    poll - edge
                    for edge in segment
                    for poll in dumped
                    if offset <= poll - edge <= offset + self.slack
                }
            )
            if candidates:
                best = max(
                    candidates,
                    key=lambda shift: (
                        sum(edge + shift in self.by_poll for edge in segment),
                        -shift,
                    ),
                )
                offset = best
            for edge in segment:
                self.mapping[edge] = edge + offset

    def lookup(self, poll: int, menus: dict[str, grammar.Menu]) -> Lookup:
        host = poll if self.mapping is None else self.mapping.get(poll, poll)
        event = self.by_poll.get(host)
        shown = (
            f"poll {poll}"
            if host == poll
            else f"poll {poll} (host poll {host} = record poll + {host - poll})"
        )
        if event is None:
            return Lookup(
                None,
                f"no button dump for the press at {shown} (the dump run must replay this route from poll 0, dumped presses: {len(self.by_poll)}; "
                "the dump polls are HOST polls, mark stalls shift them: --stall-polls markK=N,... or --host-poll-offset N)",
                self.name,
            )
        if not event.before.is_file():
            return Lookup(None, f"dump file {event.before.name} is missing", self.name)
        assert isinstance(menus, grammar.MenuTable)
        found = state_from_snapshot(Snapshot.load(event.before), menus, poll, self.name)
        if host != poll:
            found.source = f"{self.name}, host poll {host} = record poll {poll} + {host - poll}"
        return found
