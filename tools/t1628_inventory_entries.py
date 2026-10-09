# SPDX-License-Identifier: GPL-3.0-or-later
"""T1628: weapon entry table (0x4FA128, stride 0x3C) as inventory rules, and the guarded poke plan.

    python -m tools.t1628_inventory_entries --xbe tmp/build-t565/default.xbe \
        --out docs/data/t-player-inventory-entries.csv
    python -m tools.t1628_inventory_entries --xbe tmp/build-t565/default.xbe \
        --plan 0x04,0x36 --mode on [--ammo default|max|keep|N] [--caps] [--slot N]

Read-only analysis of the pinned XBE (byte reads, nothing is run). Every rule is INFERRED from the
disassembly of 0x1BFAA0 (grant), 0x1BA410 (selectable predicate), 0x1B6B40 (group numbering) and
0x1BA130 (select), see docs/t-player-inventory.md. The CSV carries structural fields only (flags,
record and ammo type numbers, group numbers). `--plan` prints the guarded poke lines
(`*ADDR+OFF=VALUE:W`, the grammar of src/host/guest_poke.c) for tmp/inventory_poke.fish;
the lines contain game numbers (ammo amounts) so they are only ever printed, never stored in docs.
Every poke is FABRICATED-STATE and UNPROVEN until a live dump shows the ownership bytes flip.
"""

from __future__ import annotations

import argparse
import csv
import struct
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from tools.xbe.parser import Xbe, parse_xbe

TABLE = 0x4FA128
STRIDE = 0x3C
ENTRIES = 0x46
ORDER_TABLE = 0x4FC888
RECORDS = 0x4EEF74
RECORD_STRIDE = 0x29C
RECORD_COUNT = 0x42
AMMO_TABLE = 0x4EA3F8
AMMO_STRIDE = 0xE0
AMMO_TYPES = 44
FLAG_X2 = 0x04
FLAG_LEFT = 0x08
FLAG_NOT_CYCLE = 0x10
FLAG_HEAD = 0x80
FLAG_HIDDEN_BY_X2 = 0x100
FLAG_NAMES = (
    (0x1, "f01"),
    (0x2, "never_selectable"),
    (FLAG_X2, "x2"),
    (FLAG_LEFT, "left_hand"),
    (FLAG_NOT_CYCLE, "not_in_cycle"),
    (0x20, "f20"),
    (0x40, "f40"),
    (FLAG_HEAD, "chain_head"),
    (FLAG_HIDDEN_BY_X2, "hidden_when_x2_owned"),
    (0x200, "f200"),
)

# Layout of the poked memory (INFERRED, see docs/t-player-inventory.md)
EXT_POINTER = 0x7356D8  # global: pointer to the player extension blocks, stride 0xEA0
EXT_STRIDE = 0xEA0
OWNED_OFFSET = 0x54C  # ext + 0x54C + entry, one byte per entry
AMMO_OFFSET = 0x61E  # ext + 0x61E + 2 * ammo type, reserve word
CAP_A_OFFSET = 0x134  # ext + 0x134 + 2 * ammo type, cap words written by 0x1BFAA0
CAP_B_OFFSET = 0x264
FLAGS_OFFSET = (
    0x28  # ext + 0x28: bit 8 is set by every pickup (0x1BFCA0), only 0 and 8 were ever seen
)
THROWABLE_OFFSET = 0xA0  # ext + 0xA0: default throwable entry (LT throws it, D-pad down cycles it)
RECORD_POINTER = 0x7B0C48  # global: pointer to the player records, stride 0x1584
RECORD_STRIDE_PLAYER = 0x1584
ORDER_OFFSET = 0xE3C  # record + 0xE3C + group, one byte: the entry shown for the group
MAX_POKE_LINES = 16  # host limit per request


@dataclass(frozen=True)
class Entry:
    entry: int
    group: int
    role: str
    flags: int
    class_word: int
    priority: int
    left_record: int
    record: int
    left_hand_byte: int
    ammo_types: tuple[int, int]
    ammo_defaults: tuple[int, int]
    clips: tuple[int, int]
    order_index: int
    members: tuple[int, ...]


@dataclass(frozen=True)
class Table:
    entries: tuple[Entry, ...]
    ammo_max: tuple[int, ...]

    def x2_members(self, entry: Entry) -> tuple[int, ...]:
        return tuple(m for m in entry.members if self.entries[m].role == "x2")

    def owned_on_grant(self, entry: Entry) -> tuple[int, ...]:
        """0x1BFAA0: group members without flag 4, plus the x2 members only when granting an x2."""
        if entry.role == "x2":
            return entry.members
        return tuple(m for m in entry.members if self.entries[m].role != "x2")

    def hidden_when_owned(self, entry: Entry) -> tuple[int, ...]:
        """0x1BA410: an entry with flag 0x100 is not selectable while a group x2 entry is owned."""
        return self.x2_members(entry) if entry.flags & FLAG_HIDDEN_BY_X2 else ()


def read_u32(data: bytes, xbe: Xbe, va: int) -> int:
    offset = xbe.va_to_offset(va)
    if offset is None:
        raise SystemExit(f"address 0x{va:X} is not in the image")
    return struct.unpack_from("<I", data, offset)[0]


def signed(value: int) -> int:
    return value - (1 << 32) if value >= 1 << 31 else value


def decode(data: bytes) -> Table:
    xbe = parse_xbe(data)
    raw = [
        [read_u32(data, xbe, TABLE + entry * STRIDE + 4 * index) for index in range(15)]
        for entry in range(ENTRIES)
    ]
    order = [read_u32(data, xbe, ORDER_TABLE + 4 * index) for index in range(ENTRIES)]
    # 0x1B6B40: group = running counter, incremented once per entry whose flags & 0xC == 0
    groups: list[int] = []
    counter = -1
    for words in raw:
        if words[0] & (FLAG_X2 | FLAG_LEFT) == 0:
            counter += 1
        groups.append(counter)
    entries = []
    for entry, words in enumerate(raw):
        flags = words[0]
        record = signed(words[4])
        types, defaults, clips = (0, 0), (0, 0), (0, 0)
        if 0 <= record < RECORD_COUNT:
            base = RECORDS + record * RECORD_STRIDE
            types = (read_u32(data, xbe, base + 0x14), read_u32(data, xbe, base + 0x1C))
            clips = (
                max(signed(read_u32(data, xbe, base + 0x18)), 0),
                max(signed(read_u32(data, xbe, base + 0x20)), 0),
            )
            word = read_u32(data, xbe, base + 0x28)
            defaults = (word & 0xFFFF, (word >> 16) & 0xFFFF)
        role = "x2" if flags & FLAG_X2 else "left_hand" if flags & FLAG_LEFT else "primary"
        entries.append(
            Entry(
                entry=entry,
                group=groups[entry],
                role=role,
                flags=flags,
                class_word=words[1],
                priority=words[2],
                left_record=signed(words[3]),
                record=record,
                left_hand_byte=words[5] & 0xFF,
                ammo_types=types,
                ammo_defaults=defaults,
                clips=clips,
                order_index=order.index(entry) if entry in order else -1,
                members=tuple(e for e in range(ENTRIES) if groups[e] == groups[entry]),
            )
        )
    maxima = tuple(read_u32(data, xbe, AMMO_TABLE + t * AMMO_STRIDE) for t in range(AMMO_TYPES))
    return Table(tuple(entries), maxima)


def hex_list(values: Sequence[int]) -> str:
    return " ".join(f"0x{v:02X}" for v in values) or "-"


def csv_rows(table: Table) -> list[dict[str, object]]:
    rows = []
    for e in table.entries:
        rows.append(
            {
                "entry": f"0x{e.entry:02X}",
                "group": e.group,
                "role": e.role,
                "flags": f"0x{e.flags:X}",
                "flag_names": "|".join(n for bit, n in FLAG_NAMES if e.flags & bit) or "-",
                "class_0x04": e.class_word,
                "priority_0x08": e.priority,
                "left_record_0x0C": e.left_record,
                "record_0x10": e.record,
                "left_hand_byte_0x14": e.left_hand_byte,
                "ammo_type_1": e.ammo_types[0] if e.record >= 0 else "",
                "ammo_type_2": e.ammo_types[1] if e.record >= 0 else "",
                "cycle_order_index": e.order_index,
                "owned_on_grant": hex_list(table.owned_on_grant(e)),
                "hidden_when_owned": hex_list(table.hidden_when_owned(e)),
            }
        )
    return rows


@dataclass(frozen=True)
class PlanOptions:
    mode: str = "on"
    ammo: str = "default"
    caps: bool = True
    order: bool = True
    no_x2: bool = False
    only_entry: bool = False
    zero_ammo: bool = False
    slot: int = 0
    loaded: int = 0
    throwable: bool = True
    flags: bool = False


def parse_entry_list(table: Table, text: str, no_x2: bool = False) -> list[int]:
    """`all` = entries 0x02..0x3B without the Uplink 0x38/0x39 and the left-hand companions."""
    if text == "all":
        return [
            e.entry
            for e in table.entries
            if 2 <= e.entry <= 0x3B
            and e.entry not in (0x38, 0x39)
            and e.role != "left_hand"
            and not (no_x2 and e.role == "x2")
        ]
    wanted = []
    for item in text.split(","):
        value = int(item, 0)
        if not 1 <= value <= 0x3B:
            raise ValueError(f"entry {item} is outside 0x01..0x3B (0x3C and up are not weapons)")
        wanted.append(value)
    return wanted


def ammo_amount(mode: str, default: int, maximum: int, loaded: int = 0) -> int:
    """Reserve amount: the game keeps reserve + loaded rounds at or below the type maximum.

    MEASURED (T1642, owner session button-dumps-20261008-202004): both a weapon pickup and an
    ammo pickup stopped exactly at reserve + loaded == maximum, so `0x1BA220` clamps to
    `maximum - loaded`. `loaded` is the number of rounds of this ammo type in the held weapon.
    """
    amount = {"default": default, "max": maximum}.get(mode)
    if amount is None:
        amount = int(mode, 0)
    return min(amount, max(maximum - loaded, 0)) if maximum > 0 else amount


def poke(offset: int, value: int, width: int, base_pointer: int) -> str:
    return f"*0x{base_pointer:X}+0x{offset:X}={value}:{width}"


def build_plan(table: Table, wanted: Sequence[int], options: PlanOptions) -> list[str]:
    ext = options.slot * EXT_STRIDE
    record = options.slot * RECORD_STRIDE_PLAYER
    lines: list[str] = []
    ammo: dict[int, int] = {}
    default_throwable_set = False

    def add(line: str) -> None:
        if line not in lines:
            lines.append(line)

    def note_ammo(kind: int, value: int) -> None:
        ammo[kind] = max(ammo.get(kind, 0), value)

    for number in wanted:
        e = table.entries[number]
        if options.mode == "on":
            for member in table.owned_on_grant(e):
                add(poke(ext + OWNED_OFFSET + member, 1, 1, EXT_POINTER))
            if options.throwable and e.flags & FLAG_NOT_CYCLE and not default_throwable_set:
                # 0x1BFAA0: a grenade like entry (flag 0x10) becomes the default throwable when none
                # is set (MEASURED T1642: ext+0xA0 = 0x28 after the first grenade pickup), and the
                # LT throw uses it, so a granted grenade needs it
                add(poke(ext + THROWABLE_OFFSET, number, 4, EXT_POINTER))
                default_throwable_set = True
            if options.flags:
                add(poke(ext + FLAGS_OFFSET, 8, 4, EXT_POINTER))
            if options.ammo != "keep":
                for slot_index in (0, 1):
                    kind = e.ammo_types[slot_index]
                    if kind <= 0 or kind >= AMMO_TYPES:
                        continue
                    if slot_index == 1 and kind == e.ammo_types[0]:
                        continue
                    maximum = table.ammo_max[kind]
                    note_ammo(
                        kind,
                        ammo_amount(
                            options.ammo, e.ammo_defaults[slot_index], maximum, options.loaded
                        ),
                    )
            if options.caps and e.ammo_types[0] > 0 and e.clips[0] > 0:
                # 0x1BFAA0 writes the main hand cap word always and the left hand cap word only
                # when the entry has a left hand record (MEASURED: the x1 pistol wrote only B)
                add(poke(ext + CAP_B_OFFSET + 2 * e.ammo_types[0], e.clips[0], 2, EXT_POINTER))
                if e.left_record >= 0 and e.left_record == e.record:
                    add(poke(ext + CAP_A_OFFSET + 2 * e.ammo_types[0], e.clips[0], 2, EXT_POINTER))
            if options.order and e.role == "x2":
                x1 = [m for m in e.members if table.entries[m].flags & FLAG_HIDDEN_BY_X2]
                if x1:
                    # 0x1BFAA0 replaces the order byte when the x2 entry hides the x1 entry
                    add(poke(record + ORDER_OFFSET + e.group, e.entry, 1, RECORD_POINTER))
        else:
            if options.only_entry:
                cleared = [number]
            elif e.role == "x2":
                cleared = list(table.x2_members(e))
            else:
                cleared = list(e.members)
            for member in cleared:
                add(poke(ext + OWNED_OFFSET + member, 0, 1, EXT_POINTER))
            if options.zero_ammo:
                for kind in e.ammo_types:
                    if 0 < kind < AMMO_TYPES:
                        note_ammo(kind, 0)
            if options.order and e.role == "x2":
                x1 = [m for m in e.members if table.entries[m].flags & FLAG_HIDDEN_BY_X2]
                if x1:
                    add(poke(record + ORDER_OFFSET + e.group, x1[0], 1, RECORD_POINTER))
    for kind in sorted(ammo):
        add(poke(ext + AMMO_OFFSET + 2 * kind, ammo[kind], 2, EXT_POINTER))
    return lines


def batches(lines: Sequence[str], size: int = MAX_POKE_LINES - 1) -> list[list[str]]:
    return [list(lines[i : i + size]) for i in range(0, len(lines), size)]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--xbe", type=Path, required=True, help="pinned default.xbe (owner file)")
    parser.add_argument("--out", type=Path, default=None, help="CSV to write (default: stdout)")
    parser.add_argument(
        "--plan", default=None, help="entry list (0x04,0x36) or all: print poke lines"
    )
    parser.add_argument("--mode", choices=("on", "off"), default="on")
    parser.add_argument("--ammo", default="default", help="default, max, keep or a number")
    parser.add_argument("--caps", action="store_true", help="poke the clip cap words (default on)")
    parser.add_argument("--no-caps", action="store_true", help="do not poke the clip cap words")
    parser.add_argument(
        "--no-throwable", action="store_true", help="do not set the default throwable ext+0xA0"
    )
    parser.add_argument(
        "--flag28", action="store_true", help="also poke ext+0x28 = 8 like a pickup"
    )
    parser.add_argument(
        "--loaded", type=int, default=0, help="rounds of the ammo type loaded in the held weapon"
    )
    parser.add_argument("--no-order", action="store_true", help="leave the group order byte")
    parser.add_argument("--no-x2", action="store_true", help="with all: x1 entries only")
    parser.add_argument("--only-entry", action="store_true", help="off: clear only the named byte")
    parser.add_argument("--zero-ammo", action="store_true", help="off: zero the reserve ammo")
    parser.add_argument("--slot", type=int, default=0, help="player slot (default 0)")
    args = parser.parse_args(argv)
    table = decode(args.xbe.read_bytes())
    if args.plan is not None:
        options = PlanOptions(
            mode=args.mode,
            ammo=args.ammo,
            caps=not args.no_caps,
            order=not args.no_order,
            no_x2=args.no_x2,
            only_entry=args.only_entry,
            zero_ammo=args.zero_ammo,
            slot=args.slot,
            loaded=args.loaded,
            throwable=not args.no_throwable,
            flags=args.flag28,
        )
        try:
            wanted = parse_entry_list(table, args.plan, args.no_x2)
            lines = build_plan(table, wanted, options)
        except ValueError as error:
            print(f"error: {error}", file=sys.stderr)
            return 2
        print(f"# {len(lines)} pokes for entries {hex_list(wanted)}", file=sys.stderr)
        for line in lines:
            print(line)
        return 0
    rows = csv_rows(table)
    stream = args.out.open("w", newline="", encoding="utf-8") if args.out else sys.stdout
    try:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    finally:
        if args.out:
            stream.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
