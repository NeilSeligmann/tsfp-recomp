# SPDX-License-Identifier: GPL-3.0-or-later
"""Frozen synthetic native build recipes; no compiler/process execution on import."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .provenance import tree_digest
from .scoped_fixture_authority import digest

ROOT = Path(__file__).resolve().parents[2]
MUTANTS = ("eax-payload", "guest-write-omission", "cleanup-delta")
PATCHES = {
    "eax-payload": (
        "    g_eax = guest_read32(0x20010u + 4u * g_eax);",
        "    g_eax = guest_read32(0x20010u + 4u * g_eax);\n    g_eax ^= 1u;",
    ),
    "guest-write-omission": (
        "    guest_write32(0xDC0004u, g_edx);",
        "    /* Named negative control: omit the guest store. */",
    ),
    "cleanup-delta": (
        "    guest_write32(0xDC0004u, g_edx);",
        "    guest_write32(0xDC0004u, g_edx);\n    g_esp += 4u;",
    ),
}


@dataclass(frozen=True)
class BuildRecipe:
    opt: str
    mutant: str | None
    source: bytes
    dispatch: bytes
    source_digest: str
    source_pins: tuple[tuple[str, str], ...]

    def materialize(self, directory: Path) -> tuple[Path, Path]:
        """Fresh source sink only; this does not compile or execute anything."""
        directory.mkdir(parents=True, exist_ok=False)
        source, dispatch = directory / "replacements.c", directory / "dispatch.c"
        source.write_bytes(self.source)
        dispatch.write_bytes(self.dispatch)
        return source, dispatch

    def command(self, directory: Path, binary: Path) -> list[str]:
        """Explicit serial command; orchestration must enforce runtime/deadline pins."""
        if (
            self.opt not in ("O0", "O3")
            or not isinstance(directory, Path)
            or not isinstance(binary, Path)
        ):
            raise ValueError("frozen synthetic compile identity required")
        source = directory / "replacements.c"
        dispatch = directory / "dispatch.c"
        if source.read_bytes() != self.source or dispatch.read_bytes() != self.dispatch:
            raise ValueError("materialized synthetic compile source drift")
        return [
            "cc",
            f"-{self.opt}",
            "-fPIE",
            "-pie",
            "-DHARNESS_REPLACEMENT=1",
            "-I",
            str(ROOT / "src/game"),
            "-I",
            str(ROOT / "tools/harness"),
            f'-DHARNESS_TREE_SHA="{self.source_digest[:16]}"',
            '-DHARNESS_GEN_DIR="synthetic-t1550"',
            f'-DHARNESS_REPL_SHA="{tree_digest(directory)[:16]}"',
            *(
                str(ROOT / relative)
                for relative in (
                    "tools/harness/driver.c",
                    "tools/harness/call_stub.c",
                    "tools/harness/runtime_min.c",
                    "src/game/game_registry.c",
                )
            ),
            str(source.resolve()),
            str(dispatch.resolve()),
            "-lm",
            "-o",
            str(binary.resolve()),
        ]


def recipe(opt: str, mutant: str | None = None) -> BuildRecipe:
    if opt not in ("O0", "O3") or mutant is not None and mutant not in MUTANTS:
        raise ValueError("unsupported frozen build/mutation")
    source = (ROOT / "tests/c/t1550/replacements.c").read_bytes()
    dispatch = (ROOT / "tests/c/t1550/dispatch.c").read_bytes()
    if mutant is not None:
        before, after = (part.encode() for part in PATCHES[mutant])
        if source.count(before) != 1:
            raise ValueError("named native mutant source seam changed")
        source = source.replace(before, after)
    pins = tuple(
        (name, digest((ROOT / name).read_bytes()))
        for name in (
            "tests/c/t1550/replacements.c",
            "tests/c/t1550/dispatch.c",
            "tools/harness/driver.c",
            "tools/harness/call_stub.c",
            "tools/harness/runtime_min.c",
            "src/game/game_registry.c",
            "src/game/game_replace.h",
            "src/game/game_guest.h",
        )
    )
    # Pins are also retained independently; source identity includes the actual mutant bytes.
    from .scoped_fixture_authority import canonical

    identity = digest(canonical((opt, mutant, source, dispatch, pins)))
    return BuildRecipe(opt, mutant, source, dispatch, identity, pins)
