# SPDX-License-Identifier: GPL-3.0-or-later
"""Recover plaintext asset paths for hash-keyed PAK entries.

`P5CK` archives key entries by zlib.crc32 of the asset path, case preserved, so
names are recoverable only by hashing candidate paths and matching.

The primary source is the `.c2n` sidecar: every `.pak` on the GameCube disc ships
one, a plaintext CRC-to-path index. Measured across the real disc, 336 files gave
18,545 entries, all satisfying zlib.crc32(path) == key, naming 100% of the
GameCube archives' keys.

Because GameCube asset paths embed the platform, the same index names the PS2 and
Xbox archives after a mechanical transform. Measured coverage against the real
discs using only the GameCube index:

    Xbox  anim.pak    77 / 77   100.0%
    Xbox  gun.pak    842 / 969   86.9%
    Xbox  chr.pak    717 / 911   78.7%
    PS2   anim+gun   991 / 991  100.0%
"""

from __future__ import annotations

import re
import zlib
from collections.abc import Iterable
from pathlib import Path

from tools.pak.model import PakArchive

# PS2 and GameCube use identical paths except for the geometry extension.
EXTENSION_SIBLINGS = {".raw": ".war", ".war": ".raw"}

# GameCube paths embed the platform, e.g.
#   assets/ts/models/chrs/<name>/gamecube/output/<name>.gcr
# and the Xbox build uses the sibling directory and extensions. The XBE itself
# contains the format string assets/ts/models/%s/xbox/output/%s.xbr, confirming
# this is the shipped convention rather than a guess.
PLATFORM_DIRS = {
    "gamecube": ("xbox", "ps2"),
    "xbox": ("gamecube", "ps2"),
    "ps2": ("gamecube", "xbox"),
}
PLATFORM_EXTENSIONS = {
    "gamecube": {".gcr": ".gcr", ".gct": ".gct", ".war": ".war", ".dsp": ".dsp"},
    "xbox": {".gcr": ".xbr", ".gct": ".xbt", ".war": ".raw", ".dsp": ".wav"},
    "ps2": {".gcr": ".raw", ".gct": ".raw", ".war": ".raw", ".dsp": ".vag"},
}

# A .c2n line is a hex key, whitespace, then the path. Paths may contain spaces,
# so the path group runs to end-of-line rather than \S+.
_C2N_LINE = re.compile(r"^\s*0[xX]([0-9a-fA-F]{1,8})\s+(\S.*?)\s*$")


def parse_c2n(text: str) -> dict[int, str]:
    """Parse a `.c2n` index into a key -> path mapping.

    Malformed and blank lines are skipped rather than raising, because these are
    build artefacts and one bad line should not discard a whole index.
    """
    mapping: dict[int, str] = {}
    for line in text.splitlines():
        match = _C2N_LINE.match(line)
        if match:
            mapping[int(match.group(1), 16)] = match.group(2)
    return mapping


def load_c2n_dir(root: Path) -> dict[int, str]:
    """Merge every `.c2n` index found anywhere under `root`. First writer wins."""
    merged: dict[int, str] = {}
    for path in sorted(root.rglob("*.c2n")):
        if not path.is_file():
            continue
        for key, name in parse_c2n(path.read_text(encoding="latin-1")).items():
            merged.setdefault(key, name)
    return merged


def pak_key(path: str) -> int:
    """The P5CK table-of-contents key for `path`. Case is significant."""
    return zlib.crc32(path.encode())


def _swap_extension(path: str, mapping: dict[str, str]) -> str:
    for source, target in mapping.items():
        if path.endswith(source):
            return path[: -len(source)] + target
    return path


def translate_platform(path: str, target: str) -> str | None:
    """Rewrite a GameCube-style asset path for another platform.

    Two independent rewrites are applied, and either alone is sufficient:

    * the platform directory component, where present
      (`.../gamecube/output/...` -> `.../xbox/output/...`)
    * the platform-specific extension, always
      (`.gcr` -> `.xbr`, `.gct` -> `.xbt`, `.war` -> `.raw`, `.dsp` -> `.wav`)

    Gating the extension swap on finding a directory component is a mistake worth
    naming: texture paths are bare (`textures/cc95c8.gct`) and carry no platform
    directory at all, yet still need `.gct` -> `.xbt`. Doing so dropped real Xbox
    coverage from 86.9% to 55.2% on `gun.pak`.

    Returns None when neither rewrite changes anything.
    """
    if target not in PLATFORM_EXTENSIONS:
        raise KeyError(f"unknown platform: {target!r}")
    result = path
    for source in PLATFORM_DIRS:
        marker = f"/{source}/"
        if marker in result and source != target:
            result = result.replace(marker, f"/{target}/")
            break
    result = _swap_extension(result, PLATFORM_EXTENSIONS[target])
    return result if result != path else None


def expand_variants(path: str) -> list[str]:
    """Return `path` plus its cross-platform siblings, original first."""
    variants = [path]

    def add(candidate: str) -> None:
        if candidate and candidate not in variants:
            variants.append(candidate)

    add(_swap_extension(path, EXTENSION_SIBLINGS))
    for target in PLATFORM_EXTENSIONS:
        translated = translate_platform(path, target)
        if translated:
            add(translated)
            add(_swap_extension(translated, EXTENSION_SIBLINGS))
    return variants


def build_dictionary(paths: Iterable[str]) -> dict[int, str]:
    """Map key -> path for every candidate and its variants. First writer wins."""
    dictionary: dict[int, str] = {}
    for path in paths:
        for variant in expand_variants(path):
            dictionary.setdefault(pak_key(variant), variant)
    return dictionary


def resolve(keys: Iterable[int], dictionary: dict[int, str]) -> tuple[dict[int, str], list[int]]:
    """Split `keys` into those the dictionary names and those it does not."""
    resolved: dict[int, str] = {}
    unresolved: list[int] = []
    for key in keys:
        name = dictionary.get(key)
        if name is None:
            unresolved.append(key)
        else:
            resolved[key] = name
    return resolved, unresolved


def harvest_plaintext(archives: Iterable[PakArchive]) -> list[str]:
    """Collect every plaintext entry name from the given archives."""
    names: list[str] = []
    for archive in archives:
        for entry in archive.entries:
            if entry.name:
                names.append(entry.name)
    return names
