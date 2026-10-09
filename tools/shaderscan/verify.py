# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare a native shader-compiler run with the original only where the original RETURNED.

T202 problem: the emulated original can stop for reasons that are not a return. Its
instruction budget can expire with EAX holding some intermediate value, it can fault, and
it can call `RtlRaiseException` whose dispatch is not emulated. None of those is an HRESULT
and none may be used as an expected value. This module keeps three questions apart.

  * What did the original do? An `Observation` of one `assemble.Run`, whose `outcome` is
    `returned` only when the stdcall return was observed.
  * Does the original read uninitialised heap? Each case also runs on emulators whose
    allocator hands back blocks filled with a byte (`DIRTY_HEAP_FILLS`), as a used title heap
    would. A result that changes with the fill depends on stale heap contents, which the zero
    heap of the reference cannot model, so it is not a stable value to compare either (T387).
  * Does the input make the original read past its end? Each case runs twice, once in a
    zero-padded buffer and once flush against an unmapped guard (`assemble.Run`
    `flush_to_guard`). A result that differs between the two depends on memory the caller
    did not supply, so it is not a stable value to compare against either.
  * Do native and original agree? `compare` returns MATCH or MISMATCH only when the
    original returned from a stable run, and NOT_COMPARED for every other reference outcome
    no matter what the native side did.

The native side supplies its own observations as JSON (`NATIVE_SCHEMA` in `main`), because
how a native run is produced belongs to the host route, not to this reference tool.
Expected values are never stored: the reference JSON is produced by running the original.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from collections.abc import Callable, Sequence
from dataclasses import asdict, dataclass, field
from enum import Enum
from pathlib import Path

from tools.shaderscan import malformed, text_corpus
from tools.shaderscan.assemble import (
    INSN_LIMIT,
    AssemblerEmulator,
    AssemblerSpec,
    Outcome,
    Run,
)
from tools.shaderscan.image import Image
from tools.shaderscan.malformed import Case

#: Bytes the dirty-heap runs fill allocations with. Measured on the retail original: with
#: any of these the 41 returned corpus cases but one keep their result, see
#: docs/shader-assembler-limits.md.
DIRTY_HEAP_FILLS = (0xAA, 0x55, 0xFF)
OVERREAD_REASON = "original result depends on memory past the end of the input"
HEAP_REASON = "original result depends on uninitialised heap contents"
OVERRUN_REASON = "original writes past the end of a heap block, its heap manager decides the rest"

#: Stack argument indexes of the extra outputs (T385), measured with a text source that does
#: not assemble: the constants buffer and the compilation-errors text buffer. A NULL slot
#: means the caller did not ask for it.
CONSTANTS_ARGUMENT = 4
ERRORS_ARGUMENT = 6


@dataclass(frozen=True)
class CorpusSpec:
    """A named corpus and the extra output arguments every case of it is run with."""

    name: str
    cases: Callable[[], list[Case]]
    output_args: tuple[int, ...]


#: `binary` is the T202 vertex token corpus with every optional output NULL, `text` the T385
#: ASCII sources run with the constants and compilation-errors outputs supplied.
CORPORA = {
    "binary": CorpusSpec("binary", malformed.corpus, ()),
    "text": CorpusSpec("text", text_corpus.corpus, (CONSTANTS_ARGUMENT, ERRORS_ARGUMENT)),
}

#: Outcomes a native observation may report. `returned` is the only one with an HRESULT.
NATIVE_OUTCOMES = frozenset(outcome.value for outcome in Outcome)
SCHEMA_VERSION = 1


@dataclass(frozen=True)
class Observation:
    """One side's account of one call. Only `returned` has an HRESULT and output."""

    outcome: str
    hresult: int | None = None
    output_sha256: str | None = None
    output_length: int | None = None
    #: Free text for a human: fault address, raised exception code. Never compared.
    detail: str = ""
    #: The extra outputs (T385). `None` length means the slot stayed NULL or was not supplied,
    #: 0 means the assembler set an empty buffer (no hash for an empty buffer).
    constants_sha256: str | None = None
    constants_length: int | None = None
    errors_sha256: str | None = None
    errors_length: int | None = None
    seh_trace: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if not isinstance(self.seh_trace, tuple):
            object.__setattr__(self, "seh_trace", tuple(self.seh_trace))
        if self.outcome not in NATIVE_OUTCOMES:
            raise ValueError(f"unknown outcome {self.outcome!r}")
        if self.outcome != Outcome.RETURNED.value and (
            self.hresult is not None
            or self.output_sha256 is not None
            or self.constants_length is not None
            or self.errors_length is not None
        ):
            # An HRESULT on an unfinished run is the T202 defect, refuse to build it.
            raise ValueError(f"outcome {self.outcome!r} cannot carry an HRESULT or output")
        if self.outcome == Outcome.RETURNED.value and self.hresult is None:
            raise ValueError("a returned observation needs its HRESULT")


def describe(run: Run) -> str:
    """A short human description of why a run ended, for reports."""
    if run.fault is not None:
        fault = run.fault
        where = "" if fault.address is None else f" {fault.access} at {fault.address:#x}"
        region = "" if fault.region is None else f" ({fault.region})"
        return f"{fault.error} at eip {fault.eip:#x}{where}{region}"
    if run.raised is not None:
        raised = run.raised
        code = "unreadable record" if raised.code is None else f"code {raised.code:#010x}"
        handlers = ",".join(f"{handler:#x}" for handler in raised.handlers)
        ordinal = run.result.kernel_call if run.result is not None else None
        return f"kernel ordinal {ordinal} {code} handlers [{handlers}]"
    if run.outcome is Outcome.KERNEL_CALL and run.result is not None:
        return f"kernel ordinal {run.result.kernel_call}"
    if run.outcome in (Outcome.BUDGET_EXHAUSTED, Outcome.HALTED, Outcome.BAD_CLEANUP):
        return f"stopped at eip {run.eip:#x} esp {run.esp:#x}"
    return ""


def digest_and_length(data: bytes | None) -> tuple[str | None, int | None]:
    """Hash and length of one buffer. NULL gives (None, None), an empty buffer (None, 0)."""
    if data is None:
        return None, None
    return (hashlib.sha256(data).hexdigest() if data else None), len(data)


def observe(run: Run) -> Observation:
    """The `Observation` of a reference run. HRESULT and output only when it returned."""
    if run.outcome is Outcome.RETURNED and run.result is not None:
        data = run.result.data
        extras = dict(run.result.extras)
        constants = digest_and_length(extras.get(CONSTANTS_ARGUMENT))
        errors = digest_and_length(extras.get(ERRORS_ARGUMENT))
        return Observation(
            run.outcome.value,
            run.result.status,
            hashlib.sha256(data).hexdigest() if data else None,
            len(data),
            describe(run),
            *constants,
            *errors,
            _seh_trace(run),
        )
    return Observation(run.outcome.value, detail=describe(run), seh_trace=_seh_trace(run))


def _seh_trace(run: Run) -> tuple[str, ...]:
    steps = []
    for event in run.seh:
        if event.kind in ("raise", "unwind"):
            steps.append(event.kind)
        elif event.kind == "disposition":
            steps.append(f"{event.handler:#x}={event.disposition}")
    return tuple(steps)


class Status(Enum):
    MATCH = "match"
    MISMATCH = "mismatch"
    NOT_COMPARED = "not_compared"


@dataclass(frozen=True)
class Verdict:
    status: Status
    reason: str
    accepted_by_policy: bool = False


def compare(
    reference: Observation,
    native: Observation,
    *,
    stable: bool = True,
    unstable_reason: str = OVERREAD_REASON,
) -> Verdict:
    """Compare native with the original, only when the original returned and is stable.

    `stable` is False when the reference result changed with what lies past the input or
    with the heap contents, and `unstable_reason` says which.
    """
    if reference.outcome != Outcome.RETURNED.value:
        return Verdict(
            Status.NOT_COMPARED,
            f"original did not return ({reference.outcome}), no expected value exists",
        )
    if not stable:
        return Verdict(Status.NOT_COMPARED, unstable_reason)
    if native.outcome != Outcome.RETURNED.value:
        return Verdict(Status.MISMATCH, f"original returned, native {native.outcome}")
    differences = []
    if native.hresult != reference.hresult:
        differences.append(f"HRESULT native {native.hresult:#x} original {reference.hresult:#x}")
    if native.output_length != reference.output_length:
        differences.append(
            f"output length native {native.output_length} original {reference.output_length}"
        )
    if native.output_sha256 != reference.output_sha256:
        differences.append("output bytes differ")
    for label in ("constants", "errors"):
        pair = (getattr(native, f"{label}_length"), getattr(reference, f"{label}_length"))
        if pair[0] != pair[1]:
            differences.append(f"{label} length native {pair[0]} original {pair[1]}")
        elif getattr(native, f"{label}_sha256") != getattr(reference, f"{label}_sha256"):
            differences.append(f"{label} bytes differ")
    if differences:
        return Verdict(Status.MISMATCH, "; ".join(differences))
    return Verdict(Status.MATCH, "HRESULT and output agree")


@dataclass(frozen=True)
class Measurement:
    """The original's behaviour on one case, in both source placements."""

    name: str
    group: str
    padded: Observation
    flush: Observation
    #: True when the two placements disagree, or a fault landed past the guard.
    overread: bool
    instructions: int | None = None
    #: True when the result changed with the fill byte of the allocator (see module doc).
    heap_dependent: bool = False
    #: True when the run left non-zero bytes past the requested size of a heap block (T485).
    #: The reference heap has spare bytes there and the title's real heap has a block header.
    heap_overrun: bool = False

    @property
    def stable(self) -> bool:
        return not self.overread and not self.heap_dependent and not self.heap_overrun

    @property
    def unstable_reason(self) -> str:
        if self.overread:
            return OVERREAD_REASON
        return HEAP_REASON if self.heap_dependent else OVERRUN_REASON


def _stable_key(run: Run) -> tuple[object, ...]:
    """What must agree between placements: not addresses, which differ by construction."""
    observed = observe(run)
    ordinal = run.result.kernel_call if run.result is not None else None
    raised = None if run.raised is None else (run.raised.code, run.raised.flags)
    fault = None if run.fault is None else (run.fault.error, run.fault.eip)
    return (
        observed.outcome,
        observed.hresult,
        observed.output_sha256,
        observed.output_length,
        observed.constants_sha256,
        observed.constants_length,
        observed.errors_sha256,
        observed.errors_length,
        ordinal,
        raised,
        fault,
        observed.seh_trace,
    )


def reads_past_input(padded: Run, flush: Run) -> bool:
    """Whether the original's behaviour depends on bytes after the supplied source."""
    if flush.fault is not None and flush.fault.region == "past_source_guard":
        return True
    return _stable_key(padded) != _stable_key(flush)


def depends_on_heap(
    padded: Run,
    dirty_emulators: Sequence[AssemblerEmulator],
    case: Case,
    *,
    budget: int | None,
    output_args: Sequence[int] = (),
    dispatch_exceptions: bool = False,
) -> bool:
    """Whether the result changes when the allocator returns filled blocks."""
    return any(
        _stable_key(padded)
        != _stable_key(
            emulator.run(
                case.source,
                case.flags,
                budget=budget,
                output_args=output_args,
                dispatch_exceptions=dispatch_exceptions,
            )
        )
        for emulator in dirty_emulators
    )


def measure_case(
    emulator: AssemblerEmulator,
    case: Case,
    *,
    budget: int | None,
    count: bool = False,
    dirty_emulators: Sequence[AssemblerEmulator] = (),
    output_args: Sequence[int] = (),
    dispatch_exceptions: bool = False,
) -> Measurement:
    # An explicit budget wins, then the case's own, then the module default.
    budget = budget if budget is not None else case.budget
    options = dict(budget=budget, output_args=output_args, dispatch_exceptions=dispatch_exceptions)
    padded = emulator.run(case.source, case.flags, track_allocations=True, **options)
    executed = None
    if count and padded.outcome is not Outcome.BUDGET_EXHAUSTED:
        # Counting hooks every instruction, so skip runs that only stopped at the budget.
        executed = emulator.run(
            case.source,
            case.flags,
            budget=budget,
            count_instructions=True,
            output_args=output_args,
            dispatch_exceptions=dispatch_exceptions,
        ).instructions
    flush = emulator.run(case.source, case.flags, flush_to_guard=True, **options)
    return Measurement(
        case.name,
        case.group,
        observe(padded),
        observe(flush),
        reads_past_input(padded, flush),
        executed,
        depends_on_heap(
            padded,
            dirty_emulators,
            case,
            budget=budget,
            output_args=output_args,
            dispatch_exceptions=dispatch_exceptions,
        ),
        padded.overrun_bytes > 0,
    )


def measure(
    emulator: AssemblerEmulator,
    cases: list[Case],
    *,
    budget: int | None = None,
    count: bool = False,
    dirty_emulators: Sequence[AssemblerEmulator] = (),
    output_args: Sequence[int] = (),
    dispatch_exceptions: bool = False,
) -> list[Measurement]:
    return [
        measure_case(
            emulator,
            case,
            budget=budget,
            count=count,
            dirty_emulators=dirty_emulators,
            output_args=output_args,
            dispatch_exceptions=dispatch_exceptions,
        )
        for case in cases
    ]


@dataclass
class Report:
    verdicts: dict[str, Verdict] = field(default_factory=dict)

    def count(self, status: Status) -> int:
        return sum(1 for verdict in self.verdicts.values() if verdict.status is status)

    @property
    def clean(self) -> bool:
        """No mismatch and at least one real comparison."""
        return (
            not any(
                v.status is Status.MISMATCH and not v.accepted_by_policy
                for v in self.verdicts.values()
            )
            and self.count(Status.MATCH) > 0
        )


def compare_all(reference: dict[str, Measurement], native: dict[str, Observation]) -> Report:
    """Compare every reference case with its native observation.

    A case with no native observation is a MISMATCH when the original returned, so a native
    run that silently skipped inputs cannot pass.
    """
    report = Report()
    for name, measured in reference.items():
        observation = native.get(name)
        if observation is None:
            if measured.padded.outcome == Outcome.RETURNED.value and measured.stable:
                report.verdicts[name] = Verdict(Status.MISMATCH, "no native observation")
            else:
                report.verdicts[name] = Verdict(
                    Status.NOT_COMPARED, f"original {measured.padded.outcome}, native not run"
                )
            continue
        verdict = compare(
            measured.padded,
            observation,
            stable=measured.stable,
            unstable_reason=measured.unstable_reason,
        )
        if measured.padded.seh_trace and observation.outcome == Outcome.KERNEL_CALL.value:
            verdict = Verdict(
                verdict.status,
                verdict.reason + " (accepted by T496 policy)",
                accepted_by_policy=True,
            )
        report.verdicts[name] = verdict
    return report


def measurement_to_json(measured: Measurement) -> dict[str, object]:
    return asdict(measured)


def measurement_from_json(raw: dict[str, object]) -> Measurement:
    padded, flush = raw["padded"], raw["flush"]
    if not isinstance(padded, dict) or not isinstance(flush, dict):
        raise ValueError("reference entry lacks its observations")
    return Measurement(
        str(raw["name"]),
        str(raw["group"]),
        Observation(**padded),
        Observation(**flush),
        bool(raw["overread"]),
        raw.get("instructions"),  # type: ignore[arg-type]
        bool(raw.get("heap_dependent", False)),
        bool(raw.get("heap_overrun", False)),
    )


def load_native(path: Path) -> dict[str, Observation]:
    """Native observations: `{"schema": 1, "cases": {name: Observation fields}}`."""
    document = json.loads(path.read_text())
    if document.get("schema") != SCHEMA_VERSION:
        raise SystemExit(f"{path}: schema must be {SCHEMA_VERSION}")
    return {name: Observation(**fields) for name, fields in document["cases"].items()}


def native_dispatch(path: Path) -> bool:
    return bool(json.loads(path.read_text()).get("dispatch", False))


def load_reference(path: Path) -> dict[str, Measurement]:
    document = json.loads(path.read_text())
    if document.get("schema") != SCHEMA_VERSION:
        raise SystemExit(f"{path}: schema must be {SCHEMA_VERSION}")
    return {item["name"]: measurement_from_json(item) for item in document["cases"]}


def reference_dispatch(path: Path) -> bool:
    """Dispatch mode recorded by `corpus`; old reference files mean dispatch off."""
    return bool(json.loads(path.read_text()).get("dispatch", False))


def render_table(measurements: list[Measurement]) -> str:
    lines = [
        f"{'case':32} {'padded':16} {'flush':16} {'hresult':10} {'bytes':>5} {'insns':>8} note"
    ]
    for item in measurements:
        padded = item.padded
        hresult = "-" if padded.hresult is None else f"{padded.hresult:#010x}"
        length = "-" if padded.output_length is None else str(padded.output_length)
        count = "-" if item.instructions is None else str(item.instructions)
        extras = ""
        if padded.constants_length is not None or padded.errors_length is not None:
            extras = f"const {padded.constants_length} err {padded.errors_length} "
        note = (
            ("OVERREAD " if item.overread else "")
            + ("HEAPDEP " if item.heap_dependent else "")
            + ("OVERRUN " if item.heap_overrun else "")
            + extras
            + padded.detail
        )
        lines.append(
            f"{item.name:32} {padded.outcome:16} {item.flush.outcome:16} {hresult:10} "
            f"{length:>5} {count:>8} {note}"
        )
    return "\n".join(lines)


def _int(text: str) -> int:
    return int(text, 0)


def _spec(args: argparse.Namespace) -> AssemblerSpec:
    return AssemblerSpec(
        args.assembler, args.heap_alloc, args.heap_free, static_init=args.float_init or None
    )


def _selected(args: argparse.Namespace) -> list[Case]:
    cases = CORPORA[args.corpus].cases()
    if args.case:
        known = {case.name for case in cases}
        missing = [name for name in args.case if name not in known]
        if missing:
            raise SystemExit(f"unknown case {missing}")
        cases = [case for case in cases if case.name in set(args.case)]
    return cases


def _run_corpus(args: argparse.Namespace) -> int:
    image = Image.load(args.xbe)
    emulator = AssemblerEmulator(image, _spec(args))
    fills = () if args.no_heap_fill else DIRTY_HEAP_FILLS
    dirty = [AssemblerEmulator(image, _spec(args), heap_fill=fill) for fill in fills]
    chosen = CORPORA[args.corpus]
    measurements = measure(
        emulator,
        _selected(args),
        budget=args.budget,
        count=args.count,
        dirty_emulators=dirty,
        output_args=chosen.output_args,
        dispatch_exceptions=args.dispatch,
    )
    print(render_table(measurements))
    tally: dict[str, int] = {}
    for item in measurements:
        tally[item.padded.outcome] = tally.get(item.padded.outcome, 0) + 1
    print("outcomes (zero-padded placement):", json.dumps(tally, sort_keys=True))
    print(
        f"heap dependent (result changes with the allocator fill {[hex(f) for f in fills]}):",
        sum(1 for item in measurements if item.heap_dependent),
    )
    if args.json:
        document = {
            "schema": SCHEMA_VERSION,
            "corpus": chosen.name,
            "dispatch": args.dispatch,
            "budget": args.budget or INSN_LIMIT,
            "cases": [measurement_to_json(item) for item in measurements],
        }
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps(document, indent=1) + "\n")
    return 0


def _escalate(args: argparse.Namespace) -> int:
    """Re-run the cases that exhaust the default budget with a much larger one.

    Shows whether an exhausted case was slow (it now returns) or runaway (it still
    exhausts, or faults once it runs off a buffer). Neither is a returned error.
    """
    emulator = AssemblerEmulator(Image.load(args.xbe), _spec(args))
    exhausted = 0
    for case in _selected(args):
        if emulator.run(case.source, case.flags).outcome is not Outcome.BUDGET_EXHAUSTED:
            continue
        exhausted += 1
        run = emulator.run(case.source, case.flags, budget=args.escalated_budget)
        print(f"{case.name:32} {run.outcome.value:16} {describe(run)}")
    print(f"{exhausted} cases exhausted the default budget of {INSN_LIMIT}")
    return 0 if exhausted else 1


def _export(args: argparse.Namespace) -> int:
    args.out.mkdir(parents=True, exist_ok=True)
    manifest = []
    for case in _selected(args):
        (args.out / f"{case.name}.bin").write_bytes(case.source)
        manifest.append({"name": case.name, "flags": case.flags, "length": len(case.source)})
    chosen = CORPORA[args.corpus]
    document = {
        "schema": SCHEMA_VERSION,
        "corpus": chosen.name,
        "output_args": list(chosen.output_args),
        "dispatch": args.dispatch,
        "cases": manifest,
    }
    (args.out / "manifest.json").write_text(json.dumps(document, indent=1) + "\n")
    print(f"wrote {len(manifest)} synthetic sources to {args.out}")
    return 0


def _compare(args: argparse.Namespace) -> int:
    if reference_dispatch(args.reference) != native_dispatch(args.native):
        raise SystemExit("reference and native files disagree on --dispatch mode")
    report = compare_all(load_reference(args.reference), load_native(args.native))
    for name, verdict in report.verdicts.items():
        print(f"{verdict.status.value:13} {name:36} {verdict.reason}")
    matched, mismatched = report.count(Status.MATCH), report.count(Status.MISMATCH)
    print(f"match {matched} mismatch {mismatched} not_compared {report.count(Status.NOT_COMPARED)}")
    if matched == 0:
        print("no case was compared: the original returned for none of them", file=sys.stderr)
    return 0 if report.clean else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def add_corpus_options(command: argparse.ArgumentParser) -> None:
        command.add_argument("--case", action="append", help="run only this case, repeatable")
        command.add_argument(
            "--corpus",
            choices=sorted(CORPORA),
            default="binary",
            help="binary: T202 vertex tokens, NULL outputs. text: T385 ASCII with outputs",
        )

    def add_emulator_options(command: argparse.ArgumentParser) -> None:
        command.add_argument("--xbe", type=Path, default=Path("tmp/oxm-extract/retail/default.xbe"))
        command.add_argument("--assembler", type=_int, default=0x3EE2B3)
        command.add_argument("--heap-alloc", type=_int, default=0x383678)
        command.add_argument("--heap-free", type=_int, default=0x383DF3)
        command.add_argument(
            "--float-init",
            type=_int,
            default=0x3C843D,
            help="the title's float table initialiser, 0 for none (see AssemblerSpec.static_init)",
        )
        command.add_argument(
            "--budget", type=int, default=None, help=f"instruction budget, default {INSN_LIMIT}"
        )

    run = sub.add_parser("corpus", help="run the corpus on the original, both placements")
    add_corpus_options(run)
    add_emulator_options(run)
    run.add_argument("--json", type=Path, help="write the reference observations here")
    run.add_argument("--count", action="store_true", help="count instructions (slow)")
    run.add_argument(
        "--no-heap-fill", action="store_true", help="skip the dirty-heap stability runs"
    )
    run.add_argument("--dispatch", action="store_true", help="emulate RtlRaiseException/RtlUnwind")
    run.set_defaults(handler=_run_corpus)

    escalate = sub.add_parser("escalate", help="re-run budget-exhausted cases with a larger budget")
    add_corpus_options(escalate)
    add_emulator_options(escalate)
    escalate.add_argument("--escalated-budget", type=int, default=1_000_000_000)
    escalate.set_defaults(handler=_escalate)

    export = sub.add_parser("export", help="write the synthetic sources for a native run")
    add_corpus_options(export)
    export.add_argument("--out", type=Path, required=True)
    export.add_argument(
        "--dispatch", action="store_true", help="mark export for dispatched reference comparison"
    )
    export.set_defaults(handler=_export)

    check = sub.add_parser("compare", help="compare native observations with the reference")
    check.add_argument("--reference", type=Path, required=True)
    check.add_argument("--native", type=Path, required=True)
    check.set_defaults(handler=_compare)

    args = parser.parse_args(argv)
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())
