#!/usr/bin/env python3
# ruff: noqa: E501, ANN001, ANN201
"""T1240: closed loop aim and fire at one character of the 0x7A3580 array through a running `tools.t1240_drive` daemon.

Every round: `mem` snapshot, find the character (C = pointer in the array, O = [C+0], position O+0x64..0x6C), compute the
bearing from the player (array entry 0) and the pose yaw, pulse the right stick key (F/H) to cancel the error, fire the trigger
for a burst, log the candidate health word `O+0x1CC` and the free/dead markers (`C+0x20 == 5`, `C+0x190 == 0`).
Read only: no guest word is written, only keyboard keys are pressed (T1240).

    python -m tools.t1240_hit_loop --out-dir tmp/t1240/e1 --model 75 --rounds 20   (omit --model for the nearest character)
"""

from __future__ import annotations

import argparse
import json
import math
import socket
import struct
import sys
from pathlib import Path

from tools.guest_mem_probe import load_snapshot

ARRAY, COUNT = 0x7A3580, 0x74C37C


def ask(sock_path: Path, line: str, timeout: float = 120.0) -> str:
    """One command, one connection, like `tools.t1240_drive cmd`."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(timeout)
        client.connect(str(sock_path))
        client.sendall((line + "\n").encode())
        return client.makefile("rb").readline().decode().strip()


def reader(snapshot: dict[int, bytes]):
    def read(address: int, size: int) -> bytes:
        for start, data in snapshot.items():
            if start <= address and address + size <= start + len(data):
                return data[address - start : address - start + size]
        # T1741: a --watch-write page protection splits a mapping, so a read may span two adjacent saved regions
        out = bytearray()
        while len(out) < size:
            cursor = address + len(out)
            for start, data in snapshot.items():
                if start <= cursor < start + len(data):
                    out += data[cursor - start : cursor - start + (size - len(out))]
                    break
            else:
                raise KeyError(hex(address))
        return bytes(out)

    return read


def characters(read) -> list[dict]:
    """All live entries of the character array: pointer, model, position, health candidate, state words."""
    count = struct.unpack("<I", read(COUNT, 4))[0]
    rows = []
    for index in range(count):
        cptr = struct.unpack("<I", read(ARRAY + 4 * index, 4))[0]
        record = read(cptr, 0xEA0)
        optr = struct.unpack_from("<I", record, 0)[0]
        if not optr:
            continue
        obj = read(optr, 0x400)
        rows.append(
            {
                "index": index,
                "c": cptr,
                "o": optr,
                "slot": struct.unpack_from("<I", record, 8)[0],
                "model": struct.unpack_from("<h", record, 0x48)[0],
                "state": struct.unpack_from("<I", record, 0x20)[0],
                "pos": struct.unpack_from("<3f", obj, 0x64),
                "health": struct.unpack_from("<f", obj, 0x1CC)[0],
                "cb48": struct.unpack_from("<f", record, 0xB48)[0],
            }
        )
    return rows


def pulse(error: float) -> str:
    seconds = max(0.05, min(0.4, abs(error) / 130.0))
    return f"tap {'H' if error > 0 else 'F'} {seconds:.2f}"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out-dir", required=True)
    parser.add_argument(
        "--model", type=int, help="C+0x48 model selector of the target (default: any)"
    )
    parser.add_argument("--slot", type=int, help="C+8 slot index (default: nearest of the model)")
    parser.add_argument("--rounds", type=int, default=20)
    parser.add_argument("--burst", type=float, default=0.4, help="trigger seconds per round")
    parser.add_argument("--tolerance", type=float, default=2.5, help="yaw error in degrees")
    args = parser.parse_args()
    out = Path(args.out_dir)
    sock = out / "control.sock"
    for number in range(args.rounds):
        ask(sock, f"mem loop_{number % 2}")
        rows = characters(reader(load_snapshot(out / f"loop_{number % 2}")))
        player = rows[0]
        targets = [
            r
            for r in rows[1:]
            if (args.model is None or r["model"] == args.model)
            and r["state"] != 5
            and r["health"] > 0
        ]
        if args.slot is not None:
            targets = [r for r in targets if r["slot"] == args.slot]
        if not targets:
            print(json.dumps({"round": number, "result": "target gone", "rows": len(rows)}))
            return 0
        target = min(targets, key=lambda r: math.dist(r["pos"], player["pos"]))
        bearing = math.degrees(
            math.atan2(target["pos"][2] - player["pos"][2], target["pos"][0] - player["pos"][0])
        )
        pose = json.loads(ask(sock, "pose"))
        error = (bearing - pose["yaw_deg"] + 180.0) % 360.0 - 180.0
        if abs(error) > args.tolerance:
            ask(sock, pulse(error))
        ask(sock, "keys 2")
        ask(sock, f"run {args.burst}")
        ask(sock, "keys")
        print(
            json.dumps(
                {
                    "round": number,
                    "slot": target["slot"],
                    "health": round(target["health"], 3),
                    "state": target["state"],
                    "cb48": round(target["cb48"], 3),
                    "distance": round(math.dist(target["pos"], player["pos"]), 1),
                    "player_health": round(player["health"], 2),
                    "yaw_error": round(error, 1),
                }
            ),
            flush=True,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
