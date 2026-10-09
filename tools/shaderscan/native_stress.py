#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Repeat and concurrent native calls on the text corpus sources (T476).

`native_corpus` boots once per case, so it never sees a second call in the same process. The
retained compiler keeps state in static data (`0x405B00..0x405C1C`, see
docs/shader-original-routes.md) and a failing source may leave state a later call sees. This
driver runs the comparable corpus cases (the original returned a stable value) through
`tsfp_shader_probe --probe-corpus` in four ways and requires every call to keep its HRESULT,
output, constants and errors, the same comparison `verify compare` makes:

  repeat      each case N more times in a row in one process
  ordering    one process per ordering that puts failing sources next to successes (a failure
              between two successes, all failures first, all successes first, reversed)
  concurrent  two guest threads, each round on two different cases, through the route lock

No expected value is stored: the reference is a fresh measurement of the original. Heap
dependent cases and the throwing and runaway ones never enter, they have no stable original
value. The driver prints counts, names and classes, never shader bytes.

    python -m tools.shaderscan.verify export --corpus text --out tmp/text
    python -m tools.shaderscan.native_stress run --corpus tmp/text --reference tmp/text_ref.json
"""

from __future__ import annotations

import argparse
import tempfile
from collections import Counter
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

from tools.shaderscan import native_corpus as nc
from tools.shaderscan import native_probe, verify
from tools.shaderscan.assemble import Outcome
from tools.shaderscan.malformed import Case
from tools.shaderscan.native_probe import ProbeRun
from tools.shaderscan.verify import Measurement, Observation, Status, load_reference

#: Wall-clock watchdog for one phase process (the ordering phase makes about 6000 calls).
DEFAULT_TIMEOUT = 300.0
DEFAULT_REPEATS = 5
DEFAULT_RUNS = 5
#: The probe's cap on `--probe-corpus-repeat`.
MAX_PROBE_REPEATS = 64
#: Thread 1 of a concurrent round takes the case `count // 2` places after thread 0.
THREADS = (0, 1)

#: Cases whose native compile is measured to FAULT, and the call (0 is the first) that does.
#: Both are pixel sources of 16 or more instructions, where the original stores a combiner
#: word past the end of its 240 byte output block (T485). The title's heap has a block header
#: there, the free of the neighbouring block then dereferences null in the lifted heap
#: manager, as it would in the original. Whether the first call dies or the next one depends
#: on what the overrun overwrote, so each is pinned by name: the calls before the fault must
#: still match the original and a change in when it faults is reported. The reference
#: measures the overrun (`Measurement.heap_overrun`), so these are never comparable cases and
#: sit out of the repeat, ordering and concurrent phases.
KNOWN_FAULTS = {
    "overrun_ps_sixteen_instructions": 0,
    "overrun_ps_twenty_instructions": 1,
}
SINGLE_CALL_ONLY = frozenset(KNOWN_FAULTS)

PHASES = ("known_faults", "repeat", "ordering", "concurrent")

Footprint = tuple[int, int, int]


@dataclass(frozen=True)
class Call:
    """One `corpus_call` record: where it ran, what it returned, what state it saw."""

    index: int
    round: int
    thread: int
    phase: str
    name: str
    observation: Observation
    #: Title heap allocations, frees and blocks still allocated after the call.
    footprint: Footprint
    static_before: str
    static_after: str
    ask_ns: int
    enter_ns: int
    exit_ns: int
    done_ns: int

    @property
    def label(self) -> str:
        return f"{self.name} (line {self.index} round {self.round} thread {self.thread})"

    @property
    def returned_error(self) -> bool:
        return self.observation.hresult not in (None, 0)

    @property
    def state_moved(self) -> bool:
        return self.static_before != self.static_after


@dataclass(frozen=True)
class PhaseRun:
    """What one probe process did. `done` is the probe's own end marker, never inferred."""

    calls: list[Call]
    done: bool
    done_count: int | None
    summary: dict | None
    #: A `corpus_begin` that never got its `corpus_call`: where the process stopped.
    in_flight: str | None
    #: How a process that did not reach `corpus_done` ended (`fault`, `kernel_call`, ...).
    stop: str | None = None


def parse_call(record: dict) -> Call:
    return Call(
        record["index"],
        record["round"],
        record["thread"],
        record["phase"],
        record["name"],
        nc.observe_call(record),
        (record["allocs"], record["frees"], record["outstanding"]),
        record["static_before"],
        record["static_after"],
        record["ask_ns"],
        record["enter_ns"],
        record["exit_ns"],
        record["done_ns"],
    )


def parse_run(run: ProbeRun) -> PhaseRun:
    """The calls of a probe process, in record order, with how it ended."""
    calls = [parse_call(row) for row in nc.corpus_records(run, "corpus_call")]
    ended = {(call.index, call.round, call.thread, call.phase) for call in calls}
    open_calls = [
        row
        for row in nc.corpus_records(run, "corpus_begin")
        if (row["index"], row["round"], row["thread"], row["phase"]) not in ended
    ]
    errors = nc.corpus_records(run, "corpus_error")
    if errors:
        raise nc.DriverError(f"{errors[0]['name']}: {errors[0]['error']}")
    done = nc.corpus_records(run, "corpus_done")
    summary = nc.corpus_records(run, "corpus_concurrent")
    stop = None
    if not done:
        try:
            stop = nc.observe_stop(run).outcome
        except nc.DriverError as error:
            stop = f"unclassified: {error}"
    return PhaseRun(
        calls,
        bool(done),
        done[0]["count"] if done else None,
        summary[0] if summary else None,
        open_calls[0]["name"] if open_calls else None,
        stop,
    )


def run_phase(
    probe: Path,
    xbe: Path,
    entries: Sequence[tuple[Case, Path]],
    flags: Sequence[str],
    *,
    timeout: float,
    attempts: int = nc.ATTEMPTS,
    runner: Callable[..., ProbeRun] = native_probe.run_once,
) -> PhaseRun:
    """One boot over a spec of `(case, source file)` lines, retrying only boots that died
    before the corpus phase."""
    with tempfile.TemporaryDirectory(prefix="native_stress_") as raw:
        spec = Path(raw) / "spec.txt"
        spec.write_text("".join(nc.spec_line(case, path) for case, path in entries))
        command = [
            "--probe-corpus",
            str(spec),
            "--probe-corpus-flush",
            "0",
            "--probe-corpus-outputs",
            "1",
            # The host's own 10 s thread watchdog would end a long ordering mid-phase.
            "--thread-timeout",
            str(int(timeout * 1000)),
            *flags,
        ]
        for _ in range(attempts):
            run = runner(probe, xbe, command, timeout)
            if nc.corpus_records(run, "corpus_begin"):
                return parse_run(run)
    raise nc.DriverError(f"{attempts} boots never reached the corpus phase")


def comparable_names(reference: dict[str, Measurement]) -> set[str]:
    """Cases the original returned a stable value for: not heap dependent, not a throw or a
    runaway, and not a result that depends on memory past the source."""
    return {
        name
        for name, measured in reference.items()
        if measured.padded.outcome == Outcome.RETURNED.value and measured.stable
    }


def select(cases: Sequence[Case], reference: dict[str, Measurement]) -> list[Case]:
    names = comparable_names(reference) - SINGLE_CALL_ONLY
    return [case for case in cases if case.name in names]


def orderings(
    cases: Sequence[Case],
    reference: dict[str, Measurement],
    pair_failures: int | None = None,
) -> dict[str, list[Case]]:
    """Case orders that put failing sources (the original returned an error) next to
    successes, each run in its own process so the state starts clean. `pairs` puts every
    failing source (the first `pair_failures` of them) between two calls of every success."""
    failing = [c for c in cases if reference[c.name].padded.hresult not in (None, 0)]
    passing = [c for c in cases if reference[c.name].padded.hresult in (None, 0)]
    pairs: list[Case] = []
    for bad in failing[:pair_failures]:
        for good in passing:
            pairs += [bad, good, bad]
    return {
        "pairs": pairs,
        "failures_first": [*failing, *passing],
        "successes_first": [*passing, *failing],
        "reversed": list(reversed(cases)),
    }


def expected_sequential(cases: Sequence[Case], repeats: int) -> list[tuple[int, int, int, str]]:
    """(line, round, thread, name) of every call the sequential phase must make."""
    return [
        (index, round_, 0, case.name)
        for index, case in enumerate(cases)
        for round_ in range(repeats + 1)
    ]


def expected_concurrent(cases: Sequence[Case], rounds: int) -> list[tuple[int, int, int, str]]:
    """Round r: thread t takes line (r + t * (count // 2)) mod count."""
    count = len(cases)
    return [
        (index, round_, thread, cases[index].name)
        for round_ in range(rounds)
        for thread in THREADS
        for index in [(round_ + thread * (count // 2)) % count]
    ]


def coverage(
    calls: Sequence[Call], expected: Sequence[tuple[int, int, int, str]], label: str
) -> list[str]:
    """The calls made must be exactly the calls expected, none missing, none extra."""
    if not expected:
        return [f"{label}: nothing was expected, the phase proves nothing"]
    seen = Counter((c.index, c.round, c.thread, c.name) for c in calls)
    want = Counter(expected)
    problems = [f"{label}: missing call {key}" for key in sorted((want - seen).elements())]
    problems += [f"{label}: unexpected call {key}" for key in sorted((seen - want).elements())]
    return problems


def mismatches(
    calls: Sequence[Call],
    reference: dict[str, Measurement],
    label: str,
    *,
    trust_unstable: bool = False,
) -> list[str]:
    """Calls whose HRESULT, output, constants or errors differ from the original's.

    `trust_unstable` compares against a reference the corpus does not compare, for the calls
    of a known fault that finished before it (the overrun comes after the output is built)."""
    problems: list[str] = []
    for call in calls:
        measured = reference.get(call.name)
        if measured is None or measured.padded.outcome != Outcome.RETURNED.value:
            problems.append(f"{label}: {call.label} has no returned original to compare")
            continue
        verdict = verify.compare(
            measured.padded, call.observation, stable=trust_unstable or measured.stable
        )
        if verdict.status is not Status.MATCH:
            problems.append(f"{label}: {call.label} {verdict.status.value}: {verdict.reason}")
    return problems


def footprint_varies(calls: Sequence[Call]) -> list[str]:
    """Cases whose title heap allocations, frees or retained blocks differ between calls."""
    seen: dict[str, set[Footprint]] = {}
    for call in calls:
        seen.setdefault(call.name, set()).add(call.footprint)
    return sorted(name for name, prints in seen.items() if len(prints) > 1)


@dataclass(frozen=True)
class StateStats:
    """What the compiler's static data did across a sequence of calls (measured, not judged)."""

    calls: int
    moved_success: int
    successes: int
    moved_failure: int
    failures: int
    #: Calls whose state on entry differs from the previous call's state on exit.
    discontinuities: int
    #: Distinct states seen on entry to a call.
    distinct_entry_states: int


def state_stats(calls: Sequence[Call]) -> StateStats:
    successes = [c for c in calls if not c.returned_error]
    failures = [c for c in calls if c.returned_error]
    breaks = sum(
        1 for a, b in zip(calls, calls[1:], strict=False) if a.static_after != b.static_before
    )
    return StateStats(
        len(calls),
        sum(1 for c in successes if c.state_moved),
        len(successes),
        sum(1 for c in failures if c.state_moved),
        len(failures),
        breaks,
        len({c.static_before for c in calls}),
    )


@dataclass(frozen=True)
class Overlap:
    """How the two guest threads met in the concurrent rounds."""

    rounds: int
    #: Rounds in which both threads were inside `xdk_original_dispatch` at the same time
    #: (one held the route lock, the other waited for it).
    contended: int
    #: Rounds in which the two compiler bodies ran at the same time. The lock forbids it.
    body_overlaps: int
    #: Rounds in which the two threads ran different cases.
    different_cases: int
    #: Rounds in which one thread's call failed and the other's succeeded (state hand over).
    mixed_outcome: int


def overlap(calls: Sequence[Call]) -> Overlap:
    by_round: dict[int, dict[int, Call]] = {}
    for call in calls:
        by_round.setdefault(call.round, {})[call.thread] = call
    contended = bodies = different = mixed = rounds = 0
    for pair in by_round.values():
        if set(pair) != set(THREADS):
            continue
        a, b = pair[0], pair[1]
        rounds += 1
        contended += a.ask_ns < b.done_ns and b.ask_ns < a.done_ns
        bodies += a.enter_ns < b.exit_ns and b.enter_ns < a.exit_ns
        different += a.name != b.name
        mixed += a.returned_error != b.returned_error
    return Overlap(rounds, contended, bodies, different, mixed)


def concurrent_problems(run: PhaseRun, rounds: int, stats: Overlap) -> list[str]:
    """What the concurrent phase must show beyond matching outputs."""
    problems: list[str] = []
    summary = run.summary
    if summary is None:
        return ["concurrent: no corpus_concurrent summary record"]
    if not summary["built"] or summary["peer_stopped"] or summary["aborted"]:
        problems.append(f"concurrent: phase did not finish cleanly {summary}")
    if summary["completed_rounds"] != [rounds, rounds]:
        problems.append(f"concurrent: completed rounds {summary['completed_rounds']}")
    if summary["max_inside"] != 1:
        problems.append(f"concurrent: {summary['max_inside']} threads inside the compiler at once")
    if stats.rounds != rounds:
        problems.append(f"concurrent: {stats.rounds} paired rounds of {rounds}")
    if stats.body_overlaps:
        problems.append(f"concurrent: {stats.body_overlaps} rounds overlapped inside the compiler")
    if stats.different_cases != stats.rounds:
        problems.append("concurrent: a round gave both threads the same case")
    if stats.contended * 2 < stats.rounds:
        problems.append(
            f"concurrent: only {stats.contended} of {stats.rounds} rounds had both threads at "
            "the lock, the rendezvous did not release them together"
        )
    return problems


def phase_ended(run: PhaseRun, lines: int, label: str) -> list[str]:
    """A phase that did not reach its own end marker says where it stopped."""
    problems: list[str] = []
    if not run.done:
        where = f", in flight: {run.in_flight}" if run.in_flight else ""
        problems.append(f"{label}: the process ended ({run.stop}) before corpus_done{where}")
    elif run.done_count != lines:
        problems.append(f"{label}: corpus_done counted {run.done_count} lines of {lines}")
    return problems


@dataclass
class Result:
    problems: list[str] = field(default_factory=list)
    lines: list[str] = field(default_factory=list)


def stress_repeat(
    probe: Path,
    xbe: Path,
    directory: Path,
    cases: Sequence[Case],
    reference: dict[str, Measurement],
    *,
    repeats: int,
    timeout: float,
    runner: Callable[..., ProbeRun] = native_probe.run_once,
) -> Result:
    result = Result()
    if not 1 <= repeats <= MAX_PROBE_REPEATS:
        result.problems.append(f"repeat: {repeats} repeats, the probe caps at {MAX_PROBE_REPEATS}")
        return result
    entries = [(case, directory / f"{case.name}.bin") for case in cases]
    run = run_phase(
        probe, xbe, entries, ["--probe-corpus-repeat", str(repeats)], timeout=timeout, runner=runner
    )
    result.problems += phase_ended(run, len(entries), "repeat")
    result.problems += coverage(run.calls, expected_sequential(cases, repeats), "repeat")
    result.problems += mismatches(run.calls, reference, "repeat")
    varied = footprint_varies(run.calls)
    stats = state_stats(run.calls)
    result.lines.append(
        f"repeat: {len(cases)} cases x {repeats + 1} calls = {len(run.calls)} calls, "
        f"{len(varied)} cases with a varying heap footprint {varied}, "
        f"static data moved on {stats.moved_success} of {stats.successes} successes and "
        f"{stats.moved_failure} of {stats.failures} errors"
    )
    return result


def stress_known_faults(
    probe: Path,
    xbe: Path,
    directory: Path,
    cases: Sequence[Case],
    reference: dict[str, Measurement],
    *,
    timeout: float,
    runner: Callable[..., ProbeRun] = native_probe.run_once,
) -> Result:
    """Each `KNOWN_FAULTS` case alone, called twice: the calls before the known one must match
    the original and the process must then fault on it, so a fixed compiler is noticed."""
    result = Result()
    held = [case for case in cases if case.name in KNOWN_FAULTS]
    for missing in sorted(set(KNOWN_FAULTS) - {case.name for case in held}):
        result.problems.append(f"known fault: {missing} is not in the corpus")
    for case in held:
        run = run_phase(
            probe,
            xbe,
            [(case, directory / f"{case.name}.bin")],
            ["--probe-corpus-repeat", "1"],
            timeout=timeout,
            runner=runner,
        )
        label = f"known fault {case.name}"
        want = KNOWN_FAULTS[case.name]
        result.problems += mismatches(run.calls, reference, label, trust_unstable=True)
        finished = sorted(c.round for c in run.calls)
        if finished != list(range(want)):
            result.problems.append(
                f"{label}: rounds {finished} finished, want exactly rounds {list(range(want))}"
            )
        if run.done or run.stop != Outcome.FAULT.value or run.in_flight != case.name:
            result.problems.append(
                f"{label}: call {want} did not fault (done {run.done}, stop {run.stop}), "
                f"if it is fixed remove it from KNOWN_FAULTS"
            )
        result.lines.append(
            f"known fault {case.name}: {want} call(s) match the original, call {want} "
            f"{run.stop if not run.done else 'RETURNED'}"
        )
    return result


def stress_ordering(
    probe: Path,
    xbe: Path,
    directory: Path,
    cases: Sequence[Case],
    reference: dict[str, Measurement],
    *,
    timeout: float,
    pair_failures: int | None = None,
    runner: Callable[..., ProbeRun] = native_probe.run_once,
) -> Result:
    result = Result()
    for label, order in orderings(cases, reference, pair_failures).items():
        entries = [(case, directory / f"{case.name}.bin") for case in order]
        run = run_phase(probe, xbe, entries, [], timeout=timeout, runner=runner)
        name = f"ordering {label}"
        result.problems += phase_ended(run, len(entries), name)
        result.problems += coverage(
            run.calls, [(i, 0, 0, case.name) for i, case in enumerate(order)], name
        )
        result.problems += mismatches(run.calls, reference, name)
        stats = state_stats(run.calls)
        result.lines.append(
            f"{name}: {len(run.calls)} calls, {stats.failures} errors and {stats.successes} "
            f"successes, static data moved on {stats.moved_success} successes and "
            f"{stats.moved_failure} errors, {stats.distinct_entry_states} distinct entry "
            f"states, {stats.discontinuities} calls entered with state the previous call "
            f"did not leave"
        )
    return result


def stress_concurrent(
    probe: Path,
    xbe: Path,
    directory: Path,
    cases: Sequence[Case],
    reference: dict[str, Measurement],
    *,
    rounds: int,
    runs: int,
    timeout: float,
    runner: Callable[..., ProbeRun] = native_probe.run_once,
) -> Result:
    result = Result()
    entries = [(case, directory / f"{case.name}.bin") for case in cases]
    totals = Counter()
    for number in range(runs):
        run = run_phase(
            probe,
            xbe,
            entries,
            ["--probe-corpus-concurrent", str(rounds)],
            timeout=timeout,
            runner=runner,
        )
        label = f"concurrent run {number + 1}"
        concurrent = [c for c in run.calls if c.phase == "concurrent"]
        sequential = [c for c in run.calls if c.phase == "sequential"]
        result.problems += phase_ended(run, len(entries), label)
        result.problems += coverage(sequential, expected_sequential(cases, 0), label)
        result.problems += coverage(concurrent, expected_concurrent(cases, rounds), label)
        result.problems += mismatches(run.calls, reference, label)
        stats = overlap(concurrent)
        result.problems += [f"{label}: {p}" for p in concurrent_problems(run, rounds, stats)]
        totals.update(
            rounds=stats.rounds,
            contended=stats.contended,
            mixed=stats.mixed_outcome,
            calls=len(concurrent),
        )
    result.lines.append(
        f"concurrent: {runs} runs x {rounds} rounds x 2 threads = {totals['calls']} calls, "
        f"{totals['contended']} of {totals['rounds']} rounds had both threads at the route "
        f"lock, {totals['mixed']} rounds paired an error with a success"
    )
    return result


def _run(args: argparse.Namespace) -> int:
    probe, xbe = native_probe.find_probe(args.probe), native_probe.find_xbe(args.xbe)
    if probe is None or xbe is None:
        print(f"missing {'probe' if probe is None else 'xbe'}; build tsfp_shader_probe first")
        return 2
    reference = load_reference(args.reference)
    all_cases = nc.load_exported(args.corpus)
    cases = select(all_cases, reference)
    rounds = args.rounds or 2 * len(cases)
    results = {
        "known_faults": lambda: stress_known_faults(
            probe, xbe, args.corpus, all_cases, reference, timeout=args.timeout
        ),
        "repeat": lambda: stress_repeat(
            probe, xbe, args.corpus, cases, reference, repeats=args.repeats, timeout=args.timeout
        ),
        "ordering": lambda: stress_ordering(
            probe,
            xbe,
            args.corpus,
            cases,
            reference,
            timeout=args.timeout,
            pair_failures=args.pair_failures,
        ),
        "concurrent": lambda: stress_concurrent(
            probe,
            xbe,
            args.corpus,
            cases,
            reference,
            rounds=rounds,
            runs=args.runs,
            timeout=args.timeout,
        ),
    }
    print(f"{len(cases)} comparable cases of {len(reference)}")
    problems: list[str] = []
    for phase in args.phase:
        try:
            outcome = results[phase]()
        except nc.DriverError as error:
            problems.append(f"{phase}: {error}")
            continue
        print("\n".join(outcome.lines))
        problems += outcome.problems
    for problem in problems:
        print(f"PROBLEM {problem}")
    print(f"{len(problems)} problems")
    return 1 if problems else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run", help="repeat, ordering and concurrent phases against the original")
    run.add_argument("--corpus", type=Path, required=True, help="`verify export` directory")
    run.add_argument("--reference", type=Path, required=True)
    run.add_argument("--probe", type=Path)
    run.add_argument("--xbe", type=Path)
    run.add_argument("--repeats", type=int, default=DEFAULT_REPEATS)
    run.add_argument("--rounds", type=int, default=0, help="concurrent rounds, default 2 x cases")
    run.add_argument("--runs", type=int, default=DEFAULT_RUNS, help="concurrent boots")
    run.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    run.add_argument(
        "--pair-failures",
        type=int,
        help="ordering `pairs` over only the first N failing sources (default all, 5760 calls)",
    )
    run.add_argument(
        "--phase",
        action="append",
        choices=PHASES,
        help="only this phase, repeatable (default all)",
    )
    run.set_defaults(handler=_run)
    args = parser.parse_args(argv)
    args.phase = args.phase or list(PHASES)
    return args.handler(args)


if __name__ == "__main__":
    raise SystemExit(main())
