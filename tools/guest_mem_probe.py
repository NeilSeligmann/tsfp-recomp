#!/usr/bin/env python3
# ruff: noqa: E501
"""T1222 read-only guest memory probe for a running private host (no writes, no ptrace attach).

The recompiled guest address space is host memory identity mapped below 4 GiB (src/xbox/guest_mem.c), so the
host process' /proc/PID/mem shows guest addresses directly.  This tool only ever opens the file read-only.

    python -m tools.guest_mem_probe snapshot --pid PID --out tmp/t982/mem/s0.bin
    python -m tools.guest_mem_probe diff --a s0.bin --b s1.bin --min 1 --max 200
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from pathlib import Path

LIMIT = 1 << 32


def read_stream_pool(pid: int) -> list[dict[str, int]]:
    """T1245 retail 3cfd001a layout, read-only and non-atomic; never a call-count witness.

    0x275E4 searches ten identifiers; 0x27615 reads metadata at slot+0x4C;
    0x29B23 tests slot+4 before calling stream SetFrequency. The caller must
    establish the running host's image identity before interpreting these fields.
    """
    fd = os.open(f"/proc/{pid}/mem", os.O_RDONLY)
    try:

        def read(address: int, size: int) -> bytes:
            data = os.pread(fd, size, address)
            if len(data) != size:
                raise ValueError(f"short guest read at {address:#x}: {len(data)}/{size}")
            return data

        identifiers = struct.unpack("<10i", read(0x00581990, 40))
        rows = []
        for index, identifier in enumerate(identifiers):
            address = 0x005836D0 + index * 0x14094
            header = read(address, 0x50)
            rows.append(
                {
                    "slot": index,
                    "address": address,
                    "identifier": identifier,
                    "stream": struct.unpack_from("<I", header, 4)[0],
                    "wrapper_reset": struct.unpack_from("<I", header, 0x24)[0],
                    "metadata": struct.unpack_from("<i", header, 0x4C)[0],
                }
            )
        return rows
    finally:
        os.close(fd)


def read_tagged_sound_records(pid: int, *, max_records: int = 4096) -> dict[str, object]:
    """T1245 original 3cfd001a record table; read-only, explicitly non-atomic.

    Caller establishes image/process identity and permission to read its memory.
    Original 236070 dispatches source+8; only type 0x0B reaches 236A48,
    where source+0xC is a sound ID and 236B60 stores handle at row+0x20.
    Other record types retain raw fields, never a guessed sound classification.
    Original 2340A0 compares source+0xAC with the requested tag.
    The observer cap is a safety limit, not an original game table limit.
    """
    if not 1 <= max_records <= 4096:
        raise ValueError("observer record cap must be between 1 and 4096")
    fd = os.open(f"/proc/{pid}/mem", os.O_RDONLY)
    try:

        def words(address: int, count: int) -> tuple[int, ...]:
            size = count * 4
            if address < 0 or address + size > LIMIT:
                raise ValueError("guest record read exceeds 32-bit address space")
            data = os.pread(fd, size, address)
            if len(data) != size:
                raise ValueError(f"short guest record read at {address:#x}: {len(data)}/{size}")
            return struct.unpack("<" + "I" * count, data)

        count = words(0x75CCA8, 1)[0]
        table = words(0x78CD90, 1)[0]
        if count > max_records:
            raise ValueError("tagged record count exceeds observer cap; not truncated")
        if count and not table:
            raise ValueError("nonnull tagged record count with null table")
        if table + count * 0x48 > LIMIT:
            raise ValueError("tagged record table exceeds 32-bit address space")
        rows = []
        for index in range(count):
            record = words(table + index * 0x48, 18)
            source = record[0]
            if not source:
                raise ValueError("null tagged record source")
            source_type, source_c = words(source + 8, 2)
            tag, flags = words(source + 0xAC, 2)
            item = {
                "index": index,
                "source": source,
                "source_type": source_type,
                "source_c": source_c,
                "tag": tag,
                "source_flags": flags,
                "handle": record[8],
            }
            if source_type == 0x0B:
                item["sound_id"] = source_c
                # 5704 is already the adjacent startup buffer-pointer array.
                if source_c < 5704:
                    item["resource_fields"] = list(words(0x565B48 + source_c * 20, 2))
                    metadata = words(0x4CA9B4, 1)[0]
                    if metadata:
                        item["metadata_words"] = list(words(metadata + source_c * 20, 5))
            rows.append(item)
        return {"non_atomic": True, "table": table, "count": count, "records": rows}
    finally:
        os.close(fd)


def parse_maps(text: str, limit: int = LIMIT) -> list[tuple[int, int]]:
    """Writable private regions that start below `limit` (guest visible RAM, heaps, stacks)."""
    regions = []
    for line in text.splitlines():
        fields = line.split()
        # T1741: a page the host write watch (--watch-write) protects shows as r--p, TSFP_SNAPSHOT_READONLY=1 keeps those too
        writable = fields[1].startswith("rw") if len(fields) > 1 else False
        watched = os.environ.get("TSFP_SNAPSHOT_READONLY") == "1" and fields[1:2] == ["r--p"]
        if len(fields) < 2 or not (writable or watched):
            continue
        start, end = (int(part, 16) for part in fields[0].split("-"))
        if start < limit and "[vvar" not in line and "[vsyscall" not in line:
            regions.append((start, min(end, limit)))
    return regions


def read_snapshot(pid: int) -> dict[int, bytes]:
    """Read every region once, skipping unreadable ones.  Read-only: the memory file is opened O_RDONLY."""
    regions = parse_maps(Path(f"/proc/{pid}/maps").read_text())
    out: dict[int, bytes] = {}
    fd = os.open(f"/proc/{pid}/mem", os.O_RDONLY)
    try:
        for start, end in regions:
            try:
                out[start] = os.pread(fd, end - start, start)
            except OSError:
                continue
    finally:
        os.close(fd)
    return out


def save_snapshot(snapshot: dict[int, bytes], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as handle:
        handle.write(struct.pack("<I", len(snapshot)))
        for start, data in snapshot.items():
            handle.write(struct.pack("<QQ", start, len(data)))
            handle.write(data)


def load_snapshot(path: Path) -> dict[int, bytes]:
    out: dict[int, bytes] = {}
    with path.open("rb") as handle:
        (count,) = struct.unpack("<I", handle.read(4))
        for _ in range(count):
            start, size = struct.unpack("<QQ", handle.read(16))
            out[start] = handle.read(size)
    return out


def float_decreases(
    before: dict[int, bytes], after: dict[int, bytes], low: float, high: float
) -> list[tuple[int, float, float]]:
    """Aligned float32 words that fell between two snapshots, both values inside [low, high]."""
    import numpy as np

    hits = []
    for start, data in after.items():
        old = before.get(start)
        if old is None or len(old) != len(data):
            continue
        usable = len(data) & ~3
        a = np.frombuffer(old[:usable], dtype="<f4")
        b = np.frombuffer(data[:usable], dtype="<f4")
        with np.errstate(invalid="ignore"):
            mask = (a >= low) & (a <= high) & (b >= low) & (b <= high) & (b < a)
        for index in np.nonzero(mask)[0]:
            hits.append((start + int(index) * 4, float(a[index]), float(b[index])))
    return hits


def index_snapshot(path: Path) -> dict[int, tuple[int, int]]:
    """Region start -> (file offset of its bytes, size), without reading the data."""
    out: dict[int, tuple[int, int]] = {}
    with path.open("rb") as handle:
        (count,) = struct.unpack("<I", handle.read(4))
        for _ in range(count):
            start, size = struct.unpack("<QQ", handle.read(16))
            out[start] = (handle.tell(), size)
            handle.seek(size, 1)
    return out


def step_down_series(
    paths: list[Path], low: float, high: float, max_changes: int, chunk_words: int = 1 << 20
) -> list[dict]:
    """Float32 words that hold one value, then only ever fall (a health-like series) across ordered snapshots.

    Candidates need every sample inside [low, high], the last below the first, never a rise, and between 1 and
    `max_changes` changes (a timer changes at every snapshot and is excluded, a constant never falls).
    """
    import numpy as np

    indexes = [index_snapshot(path) for path in paths]
    common = set(indexes[0])
    for index in indexes[1:]:
        common &= {
            start for start, entry in index.items() if entry[1] == indexes[0].get(start, (0, -1))[1]
        }
    found: list[dict] = []
    maps = [np.memmap(path, dtype=np.uint8, mode="r") for path in paths]
    for start in sorted(common):
        size = indexes[0][start][1]
        words = size // 4
        for first in range(0, words, chunk_words):
            count = min(chunk_words, words - first)
            stack = np.stack(
                [
                    np.frombuffer(
                        maps[i][
                            indexes[i][start][0] + first * 4 : indexes[i][start][0]
                            + (first + count) * 4
                        ],
                        dtype="<f4",
                    )
                    for i in range(len(paths))
                ]
            )
            with np.errstate(invalid="ignore"):
                inside = ((stack >= low) & (stack <= high)).all(axis=0)
                diffs = np.diff(stack, axis=0)
                monotone = (diffs <= 0).all(axis=0)
                changes = (diffs != 0).sum(axis=0)
                keep = (
                    inside
                    & monotone
                    & (changes >= 1)
                    & (changes <= max_changes)
                    & (stack[-1] < stack[0])
                )
            for offset in np.nonzero(keep)[0]:
                found.append(
                    {
                        "address": start + (first + int(offset)) * 4,
                        "series": [float(v) for v in stack[:, offset]],
                    }
                )
    return found


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    sub = parser.add_subparsers(dest="command", required=True)
    pool = sub.add_parser("stream-pool", help="T1245 retail sound slots (read-only, non-atomic)")
    pool.add_argument("--pid", type=int, required=True)
    records = sub.add_parser(
        "tagged-sound-records", help="T1245 type-qualified records (read-only, non-atomic)"
    )
    records.add_argument("--pid", type=int, required=True)
    records.add_argument("--max-records", type=int, default=4096)
    snap = sub.add_parser(
        "snapshot", help="dump the guest visible writable regions of a running host"
    )
    snap.add_argument("--pid", type=int, required=True)
    snap.add_argument("--out", required=True)
    diff = sub.add_parser("diff", help="float32 words that decreased between two snapshots")
    diff.add_argument("--a", required=True)
    diff.add_argument("--b", required=True)
    diff.add_argument("--min", type=float, default=0.0)
    diff.add_argument("--max", type=float, default=1000.0)
    diff.add_argument("--limit", type=int, default=50)
    series = sub.add_parser(
        "series", help="float32 words that only fall across ordered snapshots (health-like)"
    )
    series.add_argument("snapshots", nargs="+")
    series.add_argument("--min", type=float, default=0.001)
    series.add_argument("--max", type=float, default=2000.0)
    series.add_argument("--max-changes", type=int, default=6)
    series.add_argument("--limit", type=int, default=80)
    args = parser.parse_args()
    if args.command == "stream-pool":
        print(json.dumps({"non_atomic": True, "slots": read_stream_pool(args.pid)}))
        return 0
    if args.command == "tagged-sound-records":
        print(json.dumps(read_tagged_sound_records(args.pid, max_records=args.max_records)))
        return 0
    if args.command == "series":
        found = step_down_series(
            [Path(item) for item in args.snapshots], args.min, args.max, args.max_changes
        )
        print(f"{len(found)} step-down words")
        for entry in found[: args.limit]:
            print(
                f"0x{entry['address']:08x} " + " ".join(f"{value:g}" for value in entry["series"])
            )
        return 0
    if args.command == "snapshot":
        shot = read_snapshot(args.pid)
        save_snapshot(shot, Path(args.out))
        print(json.dumps({"regions": len(shot), "bytes": sum(len(v) for v in shot.values())}))
        return 0
    hits = float_decreases(
        load_snapshot(Path(args.a)), load_snapshot(Path(args.b)), args.min, args.max
    )
    print(f"{len(hits)} decreased floats")
    for address, old, new in hits[: args.limit]:
        print(f"0x{address:08x} {old:g} -> {new:g}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
