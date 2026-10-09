#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the T202 malformed-input corpus through the retained NATIVE shader compiler (T387).

The driver feeds each exported source (`python -m tools.shaderscan.verify export`) to the
real boot with the opt-in route through `tsfp_shader_probe --probe-corpus` (the in-boot phase
at the lock free getter 0x3E6714, entering `xdk_original_dispatch` like the title does) and
writes schema 1 observations that `verify compare` checks against a fresh reference.

A fault ends the host process, so the default is one process per case. What happened is read
from the probe records and the host stop report, never invented:

  * a `corpus_call` record whose route dispatch ran       -> returned (HRESULT and output)
  * the host stop `fatal kernel call` at RtlRaiseException -> kernel_call
  * a host stop `host fault` (a signal such as SIGSEGV)    -> fault
  * the watchdog killing a case that began and never ended -> budget_exhausted
  * anything else                                          -> a DriverError, no observation

No HRESULT is ever attached to a case that did not return. The driver prints only counts,
sizes, hashes and classes, never shader bytes.

    python -m tools.shaderscan.verify export --out tmp/corpus
    python -m tools.shaderscan.native_corpus run --corpus tmp/corpus --out tmp/native.json
    python -m tools.shaderscan.verify compare --reference tmp/ref.json --native tmp/native.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import tempfile
from collections import Counter
from collections.abc import Callable, Sequence
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

from tools.shaderscan import native_probe, verify
from tools.shaderscan.assemble import Outcome
from tools.shaderscan.malformed import Case
from tools.shaderscan.native_probe import ProbeRun
from tools.shaderscan.verify import (
    CORPORA,
    SCHEMA_VERSION,
    Measurement,
    Observation,
    Status,
    compare_all,
    digest_and_length,
    load_reference,
)

#: The stdcall `RET 0x2C` of the compiler: return address plus 11 arguments.
EXPECTED_ESP_DELTA = 48
#: Hard wall-clock bound for an isolated case. A normal boot with one corpus case takes
#: about 0.2 s; every case has a separate process, so timeout containment cannot affect a
#: later title-owned compile or leave the route in a partially mutated state.
DEFAULT_TIMEOUT = 5.0
#: Boots that die before the corpus phase (the known 3 percent flake) are retried this often.
ATTEMPTS = 4
#: The probe copies at most this many output bytes. A larger output cannot be hashed.
PROBE_OUTPUT_LIMIT = 2048

_FAULT_ADDRESS = re.compile(r"fault address\s+(0x[0-9A-Fa-f]+)")
_STOP_MARKER = "--- where each host thread stopped ---"
#: The stop report is followed by a table of every HLE entry with call counts, which names
#: kernel imports that were never called. Only the text before it describes the stop.
_STOP_END = "HLE calls reached"
_FATAL_CALL = re.compile(r"stopped: fatal kernel call[^\n]*\n\s+ordinal\s+(\d+)\s+\((\w+)\)")


class DriverError(RuntimeError):
    """The native run could not be turned into an honest observation."""


def spec_line(case: Case, path: Path) -> str:
    """One `name flags path` line of the probe's corpus spec."""
    for text in (case.name, str(path)):
        if not text or re.search(r"\s", text):
            raise DriverError(f"{text!r} cannot be written to a whitespace separated spec")
    return f"{case.name} {case.flags} {path}\n"


def manifest_corpus(directory: Path) -> verify.CorpusSpec:
    """Which corpus an export directory holds (`binary` for a manifest from before T385)."""
    manifest = json.loads((directory / "manifest.json").read_text())
    name = manifest.get("corpus", "binary")
    if name not in CORPORA:
        raise DriverError(f"{directory}: unknown corpus {name!r}")
    chosen = CORPORA[name]
    if tuple(manifest.get("output_args", ())) != chosen.output_args:
        raise DriverError(f"{directory}: output arguments differ from the {name} corpus")
    return chosen


def manifest_dispatch(directory: Path) -> bool:
    return bool(json.loads((directory / "manifest.json").read_text()).get("dispatch", False))


def _check_dispatch(directory: Path, dispatch: bool, reference: Path | None = None) -> None:
    if manifest_dispatch(directory) != dispatch:
        raise DriverError(f"{directory}: --dispatch does not match the export manifest")
    if reference is not None and verify.reference_dispatch(reference) != dispatch:
        raise DriverError(f"{reference}: --dispatch does not match the reference measurement")


def load_exported(directory: Path) -> list[Case]:
    """The cases of an export directory, checked against the in-tree corpus.

    The sources come from the files `verify export` wrote, so the native run reads the very
    bytes that were exported. They must equal the named corpus so the reference and the
    native run cannot be built from two different corpora.
    """
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest.get("schema") != SCHEMA_VERSION:
        raise DriverError(f"{directory}: manifest schema must be {SCHEMA_VERSION}")
    known = {case.name: case for case in manifest_corpus(directory).cases()}
    cases: list[Case] = []
    for entry in manifest["cases"]:
        template = known.get(entry["name"])
        if template is None:
            raise DriverError(f"exported case {entry['name']!r} is not in the corpus")
        source = (directory / f"{entry['name']}.bin").read_bytes()
        if source != template.source or entry["flags"] != template.flags:
            raise DriverError(f"exported case {entry['name']!r} differs from the corpus")
        cases.append(template)
    return cases


def _buffer_bytes(output: dict, label: str, name: str) -> bytes:
    """The bytes of one copied buffer, refusing one the probe did not copy in full."""
    if output["copied"] != output["size"] or output["size"] > PROBE_OUTPUT_LIMIT:
        raise DriverError(f"{label} of {name} is {output['size']} bytes, not fully read")
    data = bytes.fromhex(output["hex"])
    if len(data) != output["size"]:
        raise DriverError(f"{label} hex of {name} has the wrong length")
    return data


def _output_observation(record: dict) -> tuple[str | None, int, str]:
    """Output hash, length and a note. No output is length 0, as the reference models it.

    The reference reads the output only for HRESULT 0 with a buffer, and calls everything
    else empty. The native side applies the same rule and says in the note when a slot was
    nevertheless set on a failure or its data was unreadable, so that is not hidden.
    """
    output = record.get("output")
    slot_object = record.get("slot_object", 0)
    if record["hresult"] != 0:
        return None, 0, "" if slot_object == 0 else "failure left an output object"
    if output is None:
        return None, 0, "" if slot_object == 0 else "output object unreadable"
    data = _buffer_bytes(output, "output", record["name"])
    return (hashlib.sha256(data).hexdigest() if data else None), len(data), ""


def _extra_observation(record: dict, label: str) -> tuple[str | None, int | None]:
    """Hash and length of the constants or errors buffer, as the reference models them.

    A NULL slot is (None, None) and a slot that is not an XGBuffer or holds nothing is the
    empty buffer (None, 0). A record without the key (outputs not supplied) is (None, None).
    """
    if label not in record:
        return None, None
    if record[f"{label}_slot_object"] == 0:
        return None, None
    output = record[label]
    if output is None:
        return None, 0
    return digest_and_length(_buffer_bytes(output, label, record["name"]))


def observe_call(record: dict) -> Observation:
    """The observation of a `corpus_call` record, which exists only when the call ended."""
    if not record["dispatched"]:
        raise DriverError(f"{record['name']}: the route did not dispatch the compiler")
    clean = (
        record["esp_delta"] == EXPECTED_ESP_DELTA
        and record["regs_preserved"]
        and record["fs0_preserved"]
    )
    detail = f"esp_delta {record['esp_delta']}"
    if not clean:
        # Reached the end with the wrong stack or clobbered registers: not a clean return.
        return Observation(Outcome.BAD_CLEANUP.value, detail=f"{detail} regs/fs0 not preserved")
    digest, length, note = _output_observation(record)
    return Observation(
        Outcome.RETURNED.value,
        record["hresult"],
        digest,
        length,
        f"{detail} {note}".strip(),
        *_extra_observation(record, "constants"),
        *_extra_observation(record, "errors"),
    )


def stop_section(log: str) -> str:
    """The per-thread stop report only, without the HLE call table that follows it."""
    if _STOP_MARKER not in log:
        return ""
    section = log.split(_STOP_MARKER)[-1]
    return section.split(_STOP_END)[0]


def observe_stop(run: ProbeRun) -> Observation:
    """The observation of a case that began and did not return, from how the process ended."""
    stop = stop_section(run.log)
    # Every guest thread repeats the one host-wide stop, so the reasons are a set.
    fatal = set(_FATAL_CALL.findall(stop))
    raised = fatal == {("302", "RtlRaiseException")}
    faulted = "stopped: host fault" in stop
    if run.timed_out:
        return Observation(
            Outcome.BUDGET_EXHAUSTED.value, detail="watchdog expired, process killed"
        )
    if raised and faulted:
        raise DriverError("both a raise and a fault in the stop report")
    if fatal and not raised:
        raise DriverError(f"fatal kernel call other than RtlRaiseException: {sorted(fatal)}")
    if raised:
        return Observation(Outcome.KERNEL_CALL.value, detail="fatal stop at RtlRaiseException")
    if faulted:
        found = _FAULT_ADDRESS.search(stop)
        where = f" at {found.group(1)}" if found else ""
        return Observation(Outcome.FAULT.value, detail=f"host fault, signal 11{where}")
    raise DriverError(f"exit code {run.returncode}, no recognised stop")


def corpus_records(run: ProbeRun, kind: str) -> list[dict]:
    return [row for row in run.records if row.get("kind") == kind]


def observe_run(case: Case, run: ProbeRun) -> Observation:
    """Classify one single-case process. `run` must have reached the corpus phase."""
    begun = [row for row in corpus_records(run, "corpus_begin") if row["name"] == case.name]
    if not begun:
        raise DriverError(f"{case.name}: the corpus phase never started")
    errors = corpus_records(run, "corpus_error")
    if errors:
        raise DriverError(f"{case.name}: {errors[0]['error']}")
    calls = [row for row in corpus_records(run, "corpus_call") if row["name"] == case.name]
    if len(calls) > 1:
        raise DriverError(f"{case.name}: {len(calls)} call records")
    if calls:
        return observe_call(calls[0])
    return observe_stop(run)


def run_case(
    probe: Path,
    xbe: Path,
    case: Case,
    source: Path,
    *,
    flush: bool,
    timeout: float,
    outputs: bool = False,
    attempts: int = ATTEMPTS,
    runner: Callable[..., ProbeRun] = native_probe.run_once,
) -> Observation:
    """Boot once with the case, retrying only boots that died before the corpus phase."""
    with tempfile.TemporaryDirectory(prefix="native_corpus_") as raw:
        spec = Path(raw) / "spec.txt"
        spec.write_text(spec_line(case, source))
        flags = ["--probe-corpus", str(spec), "--probe-corpus-flush", "1" if flush else "0"]
        if outputs:
            flags += ["--probe-corpus-outputs", "1"]
        for _ in range(attempts):
            run = runner(probe, xbe, flags, timeout)
            if corpus_records(run, "corpus_begin"):
                return observe_run(case, run)
    raise DriverError(f"{case.name}: {attempts} boots never reached the corpus phase")


@dataclass(frozen=True)
class Crosstab:
    """How many cases fall in each (original outcome, native outcome) pair."""

    counts: Counter[tuple[str, str]]

    def render(self) -> str:
        lines = [f"{'original':18} {'native':18} {'cases':>5}"]
        for (original, native), count in sorted(self.counts.items()):
            lines.append(f"{original:18} {native:18} {count:5d}")
        return "\n".join(lines)


def crosstab(reference: dict[str, Measurement], native: dict[str, Observation]) -> Crosstab:
    counts: Counter[tuple[str, str]] = Counter()
    for name, measured in reference.items():
        seen = native.get(name)
        counts[(measured.padded.outcome, "not run" if seen is None else seen.outcome)] += 1
    return Crosstab(counts)


def render_cases(reference: dict[str, Measurement], native: dict[str, Observation]) -> str:
    """One row per case: the original's outcome, whether it is stable, and the native result."""
    lines = [f"{'case':32} {'original':16} {'stable':6} {'native':14} {'hresult':10} {'bytes':>5}"]
    for name, measured in reference.items():
        seen = native.get(name)
        hresult = "-" if seen is None or seen.hresult is None else f"{seen.hresult:#010x}"
        length = "-" if seen is None or seen.output_length is None else str(seen.output_length)
        lines.append(
            f"{name:32} {measured.padded.outcome:16} {'yes' if measured.stable else 'NO':6} "
            f"{'not run' if seen is None else seen.outcome:14} {hresult:10} {length:>5}"
        )
    return "\n".join(lines)


def write_native(
    path: Path, observations: dict[str, Observation], *, dispatch: bool = False
) -> None:
    """Schema 1 native observations, the input of `verify compare`."""
    document = {
        "schema": SCHEMA_VERSION,
        "dispatch": dispatch,
        "cases": {
            name: {
                "outcome": item.outcome,
                "hresult": item.hresult,
                "output_sha256": item.output_sha256,
                "output_length": item.output_length,
                "detail": item.detail,
                "constants_sha256": item.constants_sha256,
                "constants_length": item.constants_length,
                "errors_sha256": item.errors_sha256,
                "errors_length": item.errors_length,
            }
            for name, item in sorted(observations.items())
        },
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=1) + "\n")


def stable_key(item: Observation) -> tuple[object, ...]:
    """What must repeat between two runs. The detail text carries addresses and is excluded."""
    return (
        item.outcome,
        item.hresult,
        item.output_sha256,
        item.output_length,
        item.constants_sha256,
        item.constants_length,
        item.errors_sha256,
        item.errors_length,
        item.seh_trace,
    )


def run_all(
    probe: Path,
    xbe: Path,
    directory: Path,
    cases: Sequence[Case],
    *,
    flush: bool,
    timeout: float,
    jobs: int,
    outputs: bool = False,
    runner: Callable[..., ProbeRun] = native_probe.run_once,
) -> tuple[dict[str, Observation], dict[str, str]]:
    """Observations of the cases that could be classified, and the driver errors of the rest.

    A case with a driver error gets NO observation, so `verify compare` counts it as a
    mismatch when the original returned.
    """

    def one(case: Case) -> Observation | str:
        try:
            return run_case(
                probe,
                xbe,
                case,
                directory / f"{case.name}.bin",
                flush=flush,
                timeout=timeout,
                outputs=outputs,
                runner=runner,
            )
        except DriverError as error:
            return str(error)

    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(one, cases))
    observed = {
        c.name: r for c, r in zip(cases, results, strict=True) if isinstance(r, Observation)
    }
    problems = {c.name: r for c, r in zip(cases, results, strict=True) if isinstance(r, str)}
    return observed, problems


def _select(cases: list[Case], names: list[str] | None) -> list[Case]:
    if not names:
        return cases
    known = {case.name for case in cases}
    missing = [name for name in names if name not in known]
    if missing:
        raise SystemExit(f"unknown case {missing}")
    return [case for case in cases if case.name in set(names)]


def _run(args: argparse.Namespace) -> int:
    probe, xbe = native_probe.find_probe(args.probe), native_probe.find_xbe(args.xbe)
    if probe is None or xbe is None:
        print(f"missing {'probe' if probe is None else 'xbe'}; build tsfp_shader_probe first")
        return 2
    _check_dispatch(args.corpus, args.dispatch, args.reference)
    cases = _select(load_exported(args.corpus), args.case)
    observed, problems = run_all(
        probe,
        xbe,
        args.corpus,
        cases,
        flush=args.flush,
        timeout=args.timeout,
        jobs=args.jobs,
        outputs=bool(manifest_corpus(args.corpus).output_args),
    )
    write_native(args.out, observed, dispatch=args.dispatch)
    tally = Counter(item.outcome for item in observed.values())
    print("native outcomes:", json.dumps(dict(sorted(tally.items()))))
    for name, message in problems.items():
        print(f"DRIVER ERROR {name}: {message}")
    if args.reference is not None:
        reference = load_reference(args.reference)
        if args.table:
            print(render_cases(reference, observed))
        print(crosstab(reference, observed).render())
    return 1 if problems else 0


def variants(runs: Sequence[dict[str, Observation]]) -> dict[str, set[tuple[object, ...]]]:
    """Per case, the distinct stable keys seen over repeated runs (one key means repeatable)."""
    seen: dict[str, set[tuple[object, ...]]] = {}
    for observed in runs:
        for name, item in observed.items():
            seen.setdefault(name, set()).add(stable_key(item))
    return seen


def _repeat(args: argparse.Namespace) -> int:
    """Run the corpus several times and report which cases do not repeat.

    Exit 1 when a case the original returned stable for does not repeat, or differs from the
    original in any run. Cases with no stable original value may vary, and are listed.
    """
    probe, xbe = native_probe.find_probe(args.probe), native_probe.find_xbe(args.xbe)
    if probe is None or xbe is None:
        print(f"missing {'probe' if probe is None else 'xbe'}; build tsfp_shader_probe first")
        return 2
    _check_dispatch(args.corpus, args.dispatch, args.reference)
    cases = _select(load_exported(args.corpus), args.case)
    reference = load_reference(args.reference)
    runs = []
    problems: dict[str, str] = {}
    for _ in range(args.runs):
        observed, failed = run_all(
            probe,
            xbe,
            args.corpus,
            cases,
            flush=args.flush,
            timeout=args.timeout,
            jobs=args.jobs,
            outputs=bool(manifest_corpus(args.corpus).output_args),
        )
        runs.append(observed)
        problems.update(failed)
    seen = variants(runs)
    comparable = {
        name
        for name, measured in reference.items()
        if measured.padded.outcome == Outcome.RETURNED.value and measured.stable
    }
    broken = []
    for observed in runs:
        report = compare_all(reference, observed)
        broken.extend(n for n, v in report.verdicts.items() if v.status is Status.MISMATCH)
    for name in sorted(seen):
        keys = seen[name]
        flag = "stable" if name in comparable else "no stable original value"
        if len(keys) > 1 or args.verbose:
            print(f"{name:32} {len(keys)} distinct over {args.runs} runs  ({flag})")
        if name in comparable and len(keys) != 1:
            broken.append(name)
    broken = sorted(set(broken))
    for name, message in problems.items():
        print(f"DRIVER ERROR {name}: {message}")
        broken.append(name)
    total_variable = sum(1 for keys in seen.values() if len(keys) > 1)
    print(
        f"{len(seen)} cases over {args.runs} runs: {total_variable} vary, "
        f"{len(comparable)} comparable, {len(broken)} comparable cases varied or mismatched"
    )
    if args.out:
        write_native(args.out, runs[0], dispatch=args.dispatch)
    return 1 if broken or not comparable else 0


def _same(args: argparse.Namespace) -> int:
    """Two native observation files must agree case for case (determinism)."""
    first, second = (json.loads(path.read_text())["cases"] for path in (args.first, args.second))
    if not first or set(first) != set(second):
        print("the two runs do not cover the same non-empty set of cases")
        return 1
    differing = []
    for name in sorted(first):
        keys = [
            stable_key(Observation(**{k: v for k, v in entry[name].items() if k != "detail"}))
            for entry in (first, second)
        ]
        if keys[0] != keys[1]:
            differing.append(name)
    print(f"{len(first)} cases, {len(differing)} differ between the runs: {differing}")
    return 1 if differing else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="one boot per case, write schema 1 observations")
    run.add_argument("--corpus", type=Path, required=True, help="`verify export` directory")
    run.add_argument("--out", type=Path, required=True)
    run.add_argument("--probe", type=Path)
    run.add_argument("--xbe", type=Path)
    run.add_argument("--case", action="append", help="only this case, repeatable")
    run.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    run.add_argument("--jobs", type=int, default=4)
    run.add_argument("--flush", action="store_true", help="source flush against an unmapped page")
    run.add_argument(
        "--dispatch", action="store_true", help="match a dispatch-enabled reference/export"
    )
    run.add_argument("--reference", type=Path, help="print the outcome crosstab against this")
    run.add_argument("--table", action="store_true", help="with --reference, one row per case")
    run.set_defaults(handler=_run)
    repeat = sub.add_parser("repeat", help="run the corpus N times, report cases that vary")
    for option, kind, default in (("--runs", int, 5), ("--timeout", float, DEFAULT_TIMEOUT)):
        repeat.add_argument(option, type=kind, default=default)
    repeat.add_argument("--corpus", type=Path, required=True)
    repeat.add_argument("--reference", type=Path, required=True)
    repeat.add_argument("--out", type=Path, help="write the first run's observations here")
    repeat.add_argument("--probe", type=Path)
    repeat.add_argument("--xbe", type=Path)
    repeat.add_argument("--case", action="append")
    repeat.add_argument("--jobs", type=int, default=4)
    repeat.add_argument("--flush", action="store_true")
    repeat.add_argument(
        "--dispatch", action="store_true", help="match a dispatch-enabled reference/export"
    )
    repeat.add_argument("--verbose", action="store_true", help="also list repeatable cases")
    repeat.set_defaults(handler=_repeat)
    same = sub.add_parser("same", help="check two native runs agree case for case")
    same.add_argument("first", type=Path)
    same.add_argument("second", type=Path)
    same.set_defaults(handler=_same)
    args = parser.parse_args(argv)
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())
