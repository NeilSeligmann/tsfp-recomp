# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic-only ordinary producer and state-isolation controls for named graphs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import subprocess
import time
from collections import Counter
from dataclasses import asdict, replace
from pathlib import Path
from types import SimpleNamespace
from typing import TextIO

from tests.test_t1525_named_global_objects import fixture
from tools.harness import cli, providers
from tools.harness.image import write_image_file
from tools.harness.model import DIVERGENT_OUTCOMES, Case, CaseResult
from tools.harness.named_global_objects import Domain
from tools.harness.oracle import UnicornOracle
from tools.harness.provenance import tree_digest
from tools.harness.replacement import ReplacementJudge
from tools.harness.seeding import SeedPolicy
from tools.harness.selection import select
from tools.harness.subject import SubjectProcess
from tools.harness.t1525_scoped_controls import guard_group, scoped_fixture
from tools.replace.manifest import cross_check, manifest_sha, parse_listrepl
from tools.replace.proof_contract import validate_document
from tools.replace.scan import scan_directory

ROOT = Path(__file__).resolve().parents[2]


def source_pins(paths: list[Path]) -> dict[str, str]:
    paths = list(paths)
    for directory, patterns in (
        ("tools/harness", ("*.py", "*.h")),
        ("tools/replace", ("*.py",)),
        ("tools/codediff", ("*.py",)),
        ("tests/c/t1525", ("*.c",)),
        ("src/game", ("*.h",)),
    ):
        for pattern in patterns:
            paths.extend((ROOT / directory).glob(pattern))
    return {
        str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(set(paths))
    }


def check_source_drift(before: dict[str, str], paths: list[Path]) -> dict[str, str]:
    after = source_pins(paths)
    if after != before:
        raise RuntimeError("source pins changed during synthetic experiment")
    return after


def start_subject(binary: Path, image: Path) -> SubjectProcess:
    """Keep ownership even when initialization fails after starting the child."""
    subject = SubjectProcess.__new__(SubjectProcess)
    try:
        SubjectProcess.__init__(subject, binary, image)
    except BaseException:
        if hasattr(subject, "_proc"):
            subject.close()
        raise
    return subject


def fault_groups(records: list[CaseResult]) -> list[dict[str, object]]:
    groups = {}
    for result in records:
        if not result.oracle_fault and not result.subject_fault:
            continue
        key = (
            result.va,
            str(result.outcome),
            result.oracle_fault,
            result.subject_fault,
            tuple(sorted(result.reach.covered_vas)),
        )
        groups.setdefault(key, []).append(result.index)
    output = []
    for (va, outcome, oracle, subject, covered), indices in groups.items():
        ranges = []
        for index in sorted(indices):
            if ranges and ranges[-1][1] + 1 == index:
                ranges[-1][1] = index
            else:
                ranges.append([index, index])
        output.append(
            dict(
                va=va,
                outcome=outcome,
                oracle=oracle,
                subject=subject,
                covered_vas=covered,
                count=len(indices),
                index_ranges=ranges,
            )
        )
    return output


def expected_valid(case: Case, domain: Domain) -> tuple[tuple[int, ...], dict[int, int]]:
    position = case.index - (91 << 40)
    parent, child = {2232: (0x12345678, 1), 792: (1, 0x12345678)}[position]
    shared = domain.va == 0x10000
    value = ((4 * parent + 5 * child) if shared else (3 * parent + 6 * child)) & 0xFFFFFFFF
    regs = list(case.regs)
    regs[0] = value
    regs[1] = domain.arena if shared else domain.arena + 16
    regs[2] = (5 * child) & 0xFFFFFFFF
    regs[4] += 4
    output = 0x800010 if shared else 0x800014
    writes = {
        output + index: byte for index, byte in enumerate(value.to_bytes(4, "little")) if byte
    }
    return tuple(regs), writes


SEEDS = (20261001, 20261006)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--code-safety", choices=("raw", "effective"))
    args = parser.parse_args()
    os.setpriority(os.PRIO_PROCESS, 0, 19)
    os.sched_setaffinity(0, set(range(24, 32)))
    assert os.getpriority(os.PRIO_PROCESS, 0) == 19
    assert os.sched_getaffinity(0) == set(range(24, 32))

    def timed_out(signum: int, frame: object) -> None:
        raise TimeoutError("synthetic root phase exceeded60s")

    signal.signal(signal.SIGALRM, timed_out)
    signal.signal(signal.SIGTERM, timed_out)
    started = time.monotonic()
    args.output.mkdir(parents=True, exist_ok=False)
    receipt = {
        "kind": "synthetic-only-named-graph-controls",
        "scheduler": {"nice": 19, "cpus": list(range(24, 32))},
        "commands": [],
        "runs": [],
        "isolation": [],
        "mutants": [],
    }
    if args.code_safety:
        receipt.update(
            code_safety_mode=args.code_safety,
            guard_controls=[],
            guard_mutants=[],
            guard_restored=[],
            expected=dict(
                runs=8,
                comparisons=30336,
                isolation=144,
                old_mutants=40,
                guard_controls=16,
                guard_mutants=10,
                restored=2,
            ),
        )
    receipt_file = args.output / "receipt.json"

    def save() -> None:
        receipt["elapsed_seconds"] = time.monotonic() - started
        pending = receipt_file.with_suffix(".pending.json")
        pending.write_text(json.dumps(receipt, indent=2) + "\n")
        pending.replace(receipt_file)

    def checked(command: list[str]) -> str:
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired as error:
            receipt["commands"].append(
                dict(
                    command=command,
                    exit=None,
                    timeout_seconds=120,
                    stdout=(error.stdout or b"").decode(errors="replace")
                    if isinstance(error.stdout, bytes)
                    else error.stdout,
                    stderr=(error.stderr or b"").decode(errors="replace")
                    if isinstance(error.stderr, bytes)
                    else error.stderr,
                )
            )
            receipt["infrastructure_failure"] = repr(error)
            save()
            raise
        receipt["commands"].append(
            dict(
                command=command, exit=result.returncode, stdout=result.stdout, stderr=result.stderr
            )
        )
        save()
        if result.returncode:
            raise RuntimeError(f"infrastructure command exit {result.returncode}: {command!r}")
        return result.stdout

    source_paths = [
        ROOT / name
        for name in (
            "tools/harness/t1525_controls.py",
            "tools/harness/named_global_objects.py",
            "tools/harness/providers.py",
            "tools/harness/cli.py",
            "tools/harness/image.py",
            "tools/harness/oracle.py",
            "tools/harness/subject.py",
            "tools/harness/driver.c",
            "tools/harness/runtime_min.c",
            "tools/harness/call_stub.c",
            "src/game/game_registry.c",
            "tests/test_t1525_named_global_objects.py",
            "docs/evidence/t1525/synthetic-layout.json",
        )
    ]
    source_paths.extend((ROOT / "tests/c/t1525").glob("*.c"))
    source_paths.extend((ROOT / "src/game").glob("*.h"))
    # Include imported harness/replace modules and native headers, not just direct entrypoints.
    source_paths.extend((ROOT / "tools/harness").glob("*.py"))
    source_paths.extend((ROOT / "tools/harness").glob("*.h"))
    source_paths.extend((ROOT / "tools/replace").glob("*.py"))
    receipt["source_pins"] = source_pins(source_paths)
    receipt["git_context"] = {
        "commit": checked(["git", "-C", str(ROOT), "rev-parse", "HEAD"]).strip(),
        "status": checked(["git", "-C", str(ROOT), "status", "--porcelain"]),
    }
    source_sha = hashlib.sha256(
        json.dumps(receipt["source_pins"], sort_keys=True).encode()
    ).hexdigest()
    receipt["compiler"] = checked(["cc", "--version"])
    try:
        image, factory = fixture()
        source_paths_selected = None
        if args.code_safety:
            image, factory, source_paths_selected = scoped_fixture(
                image, factory, args.output / "selected-source", args.code_safety
            )
            receipt["selected_sources"] = {
                role: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                if path is not None
                else None
                for role, path in zip(
                    ("functions", "overrides", "additions"), source_paths_selected, strict=True
                )
            }
        image_path = args.output / "image.img"
        write_image_file(image, image_path)
        receipt["image_sha256"] = hashlib.sha256(image.data).hexdigest()
        receipt["contracts"] = [domain.document() for domain in factory.domains]
        registry = (*providers.REGISTRY, providers.named_global_provider(factory))
        providers.validate_registry(registry)
        fixture_dir = ROOT / "tests/c/t1525"
        for opt in ("O0", "O3"):
            binary = args.output / f"subject-{opt}"
            checked(
                [
                    "cc",
                    f"-{opt}",
                    "-fPIE",
                    "-pie",
                    "-DHARNESS_REPLACEMENT=1",
                    "-I",
                    str(ROOT / "src/game"),
                    "-I",
                    str(ROOT / "tools/harness"),
                    f'-DHARNESS_TREE_SHA="{source_sha[:16]}"',
                    '-DHARNESS_GEN_DIR="synthetic-t1525"',
                    f'-DHARNESS_REPL_SHA="{tree_digest(fixture_dir)[:16]}"',
                    str(ROOT / "tools/harness/driver.c"),
                    str(ROOT / "tools/harness/call_stub.c"),
                    str(ROOT / "tools/harness/runtime_min.c"),
                    str(ROOT / "src/game/game_registry.c"),
                    str(fixture_dir / "leaves.c"),
                    str(fixture_dir / "dispatch.c"),
                    "-lm",
                    "-o",
                    str(binary),
                ]
            )
            binary_sha = hashlib.sha256(binary.read_bytes()).hexdigest()
            for seed in SEEDS:
                for phase in ("baseline", "default"):
                    subject = None
                    case_stream = None
                    records = []
                    receipt["active_phase"] = dict(opt=opt, seed=seed, phase=phase)
                    save()
                    try:
                        subject = start_subject(binary, image_path)
                        oracle = UnicornOracle(image.data, record_loads=True)
                        case_path = args.output / f"rows-{opt}-{seed}-{phase}.jsonl"
                        case_stream = case_path.open("x", buffering=1)
                        entries = parse_listrepl(subject.list_replacements())
                        assert not cross_check(entries, scan_directory(fixture_dir))
                        judge = ReplacementJudge(entries)

                        def record(
                            result: CaseResult,
                            *,
                            rows: list[CaseResult] = records,
                            stream: TextIO = case_stream,
                        ) -> None:
                            rows.append(result)
                            stream.write(
                                json.dumps(
                                    dict(
                                        index=result.index,
                                        va=result.va,
                                        outcome=str(result.outcome),
                                        oracle_fault=result.oracle_fault,
                                        subject_fault=result.subject_fault,
                                        covered_vas=sorted(result.reach.covered_vas),
                                        insns=result.reach.insns,
                                        diagnosis=result.diagnosis,
                                    )
                                )
                                + "\n"
                            )

                        settings = SimpleNamespace(
                            seed=seed,
                            cases_per_function=600,
                            edge_cases=True,
                            live_call_closure=False,
                            live_vector_state=False,
                            no_fixture_providers=phase == "baseline",
                            fixture_provider_selection=None,
                        )
                        if source_paths_selected is not None:
                            settings.functions, settings.overrides, settings.additions = (
                                source_paths_selected
                            )
                        for domain in factory.domains:
                            selection = select([(domain.va, domain.body_size)], image.code_at)
                            assert not selection.skipped and len(selection.candidates) == 1
                            candidate = selection.candidates[0]
                            signal.alarm(60)
                            ran, aborted = cli.drive_replacement(
                                settings,
                                SeedPolicy(),
                                image,
                                oracle,
                                subject,
                                judge,
                                SimpleNamespace(write_case=record),
                                candidate,
                                0,
                                registry=registry,
                            )
                            signal.alarm(0)
                            assert not aborted and ran == 600 + (
                                domain.count if phase == "default" else 0
                            )
                        reach = {}
                        # Actual original-instruction union, never a configured coverage value.
                        for domain in factory.domains:
                            covered = set()
                            for result in records:
                                if result.va == domain.va:
                                    covered.update(result.reach.covered_vas)
                            reach[domain.va] = (len(covered) / 10, 10, len(covered) < 5)
                        proof = judge.document(
                            manifest_sha=manifest_sha(entries, tree_digest(fixture_dir)),
                            seed=seed,
                            cases_per_function=600,
                            edge_cases=True,
                            subject=asdict(subject.provenance),
                            reach=reach,
                        )
                        if args.code_safety and phase == "default":
                            validate_document(proof)
                            for row in proof["functions"]:
                                observed = row["named_global_code_safety"]
                                assert observed["checked_cases"] == row["cases"]
                                assert observed["returned"] + observed["faulted"] == row["cases"]
                                assert observed["validated"] is True
                        else:
                            validate_document(proof)
                        faults = fault_groups(records)
                        disagreements = [
                            dict(
                                index=result.index,
                                va=result.va,
                                outcome=str(result.outcome),
                                diagnosis=result.diagnosis,
                            )
                            for result in records
                            if result.outcome in DIVERGENT_OUTCOMES
                        ]
                        receipt["runs"].append(
                            dict(
                                opt=opt,
                                seed=seed,
                                binary_sha256=binary_sha,
                                phase=phase,
                                rows_sha256=hashlib.sha256(case_path.read_bytes()).hexdigest(),
                                proof=proof,
                                outcomes=dict(Counter(str(result.outcome) for result in records)),
                                faults=faults,
                                disagreements=disagreements,
                                metric_gates={
                                    row["va"]: dict(
                                        zero_disagree=row["disagree"] == 0,
                                        zero_subject_fault=row["subject_faulted"] == 0,
                                        minimum_verdicts=row["verdicts"] >= 100,
                                        coverage=row["coverage"] >= 0.9,
                                        null_model=row["null_agree_rate"] <= 0.9,
                                        nonvacuous=not row["near_vacuous"],
                                        dispatch=row["replaced_confirmed"],
                                        producer_judgeable=row["unjudgeable"] is None,
                                    )
                                    for row in proof["functions"]
                                }
                                if args.code_safety
                                else None,
                            )
                        )
                        save()
                        assert not disagreements
                    finally:
                        signal.alarm(0)
                        if case_stream is not None:
                            case_stream.close()
                        receipt["active_phase"]["captured_rows"] = len(records)
                        save()
                        if subject is not None:
                            subject.close()
            # Bounded observed-state reset and deliberately divergent one-side inputs.
            for seed in SEEDS:
                for domain in factory.domains:
                    valid = domain.case(seed, 2232, domain.body_size)
                    other = domain.case(seed, 792, domain.body_size)
                    fault = domain.case(seed, 2256, domain.body_size)
                    sequence = [valid, fault, other, valid, fault, other]
                    signal.alarm(60)
                    reused_oracle = UnicornOracle(image.data)
                    reused_subject = start_subject(binary, image_path)
                    scoped_kwargs = (
                        {}
                        if source_paths_selected is None
                        else dict(
                            overrides=source_paths_selected[1], additions=source_paths_selected[2]
                        )
                    )
                    binding = (
                        None
                        if source_paths_selected is None
                        else factory.bind(
                            domain.va, image, source_paths_selected[0], **scoped_kwargs
                        )
                    )
                    try:
                        for order, cases in (
                            ("forward", sequence),
                            ("reverse", list(reversed(sequence))),
                            ("repeat", sequence),
                        ):
                            for case in cases:
                                fresh_oracle = UnicornOracle(image.data)
                                fresh_subject = start_subject(binary, image_path)
                                try:
                                    if binding is None:
                                        left = reused_oracle.run(case)
                                        right = reused_subject.run(case)
                                        expected_left, expected_right = (
                                            fresh_oracle.run(case),
                                            fresh_subject.run(case),
                                        )
                                    else:
                                        left, right = cli.run_case(
                                            reused_oracle,
                                            reused_subject,
                                            case,
                                            code_binding=binding,
                                        )
                                        fresh_binding = factory.bind(
                                            domain.va,
                                            image,
                                            source_paths_selected[0],
                                            **scoped_kwargs,
                                        )
                                        expected_left, expected_right = cli.run_case(
                                            fresh_oracle,
                                            fresh_subject,
                                            case,
                                            code_binding=fresh_binding,
                                        )
                                    same = left == expected_left and right == expected_right
                                    if case.index - (91 << 40) in (2232, 792):
                                        expected_regs, expected_writes = expected_valid(
                                            case, domain
                                        )
                                        same = same and left.regs == right.regs == expected_regs
                                        same = (
                                            same and left.writes == right.writes == expected_writes
                                        )
                                        same = same and left.fault is None and right.fault is None
                                    else:
                                        same = (
                                            same
                                            and left.fault is not None
                                            and right.fault is not None
                                        )
                                    receipt["isolation"].append(
                                        dict(
                                            opt=opt,
                                            seed=seed,
                                            va=domain.va,
                                            index=case.index,
                                            order=order,
                                            equal=same,
                                            oracle_fault=left.fault,
                                            subject_fault=right.fault,
                                        )
                                    )
                                    save()
                                    assert same
                                finally:
                                    fresh_subject.close()
                        baseline_oracle, baseline_subject = cli.run_case(
                            reused_oracle, reused_subject, valid, code_binding=binding
                        )
                        assert baseline_oracle.fault is None and baseline_subject.fault is None
                        head, tail = domain.addresses().values()
                        for name in (
                            "drop-global",
                            "reverse-link",
                            "endianness",
                            "omit-payload",
                            "different-global",
                        ):
                            patches = list(valid.patches)
                            if name == "drop-global":
                                patches = [
                                    (address, data)
                                    for address, data in patches
                                    if address != domain.globals[0].address
                                ]
                            elif name == "omit-payload":
                                patches = [
                                    (address, data)
                                    for address, data in patches
                                    if address not in (head, tail)
                                ]
                            elif name in ("reverse-link", "endianness"):
                                patches = [
                                    (
                                        address,
                                        (
                                            data[:4] + head.to_bytes(4, "little") + data[8:]
                                            if name == "reverse-link"
                                            else data[:4][::-1] + data[4:]
                                        )
                                        if address == head
                                        else data,
                                    )
                                    for address, data in patches
                                ]
                            else:
                                patches = [
                                    (
                                        address,
                                        (
                                            tail if domain.globals[1].target == "head" else head
                                        ).to_bytes(4, "little")
                                        if address == domain.globals[1].address
                                        else data,
                                    )
                                    for address, data in patches
                                ]
                            changed = reused_subject.run(replace(valid, patches=tuple(patches)))
                            killed = (
                                changed.fault is not None
                                or changed.regs != baseline_oracle.regs
                                or changed.writes != baseline_oracle.writes
                            )
                            receipt["mutants"].append(
                                dict(
                                    opt=opt,
                                    seed=seed,
                                    va=domain.va,
                                    name=name,
                                    killed=killed,
                                    subject_fault=changed.fault,
                                    regs=changed.regs,
                                    expected_regs=baseline_oracle.regs,
                                    writes=changed.writes,
                                    expected_writes=baseline_oracle.writes,
                                )
                            )
                            save()
                            assert killed
                    finally:
                        signal.alarm(0)
                        reused_subject.close()
            if args.code_safety:
                signal.alarm(60)
                try:
                    guard_group(args.output / f"guard-{opt}", args.code_safety, opt, receipt, save)
                finally:
                    signal.alarm(0)
        if args.code_safety:
            actual_counts = dict(
                runs=len(receipt["runs"]),
                comparisons=sum(sum(run["outcomes"].values()) for run in receipt["runs"]),
                isolation=len(receipt["isolation"]),
                old_mutants=len(receipt["mutants"]),
                guard_controls=len(receipt["guard_controls"]),
                guard_mutants=len(receipt["guard_mutants"]),
                restored=len(receipt["guard_restored"]),
            )
            receipt["completed"] = actual_counts
            save()
            assert actual_counts == receipt["expected"]
        receipt["post_source_pins"] = source_pins(source_paths)
        if source_paths_selected is not None:
            actual_selected = {
                role: {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                if path is not None
                else None
                for role, path in zip(
                    ("functions", "overrides", "additions"), source_paths_selected, strict=True
                )
            }
            receipt["post_selected_sources"] = actual_selected
            assert actual_selected == receipt["selected_sources"]
        save()
        check_source_drift(receipt["source_pins"], source_paths)
    except Exception as error:
        receipt["infrastructure_failure"] = repr(error)
        try:
            receipt["post_source_pins"] = source_pins(source_paths)
        except Exception as pin_error:
            receipt["post_source_pin_failure"] = repr(pin_error)
        save()
        raise
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
