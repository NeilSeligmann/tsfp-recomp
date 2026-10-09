# SPDX-License-Identifier: GPL-3.0-or-later
"""Signature database: deduplicated masked bodies, stored as gzipped JSON.

The database holds library-derived bytes, so it may only be written to ignored
paths (see `tools.libsig.guard`). It is never committed.
"""

from __future__ import annotations

import gzip
import json
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

from tools.libsig.coff import FunctionBody, functions_in_archive

DB_VERSION = 2  # 2 (T341): relocs carry a 4th element, the DIR32 in-place addend


class DbVersionError(ValueError):
    """The database was written by an incompatible libsig version."""


@dataclass
class Signature:
    """One distinct masked body and every (symbol, lib, build) that carries it."""

    data: bytes
    mask: bytes
    relocs: list[tuple[int, int, str, int]]  # offset, kind, target, DIR32 addend
    origins: set[tuple[str, str, str, str, str]] = field(
        default_factory=set
    )  # name, lib, member, build, section

    @property
    def fixed_bytes(self) -> int:
        return sum(1 for value in self.mask if value)

    @property
    def prefix_len(self) -> int:
        """Bytes before the first wildcard, capped at 4: the index key length."""
        for position, value in enumerate(self.mask[:4]):
            if not value:
                return position
        return min(4, len(self.mask))


def _masked(data: bytes, mask: bytes) -> bytes:
    return bytes(a & b for a, b in zip(data, mask, strict=True))


class SignatureDb:
    def __init__(self) -> None:
        self.signatures: dict[tuple[bytes, bytes, tuple], Signature] = {}
        self.builds: set[str] = set()
        self.skipped_members = 0

    def add(self, body: FunctionBody, build: str) -> None:
        relocs = tuple((r.offset, r.kind, r.target, r.addend) for r in body.relocs)
        key = (_masked(body.data, body.mask), body.mask, relocs)
        sig = self.signatures.get(key)
        if sig is None:
            sig = Signature(bytes(key[0]), body.mask, list(relocs))
            self.signatures[key] = sig
        for name in body.names:
            sig.origins.add((name, body.lib, body.member, build, body.section))

    def add_archive(self, blob: bytes, lib: str, build: str) -> int:
        bodies, skipped = functions_in_archive(blob, lib)
        for body in bodies:
            self.add(body, build)
        self.builds.add(build)
        self.skipped_members += skipped
        return len(bodies)

    def function_names(self) -> set[str]:
        return {origin[0] for sig in self.signatures.values() for origin in sig.origins}

    def save(self, path: Path) -> None:
        payload = {
            "version": DB_VERSION,
            "builds": sorted(self.builds),
            "skipped_members": self.skipped_members,
            "signatures": [
                [s.data.hex(), s.mask.hex(), s.relocs, sorted(s.origins)]
                for s in self.signatures.values()
            ],
        }
        path.parent.mkdir(parents=True, exist_ok=True)
        with gzip.open(path, "wt", encoding="utf-8") as handle:
            json.dump(payload, handle)

    @classmethod
    def load(cls, path: Path) -> SignatureDb:
        with gzip.open(path, "rt", encoding="utf-8") as handle:
            payload = json.load(handle)
        if payload.get("version") != DB_VERSION:
            raise DbVersionError(
                f"{path}: signature db version {payload.get('version')} is not the supported "
                f"version {DB_VERSION} (relocation addends are missing): rebuild it with "
                "`python -m tools.libsig.cli build`"
            )
        db = cls()
        db.builds = set(payload["builds"])
        db.skipped_members = payload["skipped_members"]
        for data_hex, mask_hex, relocs, origins in payload["signatures"]:
            data, mask = bytes.fromhex(data_hex), bytes.fromhex(mask_hex)
            rel = [tuple(r) for r in relocs]
            sig = Signature(data, mask, rel, {tuple(o) for o in origins})
            db.signatures[(data, mask, tuple(rel))] = sig
        return db


def build_db(lib_paths: Iterable[tuple[Path, str]]) -> SignatureDb:
    """Build from (lib path, build label) pairs."""
    db = SignatureDb()
    for path, build in lib_paths:
        db.add_archive(path.read_bytes(), path.name, build)
    return db
