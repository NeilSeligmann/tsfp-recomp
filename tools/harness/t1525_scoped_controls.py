# SPDX-License-Identifier: GPL-3.0-or-later
"""Frozen original-independent scoped guard fixtures; execution requires a CPU lease."""

from __future__ import annotations

import hashlib
from collections.abc import Callable
from copy import deepcopy
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

from . import code_safety
from .code_safety import Declaration, ExecutionGuard, NamedGlobalCodeSafetyError, SourceBinding
from .effective_index import SelectedSources
from .image import GuestImage
from .model import Case, ExecResult
from .named_global_objects import Factory, section_digest
from .oracle import UnicornOracle
from .seeding import GUEST_LO, GUEST_SPAN, make_case

Paths = tuple[Path, Path | None, Path | None]
SEEDS = (20261001, 20261006)
HEADER = b"entry_va,size_bytes,name,is_thunk,body_max_va\n"


def sources(
    directory: Path, mode: str, rows: tuple[tuple[int, int], ...], *, callees: bool = False
) -> Paths:
    if mode not in ("raw", "effective"):
        raise ValueError("unknown scoped synthetic mode")
    directory.mkdir(exist_ok=False)
    functions = directory / "functions.csv"
    overrides = directory / "overrides.csv" if mode == "effective" else None
    additions = directory / "additions.csv" if mode == "effective" else None
    lines = []
    for position, (va, size) in enumerate(rows):
        if mode == "effective" and callees and position:
            continue
        count = size + 16 if mode == "effective" else size
        lines.append(f"0x{va:08x},{count},synthetic_{va:x},false,0x{va + count - 1:08x}\n")
    functions.write_bytes(HEADER + "".join(lines).encode())
    if overrides is not None:
        overrides.write_text(
            "entry_va,size_bytes,reason\n"
            + "".join(
                f"0x{va:08x},{size},frozen synthetic actual extent\n"
                for va, size in (rows[:1] if callees else rows)
            )
        )
        additions.write_text(
            "entry_va,size_bytes,evidence\n"
            + "".join(
                f"0x{va:08x},{size},frozen synthetic callee bytes\n"
                for va, size in (rows[1:] if callees else ())
            )
        )
    return functions, overrides, additions


def declaration(
    image: GuestImage, mode: str, paths: Paths, root: int, slots: tuple[tuple[int, int], ...]
) -> Declaration:
    functions, overrides, additions = paths
    if mode == "raw":
        cert = code_safety.build_certificate(image, functions.read_bytes(), root, slots)
    else:
        effective = SelectedSources(functions, overrides, additions).effective
        cert = code_safety.build_certificate(
            image, effective.table, root, slots, effective_source=effective
        )
    return Declaration.from_document(cert.document())


def scoped_fixture(
    image: GuestImage, factory: Factory, directory: Path, mode: str
) -> tuple[GuestImage, Factory, Paths]:
    sections = tuple(replace(section, executable=True) for section in image.source_sections)
    image = replace(image, source_sections=sections)
    paths = sources(
        directory, mode, tuple((domain.va, domain.body_size) for domain in factory.domains)
    )
    domains = tuple(
        replace(
            domain,
            sections_sha256=section_digest(sections),
            code_safety=declaration(
                image,
                mode,
                paths,
                domain.va,
                tuple((g.address, g.address + 4) for g in domain.globals),
            ),
        )
        for domain in factory.domains
    )
    return image, Factory(domains), paths


PROGRAMS = {
    "straight": (bytes.fromhex("b807000000c3"), None),
    "call": (bytes.fromhex("e8fb000000c3"), bytes.fromhex("b807000000c3")),
    "ret4": (bytes.fromhex("b807000000c20400"), None),
    "load-fault": (bytes.fromhex("8b00c3"), None),
    "ret-read-fault": (bytes.fromhex("bc00000090c3"), None),
    "wrong-return": (bytes.fromhex("b807000000c3"), None),
    "unexecuted-callee": (bytes.fromhex("31c085c07405e8f5000000c3"), bytes.fromhex("b807000000c3")),
    "code-write": (bytes.fromhex("c6050700010090b807000000c3"), None),
}


def micro_fixture(directory: Path, mode: str, name: str) -> tuple[SourceBinding, Case]:
    from .image import SourceSection

    root, callee = PROGRAMS[name]
    raw = bytearray(GUEST_SPAN)
    offset = 0x10000 - GUEST_LO
    raw[offset : offset + len(root)] = root
    rows = [(0x10000, len(root))]
    if callee is not None:
        raw[offset + 0x100 : offset + 0x100 + len(callee)] = callee
        rows.append((0x10100, len(callee)))
    image = GuestImage(bytes(raw), GUEST_LO, (SourceSection(0x10000, 0x1000, True, True),))
    paths = sources(directory, mode, tuple(rows), callees=True)
    declared = declaration(image, mode, paths, 0x10000, ((0x10800, 0x10804),))
    binding = SourceBinding(declared, image, paths[0], overrides=paths[1], additions=paths[2])
    case = make_case(20261001, 0, 0x10000, len(root))
    if name == "load-fault":
        case = replace(case, regs=(0x90000000, *case.regs[1:]))
    elif name == "wrong-return":
        case = replace(case, patches=(*case.patches, (case.esp, (0x10800).to_bytes(4, "little"))))
    elif name == "unexecuted-callee":
        case = replace(case, patches=(*case.patches, (0x10101, b"\x08")))
    return binding, case


def observe(oracle: UnicornOracle, binding: SourceBinding, case: Case) -> ExecResult:
    result = oracle.run(case, code_safety=binding.certificate, code_safety_source=binding.source())
    binding.observe(case, getattr(oracle, "code_safety_observation", None))
    return result


def state(result: ExecResult) -> dict[str, object]:
    return dict(
        regs=result.regs,
        flags=result.flags,
        writes=result.writes,
        fault=result.fault,
        fp=result.fp,
        fp_control=result.fp_control,
        fp_status=result.fp_status,
        covered_vas=sorted(result.reach.covered_vas),
        insns=result.reach.insns,
    )


def require_local_reader_refusal(proof: dict[str, object]) -> None:
    from tools.replace.proof_contract import validate_document

    try:
        validate_document(proof)
    except ValueError as error:
        assert "unvalidated local" in str(error)
        return
    raise AssertionError("ordinary proof reader accepted locally unvalidated capability")


def guard_group(
    directory: Path, mode: str, opt: str, receipt: dict, save: Callable[[], None]
) -> None:
    """Run exactly eight actual emulator observations and five named mutations."""
    directory.mkdir(exist_ok=False)
    fixtures = {}
    for name in PROGRAMS:
        binding, case = micro_fixture(directory / name, mode, name)
        fixtures[name] = binding, case
        oracle = UnicornOracle(binding.image.data)
        row = dict(
            mode=mode,
            opt=opt,
            name=name,
            certificate=binding.certificate.document(),
            input_index=case.index,
            expected="fatal"
            if name in ("wrong-return", "unexecuted-callee", "code-write")
            else "fault"
            if name in ("load-fault", "ret-read-fault")
            else "return",
        )
        receipt["guard_controls"].append(row)
        save()
        try:
            result = observe(oracle, binding, case)
        except NamedGlobalCodeSafetyError as error:
            row.update(kind=error.kind, error=str(error), observed=error.observed)
            if name == "code-write":
                row["prior_write_evidence"] = {
                    "address": 0x10007,
                    "original": binding.image.code_at(0x10007, 1).hex(),
                    "actual": bytes(oracle._uc.mem_read(0x10007, 1)).hex(),
                    "touched_pages": sorted(oracle._touched),
                    "executed_insns": oracle._insns,
                }
                assert row["prior_write_evidence"]["original"] == "b8"
                assert row["prior_write_evidence"]["actual"] == "90"
                assert 0x10000 in oracle._touched and oracle._insns >= 1
            elif name == "wrong-return":
                assert "consumed return" in str(error)
            elif name == "unexecuted-callee":
                assert "post-patch certified code changed" in str(error)
                assert oracle._insns == 0
            save()
            assert row["expected"] == "fatal"
        else:
            row.update(state=state(result), observation=oracle.code_safety_observation)
            save()
            assert row["expected"] != "fatal"
            if row["expected"] == "fault":
                assert result.fault is not None and "READ" in result.fault
                assert oracle.code_safety_observation["outcome"] == "faulted"
            else:
                assert result.fault is None and result.regs[0] == 7
                assert result.regs[4] == case.esp + (8 if name == "ret4" else 4)
                assert oracle.code_safety_observation["outcome"] == "returned"
        row["passed"] = True
        save()

    def require_fatal(name: str) -> None:
        binding, case = fixtures[name]
        oracle = UnicornOracle(binding.image.data)
        try:
            result = observe(oracle, binding, case)
        except NamedGlobalCodeSafetyError:
            return
        row["mutant_execution"] = state(result)
        raise AssertionError(f"{name} lost required named campaign fatal refusal")

    mutations = (
        (
            "disable-all-node-preflight",
            patch.object(ExecutionGuard, "preflight", lambda *a: None),
            lambda: require_fatal("unexecuted-callee"),
        ),
        (
            "disable-actual-instruction-hook",
            patch.object(ExecutionGuard, "before", lambda *a: None),
            lambda: require_fatal("wrong-return"),
        ),
    )
    for name, mutation, witness in mutations:
        row = dict(mode=mode, opt=opt, name=name, killed=False)
        if name == "disable-all-node-preflight":
            baseline_binding, bad_case = fixtures["unexecuted-callee"]
            clean_case = replace(bad_case, patches=bad_case.patches[:-1])
            clean = observe(
                UnicornOracle(baseline_binding.image.data), baseline_binding, clean_case
            )
            assert clean.fault is None and clean.regs[0] == 0
            assert clean.regs[4] == clean_case.esp + 4
            row["clean_prerequisite"] = state(clean)
        receipt["guard_mutants"].append(row)
        with mutation:
            try:
                witness()
            except AssertionError as error:
                row.update(killed=True, failed_assertion=str(error))
        save()
        assert row["killed"]

    # These exercise actual completed emulator observations and producer/reader
    # failure paths, rather than substituting a mock execution for a valid fixture.
    binding, case = micro_fixture(directory / "producer-mutants", mode, "straight")
    oracle = UnicornOracle(binding.image.data)
    result = observe(oracle, binding, case)
    assert result.fault is None and result.regs[0] == 7
    original = binding.functions if mode == "raw" else binding._selected.paths[1]
    before = original.read_bytes()
    original.write_bytes(before + b"\n")
    try:

        def require_live_refusal() -> None:
            try:
                binding.source()
            except ValueError:
                return
            raise AssertionError("producer accepted changed selected source bytes")

        require_live_refusal()
        target = (
            patch.object(SourceBinding, "source", lambda self: (self.image, self.index))
            if mode == "raw"
            else patch.object(SelectedSources, "check", lambda self: None)
        )
        drift_row = dict(mode=mode, opt=opt, name="disable-live-source-check", killed=False)
        receipt["guard_mutants"].append(drift_row)
        with target:
            try:
                require_live_refusal()
            except AssertionError as error:
                drift_row.update(killed=True, failed_assertion=str(error))
            allowed = observe(UnicornOracle(binding.image.data), binding, case)
            assert allowed.fault is None and allowed.regs[0] == 7
        save()
        assert drift_row["killed"]
    finally:
        original.write_bytes(before)

    from . import cli

    class UnexpectedSubjectInvocation(AssertionError):
        pass

    class ClearingOracle:
        code_safety_observation = None

        def run(self, *args: object, **kwargs: object) -> ExecResult:
            actual = UnicornOracle(binding.image.data)
            observed = actual.run(*args, **kwargs)
            assert observed.fault is None and observed.regs[0] == 7
            missing_row["actual_prerequisite_state"] = state(observed)
            missing_row["cleared_actual_observation"] = actual.code_safety_observation
            return observed

    class SubjectBarrier:
        invocations = 0

        def run(self, *args: object, **kwargs: object) -> ExecResult:
            self.invocations += 1
            raise UnexpectedSubjectInvocation(
                "subject reached after missing actual guard observation"
            )

    missing_row = dict(mode=mode, opt=opt, name="disable-missing-observation-refusal", killed=False)
    receipt["guard_mutants"].append(missing_row)
    barrier = SubjectBarrier()
    try:
        cli.run_case(ClearingOracle(), barrier, case, code_binding=binding)
    except ValueError as error:
        assert "observation" in str(error)
    else:
        raise AssertionError("missing-observation real CLI baseline did not refuse")
    assert barrier.invocations == 0
    with patch.object(SourceBinding, "observe", lambda *args: None):
        try:
            cli.run_case(ClearingOracle(), barrier, case, code_binding=binding)
        except UnexpectedSubjectInvocation as error:
            missing_row.update(killed=True, failed_assertion=str(error))
    missing_row["subject_invocations"] = barrier.invocations
    save()
    assert missing_row["killed"] and barrier.invocations == 1

    actual_proof = deepcopy(
        next(
            run["proof"]
            for run in receipt["runs"]
            if run["opt"] == opt and run["phase"] == "default"
        )
    )
    for proof_row in actual_proof["functions"]:
        proof_row["named_global_code_safety"]["validated"] = True

    with patch.object(code_safety, "RUNTIME_VALIDATED", False):
        require_local_reader_refusal(actual_proof)
    reader_row = dict(mode=mode, opt=opt, name="bypass-unvalidated-reader-capability", killed=False)
    receipt["guard_mutants"].append(reader_row)
    with patch.object(code_safety, "RUNTIME_VALIDATED", False):
        require_local_reader_refusal(actual_proof)
        with patch.object(code_safety, "RUNTIME_VALIDATED", True):
            try:
                require_local_reader_refusal(actual_proof)
            except AssertionError as error:
                reader_row.update(killed=True, failed_assertion=str(error))
    save()
    assert reader_row["killed"]
    final = observe(UnicornOracle(binding.image.data), binding, case)
    assert final.fault is None and final.regs[0] == 7
    assert code_safety.RUNTIME_VALIDATED is True
    receipt["guard_restored"].append(
        dict(
            mode=mode,
            opt=opt,
            eax=final.regs[0],
            source_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),
            capability=code_safety.RUNTIME_VALIDATED,
        )
    )
    save()
