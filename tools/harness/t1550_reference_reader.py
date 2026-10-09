# SPDX-License-Identifier: GPL-3.0-or-later
"""Prospective original prefix reference metadata; requires a fresh CPU allocation.

No instruction decoding, emulator, native build, original execution or trust activation.
"""

from __future__ import annotations

import argparse
import json
import os
import signal
import stat
import subprocess
import time
from dataclasses import asdict
from pathlib import Path

from . import scoped_fixture_authority as authority
from . import scoped_fixture_prefix as prefix
from .image import build_guest_image
from .seeding import SeedPolicy

XBE_SHA = "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"
RAW_SHA = "05f426225c494ffd94a6b3150e02eb2a97f199f49c66e86bbd275a60f7dbd9ee"
SOURCE_ROOT = Path(__file__).resolve().parents[2]
MAX_PRIVATE_BYTES = 16 * 1024 * 1024
PROVENANCE = (
    "docs/t1276-exact-body-ranges.md",
    "docs/t1282-original-exact-ranges.md",
    "docs/evidence/t1550/original-prefix-reference-read-predeclaration.md",
)


def source_pins() -> dict[str, str]:
    selected = set()
    for directory, pattern in (("tools", "*.py"), ("src/game", "*.h")):
        selected.update((SOURCE_ROOT / directory).rglob(pattern))
    selected.update(
        SOURCE_ROOT / name for name in (*PROVENANCE, "tools/ghidra/ExportFunctionBounds.java")
    )
    return {
        str(path.relative_to(SOURCE_ROOT)): authority.digest(path.read_bytes())
        for path in sorted(selected)
    }


def read_bounded(path: Path) -> bytes:
    before = path.stat()

    def identity(value: os.stat_result) -> tuple[int, int, int, int]:
        return value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns

    if not stat.S_ISREG(before.st_mode) or not 0 < before.st_size <= MAX_PRIVATE_BYTES:
        raise ValueError("private input exceeds bounded regular-file size")
    with path.open("rb") as stream:
        if identity(os.fstat(stream.fileno())) != identity(before):
            raise ValueError("private input changed before bounded read")
        raw = stream.read(MAX_PRIVATE_BYTES + 1)
        finished = os.fstat(stream.fileno())
    if (
        len(raw) != before.st_size
        or identity(finished) != identity(before)
        or identity(path.stat()) != identity(before)
    ):
        raise ValueError("private input changed during bounded read")
    return raw


def read_reference(main_checkout: Path, output: Path) -> dict[str, object]:
    if os.getpriority(os.PRIO_PROCESS, 0) != 19 or os.sched_getaffinity(0) != set(range(24, 32)):
        raise RuntimeError("reference reader requires absoluteNI19/cores24–31")
    output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    receipt = dict(
        status="STARTED",
        execution=False,
        trust_activated=False,
        nice=19,
        cpus=sorted(os.sched_getaffinity(0)),
        commands=[],
    )
    path = output / "receipt.json"

    def save() -> None:
        receipt["elapsed_seconds"] = time.monotonic() - started
        temporary = path.with_suffix(".pending.json")
        temporary.write_text(json.dumps(receipt, indent=2) + "\n")
        temporary.replace(path)

    receipt["phase"] = "INITIALIZED"
    save()
    before = None
    inputs = {}

    def interrupted(signum: int, frame: object) -> None:
        raise TimeoutError(f"reference read interrupted by signal{signum}")

    previous_alarm = signal.signal(signal.SIGALRM, interrupted)
    previous_term = signal.signal(signal.SIGTERM, interrupted)
    signal.alarm(60)
    try:
        receipt["phase"] = "SOURCE-PREFLIGHT"
        save()
        before = source_pins()
        receipt["source_before"] = before
        for args in (["git", "rev-parse", "HEAD"], ["git", "status", "--porcelain"]):
            completed = subprocess.run(
                args, cwd=SOURCE_ROOT, capture_output=True, text=True, check=False, timeout=5
            )
            receipt["commands"].append(
                dict(
                    command=args,
                    exit=completed.returncode,
                    stdout=completed.stdout,
                    stderr=completed.stderr,
                )
            )
            completed.check_returncode()
        if receipt["commands"][1]["stdout"]:
            raise ValueError("reference source checkout must be clean")
        xbe = main_checkout / "build/default.xbe"
        functions = main_checkout / "generated/retail/functions.csv"
        receipt["phase"] = "PRIVATE-XBE-BOUNDED-READ"
        save()
        xbe_raw = read_bounded(xbe)
        inputs[str(xbe)] = authority.digest(xbe_raw)
        receipt["inputs_before"] = dict(inputs)
        receipt["phase"] = "PRIVATE-EXPORT-BOUNDED-READ"
        save()
        raw = read_bounded(functions)
        inputs[str(functions)] = authority.digest(raw)
        receipt["inputs_before"] = dict(inputs)
        if inputs[str(xbe)] != XBE_SHA or inputs[str(functions)] != RAW_SHA:
            raise ValueError("independent T1276/T1282 whole original/export anchors mismatch")
        # PhysicalSource.original independently rebuilds and checks the exact shared image.
        receipt["phase"] = "AUTHENTICATED-IMAGE-METADATA"
        save()
        image = build_guest_image(xbe, policy=SeedPolicy())
        deps = authority.Dependencies.capture(("tools",))
        physical = authority.PhysicalSource.original(xbe, image, deps, SeedPolicy())
        if physical.source_sha256 != XBE_SHA:
            raise ValueError("actual parsed original source differs from independent XBE anchor")
        receipt["phase"] = "WHOLE-EXPORT-MEMBERSHIP"
        save()
        members = prefix.rows(raw, physical)
        receipt["prospective_reference"] = dict(
            type="original-export-prefix-v1",
            semantics=prefix.SEMANTICS,
            source_xbe_sha256=XBE_SHA,
            image_sha256=physical.image_sha256,
            sections_sha256=physical.sections_sha256,
            sections=[asdict(section) for section in image.source_sections],
            mapped_start=image.base,
            mapped_end=image.base + len(image.data),
            raw_sha256=authority.digest(raw),
            row_count=len(members),
            membership_sha256=authority.digest(authority.canonical(members)),
            protected_start=image.base,
            protected_end=1 + max(row[4] for row in members),
            exporter_sha256=before["tools/ghidra/ExportFunctionBounds.java"],
            provenance={name: before[name] for name in PROVENANCE},
        )
        receipt["status"] = "PROSPECTIVE-UNRATIFIED"
    except BaseException as error:
        receipt["status"] = "UNPROVEN"
        receipt["error"] = repr(error)
    finally:
        receipt["phase"] = "POSTCHECK"
        save()
        # Preserve original failure if any post-read/hash operation also fails.
        try:
            after = source_pins()
            receipt["source_after"] = after
            if before is None or after != before:
                raise ValueError("reference source membership/content drift")
            if inputs:
                actual = {name: authority.digest(read_bounded(Path(name))) for name in inputs}
                receipt["inputs_after"] = actual
                if actual != inputs:
                    raise ValueError("reference private input drift")
        except BaseException as error:
            receipt["postcheck_error"] = repr(error)
            receipt["status"] = "UNPROVEN"
        finally:
            signal.alarm(0)
            signal.signal(signal.SIGALRM, previous_alarm)
            signal.signal(signal.SIGTERM, previous_term)
            receipt["phase"] = "FINISHED"
            save()
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--main-checkout", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    os.setpriority(os.PRIO_PROCESS, 0, 19)
    os.sched_setaffinity(0, set(range(24, 32)))
    receipt = read_reference(args.main_checkout, args.output)
    return 0 if receipt["status"] == "PROSPECTIVE-UNRATIFIED" else 1


if __name__ == "__main__":
    raise SystemExit(main())
