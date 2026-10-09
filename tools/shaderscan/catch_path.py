# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe what the original compiler returns when its C++ throw is caught (T446).

`XGAssembleShader` rejects some sources by throwing (`RtlRaiseException`, code `0xE06D7363`).
The harness used to stop at that call, so nothing was known about what the original then
returns. With `AssemblerEmulator.run(dispatch_exceptions=True)` (`seh.Dispatcher`) the guest's
own `__CxxFrameHandler`, catch block and unwinder run, and the call returns or ends.

Two populations, both measured on the user's own executable and never committed:

  * `corpus`: the 62 synthetic cases of `malformed.py`.
  * `keyproduct`: every vertex source the title's builder can form (21,888), of which 8,448
    stop at the throw without dispatch.

Every case runs in a zero-padded and a flush-to-guard source and on three dirty-heap
emulators, so a result that depends on memory the caller did not supply is flagged and not
trusted (`unstable`), the same rule as `verify.py`. Only returned HRESULTs are reported as
returns. Nothing is printed per shader, only counts and distinct results.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import multiprocessing
import sys
from collections import Counter
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path

from tools.shaderscan.assemble import (
    ORDINAL_RAISE_EXCEPTION,
    AssemblerEmulator,
    AssemblerSpec,
    Outcome,
    Run,
)
from tools.shaderscan.image import Image
from tools.shaderscan.verify import CORPORA, DIRTY_HEAP_FILLS


@dataclass(frozen=True)
class Observed:
    """One dispatching run, reduced to what is compared across placements and heaps."""

    outcome: str
    hresult: int | None
    output_length: int | None
    output_sha256: str | None
    #: Optional output buffers by argument index, as a digest (None for a NULL output).
    extras: tuple[tuple[int, str | None], ...]
    #: Guest handlers called, in order, as `handler:disposition` and `unwind:` markers.
    trace: tuple[str, ...]
    stop: str | None

    @property
    def key(self) -> tuple[object, ...]:
        return (
            self.outcome,
            self.hresult,
            self.output_sha256,
            self.extras,
            self.trace,
            self.stop,
        )


def trace_of(run: Run) -> tuple[str, ...]:
    """Dispatch steps as compact strings, handler addresses in hex."""
    steps: list[str] = []
    for event in run.seh:
        if event.kind in ("raise", "unwind"):
            steps.append(event.kind)
        elif event.kind == "disposition":
            steps.append(f"{event.handler:#x}={event.disposition}")
    return tuple(steps)


def observed(run: Run) -> Observed:
    result = run.result
    if run.outcome is Outcome.RETURNED and result is not None:
        digest = hashlib.sha256(result.data).hexdigest() if result.data else None
        extras = tuple(
            (argument, None if data is None else hashlib.sha256(data).hexdigest())
            for argument, data in result.extras
        )
        return Observed(
            run.outcome.value, result.status, len(result.data), digest, extras, trace_of(run), None
        )
    return Observed(run.outcome.value, None, None, None, (), trace_of(run), run.seh_stop)


def measure(
    emulator: AssemblerEmulator,
    dirty: Sequence[AssemblerEmulator],
    source: bytes,
    flags: int,
    *,
    budget: int | None = None,
    output_args: Sequence[int] = (),
) -> tuple[Observed, bool]:
    """The dispatching result and whether it changes with source placement or heap contents."""
    options = {"dispatch_exceptions": True, "budget": budget, "output_args": output_args}
    base = observed(emulator.run(source, flags, **options))
    others = [observed(emulator.run(source, flags, flush_to_guard=True, **options))]
    others += [observed(machine.run(source, flags, **options)) for machine in dirty]
    return base, any(other.key != base.key for other in others)


def _emulators(
    image: Image, spec: AssemblerSpec
) -> tuple[AssemblerEmulator, list[AssemblerEmulator]]:
    return AssemblerEmulator(image, spec), [
        AssemblerEmulator(image, spec, heap_fill=fill) for fill in DIRTY_HEAP_FILLS
    ]


def _tally(items: Iterable[tuple[str, Observed, bool]]) -> dict[str, object]:
    rows = list(items)
    classes: Counter[str] = Counter()
    results: Counter[str] = Counter()
    for _, seen, unstable in rows:
        label = seen.outcome if seen.stop is None else f"{seen.outcome}: {seen.stop}"
        classes[label + (" (unstable)" if unstable else "")] += 1
        if seen.outcome == Outcome.RETURNED.value and not unstable:
            hresult = f"{seen.hresult:#010x}"
            results[f"{hresult} len={seen.output_length} trace={'>'.join(seen.trace)}"] += 1
    return {"cases": len(rows), "outcomes": dict(classes), "returned_results": dict(results)}


def _spec(args: argparse.Namespace) -> AssemblerSpec:
    return AssemblerSpec(
        args.assembler, args.heap_alloc, args.heap_free, static_init=args.float_init or None
    )


def _corpus(args: argparse.Namespace) -> int:
    image = Image.load(args.xbe)
    emulator, dirty = _emulators(image, _spec(args))
    chosen = CORPORA[args.corpus]
    rows: list[tuple[str, Observed, bool]] = []
    changed: list[str] = []
    for case in chosen.cases():
        before = emulator.run(
            case.source, case.flags, budget=case.budget, output_args=chosen.output_args
        )
        seen, unstable = measure(
            emulator,
            dirty,
            case.source,
            case.flags,
            budget=case.budget,
            output_args=chosen.output_args,
        )
        rows.append((case.name, seen, unstable))
        if before.outcome is Outcome.KERNEL_CALL:
            changed.append(f"{case.name}: {before.outcome.value} -> {seen.outcome} {seen.hresult}")
    report = _tally(rows)
    report["stopped_at_throw_without_dispatch"] = changed
    print(json.dumps(report, indent=1))
    return 0


#: Worker state, set before the fork so children inherit the image without pickling it.
_WORK: dict[str, object] = {}
#: Per process emulators, built on first use.
_LOCAL: dict[str, object] = {}


def _key_chunk(keys: list[int]) -> dict[str, tuple[bytes, int]]:
    """The distinct sources the vertex builder emits for `keys`."""
    from tools.shaderscan import builders

    if "builder" not in _LOCAL:
        _LOCAL["builder"] = builders.BuilderEmulator(
            _WORK["image"],
            _WORK["builder_spec"],  # type: ignore[arg-type]
        )
    found = builders.enumerate_keys(_LOCAL["builder"], keys, keep_sources=True)  # type: ignore[arg-type]
    return found.sources


def _measure_chunk(
    items: list[tuple[str, bytes, int]],
) -> list[tuple[str, Observed, bool, bool]]:
    if "machines" not in _LOCAL:
        _LOCAL["machines"] = _emulators(_WORK["image"], _WORK["spec"])  # type: ignore[arg-type]
    emulator, dirty = _LOCAL["machines"]  # type: ignore[misc]
    rows = []
    for digest, source, flags in items:
        plain = emulator.run(source, flags)
        threw = plain.result is not None and plain.result.kernel_call == ORDINAL_RAISE_EXCEPTION
        if _WORK["throws_only"] and not threw:
            rows.append((digest[:12], observed(plain), False, False))
            continue
        seen, unstable = measure(emulator, dirty, source, flags)
        rows.append((digest[:12], seen, unstable, threw))
    return rows


def _chunks(items: Sequence[object], count: int) -> list[list[object]]:
    edges = [len(items) * index // count for index in range(count + 1)]
    return [list(items[start:stop]) for start, stop in zip(edges, edges[1:], strict=False)]


def _keyproduct(args: argparse.Namespace) -> int:
    # Imported here: the builder walk pulls the whole analysis stack, `corpus` does not need it.
    from tools.nv2a import corpus as nv2a
    from tools.shaderscan import builders
    from tools.xdk_abi import SectionMap, executable_sections, walk_function

    image = Image.load(args.xbe)
    name, entry, register, mask = nv2a.VERTEX_BUILDER
    walk = walk_function(SectionMap(executable_sections(args.xbe)), entry)
    if not walk.clean:
        raise SystemExit("vertex builder control-flow walk is not clean, refusing")
    effective = builders.tested_bits([(i.mnemonic, i.op_str) for i in walk.insns], register) & mask
    _WORK.update(
        image=image,
        builder_spec=builders.BuilderSpec(name, entry, register, nv2a.ASSEMBLER),
        spec=_spec(args),
        throws_only=args.throws_only,
    )
    context = multiprocessing.get_context("fork")
    keys = list(builders.subsets(effective))
    with context.Pool(args.jobs) as pool:
        parts = pool.map(_key_chunk, _chunks(keys, args.jobs * 8), chunksize=1)
    sources: dict[str, tuple[bytes, int]] = {}
    for part in parts:
        for digest, value in part.items():
            sources.setdefault(digest, value)
    print(f"  {len(keys)} keys gave {len(sources)} distinct sources", file=sys.stderr)
    items = [(digest, source, flags) for digest, (source, flags) in sources.items()]
    with context.Pool(args.jobs) as pool:
        measured = pool.map(_measure_chunk, _chunks(items, args.jobs * 8), chunksize=1)
    rows = [row for part in measured for row in part]
    throwing = [row for row in rows if row[3]]
    chosen = throwing if args.throws_only else rows
    report = _tally((digest, seen, unstable) for digest, seen, unstable, _ in chosen)
    report["sources"] = len(sources)
    report["stopped_at_throw_without_dispatch"] = len(throwing)
    print(json.dumps(report, indent=1))
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--xbe", type=Path, default=Path("tmp/oxm-extract/retail/default.xbe"))
    parser.add_argument("--assembler", type=lambda text: int(text, 0), default=0x3EE2B3)
    parser.add_argument("--heap-alloc", type=lambda text: int(text, 0), default=0x383678)
    parser.add_argument("--heap-free", type=lambda text: int(text, 0), default=0x383DF3)
    parser.add_argument(
        "--float-init",
        type=lambda text: int(text, 0),
        default=0x3C843D,
        help="the title's float table initialiser, 0 for none (as verify.py)",
    )
    commands = parser.add_subparsers(dest="command", required=True)
    corpus_command = commands.add_parser("corpus", help="a synthetic corpus of verify.CORPORA")
    corpus_command.add_argument("--corpus", choices=sorted(CORPORA), default="binary")
    corpus_command.set_defaults(run=_corpus)
    key = commands.add_parser("keyproduct", help="every vertex source the builder can form")
    key.add_argument(
        "--throws-only",
        action="store_true",
        help="measure only the sources that stop at the throw without dispatch",
    )
    key.add_argument("--jobs", type=int, default=16, help="worker processes")
    key.set_defaults(run=_keyproduct)
    args = parser.parse_args(argv)
    return int(args.run(args))


if __name__ == "__main__":
    sys.exit(main())
