#!/usr/bin/env python3
# ruff: noqa: E501
"""T1763: two-launch save/reload receipt for the host-directory HDD (the U: save partition).

Launch 1 plays and writes a save, launch 2 starts on the SAME --hdd directory and loads it. This tool does the bookkeeping:

    python -m tools.save_reload_receipt snapshot --hdd DIR --out after1.json        # path, size, sha256 per file
    python -m tools.save_reload_receipt receipt --after1 after1.json --before2 before2.json --after2 after2.json \
        --log1 play1.log --log2 play2.log [--json out.json]
    python -m tools.save_reload_receipt logsummary play1.log                          # the NT file call log summary

The host logs these lines (src/xbox/kernel_file.c, kernel_io.c; the NtReadFile line is the T1763 addition):
  NtCreateFile("P") -> handle H, a file just CREATED ...   |   ... an existing file TRUNCATED TO ZERO ...
  NtWriteFile("P") wrote N byte(s) at OFF (...)
  NtFlushBuffersFile("P") -> success ...
  NtReadFile("P") read N byte(s) at OFF (host directory file)
A save counts as LISTED when a payload file (not TitleMeta/TitleImage/SaveImage/SaveMeta) exists on the HDD before launch 2 (its
SaveMeta.xbx read in launch 2 is shown as listing_read_in_launch2), LOADED when launch 2 read bytes from the payload, IDENTICAL when its sha256 is the same after launch 1, before launch 2 and after launch 2 (launch 2 never rewrote it).
Evidence labels come from the logs only: this tool never forces or edits a save.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

# files the title writes at startup (docs/t959-save-contracts.md): metadata, not a save
METADATA_NAMES = {"titlemeta.xbx", "titleimage.xbx", "saveimage.xbx"}
HOST_INTERNAL_PREFIX = ".tsfp-"
TITLE_ID = "45410066"

CREATE_RE = re.compile(r'NtCreateFile\("(?P<path>[^"]*)"\) -> handle \S+, (?P<what>.*)')
WRITE_RE = re.compile(
    r'NtWriteFile\("(?P<path>[^"]*)"\) wrote (?P<n>\d+) byte\(s\) at (?P<off>\d+)'
)
FLUSH_RE = re.compile(r'NtFlushBuffersFile\("(?P<path>[^"]*)"\) -> success')
READ_RE = re.compile(
    r'NtReadFile\("(?P<path>[^"]*)"\) read (?P<n>\d+) byte\(s\) at (?P<off>\d+) \(host directory file\)'
)


@dataclass(frozen=True)
class Event:
    kind: str  # create | truncate | write | flush | read
    path: str
    nbytes: int = 0
    offset: int = 0


def normalise(path: str) -> str:
    """Guest path to a lowercase forward slash form without the device prefix, e.g. `udata/45410066/titlemeta.xbx`."""
    text = path.replace("\\", "/").lower()
    for marker in ("udata/", "tdata/"):
        index = text.find(marker)
        if index >= 0:
            return text[index:]
    # the title's own drive aliases (measured: "\??\U:\<save dir>\file", U: = partition1 UDATA/<title id>)
    for letter, root in (("u:/", "udata"), ("t:/", "tdata")):
        index = text.find(letter)
        if index >= 0:
            return f"{root}/{TITLE_ID}/{text[index + 3 :]}"
    match = re.search(r"partition(\d+)$", text)
    if match is not None:
        return f".tsfp-partition{match.group(1)}.bin"  # the raw partition image the host keeps in the HDD directory
    return text.rsplit(":", 1)[-1].lstrip("/")


def parse_log(text: str) -> list[Event]:
    events: list[Event] = []
    for line in text.splitlines():
        if (m := WRITE_RE.search(line)) is not None:
            events.append(Event("write", m["path"], int(m["n"]), int(m["off"])))
        elif (m := READ_RE.search(line)) is not None:
            events.append(Event("read", m["path"], int(m["n"]), int(m["off"])))
        elif (m := FLUSH_RE.search(line)) is not None:
            events.append(Event("flush", m["path"]))
        elif (m := CREATE_RE.search(line)) is not None:
            what = m["what"]
            if "just CREATED" in what and "directory" not in what.split("just CREATED")[0]:
                events.append(Event("create", m["path"]))
            elif "TRUNCATED" in what:
                events.append(Event("truncate", m["path"]))
    return events


def summarise_events(events: list[Event]) -> dict[str, dict[str, int]]:
    """Per normalised path: counts of each call kind and the bytes written and read."""
    table: dict[str, dict[str, int]] = defaultdict(
        lambda: {
            "create": 0,
            "truncate": 0,
            "write": 0,
            "flush": 0,
            "read": 0,
            "bytes_written": 0,
            "bytes_read": 0,
        }
    )
    for event in events:
        row = table[normalise(event.path)]
        row[event.kind] += 1
        if event.kind == "write":
            row["bytes_written"] += event.nbytes
        if event.kind == "read":
            row["bytes_read"] += event.nbytes
    return dict(table)


def snapshot(hdd: Path) -> dict[str, dict[str, object]]:
    """Every regular file under the HDD directory: relative path (lowercase key kept as found), size, sha256."""
    result: dict[str, dict[str, object]] = {}
    for path in sorted(hdd.rglob("*")):
        if path.is_file():
            data = path.read_bytes()
            result[path.relative_to(hdd).as_posix()] = {
                "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
            }
    return result


def is_save_file(rel: str) -> bool:
    name = rel.rsplit("/", 1)[-1].lower()
    skipped = METADATA_NAMES | {
        "savemeta.xbx"
    }  # SaveMeta.xbx is the listing record of a save directory, not the payload
    return (
        not name.startswith(HOST_INTERNAL_PREFIX)
        and name not in skipped
        and rel.lower().startswith("udata/")
    )


def judge(
    after1: dict, before2: dict, after2: dict, events1: list[Event], events2: list[Event]
) -> dict:
    """Per U: save candidate file the three-way sha comparison plus the call evidence of both launches."""
    summary1, summary2 = summarise_events(events1), summarise_events(events2)
    files = []
    for rel in sorted(set(after1) | set(before2) | set(after2)):
        key = normalise(rel)
        sha = [snap.get(rel, {}).get("sha256") for snap in (after1, before2, after2)]
        row1, row2 = summary1.get(key, {}), summary2.get(key, {})
        files.append(
            {
                "path": rel,
                "save_candidate": is_save_file(rel),
                "size_before2": before2.get(rel, {}).get("size"),
                "sha_after1": sha[0],
                "sha_before2": sha[1],
                "sha_after2": sha[2],
                "identical": sha[0] is not None and sha[0] == sha[1] == sha[2],
                "launch1": row1,
                "launch2": row2,
                "listed": rel in before2 and before2[rel]["size"] > 0,
                "listing_read_in_launch2": summary2.get(
                    normalise(rel.rsplit("/", 1)[0] + "/SaveMeta.xbx"), {}
                ).get("bytes_read", 0)
                > 0,
                "written_in_launch1": row1.get("bytes_written", 0) > 0,
                "loaded_in_launch2": row2.get("bytes_read", 0) > 0,
                "rewritten_in_launch2": row2.get("bytes_written", 0) > 0
                or row2.get("truncate", 0) > 0,
            }
        )
    saves = [f for f in files if f["save_candidate"]]
    proven = [
        f
        for f in saves
        if f["listed"]
        and f["loaded_in_launch2"]
        and f["identical"]
        and not f["rewritten_in_launch2"]
    ]
    return {
        "files": files,
        "save_files": len(saves),
        "proven": [f["path"] for f in proven],
        "verdict": "PASS" if proven else "NOT PROVEN",
        "reason": ""
        if proven
        else (
            "no U: save file (besides TitleMeta/TitleImage/SaveImage) was listed, read in launch 2 and byte identical"
            if not saves
            else "a save file exists but launch 2 did not read it, or it changed (see per-file rows)"
        ),
    }


def render(result: dict, events1: list[Event], events2: list[Event]) -> str:
    out = ["T1763 save/reload receipt", ""]
    out.append(
        f"{'path':58} {'size':>9} {'sha256 before launch 2':64} {'sha256 after launch 2':64} same"
    )
    for f in result["files"]:
        if (
            Path(f["path"]).name.startswith(HOST_INTERNAL_PREFIX)
            and not f["launch1"]
            and not f["launch2"]
        ):
            continue
        out.append(
            f"{f['path']:58} {f['size_before2'] if f['size_before2'] is not None else '-':>9} "
            f"{f['sha_before2'] or '-':64} {f['sha_after2'] or '-':64} {'yes' if f['identical'] else 'NO'}"
        )
    for label, events in (("launch 1", events1), ("launch 2", events2)):
        out += ["", f"NT file call log, {label}: {len(events)} events"]
        for key, row in sorted(summarise_events(events).items()):
            out.append(
                f"  {key:56} create {row['create']} truncate {row['truncate']} write {row['write']} ({row['bytes_written']} B) "
                f"flush {row['flush']} read {row['read']} ({row['bytes_read']} B)"
            )
    out += [
        "",
        f"save files on the HDD: {result['save_files']}; proven listed+loaded+identical: {result['proven'] or 'none'}",
    ]
    out.append(
        f"VERDICT: {result['verdict']}" + (f" ({result['reason']})" if result["reason"] else "")
    )
    return "\n".join(out)


def load_json(path: str) -> dict:
    return json.loads(Path(path).read_text())


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="command", required=True)
    snap = sub.add_parser(
        "snapshot", help="path, size, sha256 of every file under the HDD directory"
    )
    snap.add_argument("--hdd", required=True)
    snap.add_argument("--out", help="write JSON here (default: print a table)")
    summ = sub.add_parser("logsummary", help="NT file call summary of one host log")
    summ.add_argument("log")
    rec = sub.add_parser("receipt", help="the two launch receipt")
    for name in ("after1", "before2", "after2", "log1", "log2"):
        rec.add_argument(f"--{name}", required=True)
    rec.add_argument("--json", help="also write the machine readable receipt here")
    args = parser.parse_args(argv)

    if args.command == "snapshot":
        data = snapshot(Path(args.hdd))
        if args.out:
            Path(args.out).write_text(json.dumps(data, indent=1, sort_keys=True))
        for rel, info in data.items():
            print(f"{rel:58} {info['size']:>9} {info['sha256']}")
        return 0
    if args.command == "logsummary":
        events = parse_log(Path(args.log).read_text(errors="replace"))
        print(
            render(
                {"files": [], "save_files": 0, "proven": [], "verdict": "n/a", "reason": ""},
                events,
                [],
            )
        )
        return 0
    events1 = parse_log(Path(args.log1).read_text(errors="replace"))
    events2 = parse_log(Path(args.log2).read_text(errors="replace"))
    result = judge(
        load_json(args.after1), load_json(args.before2), load_json(args.after2), events1, events2
    )
    print(render(result, events1, events2))
    if args.json:
        Path(args.json).write_text(json.dumps(result, indent=1, sort_keys=True))
    return 0 if result["verdict"] == "PASS" else 3


if __name__ == "__main__":
    sys.exit(main())
