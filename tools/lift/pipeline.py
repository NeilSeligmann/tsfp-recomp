# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the vendored lifter end to end: XBE in, C out.

Five stages, each a subprocess of the vendored tree, each handing the next its
results as JSON on disk:

===============  ==============================================================
``xbe_parser``   header, sections, kernel imports
``disasm``       linear sweep plus 11-pass function detection (~30 s)
``func_id``      CRT / library / game classification
``abi_analysis`` calling convention, parameter count, frame shape
``recomp``       the lift itself (~15 min, ~2.6 M lines)
===============  ==============================================================

TWO THINGS ARE LOAD-BEARING HERE.

**Subprocess, not import.** Upstream's modules say ``from tools.disasm import
...`` and so do ours; both packages are called ``tools``. Importing theirs into
our interpreter would shadow ours (or be shadowed by it) depending on
``sys.path`` order, and the failure would be a confusing ``ImportError`` deep in
a stage. Running each stage with ``cwd`` at the vendored root and a ``PYTHONPATH``
of exactly that root makes the resolution unambiguous.

**Every output path is passed explicitly.** Each upstream stage defaults to
writing its results back into its own source tree (``tools/disasm/output/`` and
friends, ~160 MB on this binary). A vendored tree is source, not scratch, so
every stage is told where to write and nothing lands under ``third_party/``.

**Do not pass our own ``functions.csv`` via ``--functions``.** Measured in
``docs/lifter-evaluation.md`` §6.1: supplying our bounds *loses* 3,891 entry
points to gain 23, because upstream's detector is a near-superset of ours on this
binary. The option exists; using it is a regression.

**``manual_functions`` is how a host-side boundary becomes reachable.** An address
in that file gets no generated body, and every DIRECT call to it is re-emitted as
``RECOMP_ICALL_SAFE`` and every tail jump as ``RECOMP_ITAIL`` -- both of which
consult ``recomp_lookup_manual``. Without it a direct call is ``(fn)()`` with the
guest address discarded, so no address-keyed dispatcher can ever be reached. See
``docs/xdk-dispatch.md`` §2. The suppressed symbols must still be DEFINED by hand
or the link fails, because suppressing a body does not remove the address from the
generated dispatch table.
"""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import time
from dataclasses import dataclass, field, replace
from pathlib import Path

from tools.lift.callsites import emit_c, measure, thunk_slot_ordinals

#: Functions per generated chunk. 250 is what the evaluation measured: it gives
#: 67 chunks of a size GCC compiles without pathological memory use. One file of
#: 2.56 M lines does not build.
DEFAULT_CHUNK_SIZE = 250

#: Sections marked non-executable in the XBE header that nonetheless hold code.
#: Without these the lift silently omits them -- the evaluation's function counts
#: (16,211 across eight sections) depend on this list.
EXTRA_CODE_SECTIONS = "DOLBY,XON_RD"

#: Observed address-taken entries for the certified title, not suppressed host
#: boundaries. Other executables never receive these addresses automatically.
DEFAULT_THREAD_ENTRIES = Path(__file__).resolve().parents[1] / "config/lift_thread_entries.json"


def _entry_seed_paths(xbe: Path, explicit: tuple[Path, ...]) -> tuple[Path, ...]:
    digest = hashlib.sha256(xbe.read_bytes()).hexdigest()
    selected = []
    for path, bundled in [(DEFAULT_THREAD_ENTRIES, True), *((p, False) for p in explicit)]:
        path = path.resolve()
        try:
            entries = json.loads(path.read_text())
        except (OSError, ValueError) as error:
            raise LiftError(f"cannot read entry seeds {path}: {error}") from error
        if not isinstance(entries, list):
            raise LiftError(f"entry seeds {path} must be a vendored seed-functions list")
        bindings = {entry.get("xbe_sha256") for entry in entries if isinstance(entry, dict)}
        bindings.discard(None)
        if bundled and (
            len(bindings) != 1
            or any(not isinstance(entry, dict) or not entry.get("xbe_sha256") for entry in entries)
        ):
            raise LiftError(f"bundled entry seeds {path} require one explicit XBE binding")
        if bindings and bindings != {digest}:
            if bundled:
                continue
            raise LiftError(f"entry seeds {path} are bound to another XBE")
        if path not in selected:
            selected.append(path)
    return tuple(selected)


@dataclass(frozen=True)
class StageResult:
    """One pipeline stage that ran to completion."""

    name: str
    command: tuple[str, ...]
    seconds: float
    #: Last few lines of the stage's own output, kept for the failure message.
    tail: str


class LiftError(RuntimeError):
    """A stage exited non-zero, or produced no usable output."""


@dataclass(frozen=True)
class LiftResult:
    """What a completed lift produced.

    The counts are read from the lifter's own ``summary.json`` rather than
    recomputed, so a number reported here is a number the lifter stands behind.
    """

    xbe: Path
    out_dir: Path
    gen_dir: Path
    #: Guest functions with a generated body.
    functions: int
    #: Functions the lifter gave up on. Nonzero means the lift is incomplete.
    failed: int
    #: Total lines of generated C.
    lines: int
    #: Generated ``recomp_*.c`` chunks, excluding the dispatch and stub files.
    chunks: int
    #: Call targets with no detected function, emitted as esp-correcting no-ops.
    unresolved_stubs: int
    #: Mnemonic -> the guest addresses the lifter could not translate. The
    #: evaluation measured 917 sites over 39 mnemonics, almost all data decoded as
    #: code -- `outsd`, `insb`, `arpl` and friends, which no user-mode MSVC game
    #: emits. The addresses are kept, not just the counts, because the profile is
    #: what distinguishes garbage decode from a real gap in the lifter.
    unimplemented: dict[str, tuple[int, ...]] = field(default_factory=dict)
    stages: tuple[StageResult, ...] = ()
    #: Kernel ordinals whose stack-argument count was measured from the guest's own
    #: call sites. See tools/lift/callsites.py; the host needs these to unwind a
    #: kernel call, and refuses to continue past an ordinal that is not here.
    measured_arities: int = 0
    #: How many independently-known arities the measurement reproduced. Zero means
    #: it was never put to the test.
    arities_validated: int = 0
    #: Bodies the lifter suppressed because `--manual-functions` named them. Read
    #: from the lifter's own summary rather than from the input file, because the two
    #: DISAGREE whenever an address is absent from the function database: such an
    #: address still has its call sites rewritten but is never counted here. A zero
    #: when a list was passed means the suppression did not happen.
    manual_functions: int = 0
    #: Addresses the input file asked to suppress. Kept beside the above so the gap
    #: between asked and done is visible without re-reading the input.
    manual_requested: int = 0

    @property
    def unimplemented_sites(self) -> int:
        return sum(len(sites) for sites in self.unimplemented.values())

    def summary_lines(self) -> list[str]:
        """Human-readable counts, one per line."""
        ranked = sorted(self.unimplemented.items(), key=lambda kv: -len(kv[1]))
        top = [(mnemonic, len(sites)) for mnemonic, sites in ranked[:5]]
        return [
            f"xbe              {self.xbe}",
            f"output           {self.gen_dir}",
            f"functions lifted {self.functions}",
            f"failed           {self.failed}",
            f"lines of C       {self.lines}",
            f"chunks           {self.chunks}",
            f"unresolved stubs {self.unresolved_stubs}",
            f"unimpl sites     {self.unimplemented_sites} over {len(self.unimplemented)} mnemonics",
            "unimpl top       " + ", ".join(f"{m}={n}" for m, n in top),
            f"ordinal arities  {self.measured_arities} measured, "
            f"{self.arities_validated} cross-checked against known answers",
            f"manual bodies    {self.manual_functions} suppressed of "
            f"{self.manual_requested} requested",
        ]


def _run_stage(
    name: str,
    module: str,
    args: list[str],
    vendor_root: Path,
    log_dir: Path,
    timeout: int,
    python: str,
) -> StageResult:
    """Run one vendored stage, or raise ``LiftError``.

    Output goes to a file rather than a pipe: ``disasm`` emits a progress line per
    percent per section and the full log is ~25 kB of carriage returns, which is
    useless on a terminal and valuable when a stage fails.
    """
    command = (python, "-m", module, *args)
    env = dict(os.environ)
    # Exactly the vendored root, so upstream's `tools` package wins and ours is
    # not even visible. "" or "." would let the caller's cwd leak in.
    env["PYTHONPATH"] = str(vendor_root)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"{name}.log"
    started = time.monotonic()
    with log_path.open("wb") as log:
        completed = subprocess.run(
            command,
            cwd=vendor_root,
            env=env,
            stdout=log,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    elapsed = time.monotonic() - started
    tail = _tail(log_path)
    if completed.returncode != 0:
        raise LiftError(
            f"stage {name} exited {completed.returncode} after {elapsed:.1f}s\n"
            f"  command: {' '.join(command)}\n"
            f"  log:     {log_path}\n{tail}"
        )
    return StageResult(name=name, command=command, seconds=elapsed, tail=tail)


def _tail(path: Path, lines: int = 20) -> str:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
    # Progress bars overwrite with \r; splitting on both keeps the tail readable.
    parts = text.replace("\r", "\n").splitlines()
    return "\n".join(f"  | {line}" for line in parts[-lines:] if line.strip())


def run_pipeline(
    xbe: Path,
    out_dir: Path,
    vendor_root: Path,
    *,
    chunk_size: int = DEFAULT_CHUNK_SIZE,
    python: str = "python3",
    skip_existing: bool = False,
    timeout: int = 3600,
    manual_functions: Path | None = None,
    seed_functions: tuple[Path, ...] = (),
    only_functions: tuple[int, ...] = (),
    publish_eflags: bool = False,
    flag_bridge: Path | None = None,
    local_float_relations: bool = False,
) -> LiftResult:
    """Lift `xbe` into C under `out_dir`.

    `skip_existing` reuses any stage whose output is already present. The lift is
    deterministic given the same XBE and the same vendored tree, so skipping is
    safe for iteration; it is off by default because a stale intermediate from a
    different binary is exactly the kind of wrong-but-plausible result this
    project has been bitten by. Upstream's own ``--skip-binary-check`` is never
    passed, so a mismatched intermediate is caught rather than used.
    """
    if type(local_float_relations) is not bool:
        raise ValueError("local_float_relations must be a bool")
    xbe = xbe.resolve()
    if not xbe.is_file():
        raise FileNotFoundError(f"no XBE at {xbe}")
    vendor_root = vendor_root.resolve()
    if not (vendor_root / "tools" / "recomp").is_dir():
        raise FileNotFoundError(
            f"no vendored lifter at {vendor_root}; run: uv run python -m tools.lift vendor"
        )

    # Resolved because every stage runs with cwd at the VENDORED root, so a relative
    # path here resolves against third_party/ and the stage dies on a missing file.
    manual_functions = manual_functions.resolve() if manual_functions else None
    flag_bridge = flag_bridge.resolve() if flag_bridge else None
    if flag_bridge and not flag_bridge.is_file():
        raise FileNotFoundError(f"no flag-bridge list at {flag_bridge}")
    requested = _count_manual_requested(manual_functions) if manual_functions else 0
    entry_seed_paths = _entry_seed_paths(xbe, seed_functions)

    out_dir = out_dir.resolve()
    disasm_dir = out_dir / "disasm"
    func_id_dir = out_dir / "func_id"
    abi_dir = out_dir / "abi"
    gen_dir = out_dir / "gen"
    log_dir = out_dir / "logs"
    summary_dir = out_dir / "summary"
    if (
        entry_seed_paths
        and skip_existing
        and any(
            marker.exists()
            for marker in (
                disasm_dir / "functions.json",
                func_id_dir / "identified_functions.json",
                abi_dir / "abi_functions.json",
                summary_dir / "summary.json",
            )
        )
    ):
        raise LiftError(
            "entry seeds with --skip-existing cannot reuse discovery/ABI/recomp outputs; "
            "use a fresh --out-dir or remove those stage outputs"
        )
    summary_marker = summary_dir / "summary.json"
    if skip_existing and summary_marker.exists():
        if local_float_relations:
            raise LiftError("--local-float-relations cannot reuse an existing recomp summary")
        previous = json.loads(summary_marker.read_text(encoding="utf-8"))
        if previous.get("local_float_relations", False) is not False:
            raise LiftError("default generation cannot reuse local-float-relations outputs")
    for directory in (disasm_dir, func_id_dir, abi_dir, gen_dir, log_dir, summary_dir):
        directory.mkdir(parents=True, exist_ok=True)

    stages: list[StageResult] = []
    # disasm looks for "<xbe stem>_analysis.json" *beside the XBE* unless told
    # otherwise. The XBE is the user's own disc extract and is not ours to write
    # into, so the analysis lands in the output directory and is passed by path.
    analysis_json = out_dir / "xbe_analysis.json"

    def stage(name: str, module: str, args: list[str], marker: Path) -> None:
        if skip_existing and marker.exists():
            return
        stages.append(_run_stage(name, module, args, vendor_root, log_dir, timeout, python))

    # A manual list only takes effect if the recomp stage actually RUNS. Reusing an
    # existing summary would report the old tree's counts for a run that suppressed
    # nothing, which is indistinguishable from success in every number we print.
    if manual_functions and skip_existing and (summary_dir / "summary.json").exists():
        raise LiftError(
            f"--manual-functions with --skip-existing, but {summary_dir / 'summary.json'} "
            f"already exists, so the recomp stage would be skipped and the list would "
            f"have no effect. Remove that summary, or lift into a fresh --out-dir."
        )
    # The same silent-skip hazard applies to the EFLAGS option: a reused summary
    # reports a tree that publishes nothing, indistinguishable by its counts.
    if publish_eflags and skip_existing and (summary_dir / "summary.json").exists():
        raise LiftError(
            f"--publish-eflags with --skip-existing, but {summary_dir / 'summary.json'} "
            f"already exists, so the recomp stage would be skipped and the option would "
            f"have no effect. Remove that summary, or lift into a fresh --out-dir."
        )

    # Same silent-skip hazard for the flag bridge: a reused summary is a tree without it.
    if flag_bridge and skip_existing and (summary_dir / "summary.json").exists():
        raise LiftError(
            f"--flag-bridge with --skip-existing, but {summary_dir / 'summary.json'} "
            f"already exists, so the recomp stage would be skipped and the bridge would "
            f"have no effect. Remove that summary, or lift into a fresh --out-dir."
        )

    stage(
        "xbe_parser",
        "tools.xbe_parser",
        [str(xbe), "--json", str(analysis_json), "--quiet"],
        analysis_json,
    )
    stage(
        "disasm",
        "tools.disasm",
        [
            str(xbe),
            "-o",
            str(disasm_dir),
            "--analysis-json",
            str(analysis_json),
            "--extra-sections",
            EXTRA_CODE_SECTIONS,
            *(
                argument
                for path in entry_seed_paths
                for argument in ("--seed-functions", str(path))
            ),
            # The stage's own cache is keyed on the XBE alone, so without this a
            # re-lift into an existing directory answers "Cache hit - results
            # unchanged" and silently skips every disasm-stage patch made since.
            "--force",
        ],
        disasm_dir / "functions.json",
    )
    stage(
        "func_id",
        "tools.func_id",
        [
            str(xbe),
            "--functions",
            str(disasm_dir / "functions.json"),
            "--strings",
            str(disasm_dir / "strings.json"),
            "--xrefs",
            str(disasm_dir / "xrefs.json"),
            "--output",
            str(func_id_dir),
        ],
        func_id_dir / "identified_functions.json",
    )
    stage(
        "abi_analysis",
        "tools.abi_analysis",
        [
            str(xbe),
            "--disasm-dir",
            str(disasm_dir),
            "--func-id-dir",
            str(func_id_dir),
            "--output-dir",
            str(abi_dir),
        ],
        abi_dir / "abi_functions.json",
    )
    selected = set(only_functions)
    if selected and flag_bridge:
        from tools.lift.flag_bridge import load_bridge

        bridge = load_bridge(flag_bridge)
        selected.update(bridge.providers)
        selected.update(bridge.consumers)
    selected_args = [
        value for address in sorted(selected) for value in ("--only-function", hex(address))
    ]
    manual_args = ["--manual-functions", str(manual_functions)] if manual_functions else []
    # Opt-in EFLAGS publication for the differential harness. Appended only
    # when asked for, so a default invocation's argv -- and therefore its
    # output -- is unchanged.
    eflags_args = ["--publish-eflags"] if publish_eflags else []
    # Opt-in flag bridge (lifter patch 21), likewise appended only when asked for.
    bridge_args = ["--flag-bridge", str(flag_bridge)] if flag_bridge else []
    stage(
        "recomp",
        "tools.recomp",
        [
            *manual_args,
            *selected_args,
            *eflags_args,
            *bridge_args,
            *(["--local-float-relations"] if local_float_relations else []),
            str(xbe),
            "--all",
            "--split",
            str(chunk_size),
            "--gen-dir",
            str(gen_dir),
            "--output-dir",
            str(summary_dir),
            "--disasm-dir",
            str(disasm_dir),
            "--func-id-dir",
            str(func_id_dir),
            "--abi-dir",
            str(abi_dir),
            "--game-name",
            "TimeSplitters FP",
        ],
        summary_dir / "summary.json",
    )

    summary_mode = json.loads(summary_marker.read_text(encoding="utf-8")).get(
        "local_float_relations", False
    )
    if summary_mode is not local_float_relations:
        raise LiftError("recomp summary does not confirm requested local-float-relations mode")
    result = _read_result(xbe, out_dir, gen_dir, summary_dir / "summary.json", tuple(stages))
    result = replace(result, manual_requested=requested)
    if selected:
        summary = json.loads(summary_marker.read_text(encoding="utf-8"))
        if summary.get("selected_functions") != sorted(selected):
            raise LiftError("partial recomp summary does not match selected functions")
        if len(summary.get("manual_context", [])) != requested:
            raise LiftError("partial recomp did not retain the complete manual-call context")
    if requested and not result.manual_functions and not selected:
        # The list was read and nothing was suppressed. Every other number the lift
        # prints is identical to a run with no list at all, so this has to be loud.
        raise LiftError(
            f"asked to suppress {requested} bodies but the lifter reports 0 suppressed. "
            f"The call sites were therefore NOT rewritten and no address-keyed "
            f"dispatcher is reachable. Check {manual_functions} parses as a JSON list "
            f"or {{address: name}} map of hex addresses."
        )
    if flag_bridge:
        # Every listed function must really carry the bridge. A lift that ignored it has the
        # same function, chunk and line counts as one that applied it.
        from tools.lift.flag_bridge import applied_problems, load_bridge

        problems = applied_problems(gen_dir, load_bridge(flag_bridge))
        if problems:
            raise LiftError("the flag bridge was not applied: " + "; ".join(problems))
    # Selected output is a graft source, not a standalone host profile. Its
    # handful of calls cannot qualify a replacement kernel-arity table. The
    # private installed baseline retains its existing measured table.
    return result if selected else _measure_arities(xbe, gen_dir, result)


def _count_manual_requested(path: Path) -> int:
    """How many distinct addresses the manual list asks the lifter to suppress.

    Counted here rather than trusted from the lifter's own figure because the two
    legitimately differ: an address absent from the function database still gets its
    call sites rewritten but is not counted as a suppressed body. Only by holding
    both numbers can "nine are not in the database" be told from "the file was
    silently empty".
    """
    if not path.is_file():
        raise FileNotFoundError(f"no manual-function list at {path}")
    entries = json.loads(path.read_text(encoding="utf-8"))
    keys = entries.keys() if isinstance(entries, dict) else entries
    addresses = set()
    for entry in keys:
        if isinstance(entry, dict):
            entry = entry.get("start") or entry.get("address")
        addresses.add(int(entry, 16) if isinstance(entry, str) else int(entry))
    if not addresses:
        raise LiftError(
            f"{path} names no addresses. A lift with an empty manual list is "
            f"byte-identical to a lift with no list, so it is refused rather than run."
        )
    return len(addresses)


def _measure_arities(xbe: Path, gen_dir: Path, result: LiftResult) -> LiftResult:
    """Measure kernel stack-argument counts and emit them beside the lifted C.

    Done here rather than as a sixth subprocess stage because it reads the lift's
    own output with our code, not upstream's. The table lands in the generated
    directory because it is derived from the user's executable and must never be
    committed.
    """
    slots = thunk_slot_ordinals(xbe)
    report = measure(gen_dir, slots)
    _mismatches, checked = report.validate()
    written = emit_c(report, gen_dir / "kernel_arity.inc")
    return replace(result, measured_arities=written, arities_validated=checked)


def _read_result(
    xbe: Path, out_dir: Path, gen_dir: Path, summary_path: Path, stages: tuple[StageResult, ...]
) -> LiftResult:
    if not summary_path.is_file():
        raise LiftError(f"the lifter wrote no summary at {summary_path}")
    raw = json.loads(summary_path.read_text(encoding="utf-8"))
    chunks = len(list(gen_dir.glob("recomp_[0-9]*.c")))
    if chunks == 0:
        raise LiftError(f"the lifter reported success but {gen_dir} holds no chunks")
    return LiftResult(
        xbe=xbe,
        out_dir=out_dir,
        gen_dir=gen_dir,
        functions=int(raw.get("translated", 0)),
        failed=int(raw.get("failed", 0)),
        lines=int(raw.get("total_lines", 0)),
        chunks=chunks,
        unresolved_stubs=int(raw.get("unresolved_stubs", 0)),
        manual_functions=int(raw.get("manual_functions", 0)),
        unimplemented={
            str(mnemonic): tuple(int(site) for site in sites)
            for mnemonic, sites in dict(raw.get("unimplemented", {})).items()
        },
        stages=stages,
    )
