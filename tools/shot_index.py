#!/usr/bin/env python3
# ruff: noqa: E501
"""T1720b: index and contact sheet of the `shot` hotkey pictures of one session folder.

The host (src/host/shot_hotkey.c) writes shot-NNN.png and shots.manifest into the shot folder of a run
(tmp/owner-profiles/render-<stamp>/). This tool turns that folder into three files Claude (or the owner) can read at a glance:

    python -m tools.shot_index DIR [--out-jsonl PATH] [--sheet-html PATH] [--sheet-png PATH]
                                   [--columns N] [--thumb-width W] [--labels-file FILE]

    DIR/shots.jsonl          one JSON object per manifest line: name, number, poll, present, phase, label, hotkey, caption, unix, bytes,
                             width, height, sha256 (of the PNG file), blank, problem ('' when the picture is fine)
    DIR/contact-sheet.html   self contained page, a grid of the pictures (relative <img src> to the PNGs, captions HTML escaped)
    DIR/contact-sheet.png    one montage PNG of integer box averaged thumbnails, the shot number stamped in the top left corner of
                             each cell (cell k = manifest line k, left to right, top to bottom). View it with the Read tool.

Captions: the phase of the manifest line (the first line of DIR/phase when the picture was taken; '-' = none) unless DIR/labels.txt
(or --labels-file) overrides it. One line per picture, `N text` or `shot-NNN.png text`, `#` starts a comment line:

    4 radar missing, two enemies on screen
    shot-007.png health bar after the rocket hit

Exit codes: 0 all pictures fine, 1 a picture is blank, unreadable or does not match the manifest (every output is still written),
2 a missing folder or manifest, more than 400 pictures, a montage that cannot fit 8192x8192 even with the smallest thumbnails,
a bad labels file argument or an output that cannot be written. Outputs are written atomically (temp file in the same folder,
then os.replace), so an interrupted run never leaves a half file.
"""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import os
import re
import sys
import tempfile
import urllib.parse
import zlib
from collections.abc import Iterable
from dataclasses import dataclass
from pathlib import Path

from tools import render_shots

JSONL_NAME = "shots.jsonl"
HTML_NAME = "contact-sheet.html"
PNG_NAME = "contact-sheet.png"
LABELS_NAME = "labels.txt"
MAX_SHOTS = 400
MAX_SIDE = 8192
MAX_FACTOR = 16  # box factor limit: 32 bit lanes hold f*f*255 and the multiply-shift divide stays exact below 16
GAP = 4
BACKGROUND = (28, 28, 32)
UNREADABLE = (150, 20, 20)
STAMP_SCALE = 3
STAMP_PAD = 2
LABEL_LINE_RE = re.compile(r"(?P<key>shot-\d{3,}\.png|\d+)\s+(?P<text>\S.*)")

# 3x5 digit font, one string per row, '#' = on
DIGITS = {
    "0": ("###", "# #", "# #", "# #", "###"),
    "1": (" # ", "## ", " # ", " # ", "###"),
    "2": ("###", "  #", "###", "#  ", "###"),
    "3": ("###", "  #", "###", "  #", "###"),
    "4": ("# #", "# #", "###", "  #", "  #"),
    "5": ("###", "#  ", "###", "  #", "###"),
    "6": ("###", "#  ", "###", "# #", "###"),
    "7": ("###", "  #", "  #", "  #", "  #"),
    "8": ("###", "# #", "###", "# #", "###"),
    "9": ("###", "# #", "###", "  #", "###"),
}


class ShotIndexError(Exception):
    """A refusal (exit 2): bad folder, too many shots, a montage that cannot fit, an unwritable output."""


@dataclass
class Thumb:
    width: int
    height: int
    rgb: bytes


@dataclass
class Entry:
    shot: render_shots.Shot
    number: int
    caption: str
    sha256: str | None
    width: int
    height: int
    blank: bool
    problem: str
    thumb: Thumb | None


# ------------------------------------------------------------------------------------------------ downscale
def downscale(width: int, height: int, rgb: bytes, factor: int) -> Thumb:
    """Integer box average by `factor` (a 1..16 block of pixels -> one pixel) without a per-pixel Python loop.

    Each colour plane is spread into 32 bit lanes of one big integer. Shifting by whole rows and then whole lanes and adding sums the
    factor x factor block in every lane at once, a multiply and a shift divides, and slicing picks every factor-th lane.
    """
    if not 1 <= factor <= MAX_FACTOR:
        raise ValueError(f"factor {factor} outside 1..{MAX_FACTOR}")
    out_w, out_h = width // factor, height // factor
    if factor == 1 or out_w == 0 or out_h == 0:
        return Thumb(width, height, rgb)
    area = factor * factor
    multiplier = (65536 + area - 1) // area
    planes = []
    for channel in range(3):
        lanes = bytearray(4 * width * height)
        lanes[0::4] = rgb[channel::3]
        wide = int.from_bytes(lanes, "little")
        vertical = wide
        for step in range(1, factor):
            vertical += wide >> (step * width * 32)
        both = vertical
        for step in range(1, factor):
            both += vertical >> (step * 32)
        scaled = (both * multiplier) >> 16
        raw = scaled.to_bytes(4 * width * height, "little")
        rows = [
            raw[
                (row * factor * width) * 4 : (row * factor * width + out_w * factor) * 4 : factor
                * 4
            ]
            for row in range(out_h)
        ]
        planes.append(b"".join(rows))
    out = bytearray(out_w * out_h * 3)
    for channel in range(3):
        out[channel::3] = planes[channel]
    return Thumb(out_w, out_h, bytes(out))


def stamp_number(thumb: Thumb, number: int) -> Thumb:
    """White digits on a black box in the top left corner of the thumbnail (3x5 font, scaled), clipped to the thumbnail."""
    text = str(number)
    glyph_w = (3 * STAMP_SCALE) + STAMP_SCALE
    box_w = STAMP_PAD * 2 + len(text) * glyph_w - STAMP_SCALE
    box_h = STAMP_PAD * 2 + 5 * STAMP_SCALE
    pixels = bytearray(thumb.rgb)
    stride = thumb.width * 3
    for y in range(min(box_h, thumb.height)):
        row = y * stride
        end = min(box_w, thumb.width) * 3
        pixels[row : row + end] = bytes(end)
    for index, char in enumerate(text):
        for glyph_row, line in enumerate(DIGITS[char]):
            for glyph_col, cell in enumerate(line):
                if cell != "#":
                    continue
                for dy in range(STAMP_SCALE):
                    y = STAMP_PAD + glyph_row * STAMP_SCALE + dy
                    if y >= thumb.height:
                        continue
                    x0 = STAMP_PAD + index * glyph_w + glyph_col * STAMP_SCALE
                    x1 = min(x0 + STAMP_SCALE, thumb.width)
                    if x0 < x1:
                        pixels[y * stride + x0 * 3 : y * stride + x1 * 3] = b"\xff" * (
                            (x1 - x0) * 3
                        )
    return Thumb(thumb.width, thumb.height, bytes(pixels))


# ------------------------------------------------------------------------------------------------ labels
def read_labels(path: Path | None, warnings: list[str]) -> dict[str, str]:
    """name (shot-NNN.png) -> caption text. A line `N text` or `shot-NNN.png text`; a later line wins."""
    if path is None or not path.is_file():
        return {}
    labels: dict[str, str] = {}
    for line_number, line in enumerate(
        path.read_text(encoding="utf-8", errors="replace").splitlines(), 1
    ):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        match = LABEL_LINE_RE.fullmatch(stripped)
        if match is None:
            warnings.append(
                f"{path.name}:{line_number}: ignored (want 'N text' or 'shot-NNN.png text')"
            )
            continue
        key = match["key"]
        name = key if key.startswith("shot-") else f"shot-{int(key):03d}.png"
        labels[name] = match["text"].strip()
    return labels


def shot_number(name: str) -> int:
    return int(name[len("shot-") : -len(".png")])


# ------------------------------------------------------------------------------------------------ inspect
def choose_factor(
    shots: list[render_shots.Shot], thumb_width: int, columns: int
) -> tuple[int, int, int]:
    """(factor, cell width, cell height) so the montage fits MAX_SIDE x MAX_SIDE, thumbnails as large as possible."""
    widest = max((shot.width for shot in shots), default=1) or 1
    tallest = max((shot.height for shot in shots), default=1) or 1
    rows = -(-len(shots) // columns) if shots else 1
    factor = max(1, -(-widest // thumb_width))
    while factor <= MAX_FACTOR:
        cell_w, cell_h = max(1, widest // factor), max(1, tallest // factor)
        total_w = GAP + columns * (cell_w + GAP)
        total_h = GAP + rows * (cell_h + GAP)
        if total_w <= MAX_SIDE and total_h <= MAX_SIDE:
            return factor, cell_w, cell_h
        factor += 1
    raise ShotIndexError(
        f"{len(shots)} shots in {columns} column(s) do not fit {MAX_SIDE}x{MAX_SIDE} even at 1/{MAX_FACTOR} size, use more --columns"
    )


def inspect_shot(
    directory: Path, shot: render_shots.Shot, factor: int, captions: dict[str, str]
) -> Entry:
    path = directory / shot.name
    number = shot_number(shot.name)
    override = captions.get(shot.name)
    caption = override if override is not None else ("" if shot.phase == "-" else shot.phase)
    sha256: str | None = None
    width, height = shot.width, shot.height
    blank = False
    problem = ""
    thumb: Thumb | None = None
    try:
        data = path.read_bytes()
        sha256 = hashlib.sha256(data).hexdigest()
        width, height, rgb = render_shots.decode_png(data)
        blank = render_shots.is_blank(rgb)
        if (width, height) != (shot.width, shot.height):
            problem = f"PNG is {width}x{height}, manifest says {shot.width}x{shot.height}"
        elif shot.bytes is not None and shot.bytes != len(data):
            problem = f"file is {len(data)} bytes, manifest says {shot.bytes}"
        elif blank:
            problem = "BLANK (one colour only)"
        thumb = downscale(width, height, rgb, factor)
    except (OSError, render_shots.ShotError, zlib.error) as error:
        problem = f"unreadable: {error}"
    return Entry(shot, number, caption, sha256, width, height, blank, problem, thumb)


# ------------------------------------------------------------------------------------------------ outputs
def jsonl_text(entries: Iterable[Entry]) -> str:
    lines = []
    for entry in entries:
        shot = entry.shot
        record = {
            "name": shot.name,
            "number": entry.number,
            "poll": shot.poll,
            "present": shot.present,
            "phase": shot.phase,
            "label": shot.label,
            "hotkey": shot.hotkey,
            "caption": entry.caption,
            "unix": shot.unix,
            "bytes": shot.bytes,
            "width": entry.width,
            "height": entry.height,
            "sha256": entry.sha256,
            "blank": entry.blank,
            "problem": entry.problem,
        }
        lines.append(json.dumps(record, ensure_ascii=True))
    return "\n".join(lines) + ("\n" if lines else "")


def montage_png(
    entries: list[Entry], columns: int, cell_w: int, cell_h: int
) -> tuple[int, int, bytes]:
    rows = -(-len(entries) // columns) if entries else 1
    total_w = GAP + columns * (cell_w + GAP)
    total_h = GAP + rows * (cell_h + GAP)
    canvas = bytearray(bytes(BACKGROUND) * (total_w * total_h))
    for index, entry in enumerate(entries):
        left = GAP + (index % columns) * (cell_w + GAP)
        top = GAP + (index // columns) * (cell_h + GAP)
        if entry.thumb is None:
            cell = Thumb(cell_w, cell_h, bytes(UNREADABLE) * (cell_w * cell_h))
        else:
            cell = entry.thumb
        cell = stamp_number(cell, entry.number)
        copy_w = min(cell.width, cell_w)
        for y in range(min(cell.height, cell_h)):
            start = ((top + y) * total_w + left) * 3
            canvas[start : start + copy_w * 3] = cell.rgb[
                y * cell.width * 3 : y * cell.width * 3 + copy_w * 3
            ]
    return total_w, total_h, render_shots.encode_png(total_w, total_h, bytes(canvas))


def html_text(
    entries: list[Entry], directory: Path, html_path: Path, columns: int, thumb_width: int
) -> str:
    base = Path(os.path.relpath(directory.resolve(), html_path.resolve().parent))
    cells = []
    for entry in entries:
        shot = entry.shot
        source = urllib.parse.quote((base / shot.name).as_posix())
        problem = (
            f'<div class="problem">{html.escape(entry.problem)}</div>' if entry.problem else ""
        )
        caption = html.escape(entry.caption) if entry.caption else "&nbsp;"
        cells.append(
            "<figure>"
            f'<a href="{source}"><img src="{source}" alt="{html.escape(shot.name)}" loading="lazy"></a>'
            f"<figcaption><b>#{entry.number}</b> poll {shot.poll} present {shot.present}"
            f' phase {html.escape(shot.phase)}<div class="caption">{caption}</div>{problem}</figcaption>'
            "</figure>"
        )
    title = html.escape(f"{len(entries)} shot(s) of {directory.name}")
    return (
        '<!DOCTYPE html>\n<html><head><meta charset="utf-8">'
        f"<title>{title}</title>\n"
        "<style>body{font:14px sans-serif;background:#1c1c20;color:#ddd;margin:12px}"
        f".grid{{display:grid;grid-template-columns:repeat({columns},minmax(0,1fr));gap:10px}}"
        "figure{margin:0}img{width:100%;height:auto;display:block;background:#000;image-rendering:auto}"
        "figcaption{padding:3px 0}.caption{color:#fff}.problem{color:#f66;font-weight:bold}"
        f"</style></head><body>\n<h1>{title}</h1>\n"
        f"<!-- thumbnails scale to the page, montage thumb width {thumb_width} -->\n"
        '<div class="grid">\n' + "\n".join(cells) + "\n</div>\n</body></html>\n"
    )


def write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    handle, temporary = tempfile.mkstemp(dir=path.parent, prefix=path.name + ".", suffix=".new")
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(data)
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


# ------------------------------------------------------------------------------------------------ main
def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m tools.shot_index",
        description=f"T1720b: write {JSONL_NAME}, {HTML_NAME} and {PNG_NAME} for the shot-NNN.png pictures of a session folder. "
        "Exit 0 ok, 1 a picture is blank or unreadable (outputs are still written), 2 missing folder or manifest or a refusal.",
    )
    parser.add_argument("directory", type=Path, help="folder with shots.manifest and shot-NNN.png")
    parser.add_argument("--out-jsonl", type=Path, help=f"default DIR/{JSONL_NAME}")
    parser.add_argument("--sheet-html", type=Path, help=f"default DIR/{HTML_NAME}")
    parser.add_argument("--sheet-png", type=Path, help=f"default DIR/{PNG_NAME}")
    parser.add_argument(
        "--columns", type=int, default=4, help="thumbnails per row, 1 to 64 (default 4)"
    )
    parser.add_argument(
        "--thumb-width",
        type=int,
        default=320,
        help="widest thumbnail in pixels, 16 to 2048 (default 320)",
    )
    parser.add_argument(
        "--labels-file", type=Path, help=f"caption overrides, default DIR/{LABELS_NAME}"
    )
    return parser


def run(args: argparse.Namespace) -> int:
    directory: Path = args.directory
    if not directory.is_dir():
        raise ShotIndexError(f"no such folder: {directory}")
    if not 1 <= args.columns <= 64:
        raise ShotIndexError("--columns must be 1 to 64")
    if not 16 <= args.thumb_width <= 2048:
        raise ShotIndexError("--thumb-width must be 16 to 2048")
    try:
        shots = render_shots.read_manifest(directory)
    except render_shots.ShotError as error:
        raise ShotIndexError(str(error)) from error
    if len(shots) > MAX_SHOTS:
        raise ShotIndexError(f"{len(shots)} shots in the manifest, at most {MAX_SHOTS} are indexed")
    labels_path = args.labels_file
    if labels_path is not None and not labels_path.is_file():
        raise ShotIndexError(f"no such labels file: {labels_path}")
    warnings: list[str] = []
    captions = read_labels(labels_path or directory / LABELS_NAME, warnings)
    known = {shot.name for shot in shots}
    for name in sorted(set(captions) - known):
        warnings.append(f"label for {name} ignored: not in {render_shots.MANIFEST}")
    factor, cell_w, cell_h = choose_factor(shots, args.thumb_width, args.columns)
    entries = [inspect_shot(directory, shot, factor, captions) for shot in shots]
    jsonl_path = args.out_jsonl or directory / JSONL_NAME
    html_path = args.sheet_html or directory / HTML_NAME
    png_path = args.sheet_png or directory / PNG_NAME
    width, height, png = montage_png(entries, args.columns, cell_w, cell_h)
    try:
        write_atomic(jsonl_path, jsonl_text(entries).encode("utf-8"))
        write_atomic(
            html_path,
            html_text(entries, directory, html_path, args.columns, args.thumb_width).encode(
                "utf-8"
            ),
        )
        write_atomic(png_path, png)
    except OSError as error:
        raise ShotIndexError(f"cannot write an output: {error}") from error
    for warning in warnings:
        print(f"shot_index: warning: {warning}", file=sys.stderr)
    bad = [entry for entry in entries if entry.problem]
    for entry in bad:
        print(f"shot_index: {entry.shot.name}: {entry.problem}")
    print(f"{len(entries)} shot(s), {len(bad)} with a problem")
    print(f"index:         {jsonl_path}")
    print(f"contact sheet: {html_path}")
    print(
        f"montage:       {png_path} ({width}x{height}, thumbnails 1/{factor}, {args.columns} column(s))"
    )
    return 1 if bad else 0


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return run(args)
    except ShotIndexError as error:
        print(f"shot_index: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
