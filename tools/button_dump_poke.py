# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""T1637: the spawner pokes of tmp/run_button_dumps.fish, testable outside fish.

The Map Maker preview spawns weapon N from spawner N, and spawner N reads slot N of the editor weapon set list at
`[0x7844A8] + 0x1190C + 4*(N-1)` (docs/t-weaponslot-poke.md, T1612). The owner script pokes that list with the guarded
`--forced-state` poke (T1613) at mark 3 of the route (`--poke-at-poll mark3:LABEL`, T1616), exactly like
tmp/run_forced_weapons.fish, so the preview starts with six weapons in the spawners. This module builds the request
file, prints the plan and the manual `tmp/poke_now.fish` lines, and verifies the result from the host's own records.
FABRICATED-STATE: the editor slots are poked, everything after the preview starts is unmodified game code.

    python -m tools.button_dump_poke default-weapons
    python -m tools.button_dump_poke plan --weapons 0x02,0x04,0x36 --label buttons_poke [--out DIR/guestpoke.buttons_poke]
    python -m tools.button_dump_poke manual-lines --label buttons_poke --weapons 0x02,0x04,0x36
    python -m tools.button_dump_poke verify DIR --label buttons_poke --weapons 0x02,0x04,0x36
    python -m tools.button_dump_poke route-check ROUTE

verify exit codes: 0 ok, 1 REFUSED, 2 READBACK-MISMATCH (also any failed or unexpected write), 3 a file is missing or the
poke has not been served yet (try again later), 4 bad arguments. route-check: 0 ok, 1 the route does not fit. Names come from docs/data/t-weapon-entry-table.csv, nothing is invented
(an entry without a name prints '(unnamed)').
"""

from __future__ import annotations

import argparse
import csv
import os
import re
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ENTRY_TABLE_CSV = Path("docs/data/t-weapon-entry-table.csv")

EDITOR_POINTER = 0x7844A8
SLOT_BASE_OFFSET = 0x1190C
LIST_OFFSET = 0x11904  # the dump range *0x7844A8+0x11904:0x44, slot i is at +8+4*i
LIST_LENGTH = 0x44
LIST_FIRST_SLOT = SLOT_BASE_OFFSET - LIST_OFFSET
MAX_SPAWNERS = 6
MAX_ENTRY = 0x45
FIRST_NON_WEAPON_ENTRY = 0x3C
DEFAULT_WEAPONS: tuple[int, ...] = (0x02, 0x04, 0x36, 0x05, 0x07, 0x08)
LABEL_RE = re.compile(r"[a-z0-9_-]{1,63}")
UNNAMED = "(unnamed)"

EXIT_OK = 0
EXIT_REFUSED = 1
EXIT_MISMATCH = 2
EXIT_NOT_YET = 3
EXIT_USAGE = 4
EXIT_ROUTE = 1


class PokeError(ValueError):
    """A bad weapon list, label or file."""


def parse_number(token: str) -> int:
    text = token.strip()
    if re.fullmatch(r"0[xX][0-9a-fA-F]+", text):
        return int(text, 16)
    if re.fullmatch(r"[0-9]+", text):
        return int(text, 10)
    raise PokeError(f"not a number: {token!r} (hex 0x.. or decimal)")


def parse_weapons(text: str) -> list[int]:
    """`0x02,0x04,54` -> entries. 1..6 entries, each 0..0x45. Duplicates are allowed (the same weapon in two slots)."""
    tokens = text.split(",")
    if not text.strip() or any(not token.strip() for token in tokens):
        raise PokeError(f"empty weapon in the list {text!r}")
    if len(tokens) > MAX_SPAWNERS:
        raise PokeError(f"{len(tokens)} weapons given, at most {MAX_SPAWNERS} (one per spawner)")
    entries = [parse_number(token) for token in tokens]
    for entry in entries:
        if entry > MAX_ENTRY:
            raise PokeError(
                f"entry 0x{entry:X} is above 0x{MAX_ENTRY:02X}, the table has 0x46 entries"
            )
    return entries


def check_label(label: str) -> str:
    if not LABEL_RE.fullmatch(label):
        raise PokeError(f"bad label {label!r}: use 1..63 of a-z 0-9 _ -")
    return label


def slot_offset(index: int) -> int:
    """Offset from [0x7844A8] of the weapon of spawner index+1."""
    return SLOT_BASE_OFFSET + 4 * index


def request_lines(weapons: list[int]) -> list[str]:
    """The guestpoke.LABEL request, one poke per line (the format of run_forced_weapons.fish)."""
    return [
        f"*0x{EDITOR_POINTER:X}+0x{slot_offset(index):X}=0x{entry:02X}"
        for index, entry in enumerate(weapons)
    ]


def load_names(csv_path: Path | None = None) -> dict[int, str]:
    """Entry number -> name: the measured display name when there is one, else the static public name."""
    path = csv_path if csv_path is not None else ROOT / ENTRY_TABLE_CSV
    names: dict[int, str] = {}
    try:
        with path.open(encoding="utf-8", newline="") as handle:
            for row in csv.DictReader(handle):
                try:
                    entry = parse_number(row.get("entry", ""))
                except PokeError:
                    continue
                name = (row.get("measured_display_name") or row.get("public_name") or "").strip()
                if name:
                    names[entry] = name
    except OSError:
        return {}
    return names


def name_of(names: dict[int, str], entry: int) -> str:
    return names.get(entry, UNNAMED)


def plan_lines(weapons: list[int], names: dict[int, str]) -> list[str]:
    lines = [
        f"spawner {number}: {name_of(names, entry)} (0x{entry:02X}) -> "
        f"*0x{EDITOR_POINTER:X}+0x{slot_offset(number - 1):X}=0x{entry:02X}"
        for number, entry in enumerate(weapons, start=1)
    ]
    if len(weapons) < MAX_SPAWNERS:
        lines.append(
            f"spawners {len(weapons) + 1}..{MAX_SPAWNERS}: not poked, they keep what the map has"
        )
    risky = sorted({entry for entry in weapons if entry >= FIRST_NON_WEAPON_ENTRY})
    if risky:
        lines.append(
            "WARNING: entries "
            + ", ".join(f"0x{entry:02X}" for entry in risky)
            + " have no weapon pak (non-weapon entries), forcing them is untested and may crash"
        )
    return lines


def fish_quote(text: str) -> str:
    return "'" + text.replace("\\", "\\\\").replace("'", "\\'") + "'"


def manual_line(label: str, weapons: list[int]) -> str:
    """The exact poke_now.fish command, every poke quoted (the star must not glob in fish)."""
    pokes = " ".join(fish_quote(line) for line in request_lines(weapons))
    return f"fish tmp/poke_now.fish {label} {pokes}"


# ---- verification -------------------------------------------------------------------------------------------------

SUMMARY_RE = re.compile(
    r"^(?:# )?FORCED-STATE \(T1613, FABRICATED\) poke label=(?P<label>\S+) present=(?P<present>\d+): (?P<summary>.*)$"
)
POKE_RE = re.compile(
    r"^(?:# )?poke (?P<index>\d+) label=(?P<label>\S+) present=(?P<present>\d+) "
    r"addr=(?P<addr>\S+) width=(?P<width>\d+) target=0x(?P<target>[0-9A-Fa-f]+) "
    r"old=0x(?P<old>[0-9A-Fa-f]+) new=0x(?P<new>[0-9A-Fa-f]+) "
    r"readback=0x(?P<readback>[0-9A-Fa-f]+) status=(?P<status>.*)$"
)
INDIRECT_RE = re.compile(
    r"^indirect 0x(?P<address>[0-9A-Fa-f]+) \+ 0x(?P<offset>[0-9A-Fa-f]+) length 0x(?P<length>[0-9A-Fa-f]+)$"
)
ROW_RE = re.compile(r"^[0-9A-Fa-f]{8}:((?: [0-9A-Fa-f]{2})+)$")


@dataclass
class PokeLine:
    index: int
    addr: str
    new: int
    readback: int
    status: str


@dataclass
class LogBlock:
    present: int
    summary: str
    pokes: list[PokeLine] = field(default_factory=list)


def last_block(text: str, label: str) -> LogBlock | None:
    """The newest guestpoke.log block of `label` (a summary line plus the poke lines that follow it)."""
    block: LogBlock | None = None
    collecting = False
    for line in text.splitlines():
        summary = SUMMARY_RE.match(line)
        if summary is not None:
            collecting = summary["label"] == label
            if collecting:
                block = LogBlock(int(summary["present"]), summary["summary"].strip())
            continue
        poke = POKE_RE.match(line)
        if poke is not None and collecting and block is not None and poke["label"] == label:
            block.pokes.append(
                PokeLine(
                    int(poke["index"]),
                    poke["addr"],
                    int(poke["new"], 16),
                    int(poke["readback"], 16),
                    poke["status"].strip(),
                )
            )
    return block


def dump_list_values(text: str, count: int) -> list[int] | None:
    """The first `count` slot dwords of the `*0x7844A8+0x11904:0x44` section of a guest dump, None if unreadable or absent."""
    lines = text.splitlines()
    for position, line in enumerate(lines):
        match = INDIRECT_RE.match(line)
        if match is None:
            continue
        if (
            int(match["address"], 16) != EDITOR_POINTER
            or int(match["offset"], 16) != LIST_OFFSET
            or int(match["length"], 16) != LIST_LENGTH
        ):
            continue
        data = bytearray()
        seen_range = False
        for follow in lines[position + 1 :]:
            if follow.startswith("unreadable"):
                return None
            if follow.startswith(("indirect ", "indirect2 ")):
                break
            if follow.startswith("range "):
                if seen_range:
                    break
                seen_range = True
                continue
            row = ROW_RE.match(follow)
            if row is not None and seen_range:
                data.extend(int(item, 16) for item in row[1].split())
        needed = LIST_FIRST_SLOT + 4 * count
        if len(data) < needed:
            return None
        return [
            int.from_bytes(
                data[LIST_FIRST_SLOT + 4 * index : LIST_FIRST_SLOT + 4 * index + 4], "little"
            )
            for index in range(count)
        ]
    return None


@dataclass
class Verdict:
    code: int
    result: str
    lines: list[str]


def spawner_rows(
    weapons: list[int], names: dict[int, str], pokes: list[PokeLine], good: str
) -> list[str]:
    rows = []
    for number, entry in enumerate(weapons, start=1):
        head = f"spawner {number}: {name_of(names, entry)} (0x{entry:02X})"
        poke = pokes[number - 1] if number - 1 < len(pokes) else None
        if poke is None:
            rows.append(f"{head} no poke line in the log")
        elif poke.status == "OK" and poke.readback == entry and poke.new == entry:
            rows.append(f"{head} {good.format(readback=poke.readback)}")
        elif poke.status.startswith(("refused", "skipped", "not written")):
            rows.append(f"{head} {poke.status}")
        else:
            rows.append(
                f"{head} {poke.status} (wrote 0x{poke.new:02X}, read back 0x{poke.readback:02X})"
            )
    return rows


def verify(directory: Path, label: str, weapons: list[int], names: dict[int, str]) -> Verdict:
    log_path = directory / "guestpoke.log"
    dump_path = directory / f"guestdump.{label}"
    if not log_path.is_file():
        return Verdict(
            EXIT_NOT_YET,
            "NOT-YET",
            [f"{log_path} is missing: the poke has not been served yet"],
        )
    block = last_block(log_path.read_text(encoding="utf-8", errors="replace"), label)
    if block is None:
        return Verdict(EXIT_NOT_YET, "NOT-YET", [f"no poke of label {label} in {log_path} yet"])
    head = [f"poke label={label} present={block.present}: {block.summary}"]
    if block.summary.startswith("REFUSED"):
        rows = spawner_rows(weapons, names, block.pokes, "poked OK")
        if not block.pokes:
            rows = [
                f"spawner {number}: {name_of(names, entry)} (0x{entry:02X}) REFUSED, nothing written"
                for number, entry in enumerate(weapons, start=1)
            ]
        return Verdict(EXIT_REFUSED, "REFUSED", [*head, *rows])
    problems = []
    if len(block.pokes) != len(weapons):
        problems.append(f"the log holds {len(block.pokes)} poke lines, expected {len(weapons)}")
    expected_lines = request_lines(weapons)
    for index, poke in enumerate(block.pokes[: len(weapons)]):
        wanted_address = expected_lines[index].split("=")[0]
        if _normal_address(poke.addr) != wanted_address:
            problems.append(f"poke {index} wrote {poke.addr}, expected {wanted_address}")
        if poke.new != weapons[index]:
            problems.append(f"poke {index} wrote 0x{poke.new:X}, expected 0x{weapons[index]:X}")
        if poke.status != "OK" or poke.readback != poke.new:
            problems.append(f"poke {index} status {poke.status}, read back 0x{poke.readback:X}")
    if not block.summary.startswith("OK:"):
        problems.append(f"the summary is not OK: {block.summary}")
    rows = spawner_rows(weapons, names, block.pokes, "poked OK, read back 0x{readback:02X}")
    if problems:
        return Verdict(
            EXIT_MISMATCH,
            "READBACK-MISMATCH",
            [*head, *rows, *(f"  problem: {p}" for p in problems)],
        )
    if not dump_path.is_file():
        return Verdict(
            EXIT_NOT_YET,
            "NOT-YET",
            [*head, *rows, f"{dump_path} is missing: the dump of the poke is not written yet"],
        )
    values = dump_list_values(dump_path.read_text(encoding="utf-8", errors="replace"), len(weapons))
    if values is None:
        return Verdict(
            EXIT_MISMATCH,
            "READBACK-MISMATCH",
            [
                *head,
                *rows,
                f"  problem: {dump_path} has no readable editor list section (0x{LIST_OFFSET:X})",
            ],
        )
    wrong = [
        f"  problem: the dump holds 0x{values[index]:X} for spawner {index + 1}, expected 0x{entry:X}"
        for index, entry in enumerate(weapons)
        if values[index] != entry
    ]
    if wrong:
        return Verdict(EXIT_MISMATCH, "READBACK-MISMATCH", [*head, *rows, *wrong])
    return Verdict(
        EXIT_OK,
        "OK",
        [
            *head,
            *rows,
            f"the dump {dump_path.name} holds the same {len(weapons)} entries in the editor list",
        ],
    )


def _normal_address(addr: str) -> str:
    """`*0x007844A8+0x1190C` (the log) -> `*0x7844A8+0x1190C` (the request)."""
    match = re.fullmatch(r"(\*?)0x([0-9A-Fa-f]+)\+0x([0-9A-Fa-f]+)", addr)
    if match is None:
        return addr
    return f"{match[1]}0x{int(match[2], 16):X}+0x{int(match[3], 16):X}"


# ---- the route -----------------------------------------------------------------------------------------------------

MARK_RE = re.compile(r"^# mark: at=(\d+)\s*$")
POLLS_RE = re.compile(r"^# polls: (\d+)\s*$")


def route_check(path: Path) -> tuple[bool, list[str]]:
    """Mark 3 (editor up, the poke point) and mark 4 (preview running) must be inside a completed recording."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        return False, [f"cannot read the route {path}: {error}"]
    marks = [int(m[1]) for line in text.splitlines() if (m := MARK_RE.match(line))]
    polls_found = [int(m[1]) for line in text.splitlines() if (m := POLLS_RE.match(line))]
    if not polls_found:
        return False, [f"{path} has no '# polls:' trailer (the recording was not closed normally)"]
    polls = polls_found[-1]
    if len(marks) < 4:
        return False, [
            f"{path} has {len(marks)} marks, 4 are needed: mark 3 is the poke point (editor up on the weapon set page), "
            "mark 4 the preview"
        ]
    if marks != sorted(marks) or marks[-1] > polls:
        return False, [
            f"{path}: the marks {marks} are not increasing inside the {polls} recorded polls"
        ]
    if marks[2] >= polls:
        return False, [
            f"{path}: mark 3 (at={marks[2]}) is not before the end of the record ({polls} polls)"
        ]
    return True, [
        f"route {path}: marks at {marks}, {polls} polls, the poke fires at mark 3 (at={marks[2]}), "
        f"the preview is mark 4 (at={marks[3]}), the pad is handed over at poll {polls}"
    ]


# ---- command line --------------------------------------------------------------------------------------------------


def write_atomic(path: Path, lines: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(dir=path.parent, prefix=path.name + ".", suffix=".new")
    try:
        with os.fdopen(handle, "w", encoding="utf-8") as stream:
            stream.write("\n".join(lines) + "\n")
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.button_dump_poke",
        description="T1637: spawner pokes for tmp/run_button_dumps.fish (plan, manual lines, verification).",
    )
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser(
        "default-weapons", help="print the default weapon list (entries, comma separated)"
    )
    default_text = ",".join(f"0x{entry:02X}" for entry in DEFAULT_WEAPONS)
    plan = commands.add_parser(
        "plan", help="validate, print the table, write the guestpoke request (--out)"
    )
    plan.add_argument(
        "--weapons", default=default_text, help=f"1..6 entries 0..0x45 (default {default_text})"
    )
    plan.add_argument("--label", required=True, help="poke label, a-z 0-9 _ -")
    plan.add_argument("--out", type=Path, help="write the request lines here (atomically)")
    manual = commands.add_parser(
        "manual-lines", help="print the exact fish tmp/poke_now.fish command"
    )
    manual.add_argument("--weapons", default=default_text)
    manual.add_argument("--label", required=True)
    check = commands.add_parser("verify", help="verify DIR/guestpoke.log and DIR/guestdump.LABEL")
    check.add_argument("directory", type=Path, metavar="DIR")
    check.add_argument("--weapons", default=default_text)
    check.add_argument("--label", required=True)
    route = commands.add_parser(
        "route-check", help="check that a route has the marks the poke needs"
    )
    route.add_argument("route", type=Path, metavar="ROUTE")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.command == "default-weapons":
            print(",".join(f"0x{entry:02X}" for entry in DEFAULT_WEAPONS))
            return EXIT_OK
        if args.command == "route-check":
            ok, lines = route_check(args.route)
            print("\n".join(lines))
            return EXIT_OK if ok else EXIT_ROUTE
        weapons = parse_weapons(args.weapons)
        label = check_label(args.label)
        names = load_names()
        if args.command == "plan":
            if args.out is not None:
                write_atomic(args.out, request_lines(weapons))
            print("\n".join(plan_lines(weapons, names)))
            return EXIT_OK
        if args.command == "manual-lines":
            print(manual_line(label, weapons))
            return EXIT_OK
        verdict = verify(args.directory, label, weapons, names)
        print("\n".join(verdict.lines))
        print(f"RESULT: {verdict.result}")
        return verdict.code
    except PokeError as error:
        print(f"button_dump_poke: {error}", file=sys.stderr)
        return EXIT_USAGE


if __name__ == "__main__":
    raise SystemExit(main())
