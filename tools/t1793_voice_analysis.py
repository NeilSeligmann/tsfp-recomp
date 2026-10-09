# SPDX-License-Identifier: GPL-3.0-or-later
"""Decode T1793 entry logs; publish numeric evidence only, never bank/dialogue text.

Run with --log, --resident, --route, --run, --iso, --transcript and --out.
Owner banks and API transcript are read only. Segment associations are a reviewed
map for this capture, not an automatic claim that transcript recognition is exact.
"""

from __future__ import annotations

import argparse
import collections
import csv
import hashlib
import json
import re
import struct
from collections.abc import Iterator
from pathlib import Path
from typing import Any

from tools.frontend_labels import load

ROLES = {
    0x164C40: "object_voice_request",
    0x164310: "nonobject_voice_request",
    0x461C0: "label_sample_lookup",
    0x46D80: "sample_start",
    0x148A90: "hud_voice_message",
    0x6EEA0: "cutscene_subtitle_draw",
    0x164E00: "frame_voice_scheduler",
    0x233D50: "script_voice_action",
    0x236070: "script_action_executor",
}
# Reviewed privately against owner text. None = no transcript counterpart;
# the short effort sound and radio acknowledgement were not separately transcribed.
SEGMENTS = {
    5170: [9],
    5075: [10],
    5076: [11, 12],
    5109: [],
    5164: [13],
    5121: [14],
    5078: [15],
    5088: [16],
    5123: [17],
    5124: [],
    5125: [18],
    5126: [19],
    5158: [],
    5159: [],
    5127: [20],
    5138: [21],
    5079: [22],
}


def entries(path: Path) -> Iterator[dict[str, Any]]:
    for line_number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.startswith("voice entry "):
            continue
        d = dict(re.findall(r"(\w+)=([^ ]+)", line))
        d["line_number"] = line_number
        d["va_int"] = int(d["va"], 16)
        d["words"] = [int(w, 16) for w in d["stack"].split(",")]
        if len(d["words"]) != 9 or d["va_int"] not in ROLES:
            raise ValueError(f"unexpected entry at line {line_number}")
        yield d


def csv_write(out: Path, name: str, rows: list[dict]) -> None:
    if not rows:
        raise ValueError(f"empty output {name}")
    with (out / name).open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def resident_ranges(path: Path) -> Iterator[tuple[int, str, bytes]]:
    """Consume counted binary chunks, never search binary bank data for headers."""
    data, cursor, active = path.read_bytes(), 0, None
    while cursor < len(data):
        end = data.find(b"\n", cursor)
        if end < 0:
            raise ValueError("unterminated resident header")
        header = data[cursor:end]
        cursor = end + 1
        if header.startswith(b"# range="):
            fields = dict(re.findall(rb"(\w+)=([^ ]+)", header))
            active = (int(fields[b"range"]), fields[b"pointer"].decode(), bytearray())
        elif header.startswith(b"# chunk "):
            fields = dict(re.findall(rb"(\w+)=(\d+)", header))
            size = int(fields[b"bytes"])
            if (
                active is None
                or int(fields[b"offset"]) != len(active[2])
                or cursor + size > len(data)
            ):
                raise ValueError("invalid resident chunk")
            active[2].extend(data[cursor : cursor + size])
            cursor += size
        elif header.startswith(b"# written="):
            fields = dict(re.findall(rb"(\w+)=(\d+)", header))
            if active is None or int(fields[b"written"]) != len(active[2]):
                raise ValueError("invalid resident completion")
            if int(fields[b"complete"]):
                yield active[0], active[1], bytes(active[2])
            active = None


def analyze(args: argparse.Namespace) -> None:
    calls = list(entries(args.log))
    labels = load(args.iso)
    transcript = json.loads(args.transcript.read_text())
    segments = {s["id"]: s for s in transcript["segments"]}
    groups = collections.defaultdict(list)
    requests, actions, cutscenes, starts = [], [], [], []
    for d in calls:
        va, stack = d["va_int"], d["words"]
        label = int(d.get("label", "-1"))
        key = (
            va,
            stack[0],
            label,
            int(d.get("sample_lookup", "-1")),
            int(d.get("requested_sample", "-1")),
        )
        groups[key].append(d)
        common = {
            "line": d["line_number"],
            "va": f"0x{va:08X}",
            "caller": f"0x{stack[0]:08X}",
            "ticks": int(d["ticks"]),
            "hz": int(d["hz"]),
            "guest_seconds": round(int(d["ticks"]) / int(d["hz"]), 6),
            "audio_frames": int(d["audio_frames"]),
            "audio_seconds": round(int(d["audio_frames"]) / 48000, 6),
        }
        if va in (0x164C40, 0x164310):
            # Bank lookup is deliberately consumed only in memory, never serialized.
            text = labels.text(label)
            if not text:
                raise ValueError(f"empty label {label}")
            is_object = va == 0x164C40
            ids = SEGMENTS.get(label, [])
            requests.append(
                dict(
                    common,
                    label=label,
                    sample=int(d["sample_lookup"]),
                    object=f"0x{stack[1]:08X}" if is_object else "0x00000000",
                    controls=f"0x{stack[3 if is_object else 2]:08X}",
                    event_instance=f"0x{stack[4 if is_object else 3]:08X}",
                    transcript_segment_ids=";".join(map(str, ids)),
                    capture_start=segments[ids[0]]["start"] if ids else "",
                    capture_end=segments[ids[-1]]["end"] if ids else "",
                    capture_minus_audio=round(
                        segments[ids[0]]["start"] - common["audio_seconds"], 6
                    )
                    if ids
                    else "",
                    bank_resolved=1,
                    return_observed=0,
                )
            )
        if va == 0x46D80:
            starts.append(
                dict(
                    common,
                    **{f"arg{i}": f"0x{stack[i]:08X}" for i in range(1, 9)},
                    return_observed=0,
                )
            )
        if va in (0x233D50, 0x236070):
            actions.append(
                dict(
                    common,
                    event_instance=d["event_instance"],
                    definition=d["definition"],
                    **{
                        k: d[k]
                        for k in ("def_08", "def_0C", "def_10", "def_14", "def_18", "def_B0")
                    },
                )
            )
        if va == 0x6EEA0:
            cutscenes.append(
                dict(
                    common,
                    state=d["cutscene_state"],
                    time_seconds=struct.unpack("<f", bytes.fromhex(d["time_float_bits"])[::-1])[0],
                    line_table=d["line_table"],
                    line_count=int(d["line_count"]),
                )
            )
    # Cross-check independent observed seams, rather than assuming a lookup start.
    for request in requests:
        peers = [
            d
            for d in calls
            if abs(int(d["ticks"]) - request["ticks"]) / int(d["hz"]) < 0.001
            and int(d["audio_frames"]) == request["audio_frames"]
        ]
        action = [
            d
            for d in peers
            if d["va_int"] == 0x233D50
            and int(d["event_instance"], 16) == int(request["event_instance"], 16)
        ]
        if len(action) != 1 or int(action[0]["def_10"], 16) != request["label"]:
            raise ValueError("request/action label or instance mismatch")
        if int(action[0]["def_B0"], 16) != int(request["controls"], 16):
            raise ValueError("request/action control mismatch")
        if not any(
            d["va_int"] == 0x461C0
            and int(d["label"]) == request["label"]
            and int(d["sample_lookup"]) == request["sample"]
            for d in peers
        ):
            raise ValueError("request/lookup mismatch")
        if not any(d["va_int"] == 0x46D80 and d["words"][1] == request["sample"] for d in peers):
            raise ValueError("request/start mismatch")
    args.out.mkdir(parents=True, exist_ok=True)
    aggregate = []
    for (va, caller, label, sample, requested), ds in sorted(groups.items()):
        aggregate.append(
            {
                "va": f"0x{va:08X}",
                "role": ROLES[va],
                "caller": f"0x{caller:08X}",
                "label": label,
                "lookup_sample": sample,
                "requested_sample": requested,
                "count": len(ds),
                "first_audio_frame": int(ds[0]["audio_frames"]),
                "last_audio_frame": int(ds[-1]["audio_frames"]),
            }
        )
    tables = collections.Counter(
        line for line in args.log.read_text().splitlines() if line.startswith("voice table ")
    )
    table_rows = []
    for line, n in sorted(tables.items()):
        d = dict(re.findall(r"(\w+)=([^ ]+)", line))
        table_rows.append(dict(d, observations=n))
    resident_tables, resident_lines = [], []
    for slot, pointer, data in resident_ranges(args.resident):
        if slot in (0, 1):
            for row, (sample,) in enumerate(struct.iter_unpack("<i", data)):
                resident_tables.append(
                    {"slot": slot, "pointer": pointer, "row": row, "sample": sample}
                )
        if slot == 4:
            for row, record in enumerate(struct.iter_unpack("<ffIIII", data)):
                resident_lines.append(
                    {
                        "pointer": pointer,
                        "row": row,
                        "start": record[0],
                        "duration": record[1],
                        "label": record[4],
                        "flags_byte": record[5] & 255,
                    }
                )
    for request in requests:
        matched = False
        for descriptor in table_rows:
            base, count = int(descriptor["base"]), int(descriptor["count"])
            row = request["label"] - base
            if not 0 <= row < count:
                continue
            matched |= any(
                r["pointer"] == descriptor["pointer"]
                and r["row"] == row
                and r["sample"] == request["sample"]
                for r in resident_tables
            )
        if not matched:
            raise ValueError("request/resident row mismatch")
    csv_write(args.out, "resident_samples.csv", resident_tables)
    csv_write(args.out, "resident_cutscene_records.csv", resident_lines)
    csv_write(args.out, "sample_starts.csv", starts)
    csv_write(args.out, "call_groups.csv", aggregate)
    csv_write(args.out, "voice_requests.csv", requests)
    csv_write(args.out, "script_actions.csv", actions)
    csv_write(args.out, "cutscene_draws.csv", cutscenes)
    csv_write(args.out, "table_descriptors.csv", table_rows)
    counts = collections.Counter(d["va_int"] for d in calls)
    metrics = {
        "entry_count": len(calls),
        "counts": {f"0x{va:08X}": counts[va] for va in ROLES},
        "lookup_only_is_not_voice_start": True,
        "returns_observed": 0,
        "voice_request_count": len(requests),
        "transcript_associated_requests": sum(bool(r["transcript_segment_ids"]) for r in requests),
        "uncovered_transcript_segment_ids": [
            segment
            for segment in range(20, 29)
            if not any(segment in SEGMENTS.get(r["label"], []) for r in requests)
        ],
        "source_sha256": {
            k: hashlib.sha256(getattr(args, k).read_bytes()).hexdigest()
            for k in ("log", "resident", "route", "run", "transcript")
        },
        "provenance": (
            "MEASURED private lifted-host entries; "
            "transcript associations reviewed; no xemu reference"
        ),
    }
    (args.out / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("log", "resident", "route", "run", "iso", "transcript", "out"):
        p.add_argument("--" + name, type=Path, required=True)
    analyze(p.parse_args())


if __name__ == "__main__":
    main()
