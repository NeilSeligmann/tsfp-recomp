# SPDX-License-Identifier: GPL-3.0-or-later
"""Standard location, canonical naming, and verification for game discs.

Discs are never committed. Place your own dumps in `discs/` at the repository
root, or point TSFP_DISC_DIR elsewhere. Canonical names are used instead of
redump-style filenames, which vary by dumper and region.

Sizes and hashes were measured from the dumps used during the design phase. If
yours differ, record yours — the registry identifies the dump this checkout is
developed against, and a mismatch is information rather than an error to suppress.
"""

from __future__ import annotations

import hashlib
import os
from dataclasses import dataclass
from pathlib import Path

DEFAULT_DISC_DIR = Path("discs")
DISC_DIR_ENV = "TSFP_DISC_DIR"
_HASH_CHUNK = 1 << 24


class DiscMismatch(Exception):
    """A disc file did not match its registry entry."""


@dataclass(frozen=True)
class DiscSpec:
    key: str
    filename: str
    title: str
    platform: str
    size: int
    sha256: str | None
    role: str


REGISTRY: dict[str, DiscSpec] = {
    "tsfp-xbox": DiscSpec(
        key="tsfp-xbox",
        filename="tsfp-xbox.iso",
        title="TimeSplitters: Future Perfect (USA)",
        platform="xbox",
        size=4_047_372_288,
        sha256="427421176d5d1e045e4423947968c392a8efd82c06cafd99703817b0f42ca193",
        role="primary decompilation target",
    ),
    "tsfp-ps2": DiscSpec(
        key="tsfp-ps2",
        filename="tsfp-ps2.iso",
        title="TimeSplitters: Future Perfect (USA)",
        platform="ps2",
        size=4_383_899_648,
        sha256="db9b6d86e8006caee69f1f7b6e21d7bf61c4ae66afb6757a8dad9843dfcadc34",
        role="structural cross-reference",
    ),
    "tsfp-gc": DiscSpec(
        key="tsfp-gc",
        filename="tsfp-gc.rvz",
        title="TimeSplitters: Future Perfect (USA)",
        platform="gamecube",
        size=1_046_840_276,
        # Deliberately None: RVZ is a compressed container, so its file hash
        # tracks compression settings rather than disc content. Two RVZ files with
        # different hashes can hold identical discs. GameCube content identity
        # comes from the embedded game ID G3FE69. Do not "fix" this by adding one.
        sha256=None,
        role="structural cross-reference; sole source of .c2n name indexes",
    ),
    "ts2-ps2": DiscSpec(
        key="ts2-ps2",
        filename="ts2-ps2.iso",
        title="TimeSplitters 2 (Europe) (Beta)",
        platform="ps2",
        size=2_028_109_824,
        sha256="807c252177068f4ca65c41d2cd59a2160672a67a536964d6195b35ea9e7f0a41",
        role="asset-name donor (P4CK inline plaintext paths)",
    ),
    "ts2-demo53": DiscSpec(
        key="ts2-demo53",
        filename="ts2-demo53.iso",
        title="US OPM Demo Disc 53 (SCUS-97176)",
        platform="ps2",
        size=3_955_916_800,
        # Not yet hashed; add once verified against a second dump.
        sha256=None,
        role=(
            "SYMBOL DONOR. Carries TS2/SLUS_999.99 (2001-10-05), which shipped "
            "unstripped: a 7.3 MB .mdebug stabs section and 8,099 symbols. "
            "Yields 394 struct definitions with 3,672 named, offset-annotated "
            "fields and 3,976 function signatures. The richest Free Radical "
            "engine documentation that exists."
        ),
    ),
}


def disc_dir() -> Path:
    """The directory holding disc images."""
    override = os.environ.get(DISC_DIR_ENV)
    return Path(override) if override else DEFAULT_DISC_DIR


def find_disc(key: str) -> Path | None:
    """Return the path to a registered disc, or None if it is not present."""
    spec = REGISTRY[key]
    candidate = disc_dir() / spec.filename
    return candidate if candidate.is_file() else None


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(_HASH_CHUNK):
            digest.update(chunk)
    return digest.hexdigest()


def verify_disc(path: Path, spec: DiscSpec, *, strict: bool = False) -> None:
    """Check a disc against its registry entry.

    Size is always checked. SHA-256 only when `strict`, because hashing several
    gigabytes is too slow for routine use.
    """
    actual_size = path.stat().st_size
    if actual_size != spec.size:
        raise DiscMismatch(
            f"{path}: size {actual_size} does not match expected {spec.size} for {spec.key}"
        )
    if strict and spec.sha256 is not None:
        actual = sha256_of(path)
        if actual != spec.sha256:
            raise DiscMismatch(
                f"{path}: sha256 {actual} does not match expected {spec.sha256} for {spec.key}"
            )


def available() -> dict[str, Path]:
    """Map key -> path for every registered disc that is present."""
    found: dict[str, Path] = {}
    for key in REGISTRY:
        path = find_disc(key)
        if path is not None:
            found[key] = path
    return found
