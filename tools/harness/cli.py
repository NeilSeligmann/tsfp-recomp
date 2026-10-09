# SPDX-License-Identifier: GPL-3.0-or-later
"""Run the differential harness: lifted C against the original bytes under Unicorn.

The oracle is Unicorn executing the actual x86-32 machine code from the retail XBE. The
subject is the compiled lifted C for the same address, reached through the generated
dispatch table. Both sides start from byte-identical guest memory and register state,
and both final registers AND the exact set of guest bytes each side changed are
compared.

Usage:

    ./.venv/bin/python -m tools.harness.cli \\
        --xbe tmp/oxm-extract/retail/default.xbe \\
        --functions generated/retail/functions.csv \\
        --subject tmp/harness-build/subject \\
        --out generated/harness/results.csv \\
        --cases-per-function 6 --seed 20261001

Every case is reproducible from the `seed` and `case_index` recorded on its CSV row, so
a divergence found in a twelve-thousand-case run can be re-driven on its own with
`--only-va` and the same seed.

Every row ALSO records which lifted tree the subject was built from, read from the
subject's own baked-in provenance rather than from a flag. That was the one thing missing
when three successive verdict counts went into the project record and were retracted: the
last two had been measured against a lift ten hours older than the fix they were supposed
to be testing, and nothing in the output said so. A subject that is not the shipped tree
now stops the run unless `--allow-foreign-subject REASON` says why.

A run is always stoppable: `--max-seconds` ends it cleanly at a budget, rows are
flushed as they are produced, and the summary reports exactly how many cases ran. It
does not extrapolate to the ones that did not.
"""

from __future__ import annotations

import argparse
import atexit
import hashlib
import json
import os
import sys
import time
from dataclasses import replace
from pathlib import Path

from tools.codediff.boundaries import (
    DEFAULT_ADDITIONS,
    DEFAULT_OVERRIDES,
    load_function_table,
)
from tools.replace.manifest import ManifestEntry, cross_check, manifest_sha, parse_listrepl
from tools.replace.scan import scan_directory

from . import synth_domain
from .callclosure import LiveCallClosure, LiveClosureError, discover_live_call_closure
from .callstub import StubPlan, _decoder
from .code_safety import SourceBinding
from .compare import compare
from .dispatch_source import DERIVED_LIMIT_FACTOR, MAX_DERIVED_SEEDS, derived_seed_locations
from .feedback import (
    FEEDBACK_INDEX_BASE,
    FRAME_BYTES,
    MAX_SEEDS,
    MAX_VARIANTS,
    SEED_CASES,
    TARGET_DISCRIMINATING,
    TEXT_HI,
    TEXT_LO,
    TOP_UP_VARIANTS_PER_SEED,
    body_immediates,
    feedback_variants,
    value_pool,
)
from .fixture_selection import select_labels
from .guarded_tables import GuardedJumpTable, prove_guarded_tables
from .image import GuestImage, build_guest_image, write_image_file
from .jumps import direct_jump_target
from .memmove_probe import MEMMOVE_VA, run_memmove_probe
from .model import (
    DIVERGENT_OUTCOMES,
    FP_SCALAR_MODE,
    LIFTER_MODELED_FLAGS,
    ORACLE_NONVERDICT_FAULTS,
    Case,
    ExecResult,
    Outcome,
    SkippedFunction,
)
from .oracle import DEFAULT_MAX_INSNS, UnicornOracle
from .provenance import SHIPPED_GEN_DIR, shipped_mismatch, short_sha, tree_digest
from .providers import REGISTRY, Provider, iter_provider_cases, validate_registry
from .replacement import (
    DEAD_FRAME_BYTES,
    SUBJECT_FAULT_NOT_REPLACED,
    ReplacementJudge,
    register_argument_names,
    register_arguments,
    write_proof,
)
from .results import ResultWriter, render_summary
from .seeding import (
    FLAGS_ADVERSARIAL_SEEDING,
    LEGACY_SEEDING,
    SeedPolicy,
    edge_selectors,
    make_case,
    make_edge_case,
)
from .selection import (
    DEFAULT_MAX_SIZE,
    DEFAULT_MIN_SIZE,
    Candidate,
    Selection,
    select,
    suppressed_manual_entries,
)
from .static_tables import StaticJumpTable
from .subject import DEFAULT_CASE_TIMEOUT, SubjectError, SubjectProcess
from .x87 import PRECISION_SENSITIVE_VAS, attach_fp_stack

DEFAULT_SEED = 20261001
DEFAULT_CASES_PER_FUNCTION = 6
#: A replacement is judged far deeper than a lifted function is swept: it is one of a dozen,
#: not one of four thousand, and 6 random cases is what a whole-image sweep can afford.
DEFAULT_REPLACEMENT_CASES = 600
DEFAULT_OUT = Path("generated/harness/results.csv")
DEFAULT_IMAGE = Path("generated/harness/guest.img")
DEFAULT_REPLACE_DIR = Path("src/game")
DEFAULT_PROOF = Path("generated/replace/proof.json")
#: Exit codes for the replacement mode's own refusals. 3 and 4 are the existing stub and
#: provenance refusals and are not reused, so a caller can tell them apart.
EXIT_NOT_REPLACED = 5
EXIT_NO_REPLACEMENTS = 6
EXIT_STALE_REPLACEMENT = 7
#: --compare-eflags against a subject that publishes nothing, or combined with a
#: mode whose cases could never carry flags. Refused, not silently degraded: such a
#: run would report every case unpublished and verify no flag at all.
EXIT_NO_EFLAGS = 8
PROGRESS_EVERY = 250


def _parse_synth_pin(value: str) -> tuple[int, str]:
    """`VA=SHA256` with a hex VA and a 64-hex digest, normalised to lower case."""
    try:
        left, right = value.split("=", 1)
        address, digest = int(left, 16), right.lower()
        if len(digest) != 64 or int(digest, 16) < 0:
            raise ValueError("digest")
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected HEXVA=SHA256 (64 hex digits)") from error
    return address, digest


def synth_selection(selection: tuple[str, ...] | None, enabled: bool) -> tuple[str, ...] | None:
    """An explicit provider selection must still run the opted-in synth-domain provider."""
    if enabled and selection is not None and synth_domain.LABEL not in selection:
        return (*selection, synth_domain.LABEL)
    return selection


def synth_stream_sha256(digests: list[str]) -> str:
    """sha256 over the concatenated per-case digests, in order (executed or regenerated)."""
    return hashlib.sha256("".join(digests).encode("ascii")).hexdigest()


def synth_regenerated_digests(
    domain: synth_domain.Domain, seed: int, policy: SeedPolicy
) -> list[str]:
    """Digest of every case the domain itself regenerates for `seed`, ordinal order."""
    return [
        synth_domain.case_digest(domain.make_case(seed, ordinal, policy=policy))
        for ordinal in range(domain.case_count())
    ]


def write_synth_artifact(directory: Path, domain: synth_domain.Domain) -> Path:
    """Canonical per-root domain artifact: the domain document plus its digest."""
    path = directory / f"{domain.va:08x}.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = {**domain.document, "sha256": domain.sha256}
    path.write_text(json.dumps(payload, sort_keys=True, indent=1) + "\n", encoding="utf-8")
    return path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Differentially test lifted C against the original guest code executed "
            "under Unicorn, comparing final registers and the exact write-set."
        )
    )
    parser.add_argument(
        "--xbe", type=Path, required=True, help="the original XBE (the oracle's code)"
    )
    parser.add_argument(
        "--functions",
        type=Path,
        required=True,
        metavar="CSV",
        help="ExportFunctionBounds.java CSV giving function entry VAs and sizes",
    )
    parser.add_argument(
        "--subject",
        type=Path,
        required=True,
        help="compiled driver.c linked against the lifted C (see build_subject.sh)",
    )
    parser.add_argument(
        "--image",
        type=Path,
        default=DEFAULT_IMAGE,
        help=f"flat guest image to write and hand to the subject (default {DEFAULT_IMAGE})",
    )
    parser.add_argument(
        "--out", type=Path, default=DEFAULT_OUT, help=f"results CSV (default {DEFAULT_OUT})"
    )
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help=f"default {DEFAULT_SEED}")
    parser.add_argument(
        "--cases-per-function",
        type=int,
        default=None,
        metavar="N",
        help=(
            "random initial states per function "
            f"(default {DEFAULT_CASES_PER_FUNCTION}, or {DEFAULT_REPLACEMENT_CASES} with "
            "--replacement)"
        ),
    )
    parser.add_argument(
        "--max-functions",
        type=int,
        default=None,
        metavar="N",
        help="cap the executed function count; the rest are still reported as selected",
    )
    parser.add_argument(
        "--only-va",
        type=lambda s: int(s, 16),
        default=None,
        metavar="HEX",
        help="restrict the run to one guest VA, for reproducing a recorded divergence",
    )
    parser.add_argument(
        "--overrides",
        type=Path,
        default=DEFAULT_OVERRIDES,
        metavar="CSV",
        help="tracked size/entry corrections layered over --functions (default: %(default)s)",
    )
    parser.add_argument(
        "--additions",
        type=Path,
        default=DEFAULT_ADDITIONS,
        metavar="CSV",
        help="verified function-table additions (T1266, default: %(default)s)",
    )
    parser.add_argument("--min-size", type=int, default=DEFAULT_MIN_SIZE, metavar="N")
    parser.add_argument("--max-size", type=int, default=DEFAULT_MAX_SIZE, metavar="N")
    parser.add_argument(
        "--static-jump-tables",
        action="store_true",
        help="selected replacement only: prove bounded original intraprocedural tables; "
        "runtime body/table identity changes remain no-verdict refusals",
    )
    parser.add_argument(
        "--guarded-jumps",
        action="store_true",
        help="T1576 opt-in (needs TSFP_GUARDED_JUMP_CONTRACT, set by tools.replace prove "
        "--guarded-jump-contract): guarded jump tables and exact-entry tails in the closure "
        "scan, per-case arm witness for a table root",
    )
    parser.add_argument(
        "--guarded-sweep-version",
        type=int,
        choices=(1, 2, 3),
        default=1,
        help="T1576 batch C: 1 = legacy first-four-loads sweep, 2 = derived dispatch/tail "
        "sources, no text values in the feedback pool, 3 = 2 plus depth-2 derived feedback seeds "
        "(needs --guarded-jumps)",
    )
    parser.add_argument(
        "--witness-subject",
        type=Path,
        default=None,
        metavar="BIN",
        help="T1576: the clang O0 coverage subject built from the same draft",
    )
    parser.add_argument(
        "--witness-draft",
        type=Path,
        default=None,
        metavar="C",
        help="T1576: the draft source file that holds the root's switch",
    )
    parser.add_argument(
        "--max-seconds",
        type=float,
        default=None,
        metavar="S",
        help="stop cleanly after this long; already-written rows are kept and reported",
    )
    parser.add_argument(
        "--case-timeout",
        type=float,
        default=DEFAULT_CASE_TIMEOUT,
        metavar="S",
        help=(
            f"kill and restart the subject if a case takes longer (default {DEFAULT_CASE_TIMEOUT})"
        ),
    )
    parser.add_argument(
        "--max-insns",
        type=int,
        default=DEFAULT_MAX_INSNS,
        metavar="N",
        help=f"oracle instruction budget per case (default {DEFAULT_MAX_INSNS:,})",
    )
    parser.add_argument(
        "--flush-every",
        type=int,
        default=1,
        metavar="N",
        help="flush the CSV every N rows; 1 means a kill -9 loses nothing",
    )
    parser.add_argument(
        "--reuse-image",
        action="store_true",
        help="do not rebuild the flat guest image if it already exists",
    )
    parser.add_argument(
        "--legacy-seeding",
        action="store_true",
        help=(
            "generate inputs exactly as they were generated before the input distribution "
            "was widened, so the previous verdict count can be RE-MEASURED on the current "
            "tree rather than quoted from an older one"
        ),
    )
    parser.add_argument(
        "--shipped-gen-dir",
        type=Path,
        default=SHIPPED_GEN_DIR,
        metavar="DIR",
        help=(
            "the lifted tree the repo ships, which the subject's own baked-in provenance "
            f"is checked against (default {SHIPPED_GEN_DIR})"
        ),
    )
    parser.add_argument(
        "--allow-foreign-subject",
        metavar="REASON",
        default=None,
        help=(
            "run even though the subject was not built from the shipped tree, recording "
            "REASON in the summary. Measuring a different tree is legitimate -- the "
            "single-variable control that settled the g_ebp question did exactly that -- "
            "but it has to be said out loud, because the alternative was three published "
            "and retracted answers measured against a lift ten hours out of date"
        ),
    )
    parser.add_argument(
        "--stub-calls",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "bring call-bearing functions into scope by replacing their callees with one "
            "synthetic behaviour on both sides (default: on). Requires a subject built "
            "from stub-enabled objects; the run aborts rather than compare a stubbed "
            "oracle against a subject running the real callees"
        ),
    )
    parser.add_argument(
        "--replacement",
        action="store_true",
        help=(
            "judge the hand-written replacements linked into the subject (src/game, built "
            "by `tools/replace build-subject`) instead of the lifted functions. Only the "
            "registered addresses run, at --cases-per-function random cases plus argument "
            "edge cases, and a proof JSON is written. Refuses a subject with no "
            "replacements, a subject built from different hand code than src/game now "
            "holds, and any address whose dispatch table entry is not its adapter"
        ),
    )
    parser.add_argument(
        "--live-call-closure",
        action="store_true",
        help=(
            "for one --only-va root, run its statically verified direct-call closure live "
            "on both sides without synthetic callees; report closure provenance"
        ),
    )
    parser.add_argument(
        "--live-vector-state",
        action="store_true",
        help="seed/compare all XMM registers and MXCSR; admit only supported SSE closure forms",
    )
    parser.add_argument(
        "--vector-mode",
        choices=("legacy", FP_SCALAR_MODE),
        default="legacy",
        help=(
            "T1620: with --live-vector-state, fp-scalar-v1 admits scalar float arithmetic "
            "(MXCSR fixed 0x1F80, masked sticky status, NaN-pair tripwire, pointee float seeding)"
        ),
    )
    parser.add_argument(
        "--fp-build-record",
        type=Path,
        default=None,
        metavar="JSON",
        help="T1620: build-flag record written by `tools.replace prove` for fp-scalar-v1",
    )
    parser.add_argument(
        "--live-call-boundary",
        action="append",
        type=lambda s: int(s, 16),
        default=[],
        metavar="HEX",
        help=(
            "explicitly mark one reachable closure node as an unproved boundary; its complete "
            "body is fingerprinted and still runs live, but its descendants are not claimed"
        ),
    )
    parser.add_argument(
        "--live-memory-dword",
        action="append",
        type=_parse_hex_pair,
        default=[],
        metavar="VA=VALUE",
        help="add an explicit dword initial-state patch to every live-closure case",
    )
    parser.add_argument(
        "--live-stack-dword",
        action="append",
        type=_parse_hex_pair,
        default=[],
        metavar="OFFSET=VALUE",
        help="patch a dword at entry ESP plus OFFSET in every live-closure case",
    )
    parser.add_argument(
        "--replace-dir",
        type=Path,
        default=DEFAULT_REPLACE_DIR,
        metavar="DIR",
        help=(
            "the hand-written sources the subject must have been built from "
            f"(default {DEFAULT_REPLACE_DIR})"
        ),
    )
    parser.add_argument(
        "--proof-out",
        type=Path,
        default=DEFAULT_PROOF,
        metavar="JSON",
        help=f"where --replacement writes its proof file (default {DEFAULT_PROOF})",
    )
    parser.add_argument(
        "--memmove-probe",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "run the directed memmove overlap probe after the sweep (default: on). "
            "`_memmove` is partial-decode for the random sweep, so without this check "
            "its overlap semantics -- including backward-overlapping copies -- are "
            "verified by nothing. The probe reports itself by name in the summary, and "
            "a run where it cannot execute prints a named SKIPPED line"
        ),
    )
    parser.add_argument(
        "--compare-eflags",
        action="store_true",
        help=(
            "compare the subject's published EFLAGS against the oracle's real ones, "
            "masked per exit to the bits the lifter's model claimed (CF/PF/ZF/SF/DF/OF "
            "where answerable; AF never, and it is reported as unmodelled rather than "
            "skipped). Requires a subject built from a --publish-eflags lift, which the "
            "subject's own READY line attests. Also switches the input distribution to "
            "the flags-adversarial policy: forced add/sub overflow boundaries and "
            "shift counts 0/1/31/32/33, which random pointer-shaped inputs never hit"
        ),
    )
    parser.add_argument(
        "--edge-cases",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "with --replacement, also sweep each argument slot over 0, 1, 2, the signed "
            "boundary and all-ones (default: on). Random inputs are pointer-shaped and "
            "never reach those values"
        ),
    )
    fixture_group = parser.add_mutually_exclusive_group()
    fixture_group.add_argument(
        "--no-fixture-providers",
        action="store_true",
        help="disable additive fixtures; retain random, argument-edge and feedback streams",
    )
    fixture_group.add_argument(
        "--fixture-provider",
        action="append",
        default=None,
        metavar="LABEL",
        help="select explicit additive provider labels; random/edge/feedback remain unchanged",
    )
    parser.add_argument(
        "--synth-domain",
        action="store_true",
        help=(
            "T1773: with --replacement, add the synthesized-domain provider derived from the "
            "ORIGINAL function bytes only (additive; random/edge/feedback and every other "
            "provider remain unchanged). Reported as its own evidence class"
        ),
    )
    parser.add_argument(
        "--synth-domain-version",
        choices=("t1773-synth-v1", "t1773-synth-v2"),
        default="t1773-synth-v1",
        help="frozen synthesis generator; v2 follows bounded original direct callees",
    )
    parser.add_argument(
        "--synth-domain-analysis-table",
        type=Path,
        default=None,
        help="pinned original function-bound metadata for historical synthesis artifacts",
    )
    parser.add_argument(
        "--synth-domain-dir",
        type=Path,
        default=None,
        metavar="DIR",
        help=(
            "where --synth-domain writes the per-root artifact <va:08x>.json "
            "(default: 'synth-domain' beside --proof-out)"
        ),
    )
    parser.add_argument(
        "--synth-domain-pin",
        action="append",
        default=None,
        type=_parse_synth_pin,
        metavar="VA=SHA256",
        help=(
            "T1773: pin a root's derived domain digest (hex VA, 64 hex). A fresh derivation "
            "that differs makes the root unjudgeable (synth-domain-pin-mismatch)"
        ),
    )
    return parser


def _parse_hex_pair(value: str) -> tuple[int, int]:
    try:
        left, right = value.split("=", 1)
        address, word = int(left, 16), int(right, 16)
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected HEX=HEX") from error
    if not 0 <= word <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("dword value must fit 32 bits")
    return address, word


def _fp_scalar(args: argparse.Namespace) -> bool:
    """True when the opt-in fp-scalar-v1 vector mode is selected (never by default)."""
    return getattr(args, "vector_mode", "legacy") == FP_SCALAR_MODE


def _live_case_state(case: Case, args: argparse.Namespace) -> Case:
    if getattr(args, "live_vector_state", False):
        from .vector import attach_scalar_fp_state, attach_vector_state

        if _fp_scalar(args):
            tracker = getattr(args, "scalar_fp_tracker", None)
            case = attach_scalar_fp_state(case, tracker.constants if tracker is not None else ())
        else:
            case = attach_vector_state(case)
    patches = list(case.patches)
    for address, value in args.live_memory_dword:
        patches.append((address, value.to_bytes(4, "little")))
    for offset, value in args.live_stack_dword:
        patches.append((case.esp + offset, value.to_bytes(4, "little")))
    return replace(case, patches=tuple(patches)) if len(patches) != len(case.patches) else case


def choose_functions(selection: Selection, args: argparse.Namespace) -> list[Candidate]:
    """Candidates to execute, honouring `--only-va` and `--max-functions`.

    Order is ascending by address, NOT shuffled. A shuffle would make `case_index`
    depend on the function list rather than on the seed alone, which is exactly the
    reproducibility property the CSV promises.
    """
    chosen = list(selection.candidates)
    if args.only_va is not None:
        chosen = [c for c in chosen if c.va == args.only_va]
    chosen.sort(key=lambda c: c.va)
    if args.max_functions is not None:
        chosen = chosen[: args.max_functions]
    return chosen


def _table_sections(chosen: list[Candidate], image: GuestImage, root: int) -> list[dict]:
    """Original section identity and permissions of every guarded table region (evidence only:
    the flat runtime mapping is writable, so the oracle's rehash is the actual protection)."""
    sections = image.source_sections or ()
    candidate = next((c for c in chosen if c.va == root), None)
    rows: list[dict] = []
    for table in candidate.static_tables if candidate else ():
        if not isinstance(table, GuardedJumpTable):
            continue
        for address, data in table.regions:
            owners = [
                section
                for section in sections
                if section.address <= address
                and address + len(data) <= section.address + section.size
            ]
            rows.append(
                {
                    "table_site": f"0x{table.site:08x}",
                    "region": f"0x{address:08x}",
                    "bytes": len(data),
                    "section": None
                    if len(owners) != 1
                    else {
                        "address": f"0x{owners[0].address:08x}",
                        "size": owners[0].size,
                        "executable": owners[0].executable,
                        "writable": owners[0].writable,
                    },
                }
            )
    return rows


def _render_event(event: tuple) -> dict[str, object]:
    kind, site, slot, target = event
    return {"kind": kind, "site": f"0x{site:08x}", "slot": slot, "target": f"0x{target:08x}"}


def _closure_edges(
    chosen: list[Candidate], closure: LiveCallClosure | None, image: GuestImage, root: int
) -> set[tuple]:
    """Events a campaign must have observed: each non-root tail edge, each non-root table slot
    and its default (the root's own arms are witnessed per case by the session)."""
    required: set[tuple] = set()
    for site, target in _node_tail_sites(closure, root, image).items():
        required.add(("tail", site, -1, target))
    candidate = next((c for c in chosen if c.va == root), None)
    for table in candidate.static_tables if candidate else ():
        if table.body_va == root or not isinstance(table, GuardedJumpTable):
            continue
        for slot, target in enumerate(table.targets):
            required.add(("slot", table.site, slot, target))
        required.add(("default", table.site, -1, table.default_target))
    return required


def _node_tail_sites(
    closure: LiveCallClosure | None, root: int, image: GuestImage
) -> dict[int, int]:
    """JMP site -> exact target of every out-of-body tail edge of the NON-root closure nodes, so
    the oracle trace records when the live closure actually takes one (evidence, not a gate)."""
    sites: dict[int, int] = {}
    if closure is None:
        return sites
    decoder = _decoder()
    for node in closure.nodes:
        if node.va == root or not node.tails:
            continue
        for insn in decoder.disasm(image.code_at(node.va, node.size), node.va):
            target = direct_jump_target(insn)
            if target is not None and not node.va <= target < node.va + node.size:
                sites[insn.address] = target
    return sites


def _start_guarded_session(
    args: argparse.Namespace,
    chosen: list[Candidate],
    image: GuestImage,
    functions: list[tuple[int, int]],
    oracle: UnicornOracle,
) -> str | None:
    """Build the per-case guarded-jump session for a table/tail root. Returns an error or None.

    A root with neither table nor tail (a root whose JUMPS live only in its closure) needs no
    session: its nodes are guarded statically and by the oracle's table rehash."""
    from tools.harness.callstub import CalleeConventions
    from tools.replace import arm_witness as witness
    from tools.replace.guarded_jump_session import GuardedSession, entry_function_name
    from tools.replace.tail_adapter import TailRefusal, check_draft, original_tails

    root = next((c for c in chosen if c.va == args.only_va), None)
    registration = next((r for r in scan_directory(args.replace_dir) if r.va == args.only_va), None)
    if root is None or registration is None:
        return "guarded root or its registration is missing"
    tables = tuple(
        t for t in root.static_tables if isinstance(t, GuardedJumpTable) and t.body_va == root.va
    )
    sizes = dict(functions)
    manifest = args.functions.parent / "manifest.json"
    if manifest.exists():
        for row in json.loads(manifest.read_text(encoding="utf-8")).get("functions", []):
            try:
                sizes.setdefault(int(row["address"], 16), int(row["size"]))
            except (KeyError, TypeError, ValueError):
                continue
    decoder = _decoder()
    code = image.code_at(root.va, root.size)
    try:
        tails = original_tails(
            root.va,
            root.size,
            decoder.disasm(code, root.va),
            sizes,
            CalleeConventions(sizes, image.code_at, decoder),
        )
    except TailRefusal as error:
        return str(error)
    if not tables and not tails:
        return None
    draft = args.witness_draft.read_text(encoding="utf-8")
    try:
        entry_text = witness._function_text(draft, registration.function)
    except witness.WitnessRefusal as error:
        return str(error)
    if tails:
        problems = check_draft(entry_text, registration, tails)
        if problems:
            return "typed tail adapter refused: " + "; ".join(problems)
    try:
        session = GuardedSession(
            root=root.va,
            tables=tables,
            tails=tails,
            draft_path=args.witness_draft,
            entry_function=entry_function_name(registration),
            source_function=registration.function,
            witness_binary=args.witness_subject,
            image_path=args.image,
            work_dir=args.out.parent / "witness",
        )
    except (SubjectError, OSError, ValueError, witness.WitnessRefusal) as error:
        return f"arm witness unavailable: {error}"
    session.attach(oracle)
    atexit.register(session.close)
    args.guarded_session = session
    return None


#: Guarded-table sweep case indices live above the feedback range, in (seed, va, ordinal) space.
GUARD_SWEEP_INDEX_BASE = 1 << 43


def _with_node_tables(
    candidate: Candidate, closure: LiveCallClosure, image: GuestImage
) -> tuple[StaticJumpTable, ...]:
    """Root tables plus the tables of every other closure node, re-proved from original bytes.

    The oracle hashes and write-guards every one of them, executed or not (T1576)."""
    tables = list(candidate.static_tables)
    for node in closure.nodes:
        if node.va == candidate.va or not node.tables:
            continue
        proved = prove_guarded_tables(
            node.va, node.size, image.code_at(node.va, node.size), image.code_at, allow_tails=True
        )
        if proved is None:
            raise LiveClosureError(f"closure node 0x{node.va:08x} tables no longer prove")
        tables.extend(proved)
    return tuple(tables)


def build_live_closure(
    args: argparse.Namespace,
    image: GuestImage,
    corrected: list[object],
    functions: list[tuple[int, int]],
    selected: list[Candidate],
) -> tuple[LiveCallClosure, dict[str, object]]:
    """Discover and fingerprint one explicitly requested root's live call graph."""
    roots = [candidate for candidate in selected if candidate.va == args.only_va]
    # T1595: a vector proof needs the closure channel but the root itself may be a call-free leaf
    # (the T1594 whitelist roots). Other closure runs keep the call-bearing requirement.
    plan = roots[0].stub_plan if len(roots) == 1 else None
    call_bearing = plan is not None and bool(plan.sites)
    # T1611: the waiver needs an explicit stub plan with no call sites, never a missing plan.
    waived = getattr(args, "live_vector_state", False) and plan is not None and not plan.sites
    if len(roots) != 1 or not (call_bearing or waived):
        raise LiveClosureError(f"0x{args.only_va:08x} is not one in-scope, call-bearing candidate")
    sizes = {function.entry_va: function.size_bytes for function in corrected}
    names = {function.entry_va: function.name for function in corrected}
    manifest_path = args.functions.parent / "manifest.json"
    manifest_sha = "missing"
    if manifest_path.exists():
        raw_manifest = manifest_path.read_bytes()
        manifest_sha = hashlib.sha256(raw_manifest).hexdigest()
        for row in json.loads(raw_manifest).get("functions", []):
            try:
                va = int(row["address"], 16)
                size = int(row["size"])
            except (KeyError, TypeError, ValueError):
                continue
            sizes.setdefault(va, size)
            names.setdefault(va, str(row.get("name", f"sub_{va:08x}")))
    closure = discover_live_call_closure(
        args.only_va,
        sizes=sizes,
        names=names,
        read_code=image.code_at,
        boundaries=frozenset(args.live_call_boundary),
        allow_vector=args.live_vector_state,
        vector_mode=FP_SCALAR_MODE if _fp_scalar(args) else "",
        guarded_jumps=getattr(args, "guarded_jumps", False),
    )
    document = closure.document(
        xbe_sha256=hashlib.sha256(args.xbe.read_bytes()).hexdigest(),
        functions_sha256=hashlib.sha256(args.functions.read_bytes()).hexdigest(),
    )
    document["vector_comparison"] = {
        "enabled": args.live_vector_state,
        "registers": "all8 XMM raw128-bit exact equality; missing output refuses verdict",
        "mxcsr": "exact16-bit equality; movement/bitwise preserve control/status",
        "input_stream": "vector-v1 SHA256(seed,index,VA,register), first16 little-endian bytes",
        "refused": "MMX/EMMS, arithmetic, aligned-memory movement, unlisted vector forms",
    }
    if args.live_vector_state:
        from tools.replace.proof_contract import (
            FP_SCALAR_DESCRIPTION,
            ISOLATION_CONTRACT,
            contract_copy,
            fp_scalar_contract_copy,
            validate_closure,
        )

        if getattr(UnicornOracle, "CASE_ISOLATION_CONTRACT", None) != ISOLATION_CONTRACT:
            raise LiveClosureError("vector proof requires measured per-case oracle isolation")
        if _fp_scalar(args):
            document["vector_comparison"] = dict(FP_SCALAR_DESCRIPTION)
            document["state_contract"] = fp_scalar_contract_copy()
        else:
            document["state_contract"] = contract_copy()
    document["generated_manifest_sha256"] = manifest_sha
    document["initial_state"] = {
        "retail_0x0054e0b8": "0x00002694",
        "fp_control_word": "0x037f (all exception masks set)",
        "memory_dwords": [
            {"va": f"0x{va:08x}", "value": f"0x{value:08x}"} for va, value in args.live_memory_dword
        ],
        "stack_dwords": [
            {"offset": f"0x{offset:x}", "value": f"0x{value:08x}"}
            for offset, value in args.live_stack_dword
        ],
    }
    if args.live_vector_state:
        try:
            validate_closure(document, document["root"])
        except ValueError as error:
            raise LiveClosureError(str(error)) from error
    return closure, document


def install_scalar_fp(
    args: argparse.Namespace,
    image: GuestImage,
    live_closure: LiveCallClosure,
    oracle: UnicornOracle,
) -> None:
    """T1620: attach the fp-scalar-v1 tracker (SSE site table, .rdata constants) to the oracle.

    Refuses a missing or failed build record, and a host whose MXCSR control bits are not
    0x1F80 (QEMU's hardfloat path follows the host rounding mode)."""
    from . import vector_native
    from .selection import disassembler
    from .vector_native import HostStub, assert_host_control, host_supported
    from .vector_scalar import ScalarFpTracker, build_sites, harvest_constants

    vector_native.self_check()
    record = json.loads(args.fp_build_record.read_text(encoding="utf-8"))
    if record.get("passed") is not True:
        raise ValueError(f"fp-scalar build record failed: {record.get('failures')}")
    if not host_supported():
        raise RuntimeError("fp-scalar-v1 needs an x86-64 Linux host to check the host MXCSR")
    decoder = disassembler(detail=True)
    instructions = []
    for node in live_closure.nodes:
        instructions += list(decoder.disasm(image.code_at(node.va, node.size), node.va))

    def read_dword(va: int) -> int | None:
        raw = image.code_at(va, 4)
        return int.from_bytes(raw, "little") if len(raw) == 4 else None

    sites = build_sites(instructions)
    tracker = ScalarFpTracker(sites, harvest_constants(sites, read_dword))
    tracker.build = record
    tracker.host_info = {
        "cpu_model": vector_native.cpu_model(),
        "unicorn": vector_native.UNICORN_PIN,
        "matrix": "tests/test_vector_scalar_native_matrix.py",
    }
    from tools.replace.proof_contract import fp_scalar_matrix_identity

    # T1624: the receipt names the matrix result file the mode relies on (fail closed if absent).
    tracker.matrix = fp_scalar_matrix_identity(Path(__file__).resolve().parents[2])
    tracker.host = HostStub()
    assert_host_control(tracker.host)
    oracle.scalar_fp = tracker
    args.scalar_fp_tracker = tracker


def run_case(
    oracle: UnicornOracle,
    subject: SubjectProcess,
    case: Case,
    plan: StubPlan | None = None,
    *,
    static_tables: tuple[StaticJumpTable, ...] = (),
    code_binding: SourceBinding | None = None,
    scoped_fixture_binding: object = None,
    scoped_fixture_kind: str | None = None,
) -> tuple[ExecResult, ExecResult]:
    """Execute one case on both sides, oracle first.

    When the oracle exhausts its instruction budget the subject is deliberately NOT run.
    Nothing is lost by this: a truncated oracle produces no reference, so the case can
    never reach a verdict, and unlike a genuine guest trap there is no fault for the
    subject to have swallowed. What IS lost by running it is the whole run -- a function
    that loops forever under the oracle generally loops forever in the lifted C too, and
    each such case then costs a full case timeout plus a 64 MB process restart. Measured:
    this is what stalled a 50,400-case run at roughly 12,700 cases.

    The skip is recorded as `NOT-RUN` on the CSV row rather than left blank, so these
    cases cannot be mistaken for ones where the subject was checked and behaved.
    """
    if scoped_fixture_binding is not None:
        from .scoped_fixture_binding import Binding

        if type(scoped_fixture_binding) is not Binding or code_binding is not None:
            raise ValueError("exact separate custom fixture binding required")
        oracle_result = oracle.run(
            case,
            plan,
            static_tables=static_tables,
            scoped_fixture_binding=scoped_fixture_binding,
            scoped_fixture_kind=scoped_fixture_kind,
        )
        scoped_fixture_binding.observe(case, getattr(oracle, "code_safety_observation", None))
    elif scoped_fixture_kind is not None:
        raise ValueError("orphaned custom fixture phase")
    elif code_binding is not None:
        oracle_result = oracle.run(
            case,
            plan,
            static_tables=static_tables,
            code_safety=code_binding.certificate,
            code_safety_source=code_binding.source(),
        )
        code_binding.observe(case, getattr(oracle, "code_safety_observation", None))
    else:
        oracle_result = (
            oracle.run(case, plan, static_tables=static_tables)
            if static_tables
            else oracle.run(case, plan)
        )
    if oracle_result.fault in ORACLE_NONVERDICT_FAULTS or (
        oracle_result.fault is not None and oracle_result.fault.startswith("STATIC-JUMP-TABLE-")
    ):
        return oracle_result, ExecResult(fault="NOT-RUN")
    subject_result = subject.run(case, plan)
    return oracle_result, subject_result


def drive_replacement(
    args: argparse.Namespace,
    policy: SeedPolicy,
    image: GuestImage,
    oracle: UnicornOracle,
    subject: SubjectProcess,
    judge: ReplacementJudge,
    writer: ResultWriter,
    candidate: Candidate,
    start_index: int,
    *,
    registry: tuple[Provider, ...] | None = None,
) -> tuple[int, bool]:
    """Run every case for one registered replacement. Returns `(cases run, not_replaced)`.

    Four phases, each reproducible from the seed alone:

    1. RANDOM. The same stream a lifted function would get at this index, so a replacement
       is judged on inputs the lifted code is also judged on. The oracle records what each
       of the first few seeds LOADED.
    2. EDGE. Each argument slot swept over 0, 1, 2, the signed boundary and all-ones, which
       random pointer-shaped inputs never produce.
    3. SHAPED. Explicit string, TLS or table fixtures initialize structures the
       random distribution cannot reach. Both original and subject get identical
       initial bytes; intentionally invalid structures stay original faults.
    4. FEEDBACK. Variants of the seeds from phase 1 that substitute a loaded value for an
       argument or mutate a loaded memory location (see `feedback.py`), to cross branches
       guarded by comparisons the other phases rarely satisfy.
    """
    va, size = candidate.va, candidate.size
    registry = REGISTRY if registry is None else registry
    synth_enabled = bool(getattr(args, "synth_domain", False))
    synth_executed: list[str] | None = None
    synth_block: dict[str, object] | None = None
    if synth_enabled:
        # T1773: derived from the ORIGINAL bytes only, before any case runs, and pinned.
        try:
            pinned = getattr(args, "synth_domain_analysis_table", None)
            analysis = synth_domain.load_analysis_table(pinned) if pinned else {}
            domain = synth_domain.derive(
                va,
                size,
                image,
                generator_version=getattr(args, "synth_domain_version", "t1773-synth-v1"),
                **analysis,
            )
        except (ValueError, OSError) as error:
            judge.mark_unjudgeable(va, f"synth-domain-derive-failed: {error}")
            return 0, False
        proof_out = Path(getattr(args, "proof_out", DEFAULT_PROOF))
        synth_dir = getattr(args, "synth_domain_dir", None)
        synth_dir = Path(synth_dir) if synth_dir is not None else proof_out.parent / "synth-domain"
        artifact = write_synth_artifact(synth_dir, domain)
        try:
            artifact_ref = os.path.relpath(artifact, proof_out.parent)
        except ValueError:
            artifact_ref = str(artifact)
        if artifact_ref.startswith(".."):
            artifact_ref = str(artifact)
        pin = dict(getattr(args, "synth_domain_pin", None) or ()).get(va)
        if pin is not None and pin != domain.sha256:
            judge.mark_unjudgeable(va, "synth-domain-pin-mismatch")
            return 0, False
        if domain.case_count() <= 0:
            judge.mark_unjudgeable(va, "synth-domain-empty")
            return 0, False
        synth_block = {
            "evidence_class": synth_domain.EVIDENCE_CLASS,
            "generator_version": domain.document["generator_version"],
            "domain_sha256": domain.sha256,
            "case_count": domain.case_count(),
            "case_stream_sha256": synth_stream_sha256(
                synth_regenerated_digests(domain, args.seed, policy)
            ),
            "executed_stream_sha256": None,
            "artifact": artifact_ref,
        }
        judge.evidence[va].synth_domain = synth_block
        synth_executed = []
        registry = (*registry, synth_domain.provider_for(domain))
    validate_registry(registry)
    selected_providers = synth_selection(
        getattr(args, "fixture_provider_selection", None), synth_enabled
    )
    code_binding = None
    custom_binding = None
    if args.edge_cases and not getattr(args, "no_fixture_providers", False):
        from .scoped_fixture_binding import Binding

        chosen = getattr(args, "fixture_provider_selection", None)
        for provider in registry:
            if chosen is not None and provider.label not in chosen:
                continue
            for domain in provider.custom_contracts:
                if domain.entry.va != va:
                    continue
                if custom_binding is not None:
                    raise ValueError("multiple custom fixture authorities for root")
                if (
                    domain.entry != judge.evidence[va].entry
                    or domain.size != size
                    or policy != domain.physical.policy
                    or image != domain.physical.source.image
                    or candidate.stub_plan is not None
                    or candidate.static_tables
                    or candidate.x87
                    or args.live_call_closure
                ):
                    raise ValueError("actual custom root/ABI/source/policy/control differs")
                custom_binding = Binding(
                    domain,
                    provider,
                    args.seed,
                    getattr(args, "functions", None),
                    overrides=getattr(args, "overrides", None),
                    additions=getattr(args, "additions", None),
                )
                judge.evidence[va].scoped_fixture_contract = domain.document()
                if not custom_binding.document()["validated"]:
                    judge.mark_unjudgeable(va, "custom-fixture-runtime-unvalidated")
    # Graph memory is a default additive fixture, never a hidden live-state patch.
    # Authenticate source/maps and collision constraints before even the random phase.
    if args.edge_cases and not getattr(args, "no_fixture_providers", False):
        chosen = getattr(args, "fixture_provider_selection", None)
        for provider in registry:
            if (
                provider.authenticate is not None
                and provider.case_count(va)
                and (chosen is None or provider.label in chosen)
            ):
                if custom_binding is not None:
                    raise ValueError("mixed named/custom fixture authority for root")
                contract = provider.authenticate(
                    va,
                    size,
                    image,
                    seed=args.seed,
                    policy=policy,
                    functions=getattr(args, "functions", None),
                    overrides=getattr(args, "overrides", None),
                    additions=getattr(args, "additions", None),
                )
                if provider.bind is not None:
                    code_binding = provider.bind(
                        va,
                        image,
                        getattr(args, "functions", None),
                        overrides=getattr(args, "overrides", None),
                        additions=getattr(args, "additions", None),
                    )
                    if (
                        code_binding is not None
                        and not code_binding.document(args.seed)["validated"]
                    ):
                        judge.mark_unjudgeable(va, "named-global-code-safety-runtime-unvalidated")
                judge.evidence[va].named_global_object_contract = contract
    entry = judge.evidence[va].entry
    plan = candidate.stub_plan
    register_args = register_arguments(entry)
    register_names = register_argument_names(entry)
    ran = 0
    seeds: list[tuple[Case, tuple[tuple[int, int, int], ...]]] = []
    last_run: list[tuple[Case, tuple[tuple[int, int, int], ...], bool]] = []

    def execute(case: Case, kind: str) -> bool:
        """Run and judge one case. True when the dispatch table did not reach the adapter."""
        nonlocal ran
        ran += 1
        if synth_executed is not None and kind == synth_domain.LABEL:
            # The case as generated, before any live-state or fp attachment.
            synth_executed.append(synth_domain.case_digest(case))
        if candidate.x87 or args.live_call_closure:
            case = attach_fp_stack(case, vary_precision=va in PRECISION_SENSITIVE_VAS)
            if args.live_call_closure:
                # The T419 state-path proof is specifically the retail masked-control path.
                # Keep randomized stacks while holding the control word at the captured
                # default instead of silently mixing in alternate exception masks.
                case = replace(case, fp_control=0x037F)
        if args.live_call_closure:
            case = _live_case_state(case, args)
        scalar_fp = getattr(args, "scalar_fp_tracker", None)
        if scalar_fp is not None:
            from .oracle import GUEST_LO, GUEST_SPAN
            from .vector_native import assert_host_control
            from .vector_scalar import prepare_case

            assert_host_control(scalar_fp.host)
            case = prepare_case(
                oracle, scalar_fp, case, plan, window=(GUEST_LO, GUEST_LO + GUEST_SPAN)
            )
        if custom_binding is not None:
            oracle_result, subject_result = run_case(
                oracle,
                subject,
                case,
                plan,
                scoped_fixture_binding=custom_binding,
                scoped_fixture_kind=kind,
            )
        elif candidate.static_tables:
            oracle_result, subject_result = run_case(
                oracle,
                subject,
                case,
                plan,
                static_tables=candidate.static_tables,
                code_binding=code_binding,
            )
        elif code_binding is not None:
            oracle_result, subject_result = run_case(
                oracle, subject, case, plan, code_binding=code_binding
            )
        else:
            oracle_result, subject_result = run_case(oracle, subject, case, plan)
        if scalar_fp is not None:
            scalar_fp.commit_case(oracle_result.faulted)
        if custom_binding is not None:
            judge.evidence[va].scoped_fixture_runtime = custom_binding.document()
        if code_binding is not None:
            judge.evidence[va].named_global_code_safety = code_binding.document(args.seed)
        result = judge.judge(
            case, oracle_result, subject_result, body_insns=candidate.body_insns, kind=kind
        )
        if getattr(args, "guarded_jumps", False) and result.outcome is Outcome.AGREE:
            for event in oracle.arm_events:
                args.guarded_events[event] = args.guarded_events.get(event, 0) + 1
        guarded_session = getattr(args, "guarded_session", None)
        if guarded_session is not None:
            guarded_session.observe(
                case, oracle, oracle_result, subject_result, result.outcome is Outcome.AGREE
            )
        writer.write_case(result)
        if result.outcome in DIVERGENT_OUTCOMES:
            print(
                f"{result.outcome} {va:#010x} case {result.index}: {result.diagnosis}",
                file=sys.stderr,
            )
        last_run[:] = [
            (case, oracle_result.reach.loads, oracle_result.faulted or subject_result.faulted)
        ]
        if kind == "random" and oracle_result.reach.loads and len(seeds) < MAX_SEEDS:
            if not oracle_result.faulted and not subject_result.faulted:
                seeds.append((case, oracle_result.reach.loads))
        if subject_result.fault == SUBJECT_FAULT_NOT_REPLACED:
            # The dispatch table does not reach the adapter, so every verdict for this
            # address would be the lifted body's. The cause is wiring, not code.
            print(
                f"NOT-REPLACED {va:#010x}: the dispatch table returned the lifted body, not "
                "the registered adapter. The weak override did not take effect here.",
                file=sys.stderr,
            )
            return True
        return False

    for offset in range(args.cases_per_function):
        if execute(make_case(args.seed, start_index + offset, va, size, policy=policy), "random"):
            return ran, True
    if not args.edge_cases:
        return ran, False

    selectors = edge_selectors(register_args + entry.stack_args, va=entry.va)
    for ordinal, selector in enumerate(selectors):
        edge_case = make_edge_case(
            args.seed,
            ordinal,
            va,
            size,
            register_args=register_args,
            register_names=register_names,
            stack_args=entry.stack_args,
            selector=selector,
            policy=policy,
        )
        if execute(edge_case, "edge"):
            return ran, True

    def run_fixtures(after_feedback: bool) -> bool:
        for label, fixture in iter_provider_cases(
            args.seed,
            va,
            size,
            policy=policy,
            vector=args.live_vector_state,
            disabled=getattr(args, "no_fixture_providers", False),
            selected=selected_providers,
            after_feedback=after_feedback,
            registry=registry,
        ):
            if execute(fixture, label):
                return True
        return False

    if run_fixtures(False):
        return ran, True
    if synth_executed is not None and synth_block is not None:
        synth_block["executed_stream_sha256"] = synth_stream_sha256(synth_executed)
        if synth_block["executed_stream_sha256"] != synth_block["case_stream_sha256"]:
            judge.mark_unjudgeable(va, "synth-domain-stream-mismatch")
            return ran, False

    pool = value_pool(body_immediates(image.code_at(va, size), va))
    sweep_v2 = bool(
        getattr(args, "guarded_jumps", False) and getattr(args, "guarded_sweep_version", 1) >= 2
    )
    sweep_v3 = bool(sweep_v2 and args.guarded_sweep_version >= 3)
    derived: list[tuple[Case, tuple[tuple[int, int, int], ...]]] = []
    reject_value = None
    if sweep_v2:
        # T1576 batch C (R2): no text or guarded-table address becomes a feedback value, so a
        # store root never receives a code pointer as a destination (docs batch-c-predeclared).
        reject_ranges = [(TEXT_LO, TEXT_HI)] + [
            (address, address + len(data))
            for table in candidate.static_tables
            if isinstance(table, GuardedJumpTable)
            for address, data in table.regions
        ]

        def reject_value(value: int) -> bool:
            return any(low <= value < high for low, high in reject_ranges)

        pool = tuple(v for v in pool if not reject_value(v))
    first_pass = seeds[:SEED_CASES]
    per_seed = max(1, MAX_VARIANTS // max(1, len(first_pass)))
    ordinal = 0
    passes = [(first_pass, per_seed)]

    def run_feedback(
        batch: list[tuple[Case, tuple[tuple[int, int, int], ...]]],
        limit: int,
        collect: bool = False,
    ) -> bool:
        nonlocal ordinal
        for seed_case, loads in batch:
            for variant in feedback_variants(
                seed_case,
                loads,
                pool,
                register_args=register_args,
                register_names=register_names,
                stack_args=entry.stack_args,
                limit=limit,
                reject=reject_value,
            ):
                index = FEEDBACK_INDEX_BASE + (va << 11) + ordinal
                ordinal += 1
                if execute(replace(variant, index=index), "feedback"):
                    return True
                if collect and sweep_v3 and len(derived) < MAX_DERIVED_SEEDS and last_run:
                    ran_case, ran_loads, faulted = last_run[0]
                    fresh = derived_seed_locations(
                        loads,
                        ran_loads,
                        ran_case.esp,
                        (TEXT_LO, TEXT_HI),
                        FRAME_BYTES,
                        faulted=faulted,
                    )
                    if fresh:
                        derived.append((ran_case, ran_loads))
        return False

    for batch, limit in passes:
        if run_feedback(batch, limit, collect=True):
            return ran, True
    for derived_seed in derived:
        if run_feedback([derived_seed], per_seed * DERIVED_LIMIT_FACTOR):
            return ran, True

    def run_guard_sweep(bound: int) -> bool:
        """T1576: sweep each loaded dword of the first seeds over 0..bound+2, the range of the
        ORIGINAL table (index bound, one past it, a further one), so every slot and the default
        are offered to a table whose index comes from guest memory. Fixtures come from the
        original bounds, never from the replacement's switch."""
        from .feedback import TEXT_HI, TEXT_LO

        sweep = 0
        for seed_case, loads in seeds[:2]:
            locations: list[int] = []
            for address, size, _value in loads:
                # T1576 batch A: the return-address dword [entry ESP] and everything below it
                # (the run's own pushes) are not inputs: patching the return address makes the
                # ORIGINAL fault on its `ret` while a replacement adapter returns cleanly, a
                # false DISAGREE (measured on 0x3DFA0, whose first four loads include it).
                if (
                    size == 4
                    and not TEXT_LO <= address < TEXT_HI
                    and address >= seed_case.esp + 4
                    and address not in locations
                ):
                    locations.append(address)
            for address in locations[:4]:
                for value in range(bound + 3):
                    variant = replace(
                        seed_case,
                        patches=(*seed_case.patches, (address, value.to_bytes(4, "little"))),
                        index=GUARD_SWEEP_INDEX_BASE + (va << 11) + sweep,
                    )
                    sweep += 1
                    if execute(variant, "table"):
                        return True
        return False

    root_tables = [
        t for t in candidate.static_tables if isinstance(t, GuardedJumpTable) and t.body_va == va
    ]
    root_bounds = [t.bound for t in root_tables]

    def run_derived_sweeps() -> tuple[bool, bool]:
        """T1576 batch C (R1, R3): sweep inputs from the ORIGINAL's dispatch index source and
        tail condition source. Returns (stopped, table_sweep_covered_every_table)."""
        from . import dispatch_source as source_rules

        document: dict[str, object] = {
            "version": args.guarded_sweep_version,
            "derived_seeds": len(derived),
            "rules": "docs/evidence/t1576/batch-c-predeclared.md",
            "tables": [],
            "tails": [],
        }
        args.guarded_sweep_document = document
        insns = source_rules.decode(image.code_at(va, size), va)
        source_checks = source_rules.V3_CHECKS if sweep_v3 else source_rules.DEFAULT_CHECKS
        sweeps: list[tuple[str, source_rules.Source, list[int]]] = []
        covered = True
        for table in root_tables:
            where = source_rules.dispatch_comparison(insns, table.guard_site)
            entry: dict[str, object] = {"site": f"0x{table.site:08x}", "bound": table.bound}
            if isinstance(where, source_rules.Refusal):
                entry.update(where.document())
                covered = False
            else:
                found = source_rules.derive_source(
                    insns, where[0], where[1], ranges=reject_ranges, checks=source_checks
                )
                if isinstance(found, source_rules.Refusal):
                    entry.update(found.document())
                    covered = False
                else:
                    pairs = source_rules.index_values(found, table.bound)
                    entry["source"] = found.document()
                    entry["values"] = len(pairs)
                    sweeps.append(("table", found, [raw for _index, raw in pairs]))
            document["tables"].append(entry)  # type: ignore[attr-defined]
        plans = source_rules.tail_condition_plans(
            insns, va, size, ranges=reject_ranges, checks=source_checks
        )
        for plan in plans:
            found_source = plan.get("_source")
            if isinstance(found_source, source_rules.Source):
                sweeps.append(("tail", found_source, list(plan["raw_values"])))  # type: ignore[call-overload]
        document["tails"] = source_rules.public_plan(plans)
        sweep = 0
        for _label, found_source, raws in sweeps:
            for seed_case, _loads in seeds[:2]:
                address = found_source.resolve(seed_case)
                if address is None:
                    continue
                for raw in raws:
                    variant = replace(
                        seed_case,
                        patches=(
                            *seed_case.patches,
                            (address, raw.to_bytes(found_source.width, "little")),
                        ),
                        index=GUARD_SWEEP_INDEX_BASE + (1 << 40) + (va << 11) + sweep,
                    )
                    sweep += 1
                    if execute(variant, "table"):
                        return True, covered
        document["cases"] = sweep
        return False, covered

    if sweep_v2:
        stopped, covered = run_derived_sweeps()
        if stopped:
            return ran, True
        if root_bounds and not covered and run_guard_sweep(max(root_bounds)):
            return ran, True
    elif (
        getattr(args, "guarded_jumps", False) and root_bounds and run_guard_sweep(max(root_bounds))
    ):
        return ran, True
    # Top up. A predicate that is rarely true (an equality against a loaded word) leaves a
    # function with few cases a do-nothing replacement would fail, so the later seeds are
    # replayed, a few variants each, until there are enough or the seeds run out.
    for seed in seeds[SEED_CASES:]:
        if judge.evidence[va].discriminating_cases >= TARGET_DISCRIMINATING:
            break
        if run_feedback([seed], TOP_UP_VARIANTS_PER_SEED):
            return ran, True
    if run_fixtures(True):
        return ran, True
    return ran, False


def subject_sidecar(subject: Path) -> dict[str, str]:
    """The build facts `build_subject.sh` wrote beside the subject, for the proof file.

    Only a fixed set of keys is carried. The sidecar is advisory (the READY line is the
    authority for provenance), so its absence is not an error, but the optimisation level
    the hand code was compiled at is worth having beside a proof.
    """
    sidecar = Path(f"{subject}.provenance.txt")
    if not sidecar.exists():
        return {}
    keep = {"repl_cflags", "built_at", "extra_objs"}
    found: dict[str, str] = {}
    for line in sidecar.read_text(encoding="utf-8").splitlines():
        key, _, value = line.partition("=")
        if key in keep:
            found[key] = value
    return found


def prepare_replacement(
    subject: SubjectProcess, args: argparse.Namespace
) -> list[ManifestEntry] | int:
    """Check the subject is a fresh replacement subject and read its registry.

    Returns the registered entries, or an exit code after printing why not. Three things
    are refused, each of which would otherwise produce a confident, meaningless AGREE:

    * no replacement linked: the run would test the lifted code against the oracle and
      call it a proof of hand-written code.
    * built from different hand code than `--replace-dir` now holds: a proof of
      yesterday's source.
    * (checked per case, in the main loop) an address whose dispatch table entry is not
      its adapter.
    """
    if subject.provenance.repl_count == 0:
        print(
            "ABORT: --replacement needs a subject with hand-written replacements linked in, "
            "and this one reports none. Build it with `tools/replace build-subject`.",
            file=sys.stderr,
        )
        return EXIT_NO_REPLACEMENTS
    current = tree_digest(args.replace_dir)
    if subject.provenance.repl_sha != short_sha(current):
        print(
            f"ABORT: STALE REPLACEMENT SUBJECT. It was built from {args.replace_dir} as that "
            f"directory stood at repl_sha={subject.provenance.repl_sha}, but it now digests "
            f"to {short_sha(current)}. Rebuild with `tools/replace build-subject`.",
            file=sys.stderr,
        )
        return EXIT_STALE_REPLACEMENT
    entries = parse_listrepl(subject.list_replacements())
    if len(entries) != subject.provenance.repl_count:
        print(
            f"ABORT: the subject reported repl={subject.provenance.repl_count} at READY but "
            f"listed {len(entries)} replacement(s).",
            file=sys.stderr,
        )
        return EXIT_NO_REPLACEMENTS
    # A wire entry with no optional input metadata is valid legacy. Bind that
    # interpretation to the current authenticated sources before generating cases.
    problems = cross_check(entries, scan_directory(args.replace_dir))
    if problems:
        print(
            "ABORT: linked replacement/source contracts disagree: " + "; ".join(problems),
            file=sys.stderr,
        )
        return EXIT_STALE_REPLACEMENT
    args.manifest_digest = manifest_sha(entries, current)
    return entries


def render_replacement_report(
    judge: ReplacementJudge,
    reach: dict[int, tuple[float, int, bool]],
    args: argparse.Namespace,
) -> str:
    """The per-function table printed after a replacement run."""
    lines = [
        "",
        "REPLACEMENT EVIDENCE",
        f"  sampling depth   {args.cases_per_function} random cases per function"
        + (" plus argument edge cases" if args.edge_cases else ", NO edge cases"),
        "  registers ignored are each function's declared scratch set; see the proof JSON",
        "",
        f"  {'va':<12}{'cases':>7}{'verd':>6}{'agree':>7}{'DIS':>5}{'sfault':>7}"
        f"{'ofault':>7}{'cover':>7}{'null':>6}{'discr':>6}  confirmed  name",
    ]
    for va in sorted(judge.evidence):
        evidence = judge.evidence[va]
        coverage = reach.get(va, (0.0, 0, True))[0]
        note = f"  UNJUDGEABLE: {evidence.unjudgeable}" if evidence.unjudgeable else ""
        lines.append(
            f"  {va:#010x}  {evidence.cases:>5}{evidence.verdicts:>6}{evidence.agree:>7}"
            f"{evidence.disagree:>5}{evidence.subject_faulted:>7}{evidence.oracle_faulted:>7}"
            f"{coverage:>7.0%}{evidence.null_agree_rate:>6.0%}{evidence.discriminating_cases:>6}"
            f"  {'yes' if evidence.replaced_confirmed else 'NO':<9}  {evidence.entry.name}{note}"
        )
    lines.append(f"  proof written to {args.proof_out}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        available = tuple(provider.label for provider in REGISTRY)
        if args.synth_domain:
            available += (synth_domain.LABEL,)
        args.fixture_provider_selection = synth_selection(
            select_labels(
                getattr(args, "fixture_provider", None),
                available,
                disabled=getattr(args, "no_fixture_providers", False),
            ),
            args.synth_domain,
        )
    except ValueError as error:
        print(f"ABORT: {error}", file=sys.stderr)
        return 2
    if args.synth_domain and (
        not args.replacement or not args.edge_cases or args.no_fixture_providers
    ):
        print(
            "ABORT: --synth-domain requires --replacement with edge cases and fixture "
            "providers enabled",
            file=sys.stderr,
        )
        return 2
    if args.synth_domain_dir is not None and not args.synth_domain:
        print("ABORT: --synth-domain-dir requires --synth-domain", file=sys.stderr)
        return 2
    if args.synth_domain_version != "t1773-synth-v1" and not args.synth_domain:
        print("ABORT: --synth-domain-version requires --synth-domain", file=sys.stderr)
        return 2
    if args.synth_domain_analysis_table is not None and not args.synth_domain:
        print("ABORT: --synth-domain-analysis-table requires --synth-domain", file=sys.stderr)
        return 2
    synth_pins = args.synth_domain_pin or []
    if synth_pins and not args.synth_domain:
        print("ABORT: --synth-domain-pin requires --synth-domain", file=sys.stderr)
        return 2
    if len({pin_va for pin_va, _ in synth_pins}) != len(synth_pins):
        print("ABORT: duplicate --synth-domain-pin for one address", file=sys.stderr)
        return 2
    started = time.monotonic()
    args.manifest_digest = ""
    if args.static_jump_tables and (
        not args.replacement or args.only_va is None or args.stub_calls or args.live_call_closure
    ):
        print(
            "ABORT: --static-jump-tables requires one call-free --replacement --only-va",
            file=sys.stderr,
        )
        return 2
    if args.guarded_jumps:
        from tools.replace import guarded_jump_contract as guarded_contract

        if (
            not guarded_contract.enabled()
            or not args.replacement
            or args.only_va is None
            or args.static_jump_tables
            or args.witness_subject is None
            or args.witness_draft is None
        ):
            print(
                "ABORT: --guarded-jumps needs the selected guarded-jump contract, one "
                "--replacement --only-va, no --static-jump-tables and the witness subject/draft",
                file=sys.stderr,
            )
            return 2
    if args.live_call_closure and args.only_va is None:
        print(
            "ABORT: --live-call-closure requires exactly one --only-va",
            file=sys.stderr,
        )
        return 2
    if args.live_vector_state and not args.live_call_closure:
        print("ABORT: --live-vector-state requires --live-call-closure", file=sys.stderr)
        return 2
    if args.live_call_boundary and not args.live_call_closure:
        print("ABORT: --live-call-boundary requires --live-call-closure", file=sys.stderr)
        return 2
    if _fp_scalar(args) and not (
        args.live_vector_state
        and args.replacement
        and not args.static_jump_tables
        and args.fp_build_record is not None
    ):
        print(
            "ABORT: --vector-mode fp-scalar-v1 requires --live-vector-state --replacement, "
            "no static jump tables and a --fp-build-record",
            file=sys.stderr,
        )
        return 2
    if (args.live_memory_dword or args.live_stack_dword) and not args.live_call_closure:
        print("ABORT: live input patches require --live-call-closure", file=sys.stderr)
        return 2
    if args.cases_per_function is None:
        args.cases_per_function = (
            DEFAULT_REPLACEMENT_CASES if args.replacement else DEFAULT_CASES_PER_FUNCTION
        )

    # The flags mode is refused in combinations that could never carry flags, rather
    # than silently degraded: a replacement publishes nothing, and the legacy stream
    # exists precisely to be replayed unchanged.
    if args.compare_eflags and args.replacement:
        print(
            "ABORT: --compare-eflags judges the lifted tree; --replacement judges hand "
            "code, which publishes no flags. Run them separately.",
            file=sys.stderr,
        )
        return EXIT_NO_EFLAGS
    if args.compare_eflags and args.legacy_seeding:
        print(
            "ABORT: --compare-eflags selects the flags-adversarial input policy, and "
            "--legacy-seeding exists to replay the historic stream exactly. The two "
            "cannot both hold.",
            file=sys.stderr,
        )
        return EXIT_NO_EFLAGS

    # One policy object drives both the per-case inputs AND the arena baked into the shared
    # image, so the two cannot disagree about what the guest memory looks like.
    if args.legacy_seeding:
        policy = LEGACY_SEEDING
    elif args.compare_eflags:
        policy = FLAGS_ADVERSARIAL_SEEDING
    else:
        policy = SeedPolicy()

    print(f"building guest image from {args.xbe}", file=sys.stderr)
    image: GuestImage = build_guest_image(args.xbe, policy=policy)
    if args.live_call_closure:
        offset = 0x0054E0B8 - image.base
        state = int.from_bytes(image.data[offset : offset + 4], "little")
        if state != 0x2694:
            print(
                f"ABORT: live-call proof expects retail __except1 state 0x0054E0B8=0x2694, "
                f"found 0x{state:08x}",
                file=sys.stderr,
            )
            return 2
    if not (args.reuse_image and args.image.exists()):
        written = write_image_file(image, args.image)
        print(f"wrote {written:,} bytes of guest image to {args.image}", file=sys.stderr)

    corrected = load_function_table(args.functions, args.overrides, args.additions)
    functions = [(f.entry_va, f.size_bytes) for f in corrected if not f.is_thunk]
    oracle = UnicornOracle(image.data, max_insns=args.max_insns, record_loads=args.replacement)
    stopped_early = False

    # The subject is started BEFORE the writer, because the writer stamps the subject's
    # provenance into the CSV header row and every row after it, so it cannot be opened
    # until the subject has said what it was built from.
    if args.guarded_jumps:
        os.environ["HARNESS_FAULT_ADDR"] = "1"  # driver prints FAULTADDR (fault parity)
    with SubjectProcess(args.subject, args.image, case_timeout=args.case_timeout) as subject:
        mismatch = shipped_mismatch(subject.provenance, args.shipped_gen_dir)
        if mismatch is not None and args.allow_foreign_subject is None:
            print(
                f"ABORT: {mismatch}\n"
                "       A result whose provenance nobody checked is how '0 DISAGREE' became "
                "'384 DISAGREE, a real lifter defect' became 'the defect was already "
                "fixed'.\n"
                "       Rebuild with tools/harness/build_subject.sh, or re-run with\n"
                "       --allow-foreign-subject 'why this tree is the right one to measure'.",
                file=sys.stderr,
            )
            return 4
        subject_note = None
        if mismatch is not None:
            # Printed AND carried into the summary. A warning on stderr is exactly what
            # got lost in a 355 KB run log last time.
            subject_note = f"{args.allow_foreign_subject} [{mismatch}]"
            print(
                f"WARNING: {mismatch}\n         permitted: {args.allow_foreign_subject}",
                file=sys.stderr,
            )
        print(f"subject provenance: {subject.provenance.describe()}", file=sys.stderr)

        # Attested by the subject's own READY line (build_subject.sh derives it from
        # the generated header), never by a flag a human sets: a non-publishing
        # subject in this mode would score every case unpublished and verify no flag.
        if args.compare_eflags and not subject.supports_eflags:
            print(
                "ABORT: --compare-eflags needs a subject built from a --publish-eflags "
                "lift, and this one advertises eflags=0 at READY.\n"
                "       Lift with: python -m tools.lift run <xbe> --publish-eflags "
                "--out-dir <scratch>\n"
                "       then rebuild the objects and subject from that tree.",
                file=sys.stderr,
            )
            return EXIT_NO_EFLAGS

        suppressed = frozenset()
        if not args.replacement:
            try:
                suppressed = suppressed_manual_entries(
                    subject.provenance.gen_dir, subject.provenance.tree_sha
                )
            except (OSError, ValueError) as error:
                print(
                    f"manual-body exclusions unavailable: {error}; entries remain in scope",
                    file=sys.stderr,
                )
            if suppressed:
                print(
                    f"{len(suppressed)} verified manual dispatch trampolines; "
                    "their entry bodies are outside lifted-only scope",
                    file=sys.stderr,
                )
        selection = select(
            functions,
            image.code_at,
            min_size=args.min_size,
            max_size=args.max_size,
            allow_calls=args.stub_calls or args.live_call_closure,
            # A replacement run is judged against the hardware, so x87 bodies are in
            # scope there. Lifted-only runs keep skipping them (double versus extended).
            allow_x87=args.replacement or args.live_call_closure,
            suppressed_entries=suppressed,
            vector_entries=frozenset({args.only_va}) if args.live_vector_state else frozenset(),
            vector_mode=FP_SCALAR_MODE if _fp_scalar(args) else "",
            allow_inbody_jumps=args.replacement or args.live_call_closure,
            allow_static_tables=args.static_jump_tables,
            allow_guarded_jumps=args.guarded_jumps,
            call_functions=[(f.entry_va, f.size_bytes) for f in corrected]
            if args.live_call_closure
            else None,
        )
        print(
            f"{selection.total:,} functions considered, {len(selection.candidates):,} in scope, "
            f"{len(selection.skipped):,} skipped",
            file=sys.stderr,
        )

        chosen = choose_functions(selection, args)
        if not chosen:
            print("no functions in scope to execute", file=sys.stderr)
            return 2

        live_closure_document: dict[str, object] | None = None
        if args.live_call_closure and not args.replacement:
            try:
                live_closure, live_closure_document = build_live_closure(
                    args, image, corrected, functions, chosen
                )
            except LiveClosureError as error:
                print(f"ABORT: live-call closure: {error}", file=sys.stderr)
                return 2
            chosen = [replace(c, stub_plan=None) if c.va == args.only_va else c for c in chosen]
            print(
                f"live call closure 0x{args.only_va:08x}: {len(live_closure.nodes)} "
                f"fingerprinted nodes, {len(args.live_call_boundary)} explicit boundary node(s)",
                file=sys.stderr,
            )

        entries: list[ManifestEntry] = []
        live_closure: LiveCallClosure | None = None
        if args.replacement:
            prepared = prepare_replacement(subject, args)
            if isinstance(prepared, int):
                return prepared
            entries = prepared
            wanted = {entry.va for entry in entries}
            in_scope = {candidate.va for candidate in selection.candidates}
            chosen = [candidate for candidate in chosen if candidate.va in wanted]
            if args.live_call_closure:
                try:
                    live_closure, live_closure_document = build_live_closure(
                        args, image, corrected, functions, chosen
                    )
                except LiveClosureError as error:
                    print(f"ABORT: live-call closure: {error}", file=sys.stderr)
                    return 2
                chosen = [replace(c, stub_plan=None) if c.va == args.only_va else c for c in chosen]
                if args.guarded_jumps:
                    chosen = [
                        replace(c, static_tables=_with_node_tables(c, live_closure, image))
                        if c.va == args.only_va
                        else c
                        for c in chosen
                    ]
                print(
                    f"live call closure 0x{args.only_va:08x}: {len(live_closure.nodes)} "
                    f"fingerprinted nodes, "
                    f"{len(args.live_call_boundary)} explicit boundary node(s)",
                    file=sys.stderr,
                )
                if _fp_scalar(args):
                    try:
                        install_scalar_fp(args, image, live_closure, oracle)
                    except (OSError, ValueError, RuntimeError) as error:
                        print(f"ABORT: fp-scalar-v1: {error}", file=sys.stderr)
                        return 2
            skipped_reasons = {skip.va: skip.reason for skip in selection.skipped}
            judge = ReplacementJudge(entries, require_fault_parity=args.guarded_jumps)
            for entry in entries:
                if args.only_va is not None and entry.va != args.only_va:
                    judge.mark_unjudgeable(entry.va, "not run: --only-va selected another address")
                elif entry.va not in in_scope:
                    judge.mark_unjudgeable(
                        entry.va,
                        skipped_reasons.get(entry.va, "not a function entry in the functions CSV"),
                    )
            for candidate in list(chosen):
                if candidate.stub_plan is not None and candidate.stub_plan.sites:
                    judge.mark_unjudgeable(
                        candidate.va,
                        "the original calls other functions, which a replacement cannot be "
                        "stubbed against",
                    )
                    chosen.remove(candidate)
            for entry in entries:
                reason = judge.evidence[entry.va].unjudgeable
                if reason:
                    print(f"UNJUDGEABLE {entry.va:#010x} {entry.name}: {reason}", file=sys.stderr)
            if not chosen:
                print("no registered replacement can be judged by this harness", file=sys.stderr)

        # A stubbed selection against a subject whose objects do not intercept calls
        # would compare a stubbed oracle against a subject that ran the real callees,
        # and report every difference as a lifter defect. The subject's own READY line
        # says whether it can stub, and build_subject.sh derives that from the objects by
        # inspection, so this cannot be satisfied by setting a flag.
        # A --publish-eflags tree cannot carry the verified stack-probe passthrough:
        # the publish statements change the lifted probe body, so its pinned SHA no
        # longer proves it. The two passthrough callers are dropped with a NAMED skip
        # rather than aborting the whole flags run -- or, outside the flags mode, the
        # existing abort below still protects a stale ordinary subject.
        eflags_dropped: list[SkippedFunction] = []
        if args.compare_eflags and not subject.supports_stackprobe:
            kept: list[Candidate] = []
            for c in chosen:
                if c.stub_plan is not None and c.stub_plan.passthrough_table:
                    eflags_dropped.append(
                        SkippedFunction(
                            va=c.va,
                            size=c.size,
                            reason="stackprobe-passthrough-unavailable-on-eflags-subject",
                        )
                    )
                else:
                    kept.append(c)
            if eflags_dropped:
                print(
                    f"{len(eflags_dropped)} stack-probe passthrough caller(s) skipped by "
                    "name: a --publish-eflags tree cannot carry the verified probe",
                    file=sys.stderr,
                )
            chosen = kept

        stubbed_functions = sum(1 for c in chosen if c.stub_plan and c.stub_plan.sites)
        if (
            any(c.stub_plan and c.stub_plan.passthrough_table for c in chosen)
            and not subject.supports_stackprobe
        ):
            print(
                "ABORT: selected stack-probe passthrough requires a freshly verified subject",
                file=sys.stderr,
            )
            return 4
        if stubbed_functions and not subject.supports_stubs:
            print(
                f"ABORT: {stubbed_functions:,} selected function(s) need callee stubbing "
                f"but {args.subject} was not built from stub-enabled objects.\n"
                "       Build them with tools/harness/build_stub_objects.sh and relink, "
                "or re-run with --no-stub-calls to keep call-bearing functions out of "
                "scope.",
                file=sys.stderr,
            )
            return 3

        args.guarded_session = None
        args.guarded_events = {}
        if args.guarded_jumps:
            oracle.tail_sites = _node_tail_sites(live_closure, args.only_va, image)
            oracle.watch_entries = frozenset(
                node.va
                for node in (live_closure.nodes if live_closure else ())
                if node.va != args.only_va and (node.tails or node.tables)
            )
            session_error = _start_guarded_session(args, chosen, image, functions, oracle)
            if session_error is not None:
                print(f"ABORT: {session_error}", file=sys.stderr)
                return 2

        with ResultWriter(
            args.out,
            subject.provenance,
            flush_every=args.flush_every,
            subject_kind="replacement" if args.replacement else "lifted",
            sampling=args.cases_per_function,
        ) as writer:
            # Skips are recorded FIRST, so even a run killed in its first second leaves
            # behind a complete account of what was excluded and why.
            for skipped in selection.skipped:
                writer.write_skip(skipped, args.seed)
            for skipped in eflags_dropped:
                writer.write_skip(skipped, args.seed)

            index = 0
            done = 0
            aborted_not_replaced = False
            for candidate in chosen:
                va, size = candidate.va, candidate.size
                # The plan is handed over even when it has no call sites, so that strict
                # mode stays on and an unexpected call -- one the selection failed to see
                # -- voids its case loudly instead of quietly running the real callee.
                # Whether the function counts as STUBBED for reporting is a different
                # question: a call-free function is not stubbed evidence just because
                # strict mode was on.
                plan = candidate.stub_plan
                stubbed = bool(plan is not None and plan.table)
                if args.max_seconds is not None and time.monotonic() - started > args.max_seconds:
                    stopped_early = True
                    break
                if args.replacement:
                    ran, aborted_not_replaced = drive_replacement(
                        args,
                        policy,
                        image,
                        oracle,
                        subject,
                        judge,
                        writer,
                        candidate,
                        index,
                    )
                    index += args.cases_per_function
                    done += ran
                    if aborted_not_replaced:
                        break
                    continue
                for k in range(args.cases_per_function):
                    case = make_case(args.seed, index + k, va, size, policy=policy)
                    if args.live_call_closure:
                        case = attach_fp_stack(case)
                        case = replace(case, fp_control=0x037F)
                        case = _live_case_state(case, args)
                    done += 1
                    oracle_result, subject_result = run_case(oracle, subject, case, plan)
                    if args.compare_eflags:
                        flag_kwargs = {"flag_mask": LIFTER_MODELED_FLAGS}
                    else:
                        # The channel is strictly opt-in. A publish-capable subject
                        # driven WITHOUT --compare-eflags must produce the same
                        # verdicts as any other subject, so its flags are dropped
                        # before the comparison rather than quietly compared.
                        flag_kwargs = {}
                        if subject_result.flags is not None:
                            subject_result = replace(subject_result, flags=None, flags_mask=None)
                    result = compare(
                        case,
                        oracle_result,
                        subject_result,
                        stubbed=stubbed,
                        body_insns=candidate.body_insns,
                        delegation_ratio=plan.delegation_ratio if stubbed and plan else 0.0,
                        ignored_write_ranges=(
                            ((case.esp - DEAD_FRAME_BYTES, case.esp),)
                            if args.live_call_closure
                            else ()
                        ),
                        **flag_kwargs,
                    )
                    writer.write_case(result)
                    if result.outcome in DIVERGENT_OUTCOMES:
                        print(
                            f"{result.outcome} {va:#010x} case {result.index}: {result.diagnosis}",
                            file=sys.stderr,
                        )
                    if done % PROGRESS_EVERY == 0:
                        elapsed = time.monotonic() - started
                        print(
                            f"  {done:,} cases, {elapsed:,.0f} s, "
                            f"{done / max(elapsed, 1e-9):.1f} cases/s",
                            file=sys.stderr,
                        )
                index += args.cases_per_function

            # The directed memmove overlap probe runs as part of every normal lifted
            # invocation, inside the same writer so its cases land in the same CSV. A
            # probe that cannot run still produces a NAMED skip in the report, because
            # a check that silently does not run is indistinguishable from a pass.
            if args.replacement:
                probe_skip = "a --replacement run judges hand-written code, not lifted memmove"
            elif not args.memmove_probe:
                probe_skip = "disabled with --no-memmove-probe"
            elif args.only_va is not None and args.only_va != MEMMOVE_VA:
                probe_skip = f"--only-va {args.only_va:#010x} selected another address"
            elif stopped_early:
                probe_skip = "the --max-seconds budget ran out before the probe started"
            else:
                probe_skip = None

            def probe_run(case: Case) -> tuple[ExecResult, ExecResult]:
                # The probe verifies memmove's copy semantics; its contract predates
                # the EFLAGS channel and must not shift under --compare-eflags, so a
                # publish-capable subject's flags are dropped before its compare.
                oracle_result, subject_result = run_case(oracle, subject, case)
                if subject_result.flags is not None:
                    subject_result = replace(subject_result, flags=None, flags_mask=None)
                return oracle_result, subject_result

            probe_report = run_memmove_probe(
                functions,
                probe_run,
                seed=args.seed,
                write_case=writer.write_case,
                skip_reason=probe_skip,
            )

            tally = writer.tally
        restarts, timeouts, deaths = subject.restarts, subject.timeouts, subject.deaths
        provenance = subject.provenance

    guarded_failed = False
    if args.replacement:
        reach = {
            va: (entry.coverage, entry.body_insns, entry.near_vacuous)
            for va, entry in tally.reach.items()
        }
        scalar_fp_tracker = getattr(args, "scalar_fp_tracker", None)
        if scalar_fp_tracker is not None and live_closure_document is not None:
            live_closure_document["fp_scalar"] = scalar_fp_tracker.document()
        document = judge.document(
            manifest_sha=args.manifest_digest,
            seed=args.seed,
            cases_per_function=args.cases_per_function,
            edge_cases=args.edge_cases,
            subject={
                "gen_dir": provenance.gen_dir,
                "tree_sha": provenance.tree_sha,
                "repl_sha": provenance.repl_sha,
                **subject_sidecar(args.subject),
            },
            reach=reach,
            live_call_closure=live_closure_document,
            fixture_provider_selection=getattr(args, "fixture_provider_selection", None),
            synth_domain=args.synth_domain,
        )
        if args.static_jump_tables:
            document["static_jump_tables"] = {
                "contract": "bounded original intraprocedural tables; body/table identity guarded",
                "max_size": args.max_size,
                "tables": [
                    table.document() for candidate in chosen for table in candidate.static_tables
                ],
                "modified_identity": "oracle proof refusal; subject NOT-RUN; no verdict",
            }
        if args.guarded_jumps:
            from tools.replace import guarded_jump_contract as guarded_contract

            session = args.guarded_session
            session_document = session.document() if session is not None else None
            if session is not None:
                session.close()
            guarded_failed = bool(session_document is not None and not session_document["passed"])
            attached = guarded_contract.attach_schema3(
                document,
                arm_witness=session_document
                if session_document is not None
                else {"root_tables": 0, "passed": True, "note": "root has no jump table"},
                tail_target={
                    "sites": (session_document or {}).get("tail_sites", []),
                    "taken": (session_document or {}).get("tails_taken", []),
                },
                closure_guards=live_closure_document,
            )
            if attached:
                required = _closure_edges(chosen, live_closure, image, args.only_va)
                missing = sorted(set(required) - set(args.guarded_events))
                guarded_failed = guarded_failed or bool(missing)
                document["closure_edges"] = {
                    "required": [_render_event(event) for event in sorted(required)],
                    "missing": [_render_event(event) for event in missing],
                    "passed": not missing,
                    "rule": "every non-root closure tail edge and table slot/default executed "
                    "on at least one AGREE case (gate closure-edge-not-reached)",
                }
                if getattr(args, "guarded_sweep_document", None) is not None:
                    document["guarded_sweep"] = args.guarded_sweep_document
                document["table_sections"] = _table_sections(chosen, image, args.only_va)
                document["closure_events"] = [
                    {**_render_event(event), "agree_cases": count}
                    for event, count in sorted(args.guarded_events.items())
                ]
        write_proof(args.proof_out, document)
        print(render_replacement_report(judge, reach, args), file=sys.stderr)
    elif live_closure_document is not None:
        live_closure_document["results_csv"] = {
            "path": str(args.out),
            "sha256": hashlib.sha256(args.out.read_bytes()).hexdigest(),
        }
        closure_out = args.out.with_suffix(".closure.json")
        closure_out.parent.mkdir(parents=True, exist_ok=True)
        closure_out.write_text(
            json.dumps(live_closure_document, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(f"live call closure proof written to {closure_out}", file=sys.stderr)

    if stopped_early:
        print(
            f"\nSTOPPED EARLY at the {args.max_seconds:g} s budget. The counts below are "
            "exactly what ran; they are not scaled up to the full selection.",
            file=sys.stderr,
        )

    print(
        render_summary(
            tally,
            provenance=provenance,
            seed=args.seed,
            csv_path=args.out,
            functions_selected=len(chosen),
            restarts=restarts,
            timeouts=timeouts,
            deaths=deaths,
            flags_compared=args.compare_eflags,
            adversarial_inputs=args.compare_eflags,
            elapsed_seconds=time.monotonic() - started,
            subject_note=subject_note,
            cases_per_function=args.cases_per_function,
            live_call_closure=args.live_call_closure,
            static_jump_tables=args.static_jump_tables,
        )
    )
    print(probe_report.render())
    if args.replacement and aborted_not_replaced:
        return EXIT_NOT_REPLACED
    # The probe's model check can fail even when the two sides AGREE (both wrong the
    # same way), so it participates in the exit code in its own right.
    return 1 if tally.divergent_functions or probe_report.failures or guarded_failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
