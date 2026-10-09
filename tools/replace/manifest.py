# SPDX-License-Identifier: GPL-3.0-or-later
"""The registered-replacement manifest: what is replaced, as the LINKED BINARY says.

Two programs produce the same canonical entries and must agree:

* `game_manifest`, built from src/game, prints the registry linked into it.
* The differential-harness subject answers `LISTREPL` with the registry linked into it.

Both are reduced here to `ManifestEntry` and hashed together with the content digest of
src/game into `manifest_sha`. The coverage metric accepts a proof only when the proof's
`manifest_sha` equals the manifest's, so a proof made against yesterday's hand code, or
against a different set of registrations, cannot be counted against today's.

A source scan (`scan.py`) is NOT evidence and is only compared against this: a
registration the scan sees and the binary does not, or the reverse, is reported.
"""

from __future__ import annotations

import hashlib
import json
from dataclasses import asdict, dataclass
from pathlib import Path

from .input_abi import register_inputs, validate_inputs
from .scan import CONVENTIONS, RETURNS, Registration

SCHEMA = 1
SCRATCH_NAMES = {1: "eax", 2: "ecx", 4: "edx"}


class ManifestError(ValueError):
    """The registry output could not be understood or disagrees with the source scan."""


@dataclass(frozen=True)
class ManifestEntry:
    va: int
    name: str
    convention: str
    stack_args: int
    returns: str
    scratch: tuple[str, ...]
    source: str
    register_inputs: tuple[str, ...] | None = None

    def __post_init__(self) -> None:
        validate_inputs(self.register_inputs, self.convention, self.stack_args, self.scratch)

    def as_json(self) -> dict[str, object]:
        record = asdict(self)
        record["va"] = f"0x{self.va:08x}"
        record["scratch"] = list(self.scratch)
        if self.register_inputs is None:
            record.pop("register_inputs")
        else:
            record["register_inputs"] = list(self.register_inputs)
        return record


def scratch_from_mask(mask: int) -> tuple[str, ...]:
    if type(mask) is not int or not 0 <= mask <= 7:
        raise ManifestError("unknown scratch mask bits")
    return tuple(name for bit, name in sorted(SCRATCH_NAMES.items()) if mask & bit)


def _entry(
    va_text: str,
    name: str,
    convention: str,
    stack_args: str,
    returns: str,
    scratch: tuple[str, ...],
    source: str,
    inputs: tuple[str, ...] | None = None,
) -> ManifestEntry:
    if convention not in CONVENTIONS:
        raise ManifestError(f"unknown convention {convention!r} for {name}")
    if returns not in ("eax", "void"):
        raise ManifestError(f"unknown return kind {returns!r} for {name}")
    return ManifestEntry(
        va=int(va_text, 16),
        name=name,
        convention=convention,
        stack_args=int(stack_args),
        returns=returns,
        scratch=scratch,
        source=source,
        register_inputs=inputs,
    )


def _wire_inputs(value: str) -> tuple[str, ...]:
    if not value.startswith("inputs="):
        raise ManifestError("missing exact input metadata marker")
    try:
        result = register_inputs({"register_inputs": value[7:].split(",")})
    except ValueError as error:
        raise ManifestError(str(error)) from error
    assert result is not None
    return result


def entry_from_json(record: dict[str, object]) -> ManifestEntry:
    return _entry(
        record["va"],
        record["name"],
        record["convention"],
        str(record["stack_args"]),
        record["returns"],
        tuple(record["scratch"]),
        record["source"],
        register_inputs(record),
    )


def parse_manifest_output(text: str) -> list[ManifestEntry]:
    """Parse `game_manifest`'s tab separated lines."""
    entries = []
    for line in text.splitlines():
        if not line.strip():
            continue
        fields = line.split("\t")
        if len(fields) not in (7, 8):
            raise ManifestError(f"expected 7 tab separated fields, got {len(fields)}: {line!r}")
        va, name, convention, stack_args, returns, scratch, source = fields[:7]
        inputs = _wire_inputs(fields[7]) if len(fields) == 8 else None
        entries.append(
            _entry(
                va,
                name,
                convention,
                stack_args,
                returns,
                tuple(name for name in scratch.split(",") if name),
                source,
                inputs,
            )
        )
    return sorted(entries, key=lambda entry: entry.va)


def parse_listrepl(lines: list[str]) -> list[ManifestEntry]:
    """Parse the harness subject's `REPL ...` lines (scratch is a bit mask there)."""
    entries = []
    for line in lines:
        fields = line.split()
        if len(fields) not in (8, 9) or fields[0] != "REPL":
            raise ManifestError(f"unparseable LISTREPL line: {line!r}")
        _, va, name, convention, stack_args, returns, mask, source = fields[:8]
        inputs = _wire_inputs(fields[8]) if len(fields) == 9 else None
        entries.append(
            _entry(
                va,
                name,
                convention,
                stack_args,
                returns,
                scratch_from_mask(int(mask)),
                source,
                inputs,
            )
        )
    return sorted(entries, key=lambda entry: entry.va)


def manifest_sha(entries: list[ManifestEntry], repl_digest: str) -> str:
    """Digest of the registry plus the content of src/game. 64 hex digits."""
    canonical = json.dumps(
        {"repl_digest": repl_digest, "functions": [entry.as_json() for entry in entries]},
        sort_keys=True,
        separators=(",", ":"),
    )
    return hashlib.sha256(canonical.encode()).hexdigest()


def manifest_document(entries: list[ManifestEntry], repl_digest: str) -> dict[str, object]:
    return {
        "schema": SCHEMA,
        "manifest_sha": manifest_sha(entries, repl_digest),
        "functions": [entry.as_json() for entry in entries],
    }


def write_manifest(path: Path, entries: list[ManifestEntry], repl_digest: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    document = manifest_document(entries, repl_digest)
    path.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def cross_check(entries: list[ManifestEntry], scanned: list[Registration]) -> list[str]:
    """Every way the linked registry and the source scan disagree. Empty means they agree."""
    problems: list[str] = []
    by_va = {entry.va: entry for entry in entries}
    if len(by_va) != len(entries):
        problems.append("duplicate VA in linked replacement registry")
    scan_by_va = {registration.va: registration for registration in scanned}
    for va in sorted(set(scan_by_va) - set(by_va)):
        problems.append(
            f"{va:#010x} is in the source ({scan_by_va[va].source}) but NOT in the linked "
            "registry: commented out, behind #if 0, or the objects are stale"
        )
    for va in sorted(set(by_va) - set(scan_by_va)):
        problems.append(f"{va:#010x} is in the linked registry but the source scan did not find it")
    for va in sorted(set(by_va) & set(scan_by_va)):
        entry, registration = by_va[va], scan_by_va[va]
        if entry.register_inputs != registration.register_inputs:
            problems.append(f"{va:#010x}: registry and source input metadata differ")
        if registration.exact_registers and entry.scratch:
            problems.append(f"{va:#010x}: register-exact adapter declares scratch registers")
        returns = "eax" if registration.returns == "u32" else "void"
        if (entry.convention, entry.stack_args, entry.returns, entry.name) != (
            registration.convention,
            registration.stack_args,
            returns,
            registration.function,
        ):
            problems.append(f"{va:#010x}: registry and source scan describe different functions")
    return problems


def check_declared_types() -> None:
    """Guard the two enumerations this module mirrors from scan.py."""
    assert set(RETURNS) == {"u32", "void"}
