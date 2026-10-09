# SPDX-License-Identifier: GPL-3.0-or-later
"""Explicit source-pinned authoritative function-table layers for scoped guards."""

from __future__ import annotations

import csv
import hashlib
import io
from dataclasses import dataclass
from pathlib import Path
from tempfile import TemporaryDirectory

from tools.codediff.boundaries import FUNCTION_CSV_COLUMNS, load_function_table, load_functions

ROOT = Path(__file__).resolve().parents[2]
SOURCE_VERSION = "authoritative-explicit-function-layers-v1"


def digest(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def loader_pins() -> tuple[tuple[str, str], ...]:
    paths = [ROOT / "tools/__init__.py", ROOT / "tools/harness/__init__.py", Path(__file__)]
    paths.extend((ROOT / "tools/codediff").glob("*.py"))
    return tuple(sorted((str(path.relative_to(ROOT)), digest(path.read_bytes())) for path in paths))


# Bind the implementation that was imported, not a later replacement on disk.
LOADED_LOADER_PINS = loader_pins()


def check_loader() -> None:
    if loader_pins() != LOADED_LOADER_PINS:
        raise ValueError("authoritative loader source/dependency membership drift")


def pin_valid(value: object) -> bool:
    return (
        type(value) is str
        and len(value) == 64
        and all(character in "0123456789abcdef" for character in value)
    )


@dataclass(frozen=True, slots=True)
class EffectiveIndex:
    export: bytes
    overrides: bytes | None
    additions: bytes | None
    table: bytes
    implementation: tuple[tuple[str, str], ...]

    def __post_init__(self) -> None:
        if (
            type(self.export) is not bytes
            or type(self.table) is not bytes
            or any(
                value is not None and type(value) is not bytes
                for value in (self.overrides, self.additions)
            )
            or self.implementation != LOADED_LOADER_PINS
        ):
            raise ValueError("immutable authoritative effective-index inputs required")
        check_loader()
        if canonical_table(self.export, self.overrides, self.additions) != self.table:
            raise ValueError("effective table differs from authoritative loader")

    @classmethod
    def build(
        cls, export: bytes, overrides: bytes | None, additions: bytes | None
    ) -> EffectiveIndex:
        check_loader()
        return cls(
            export,
            overrides,
            additions,
            canonical_table(export, overrides, additions),
            LOADED_LOADER_PINS,
        )

    def document(self) -> dict[str, object]:
        return {
            "version": SOURCE_VERSION,
            "selection": "explicit-cli-paths-no-inferred-defaults",
            "roles": {
                name: {
                    "enabled": raw is not None,
                    "sha256": digest(raw) if raw is not None else None,
                }
                for name, raw in (
                    ("export", self.export),
                    ("overrides", self.overrides),
                    ("additions", self.additions),
                )
            },
            "effective_sha256": digest(self.table),
            "loader_pins": dict(self.implementation),
        }


def canonical_table(export: bytes, overrides: bytes | None, additions: bytes | None) -> bytes:
    """Use shared parsing/apply semantics on frozen bytes; never skip an enabled role."""
    if type(export) is not bytes or any(
        value is not None and type(value) is not bytes for value in (overrides, additions)
    ):
        raise ValueError("immutable actual source bytes required")
    with TemporaryDirectory(prefix="t1525-effective-index-") as directory:
        base = Path(directory)
        export_path = base / "export.csv"
        export_path.write_bytes(export)
        raw = load_functions(export_path)
        if len({function.entry_va for function in raw}) != len(raw):
            raise ValueError("duplicate raw original function entry")
        selected = []
        for name, value in (("overrides", overrides), ("additions", additions)):
            path = None if value is None else base / f"{name}.csv"
            if path is not None:
                path.write_bytes(value)
            selected.append(path)
        functions = load_function_table(export_path, *selected)
    if len({function.entry_va for function in functions}) != len(functions):
        raise ValueError("duplicate effective function entry")
    output = io.StringIO(newline="")
    writer = csv.writer(output, lineterminator="\n")
    writer.writerow(FUNCTION_CSV_COLUMNS)
    for function in sorted(functions, key=lambda item: item.entry_va):
        writer.writerow(
            (
                f"0x{function.entry_va:08x}",
                function.size_bytes,
                function.name,
                str(function.is_thunk).lower(),
                f"0x{function.body_max_va:08x}",
            )
        )
    return output.getvalue().encode()


def validate_document(value: object) -> None:
    if not isinstance(value, dict) or set(value) != {
        "version",
        "selection",
        "roles",
        "effective_sha256",
        "loader_pins",
    }:
        raise ValueError("malformed authoritative effective-source contract")
    if (
        value["version"] != SOURCE_VERSION
        or value["selection"] != "explicit-cli-paths-no-inferred-defaults"
        or not pin_valid(value["effective_sha256"])
        or value["loader_pins"] != dict(LOADED_LOADER_PINS)
    ):
        raise ValueError("unknown/stale authoritative effective-source identity")
    roles = value["roles"]
    if not isinstance(roles, dict) or set(roles) != {"export", "overrides", "additions"}:
        raise ValueError("missing explicit effective-source roles")
    for name, role in roles.items():
        if not isinstance(role, dict) or set(role) != {"enabled", "sha256"}:
            raise ValueError("malformed effective-source role")
        if (
            type(role["enabled"]) is not bool
            or (name == "export" and not role["enabled"])
            or (role["enabled"] and not pin_valid(role["sha256"]))
            or (not role["enabled"] and role["sha256"] is not None)
        ):
            raise ValueError("invalid effective-source role selection/pin")
    check_loader()


class SelectedSources:
    """Actual explicit paths, retained privately; portable provenance pins their bytes."""

    def __init__(self, export: Path, overrides: Path | None, additions: Path | None) -> None:
        self.paths = (export, overrides, additions)
        before = self.read()
        self.effective = EffectiveIndex.build(*before)
        if self.read() != before:
            raise ValueError("effective source changed during authoritative load")
        self._before = before

    def read(self) -> tuple[bytes, bytes | None, bytes | None]:
        check_loader()
        values = []
        for position, path in enumerate(self.paths):
            if path is None:
                if position == 0:
                    raise ValueError("actual original export required")
                values.append(None)
            elif not isinstance(path, Path) or not path.is_file():
                raise ValueError("missing expected effective-source file")
            else:
                values.append(path.read_bytes())
        return tuple(values)

    def check(self) -> None:
        if self.read() != self._before:
            raise ValueError("actual effective-source role bytes drifted during campaign")

    def document(self) -> dict[str, object]:
        self.check()
        return self.effective.document()
