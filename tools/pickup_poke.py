#!/usr/bin/env python3
# ruff: noqa: E501
"""T1627: find the LIVE weapon pickup records of the Map Maker preview in a guest dump and write the poke request for them.

FABRICATED-STATE tooling, results INFERRED at most. Everything below the addresses comes from static reading of the pinned
XBE (docs/t-weaponslot-poke.md, "Live pickup entities", T1627), nothing was ever run against a live game:

* pickup pool pointer dword at 0x732E60 (count at 0x732E64), records of 0xD8 bytes, +0xC kind (2 = weapon pickup),
  +0x14 the weapon ENTRY NUMBER the pickup gives when collected, +0x18 pointer to the params block of its world entry
  (world entry = +0x18 minus 0x34);
* world entry array pointer dword at 0x78CC14, entries of 0x58 bytes, word at +0x10 = the Map Maker slot index (0 based).

`locate` reads a dump made with `--dump-guest-range *0x732E60+0:0xD80,*0x78CC14+0:0x210` (the forced weapons experiment does) and
`build_request` writes `guestpoke.<label>`: one `*0x732E60+OFF=ENTRY:4` line per slot, OFF = 0x14 + 0xD8 * index. Ammo type and
amount (+0x40..+0x4C) stay those of the OLD weapon, and the drawn model is chosen at creation, so a poke changes what is GIVEN,
not what is drawn.

  python -m tools.pickup_poke locate SESSION_DIR --dump LABEL --slot SLOT:OLD:NEW [--slot ...] [--write LABEL]
"""

from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

POOL_POINTER = 0x732E60
WORLD_POINTER = 0x78CC14
RECORD_SIZE = 0xD8
KIND_OFFSET = 0x0C
ENTRY_OFFSET = 0x14
PARAMS_OFFSET = 0x18
WEAPON_PICKUP_KIND = 2
WORLD_STRIDE = 0x58
WORLD_SLOT_OFFSET = 0x10
WORLD_PARAMS_OFFSET = 0x34
SIDECAR_NAME = "pickup_plan.json"

RANGE_RE = re.compile(r"range 0x([0-9A-Fa-f]+) 0x([0-9A-Fa-f]+)")
INDIRECT_RE = re.compile(r"indirect 0x([0-9A-Fa-f]+) \+ 0x([0-9A-Fa-f]+) length 0x([0-9A-Fa-f]+)")
ROW_RE = re.compile(r"([0-9A-Fa-f]{8}):((?: [0-9a-fA-F]{2})+)")


@dataclass
class DumpRange:
    address: int
    length: int
    data: bytearray
    indirect_address: int | None = (
        None  # the pointer dword this range was reached through (`*ADDR+OFF:LEN`)
    )
    indirect_offset: int = 0

    def covers(self, address: int, size: int) -> bool:
        return self.address <= address and address + size <= self.address + len(self.data)

    def u32(self, address: int) -> int | None:
        if not self.covers(address, 4):
            return None
        return struct.unpack_from("<I", self.data, address - self.address)[0]

    def u16(self, address: int) -> int | None:
        if not self.covers(address, 2):
            return None
        return struct.unpack_from("<H", self.data, address - self.address)[0]


def parse_dump(text: str) -> list[DumpRange]:
    """The ranges of a guestdump.<label> file (src/host/guest_dump.c). Unreadable ranges come back short or empty."""
    ranges: list[DumpRange] = []
    pending: tuple[int, int] | None = None
    current: DumpRange | None = None
    for line in text.splitlines():
        indirect = INDIRECT_RE.fullmatch(line.strip())
        if indirect:
            pending = (int(indirect.group(1), 16), int(indirect.group(2), 16))
            continue
        header = RANGE_RE.fullmatch(line.strip())
        if header:
            current = DumpRange(int(header.group(1), 16), int(header.group(2), 16), bytearray())
            if pending is not None:
                current.indirect_address, current.indirect_offset = pending
                pending = None
            ranges.append(current)
            continue
        if line.startswith(("unreadable", "pointer ")):
            if line.startswith("unreadable pointer"):
                pending = None
            continue
        row = ROW_RE.fullmatch(line.strip())
        if row and current is not None:
            address = int(row.group(1), 16)
            expected = current.address + len(current.data)
            if address != expected:
                continue  # a gap: keep what was contiguous
            current.data += bytes(int(b, 16) for b in row.group(2).split())
    return ranges


def range_through(ranges: list[DumpRange], pointer_address: int) -> DumpRange | None:
    for item in ranges:
        if item.indirect_address == pointer_address:
            return item
    return None


@dataclass
class Match:
    slot: int  # 1 based
    index: int  # record index in the pool
    old: int
    new: int
    how: str  # world-entry or weapon-entry


def locate(
    ranges: list[DumpRange], slots: list[tuple[int, int, int]]
) -> tuple[list[Match], list[str]]:
    """slots = (slot 1..6, old entry, new entry). Returns the matches and the notes (problems are notes, never guesses)."""
    notes: list[str] = []
    pool = range_through(ranges, POOL_POINTER)
    if pool is None:
        return [], [
            f"the dump has no range reached through the pickup pool pointer 0x{POOL_POINTER:X} (--dump-guest-range *0x{POOL_POINTER:X}+0:0xD80)"
        ]
    world = range_through(ranges, WORLD_POINTER)
    if world is None:
        notes.append(f"no world entry range (*0x{WORLD_POINTER:X}), matching by weapon entry only")
    records = []
    for index in range(len(pool.data) // RECORD_SIZE):
        base = pool.address + index * RECORD_SIZE
        kind = pool.u32(base + KIND_OFFSET)
        entry = pool.u32(base + ENTRY_OFFSET)
        params = pool.u32(base + PARAMS_OFFSET)
        if kind != WEAPON_PICKUP_KIND or entry is None:
            continue
        slot_of_entry = None
        if world is not None and params:
            world_entry = params - WORLD_PARAMS_OFFSET
            word = world.u16(world_entry + WORLD_SLOT_OFFSET)
            if (
                word is not None
                and world_entry >= world.address
                and (world_entry - world.address) % WORLD_STRIDE == 0
            ):
                slot_of_entry = word
        records.append((index, entry, slot_of_entry))
    if not records:
        return [], notes + [
            "no weapon pickup record (kind 2) in the pool dump: the preview has no spawned pickups, or the layout differs"
        ]
    matches: list[Match] = []
    taken: set[int] = set()
    for slot, old, new in slots:
        by_world = [r for r in records if r[2] == slot - 1 and r[0] not in taken]
        by_entry = [r for r in records if r[1] == old and r[0] not in taken]
        if len(by_world) == 1:
            index, entry, _ = by_world[0]
            if entry != old:
                notes.append(
                    f"slot {slot}: record {index} holds entry 0x{entry:X}, not the expected 0x{old:X} (the world entry slot decides)"
                )
            matches.append(Match(slot, index, entry, new, "world-entry"))
            taken.add(index)
        elif not by_world and len(by_entry) == 1:
            index = by_entry[0][0]
            notes.append(
                f"slot {slot}: matched record {index} by weapon entry 0x{old:X} only (no world entry slot found)"
            )
            matches.append(Match(slot, index, old, new, "weapon-entry"))
            taken.add(index)
        else:
            notes.append(
                f"slot {slot}: {len(by_world)} record(s) by world entry slot, {len(by_entry)} by weapon entry 0x{old:X}: not unique, nothing is poked for this slot"
            )
    return matches, notes


def request_text(matches: list[Match]) -> str:
    lines = [
        "# T1627 FABRICATED-STATE: live weapon pickup records (pool pointer 0x732E60, 0xD8 bytes per record, +0x14 = weapon entry)"
    ]
    for match in sorted(matches, key=lambda m: m.slot):
        offset = ENTRY_OFFSET + RECORD_SIZE * match.index
        lines.append(
            f"# slot {match.slot} record {match.index} ({match.how}) entry 0x{match.old:X} -> 0x{match.new:X}"
        )
        lines.append(f"*0x{POOL_POINTER:X}+0x{offset:X}=0x{match.new:X}")
    return "\n".join(lines) + "\n"


def build_request(session_dir: Path, label: str) -> tuple[bool, list[str]]:
    """Read DIR/pickup_plan.json, locate the pickups in DIR/guestdump.<dump>, write DIR/guestpoke.<label>. (ok, lines to show)."""
    sidecar = session_dir / SIDECAR_NAME
    try:
        plan = json.loads(sidecar.read_text())
        slots = [(int(s["slot"]), int(s["old"]), int(s["new"])) for s in plan["slots"]]
        dump_label = str(plan["dump"])
    except (OSError, ValueError, KeyError, TypeError) as error:
        return False, [f"cannot read {SIDECAR_NAME}: {error}"]
    dump = session_dir / f"guestdump.{dump_label}"
    try:
        ranges = parse_dump(dump.read_text())
    except OSError as error:
        return False, [f"cannot read {dump.name}: {error}"]
    matches, notes = locate(ranges, slots)
    lines = list(notes)
    summary = session_dir / "pickup_locate.txt"
    body = [f"# T1627 FABRICATED-STATE pickup locate from {dump.name} (INFERRED layout)"]
    body += [
        f"slot {m.slot} record {m.index} entry 0x{m.old:X} -> 0x{m.new:X} via {m.how}"
        for m in matches
    ]
    body += [f"note: {note}" for note in notes]
    summary.write_text("\n".join(body) + "\n")
    if not matches:
        return False, [*lines, "no pickup could be located: nothing is poked"]
    (session_dir / f"guestpoke.{label}").write_text(request_text(matches))
    lines.append(
        f"located {len(matches)} of {len(slots)} slot pickup(s), request guestpoke.{label} written (summary {summary.name})"
    )
    return True, lines


def parse_slot(text: str) -> tuple[int, int, int]:
    parts = text.split(":")
    if len(parts) != 3:
        raise argparse.ArgumentTypeError("want SLOT:OLD:NEW, for example 1:0x36:0x29")
    slot, old, new = (int(part, 0) for part in parts)
    if not 1 <= slot <= 6:
        raise argparse.ArgumentTypeError("SLOT is 1..6")
    return slot, old, new


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="command", required=True)
    find = sub.add_parser(
        "locate", help="show where the slot pickups are in a dump, optionally write the request"
    )
    find.add_argument("session_dir", type=Path)
    find.add_argument("--dump", required=True, help="the dump label (guestdump.<label>)")
    find.add_argument(
        "--slot", action="append", type=parse_slot, required=True, metavar="SLOT:OLD:NEW"
    )
    find.add_argument(
        "--write", metavar="LABEL", help="write guestpoke.<LABEL> into the session folder"
    )
    args = parser.parse_args(argv)
    try:
        ranges = parse_dump((args.session_dir / f"guestdump.{args.dump}").read_text())
    except OSError as error:
        print(f"cannot read the dump: {error}")
        return 2
    matches, notes = locate(ranges, args.slot)
    for match in matches:
        print(
            f"slot {match.slot}: record {match.index} entry 0x{match.old:X} -> 0x{match.new:X} ({match.how})"
        )
    for note in notes:
        print(f"note: {note}")
    if args.write and matches:
        (args.session_dir / f"guestpoke.{args.write}").write_text(request_text(matches))
        print(f"wrote guestpoke.{args.write}")
    return 0 if matches else 1


if __name__ == "__main__":
    sys.exit(main())
