# SPDX-License-Identifier: GPL-3.0-or-later
"""Bounded private output sink for one Codex assignment; not a process supervisor."""

import argparse
import json
import os
import sys
from pathlib import Path
from uuid import uuid4

HEAD_BYTES = 64 * 1024
TAIL_BYTES = 192 * 1024
LINE_BYTES = 64 * 1024


def write_json(path: Path, value: dict) -> None:
    temporary = path.with_name(f".{path.name}-{uuid4().hex}")
    try:
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as output:
            json.dump(value, output)
            output.write("\n")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def collect(directory: Path) -> None:
    total = head_size = 0
    tail = b""
    line = bytearray()
    oversized_line = False
    thread_recorded = False
    state = {"state": "collecting", "bytes_seen": 0, "exit_status": "unavailable"}
    write_json(directory / "output.json", state)
    head_fd = os.open(directory / "head.jsonl", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    tail_fd = os.open(directory / "tail.jsonl", os.O_RDWR | os.O_CREAT | os.O_EXCL, 0o600)
    with (
        os.fdopen(head_fd, "wb", buffering=0) as head,
        os.fdopen(tail_fd, "w+b", buffering=0) as last,
    ):
        while chunk := sys.stdin.buffer.read1(8192):
            total += len(chunk)
            prefix = chunk[: max(0, HEAD_BYTES - head_size)]
            head.write(prefix)
            head_size += len(prefix)
            tail = (tail + chunk)[-TAIL_BYTES:]
            last.seek(0)
            last.write(tail)
            last.truncate()
            if not thread_recorded:
                for byte in chunk:
                    if byte == 10:
                        if not oversized_line:
                            try:
                                event = json.loads(line)
                            except (ValueError, UnicodeError):
                                event = None
                            if isinstance(event, dict) and event.get("type") == "thread.started":
                                thread = event.get("thread_id")
                                if isinstance(thread, str) and thread:
                                    write_json(directory / "thread.json", {"thread_id": thread})
                                    thread_recorded = True
                        line.clear()
                        oversized_line = False
                    elif not oversized_line:
                        if len(line) < LINE_BYTES:
                            line.append(byte)
                        else:
                            line.clear()
                            oversized_line = True
            state.update(
                bytes_seen=total,
                head_bytes=head_size,
                tail_bytes=len(tail),
                truncated=total > HEAD_BYTES + TAIL_BYTES,
            )
            write_json(directory / "output.json", state)
    state.update(
        state="eof",
        bytes_seen=total,
        head_bytes=head_size,
        tail_bytes=len(tail),
        truncated=total > HEAD_BYTES + TAIL_BYTES,
    )
    write_json(directory / "output.json", state)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        collect(args.directory)
    except Exception as error:
        # Retain a bounded error class, never arbitrary child output or environment.
        write_json(
            args.directory / "output.json",
            {
                "state": "capture_failed",
                "error_type": type(error).__name__,
                "exit_status": "unavailable",
            },
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
