#!/usr/bin/env python3
# ruff: noqa: E501
"""T1720: read the `shot` hotkey output of the host (src/host/shot_hotkey.c) so a screenshot can be matched to the draw census.

The host writes, into the --hotkey-dir of the run (tmp/owner-profiles/render-<stamp>/):

    shot-001.png                     8 bit RGB PNG of the presented frame (stored deflate, written by the host itself)
    shots.manifest                   one line per shot:  shot-001.png poll=912 present=340 phase=radar label=shot hotkey=3 size=640x480
                                     (T1720b adds two trailing fields: unix=1790000000 bytes=921654, optional, old lines still parse)
    phase                            optional, written by the wrapper: first line = the phase name copied into the manifest

    python -m tools.render_shots report DIR      table of shots, PNG check (signature, CRC, size, blank or not) and a verdict
    python -m tools.render_shots check DIR       same, exit 1 when a manifest line has no valid PNG or a PNG is blank

Exit 2 on a missing folder or manifest. A blank PNG (one colour only) is reported, the host captured nothing visible there.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
import zlib
from dataclasses import dataclass
from datetime import UTC, datetime
from pathlib import Path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
MANIFEST = "shots.manifest"
LINE_RE = re.compile(
    r"(?P<name>shot-\d{3,}\.png) poll=(?P<poll>\d+) present=(?P<present>\d+) phase=(?P<phase>\S+)"
    r" label=(?P<label>\S+) hotkey=(?P<hotkey>\d+) size=(?P<width>\d+)x(?P<height>\d+)"
    r"(?: unix=(?P<unix>\d+))?(?: bytes=(?P<bytes>\d+))?"
)


class ShotError(ValueError):
    """A malformed manifest line or PNG."""


@dataclass(frozen=True)
class Shot:
    name: str
    poll: int
    present: int
    phase: str
    label: str
    hotkey: int
    width: int
    height: int
    unix: int | None = None
    bytes: int | None = None


def parse_line(text: str) -> Shot:
    match = LINE_RE.fullmatch(text.strip())
    if match is None:
        raise ShotError(f"bad manifest line: {text!r}")
    fields = match.groupdict()
    return Shot(
        name=fields["name"],
        poll=int(fields["poll"]),
        present=int(fields["present"]),
        phase=fields["phase"],
        label=fields["label"],
        hotkey=int(fields["hotkey"]),
        width=int(fields["width"]),
        height=int(fields["height"]),
        unix=None if fields["unix"] is None else int(fields["unix"]),
        bytes=None if fields["bytes"] is None else int(fields["bytes"]),
    )


def format_line(shot: Shot) -> str:
    """The inverse of parse_line, the exact format of shot_manifest_format in C."""
    line = (
        f"{shot.name} poll={shot.poll} present={shot.present} phase={shot.phase} label={shot.label}"
        f" hotkey={shot.hotkey} size={shot.width}x{shot.height}"
    )
    if shot.unix is not None:
        line += f" unix={shot.unix}"
    if shot.bytes is not None:
        line += f" bytes={shot.bytes}"
    return line


def read_manifest(directory: Path) -> list[Shot]:
    path = directory / MANIFEST
    if not path.is_file():
        raise ShotError(f"no {MANIFEST} in {directory}")
    return [parse_line(line) for line in path.read_text().splitlines() if line.strip()]


def decode_png(data: bytes) -> tuple[int, int, bytes]:
    """8 bit RGB PNG (filter 0 or any, only filter 0 is produced by the host) -> (width, height, rgb). Checks every CRC."""
    if not data.startswith(PNG_SIGNATURE):
        raise ShotError("not a PNG (bad signature)")
    at = len(PNG_SIGNATURE)
    width = height = 0
    idat = b""
    ended = False
    while at + 12 <= len(data):
        (length,) = struct.unpack(">I", data[at : at + 4])
        kind = data[at + 4 : at + 8]
        body = data[at + 8 : at + 8 + length]
        if len(body) != length or at + 12 + length > len(data):
            raise ShotError(f"bad CRC or truncated chunk {kind!r}")
        (crc,) = struct.unpack(">I", data[at + 8 + length : at + 12 + length])
        if crc != zlib.crc32(kind + body):
            raise ShotError(f"bad CRC or truncated chunk {kind!r}")
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            if depth != 8 or colour != 2:
                raise ShotError("only 8 bit RGB is supported")
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            ended = True
        at += 12 + length
    if not ended or not width or not height:
        raise ShotError("missing IHDR or IEND")
    raw = zlib.decompress(idat)
    stride = width * 3 + 1
    if len(raw) != stride * height:
        raise ShotError("pixel data size does not match the header")
    rows = []
    for y in range(height):
        if raw[y * stride] != 0:
            raise ShotError("filtered rows are not supported")
        rows.append(raw[y * stride + 1 : (y + 1) * stride])
    return width, height, b"".join(rows)


def encode_png(width: int, height: int, rgb: bytes) -> bytes:
    """Reference encoder (zlib), used by the tests and to build expected files; the host has its own C writer."""
    if len(rgb) != width * height * 3:
        raise ShotError("rgb size does not match width*height*3")
    raw = b"".join(b"\0" + rgb[y * width * 3 : (y + 1) * width * 3] for y in range(height))

    def chunk(kind: bytes, body: bytes) -> bytes:
        return (
            struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
        )

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (
        PNG_SIGNATURE
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(raw))
        + chunk(b"IEND", b"")
    )


def is_blank(rgb: bytes) -> bool:
    """True when every pixel equals the first one (an empty picture counts as blank)."""
    return not rgb or rgb == rgb[:3] * (len(rgb) // 3)


def inspect(directory: Path) -> list[tuple[Shot, str]]:
    """(shot, problem) for every manifest line, problem is '' when the PNG is valid, has the manifest size and is not blank."""
    results = []
    for shot in read_manifest(directory):
        path = directory / shot.name
        problem = ""
        try:
            width, height, rgb = decode_png(path.read_bytes())
            if (width, height) != (shot.width, shot.height):
                problem = f"PNG is {width}x{height}, manifest says {shot.width}x{shot.height}"
            elif is_blank(rgb):
                problem = "BLANK (one colour only)"
        except (OSError, ShotError, zlib.error) as error:
            problem = f"unreadable: {error}"
        results.append((shot, problem))
    return results


def format_bytes(count: int | None) -> str:
    return "-" if count is None else str(count)


def format_unix(seconds: int | None) -> str:
    if seconds is None:
        return "-"
    return datetime.fromtimestamp(seconds, tz=UTC).strftime("%Y-%m-%d %H:%M:%S")


def report_lines(directory: Path) -> list[str]:
    results = inspect(directory)
    extras = any(shot.unix is not None or shot.bytes is not None for shot, _ in results)
    header = f"{'shot':<14} {'poll':>8} {'present':>8} {'phase':<24} {'size':<9} check"
    lines = [header + (f"  {'bytes':>8} time (UTC)" if extras else "")]
    for shot, problem in results:
        size = f"{shot.width}x{shot.height}"
        line = f"{shot.name:<14} {shot.poll:>8} {shot.present:>8} {shot.phase:<24} {size:<9} {problem or 'ok'}"
        if extras:
            line += f"  {format_bytes(shot.bytes):>8} {format_unix(shot.unix)}"
        lines.append(line)
    bad = sum(1 for _, problem in results if problem)
    lines.append(f"{len(results)} shot(s), {bad} with a problem")
    return lines


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.render_shots",
        description="T1720: list and check the shot-NNN.png files of a render-<stamp> folder against shots.manifest.",
    )
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("report", "check"):
        command = sub.add_parser(name)
        command.add_argument("directory", type=Path)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        lines = report_lines(args.directory)
        results = inspect(args.directory)
    except ShotError as error:
        print(f"render_shots: {error}", file=sys.stderr)
        return 2
    print("\n".join(lines))
    failed = any(problem for _, problem in results)
    return 1 if args.command == "check" and failed else 0


if __name__ == "__main__":
    sys.exit(main())
