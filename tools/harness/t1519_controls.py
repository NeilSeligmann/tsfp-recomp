# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent synthetic exact-input ABI controls; never an original-game proof."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import subprocess
from pathlib import Path

from tools.harness.model import Case
from tools.harness.subject import SubjectProcess
from tools.replace.manifest import parse_listrepl, parse_manifest_output

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "tests/c/t1519"
SEEDS = (20261001, 20261006)


def checked(command: list[str]) -> str:
    result = subprocess.run(command, capture_output=True, text=True, timeout=180)
    if result.returncode:
        raise RuntimeError(
            f"{command!r}: exit {result.returncode}\n{result.stdout}\n{result.stderr}"
        )
    return result.stdout


def build(directory: Path, opt: str, mutant: int = 0) -> dict[str, Path]:
    directory.mkdir(parents=True, exist_ok=False)
    common = [
        "cc",
        f"-{opt}",
        "-std=gnu11",
        "-fPIE",
        "-pie",
        "-I",
        str(ROOT / "src/game"),
        "-I",
        str(ROOT / "tools/harness"),
        f"-DMUTANT={mutant}",
    ]
    fixtures = [str(FIXTURES / name) for name in ("exact.c", "legacy.c")]
    registry = str(ROOT / "src/game/game_registry.c")
    runtime = str(ROOT / "tools/harness/runtime_min.c")
    paths = {name: directory / name for name in ("subject", "manifest", "layout")}
    checked(
        [
            *common,
            "-DHARNESS_REPLACEMENT=1",
            str(ROOT / "tools/harness/driver.c"),
            str(ROOT / "tools/harness/call_stub.c"),
            runtime,
            registry,
            *fixtures,
            str(FIXTURES / "dispatch.c"),
            "-lm",
            "-o",
            str(paths["subject"]),
        ]
    )
    checked(
        [
            *common,
            str(ROOT / "src/game/game_manifest.c"),
            registry,
            *fixtures,
            "-o",
            str(paths["manifest"]),
        ]
    )
    checked(
        [
            *common,
            str(FIXTURES / "layout.c"),
            runtime,
            registry,
            *fixtures,
            "-lm",
            "-o",
            str(paths["layout"]),
        ]
    )
    return paths


def expected(regs: tuple[int, ...], arguments: tuple[int, int], variant: int) -> tuple[int, ...]:
    # Independently specified Python arithmetic, never invokes the C helper.
    inputs = (1, 1, 2, 2, 3, 3, 2)[variant]
    value = (3 * regs[1] if inputs & 1 else 0) + (5 * regs[6] if inputs & 2 else 0)
    value = (value + 7 * arguments[0] + 11 * arguments[1]) & 0xFFFFFFFF
    result = list(regs)
    result[0] = value
    result[1] ^= 0x13579BDF
    result[6] = (result[6] + 0x2468ACE0) & 0xFFFFFFFF
    result[4] += 12 if variant in (1, 3, 5) else 4
    return tuple(result)


def campaign(binary: Path, image: Path, *, cases: int, directed: bool = False) -> dict[str, object]:
    subject = SubjectProcess(binary, image)
    failures = []
    count = 0
    linked = []
    try:
        for seed in SEEDS:
            for variant in range(7):
                rng = random.Random(seed ^ variant)
                for ordinal in range(cases + (6 if directed else 0)):
                    regs = [rng.getrandbits(32) for _ in range(8)]
                    regs[3], regs[4] = 0xD00000, 0xE80000
                    args = (rng.getrandbits(32), rng.getrandbits(32))
                    if ordinal >= cases:
                        edges = (0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678)
                        rank = ordinal - cases
                        regs[1], regs[6], regs[2] = (
                            edges[rank],
                            edges[(rank + 2) % 6],
                            edges[(rank + 4) % 6],
                        )
                        args = (edges[(rank + 1) % 6], edges[(rank + 3) % 6])
                    initial = rng.getrandbits(32).to_bytes(4, "little")
                    frame = b"\x00\x00\xff\x00" + b"".join(
                        value.to_bytes(4, "little") for value in args
                    )
                    case = Case(
                        seed,
                        ordinal,
                        0x10000 + variant * 16,
                        1,
                        tuple(regs),
                        0,
                        ((regs[4], frame), (regs[3], initial)),
                    )
                    result = subject.run(case)
                    want = expected(tuple(regs), args, variant)
                    final = want[0].to_bytes(4, "little")
                    writes = {
                        regs[3] + offset: value
                        for offset, value in enumerate(final)
                        if value != initial[offset]
                    }
                    count += 1
                    if (
                        result.fault
                        or result.regs != want
                        or result.writes != writes
                        or result.replaced is not True
                    ):
                        failures.append(
                            dict(
                                seed=seed,
                                variant=variant,
                                ordinal=ordinal,
                                fault=result.fault,
                                regs=result.regs,
                                expected=want,
                                writes=result.writes,
                                expected_writes=writes,
                                replaced=result.replaced,
                            )
                        )
        linked = [entry.as_json() for entry in parse_listrepl(subject.list_replacements())]
    finally:
        subject.close()
    return dict(
        cases=count,
        random_cases=cases * 14,
        directed_cases=84 if directed else 0,
        failures=failures,
        linked=linked,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    os.setpriority(os.PRIO_PROCESS, 0, 19)
    os.sched_setaffinity(0, set(range(24, 32)))
    assert os.getpriority(os.PRIO_PROCESS, 0) == 19
    assert os.sched_getaffinity(0) == set(range(24, 32))
    args.output.mkdir(parents=True, exist_ok=False)
    image = args.output / "empty.img"
    image.write_bytes(b"")
    receipt: dict[str, object] = {
        "kind": "synthetic-abi-controls",
        "seeds": SEEDS,
        "random_per_variant": 600,
        "runs": [],
        "mutants": [],
    }
    receipt_path = args.output / "receipt.json"
    try:
        for opt in ("O0", "O3"):
            paths = build(args.output / opt, opt)
            layout = checked([str(paths["layout"])])
            manifest = [
                entry.as_json()
                for entry in parse_manifest_output(checked([str(paths["manifest"])]))
            ]
            result = campaign(paths["subject"], image, cases=600, directed=True)
            result.update(
                opt=opt,
                layout=layout,
                manifest=manifest,
                binaries={
                    name: hashlib.sha256(path.read_bytes()).hexdigest()
                    for name, path in paths.items()
                },
            )
            receipt["runs"].append(result)
            receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
            assert result["linked"] == manifest and not result["failures"]
            for mutant in (1, 2, 3, 4):
                mutant_paths = build(args.output / f"{opt}-mutant{mutant}", opt, mutant)
                changed = campaign(mutant_paths["subject"], image, cases=1)
                receipt["mutants"].append(dict(opt=opt, mutant=mutant, **changed))
                receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
                assert changed["failures"], "semantic mutant survived"
    except Exception as error:
        receipt["infrastructure_failure"] = repr(error)
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
        raise
    # Actual malformed linked records must be refused before serializer indexing.
    receipt["malformed_serializers"] = []
    for opt in ("O0", "O3"):
        for field in range(1, 6):
            binary = args.output / f"{opt}-bad{field}"
            checked(
                [
                    "cc",
                    f"-{opt}",
                    "-I",
                    str(ROOT / "src/game"),
                    f"-DBAD_FIELD={field}",
                    str(ROOT / "src/game/game_manifest.c"),
                    str(ROOT / "src/game/game_registry.c"),
                    str(FIXTURES / "exact.c"),
                    str(FIXTURES / "legacy.c"),
                    str(FIXTURES / "corrupt.c"),
                    "-o",
                    str(binary),
                ]
            )
            bad = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            receipt["malformed_serializers"].append(
                dict(
                    opt=opt, field=field, exit=bad.returncode, stdout=bad.stdout, stderr=bad.stderr
                )
            )
            receipt_path.write_text(json.dumps(receipt, indent=2) + "\n")
            assert bad.returncode == 2 and "invalid" in bad.stderr.lower()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
