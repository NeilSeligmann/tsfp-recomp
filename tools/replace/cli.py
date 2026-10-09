# SPDX-License-Identifier: GPL-3.0-or-later
"""Command line for the replacement pipeline.

    python3 -m tools.replace scan                      # list what src/game registers
    python3 -m tools.replace wire --out DIR            # the weak headers (CMake runs this)
    python3 -m tools.replace manifest                  # generated/replace/manifest.json
    python3 -m tools.replace build-subject             # a harness subject with src/game linked
    python3 -m tools.replace prove                     # build, run the harness, write proof.json
                                                       # (--only-va: out-dir/partial/ instead)
    python3 -m tools.replace audit                     # the caller scratch-register audit

All paths are relative to the repository root, which is the current directory.
`scan`, `wire` and `manifest` use only the standard library and a C compiler. `build-subject`
and `prove` need the project virtualenv (Unicorn and Capstone) and `tmp/oxm-extract`.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

from .build import BuildError, build_manifest_exe, compile_objects, prepare_baseline, run
from .codewrite_arbiter_contract import DEFAULT_NAME as DEFAULT_CODEWRITE_NAME
from .codewrite_arbiter_contract import ENV_SELECTED as CODEWRITE_ENV
from .codewrite_arbiter_contract import replace_enabled as codewrite_v2_enabled
from .manifest import cross_check, parse_manifest_output, write_manifest
from .scan import ScanError, scan_directory
from .wiring import WiringError, plan_wiring, write_wiring
from .x87_arbiter_contract import DEFAULT_NAME as DEFAULT_X87_NAME
from .x87_arbiter_contract import ENV_SELECTED as X87_ENV
from .x87_arbiter_contract import replace_enabled as x87_v2_enabled

DEFAULT_GAME_DIR = Path("src/game")
DEFAULT_GEN_DIR = Path("generated/lifted/gen")
DEFAULT_WORK_DIR = Path("tmp/replace")
DEFAULT_REPLACE_OUT = Path("generated/replace")
DEFAULT_XBE = Path("tmp/oxm-extract/retail/default.xbe")
DEFAULT_FUNCTIONS = Path("generated/retail/functions.csv")
HARNESS_DIR = Path("tools/harness")
SUBPROCESS_TIMEOUT_SECONDS = 3000


def add_common(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--game-dir", type=Path, default=DEFAULT_GAME_DIR, metavar="DIR")
    parser.add_argument("--gen-dir", type=Path, default=DEFAULT_GEN_DIR, metavar="DIR")
    parser.add_argument("--work-dir", type=Path, default=DEFAULT_WORK_DIR, metavar="DIR")
    parser.add_argument(
        "--base-dir",
        type=Path,
        default=None,
        metavar="DIR",
        help=(
            "the unmodified stub-object cache of the lifted tree; stale caches are preserved "
            "once in a fresh content-keyed directory if stale (default: <work-dir>/stub-base). "
            "Sharing one across work "
            "directories is what keeps a mutation run from recompiling 67 chunks each time"
        ),
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    scan = sub.add_parser("scan", help="list the GAME_REPLACE registrations in the sources")
    add_common(scan)

    wire = sub.add_parser("wire", help="write the weak headers for the lifted chunks")
    add_common(wire)
    wire.add_argument("--out", type=Path, required=True, metavar="DIR")

    manifest = sub.add_parser("manifest", help="dump the linked registry to manifest.json")
    add_common(manifest)
    manifest.add_argument("--out", type=Path, default=DEFAULT_REPLACE_OUT / "manifest.json")

    build = sub.add_parser("build-subject", help="build a harness subject with src/game linked")
    add_common(build)
    build.add_argument(
        "--opt-level",
        type=int,
        choices=(0, 1, 2, 3),
        default=2,
        help="gcc -O level for the hand code (the product's Release build is 3, Debug is 0)",
    )
    build.add_argument("--subject", type=Path, default=None, metavar="PATH")

    prove = sub.add_parser("prove", help="build the subject and run the harness on it")
    add_common(prove)
    prove.add_argument(
        "--opt-level",
        type=int,
        choices=(0, 1, 2, 3),
        default=2,
        help="gcc -O level for the hand code (the product's Release build is 3, Debug is 0)",
    )
    prove.add_argument("--xbe", type=Path, default=DEFAULT_XBE)
    prove.add_argument("--functions", type=Path, default=DEFAULT_FUNCTIONS)
    prove.add_argument(
        "--max-size",
        type=int,
        default=512,
        metavar="N",
        help="explicit selected harness size ceiling; default stays512",
    )
    prove.add_argument(
        "--static-jump-tables",
        action="store_true",
        help="opt in to original-validated bounded intraprocedural table proof",
    )
    prove.add_argument(
        "--min-size",
        type=int,
        default=1,
        metavar="N",
        help="harness size floor (default 1: a registered replacement is judged however small; "
        "the harness's own 8-byte floor would skip 6 and 7 byte leaves as size-out-of-range)",
    )
    prove.add_argument("--out-dir", type=Path, default=DEFAULT_REPLACE_OUT)
    prove.add_argument("--seed", type=int, default=20261001)
    prove.add_argument("--cases-per-function", type=int, default=None, metavar="N")
    prove.add_argument("--no-edge-cases", action="store_true")
    fixture_group = prove.add_mutually_exclusive_group()
    fixture_group.add_argument("--no-fixture-providers", action="store_true")
    fixture_group.add_argument("--fixture-provider", action="append", default=None, metavar="LABEL")
    prove.add_argument(
        "--synth-domain",
        action="store_true",
        help="T1773: add the synthesized-domain evidence class derived from the original bytes",
    )
    prove.add_argument(
        "--synth-domain-version",
        choices=("t1773-synth-v1", "t1773-synth-v2"),
        default="t1773-synth-v1",
    )
    prove.add_argument(
        "--synth-domain-pin",
        action="append",
        default=None,
        metavar="VA=SHA256",
        help="T1773: refuse the root unless its derived domain digest equals this pin",
    )
    prove.add_argument(
        "--only-va", default=None, metavar="HEX", help="judge only this registered address"
    )
    prove.add_argument(
        "--live-call-closure",
        action="store_true",
        help="opt in to live, stub-free proof of one selected direct-call closure",
    )
    prove.add_argument(
        "--live-vector-state",
        action="store_true",
        help="require raw all8-XMM/exact MXCSR for one complete selected live closure",
    )
    prove.add_argument(
        "--vector-mode",
        choices=("legacy", "fp-scalar-v1"),
        default="legacy",
        help="T1620: fp-scalar-v1 admits scalar float arithmetic (needs --live-vector-state)",
    )
    prove.add_argument(
        "--x87-contract",
        choices=("inherited-limited-x87-v1", "model-arbitrated-x87-v2"),
        default="inherited-limited-x87-v1",
        help="T1510: opt-in model-arbitrated-x87-v2 (needs --x87-arbiter-ack; refuses until the "
        "x87 replacement ABI exists). Default is the unchanged v1 contract",
    )
    prove.add_argument(
        "--x87-arbiter-ack",
        default=None,
        help="T1510: fingerprint of the x87 arbiter files; required by v2 and invalid with v1",
    )
    prove.add_argument(
        "--codewrite-contract",
        choices=("unicorn-per-case-reset-v1", "faithful-codewrite-arbiter-v2"),
        default="unicorn-per-case-reset-v1",
        help="T1508: opt-in faithful-codewrite-arbiter-v2 for roots whose original rewrites own "
        "code (needs --codewrite-arbiter-ack). Default is the unchanged v1 contract",
    )
    prove.add_argument(
        "--codewrite-arbiter-ack",
        default=None,
        help="T1508: fingerprint of the code-write arbiter files; required by v2, invalid with v1",
    )
    prove.add_argument(
        "--guarded-jump-contract",
        choices=("guarded-jump-schema3",),
        default=None,
        help="T1576: opt-in guarded jump tables / exact-entry tails (needs --guarded-jump-ack; "
        "off by default, schema3 receipts only for roots that use them)",
    )
    prove.add_argument(
        "--guarded-sweep-version",
        type=int,
        choices=(1, 2, 3),
        default=1,
        help="T1576 batch C: guarded sweep producer version (2 = derived sources, new contract "
        "version, one attempt per tuple)",
    )
    prove.add_argument(
        "--guarded-jump-ack",
        default=None,
        help="T1576: fingerprint of the guarded-jump guard files; required with the contract",
    )
    prove.add_argument(
        "--live-call-boundary",
        action="append",
        default=[],
        metavar="HEX",
        help="explicitly fingerprint a reachable but unproved closure boundary",
    )
    prove.add_argument("--live-memory-dword", action="append", default=[], metavar="VA=VALUE")
    prove.add_argument("--live-stack-dword", action="append", default=[], metavar="OFFSET=VALUE")
    prove.add_argument(
        "--subject-only", action="store_true", help="stop after building the subject"
    )

    audit = sub.add_parser(
        "audit", help="audit scratch registers and arithmetic EFLAGS at direct call sites"
    )
    add_common(audit)
    audit.add_argument("--xbe", type=Path, default=DEFAULT_XBE)
    audit.add_argument("--functions", type=Path, default=DEFAULT_FUNCTIONS)
    audit.add_argument("--manifest", type=Path, default=DEFAULT_REPLACE_OUT / "manifest.json")
    audit.add_argument("--out", type=Path, default=DEFAULT_REPLACE_OUT / "audit.json")
    audit.add_argument("--only-va", help="audit one exact registered guest VA")
    return parser


def cmd_scan(args: argparse.Namespace) -> int:
    for registration in scan_directory(args.game_dir):
        print(
            f"{registration.va:#010x} {registration.convention:<8} args={registration.stack_args} "
            f"{registration.returns:<4} {registration.function}  ({registration.source})"
        )
    return 0


def cmd_wire(args: argparse.Namespace) -> int:
    registrations = scan_directory(args.game_dir)
    wiring = plan_wiring(args.gen_dir, registrations)
    written = write_wiring(wiring, args.out)
    print(f"{len(registrations)} replacement(s) over {len(written)} chunk(s) wired into {args.out}")
    return 0


def repl_digest(game_dir: Path) -> str:
    from tools.harness.provenance import tree_digest

    return tree_digest(game_dir)


def cmd_manifest(args: argparse.Namespace) -> int:
    registrations = scan_directory(args.game_dir)
    exe = build_manifest_exe(args.game_dir, args.work_dir / "manifest")
    entries = parse_manifest_output(run([str(exe)]))
    problems = cross_check(entries, registrations)
    if problems:
        for problem in problems:
            print(f"MISMATCH: {problem}", file=sys.stderr)
        return 1
    write_manifest(args.out, entries, repl_digest(args.game_dir))
    print(f"{len(entries)} registered replacement(s) written to {args.out}")
    return 0


def run_script(script: Path, args: list[str], env: dict[str, str]) -> None:
    completed = subprocess.run(
        [str(script), *args],
        env={**os.environ, **env},
        capture_output=True,
        text=True,
        timeout=SUBPROCESS_TIMEOUT_SECONDS,
        check=False,
    )
    sys.stderr.write(completed.stderr[-2000:])
    if completed.returncode != 0:
        raise BuildError(f"{script.name} exited {completed.returncode}")


def build_subject(args: argparse.Namespace) -> Path:
    """wire, rebuild only the touched chunks, compile src/game, link the subject."""
    registrations = scan_directory(args.game_dir)
    if not registrations:
        raise BuildError(f"no GAME_REPLACE registration in {args.game_dir}")
    work: Path = args.work_dir
    weak_dir = work / "weak"
    write_wiring(plan_wiring(args.gen_dir, registrations), weak_dir)

    base = prepare_baseline(args.gen_dir, args.base_dir or work / "stub-base")
    weak_build = work / "stub-weak"
    run_script(
        HARNESS_DIR / "build_stub_objects.sh",
        [str(args.gen_dir), str(weak_build)],
        {"WEAK_DIR": str(weak_dir), "REUSE_OBJ_DIR": str(base / "obj")},
    )

    opt_flag = f"-O{args.opt_level}"
    opt_tag = f"O{args.opt_level}"
    fp_scalar = getattr(args, "vector_mode", "legacy") == "fp-scalar-v1"
    from .fp_build import FP_COMPILE_FLAGS

    objects = compile_objects(
        args.game_dir,
        work / f"game-objs-{opt_tag}",
        opt_level=opt_flag,
        extra_flags=FP_COMPILE_FLAGS if fp_scalar else (),
    )
    digest = repl_digest(args.game_dir)
    subject = getattr(args, "subject", None) or work / f"subject-repl-{opt_tag}"
    run_script(
        HARNESS_DIR / "build_subject.sh",
        [str(weak_build / "obj"), str(HARNESS_DIR / "runtime_min.c"), str(subject)],
        {
            "GEN_DIR": str(args.gen_dir),
            "EXTRA_OBJS": " ".join(str(path) for path in objects),
            "REPL_INCLUDE_DIR": str(args.game_dir),
            "REPL_SHA": digest[:16],
            "REPL_CFLAGS": " ".join((opt_flag, *FP_COMPILE_FLAGS)) if fp_scalar else opt_flag,
            # T1510: only the opt-in v2 proof links the raw x87 runtime into the subject
            **({"X87RAW": "1"} if x87_v2_enabled() else {}),
            # T1508: only the opt-in code-write v2 proof builds the driver stop channel
            **({"CODEWRITE": "1"} if codewrite_v2_enabled() else {}),
        },
    )
    print(f"subject: {subject} (repl_sha={digest[:16]}, hand code at {opt_flag})")
    return subject


def cmd_build_subject(args: argparse.Namespace) -> int:
    build_subject(args)
    return 0


PARTIAL_DIR_NAME = "partial"


def prove_artifact_dir(args: argparse.Namespace) -> Path:
    """Where audit, results and proof land: `--only-va` debug runs must never replace the
    whole-registry files `tools/coverage.py` reads from the out dir, so they get a subdirectory."""
    if args.only_va is None:
        return args.out_dir
    return args.out_dir / PARTIAL_DIR_NAME


def cmd_prove(args: argparse.Namespace) -> int:
    from tools.harness import cli as harness_cli
    from tools.harness.fixture_selection import select_labels
    from tools.harness.providers import REGISTRY

    synth_domain = getattr(args, "synth_domain", False)
    available = tuple(provider.label for provider in REGISTRY)
    if synth_domain:
        from tools.harness.synth_domain import LABEL as SYNTH_LABEL

        available += (SYNTH_LABEL,)
    try:
        selected_providers = select_labels(
            getattr(args, "fixture_provider", None),
            available,
            disabled=getattr(args, "no_fixture_providers", False),
        )
    except ValueError as error:
        print(f"refusing replacement proof: {error}", file=sys.stderr)
        return 2
    if synth_domain and (getattr(args, "no_fixture_providers", False) or args.no_edge_cases):
        print(
            "refusing replacement proof: --synth-domain needs edge cases and fixture providers",
            file=sys.stderr,
        )
        return 2

    if getattr(args, "live_vector_state", False) and (
        not args.live_call_closure
        or args.only_va is None
        or args.live_call_boundary
        or args.static_jump_tables
    ):
        print(
            "vector state requires one complete --only-va --live-call-closure without boundaries",
            file=sys.stderr,
        )
        return 2
    from tools.replace.x87_arbiter_contract import select as select_x87_contract

    try:
        from tools.replace.x87_arbiter_contract import extension_va_for

        selected_contract = select_x87_contract(
            getattr(args, "x87_contract", None),
            getattr(args, "x87_arbiter_ack", None),
            extension_va=extension_va_for(getattr(args, "only_va", None)),
        )
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2
    if selected_contract != DEFAULT_X87_NAME:
        os.environ[X87_ENV] = selected_contract  # scan, wire, build and mutate all see it
    from .guarded_jump_contract import ENV_SELECTED as GUARDED_ENV
    from .guarded_jump_contract import select as select_guarded_contract

    try:
        guarded_selected = select_guarded_contract(
            getattr(args, "guarded_jump_contract", None), getattr(args, "guarded_jump_ack", None)
        )
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2
    if guarded_selected:
        os.environ[GUARDED_ENV] = args.guarded_jump_contract
    from .codewrite_arbiter_contract import select as select_codewrite_contract

    try:
        selected_codewrite = select_codewrite_contract(
            getattr(args, "codewrite_contract", None),
            getattr(args, "codewrite_arbiter_ack", None),
            root_va=int(args.only_va, 16)
            if getattr(args, "only_va", None)
            and getattr(args, "codewrite_contract", None) not in (None, DEFAULT_CODEWRITE_NAME)
            else None,
        )
    except ValueError as error:
        print(str(error), file=sys.stderr)
        return 2
    if selected_codewrite != DEFAULT_CODEWRITE_NAME:
        if selected_contract != DEFAULT_X87_NAME:
            print("the x87 and code-write v2 contracts cannot be combined", file=sys.stderr)
            return 2
        os.environ[CODEWRITE_ENV] = selected_codewrite  # scan, wire, build and mutate all see it
    if synth_domain and (
        selected_contract != DEFAULT_X87_NAME or selected_codewrite != DEFAULT_CODEWRITE_NAME
    ):
        print(
            "refusing replacement proof: --synth-domain is not available for the x87-v2 or "
            "code-write-v2 proofs",
            file=sys.stderr,
        )
        return 2
    fp_scalar = getattr(args, "vector_mode", "legacy") == "fp-scalar-v1"
    if fp_scalar and not getattr(args, "live_vector_state", False):
        print("--vector-mode fp-scalar-v1 needs --live-vector-state", file=sys.stderr)
        return 2
    subject = build_subject(args)
    if args.subject_only:
        return 0
    artifact_dir = prove_artifact_dir(args)
    artifact_dir.mkdir(parents=True, exist_ok=True)
    fp_record = artifact_dir / "fp-build.json"
    if fp_scalar:
        from .fp_build import FP_COMPILE_FLAGS, build_record

        opt_flag = f"-O{args.opt_level}"
        record = build_record(
            args.game_dir,
            sorted((args.work_dir / f"game-objs-O{args.opt_level}").glob("*.o")),
            opt_flag,
            compile_flags=["-std=c11", opt_flag, "-fPIE", *FP_COMPILE_FLAGS],
        )
        fp_record.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        if not record["passed"]:
            print(f"refusing fp-scalar-v1 proof: {record['failures']}", file=sys.stderr)
            return 1
    manifest_args = argparse.Namespace(
        game_dir=args.game_dir,
        gen_dir=args.gen_dir,
        work_dir=args.work_dir,
        out=args.out_dir / "manifest.json",
    )
    status = cmd_manifest(manifest_args)
    if status != 0:
        return status
    audit_args = argparse.Namespace(
        game_dir=args.game_dir,
        gen_dir=args.gen_dir,
        xbe=args.xbe,
        manifest=manifest_args.out,
        out=artifact_dir / "audit.json",
        only_va=args.only_va,
    )
    if cmd_audit(audit_args) != 0:
        print("refusing replacement proof: caller resource audit is ineligible", file=sys.stderr)
        return 1
    if x87_v2_enabled():
        return prove_x87_v2(args, subject, artifact_dir)
    if codewrite_v2_enabled():
        return prove_codewrite_v2(args, subject, artifact_dir)
    argv = [
        "--xbe",
        str(args.xbe),
        "--functions",
        str(args.functions),
        "--subject",
        str(subject),
        "--shipped-gen-dir",
        str(args.gen_dir),
        "--image",
        str(args.work_dir / "guest.img"),
        "--out",
        str(artifact_dir / "results.csv"),
        "--proof-out",
        str(artifact_dir / "proof.json"),
        "--replace-dir",
        str(args.game_dir),
        "--replacement",
        "--seed",
        str(args.seed),
    ]
    if args.min_size is not None:
        argv += ["--min-size", str(args.min_size)]
    argv += ["--max-size", str(getattr(args, "max_size", 512))]
    if getattr(args, "static_jump_tables", False):
        # This opt-in contract proves only call-free bounded table bodies.
        argv += ["--static-jump-tables", "--no-stub-calls"]
    if args.cases_per_function is not None:
        argv += ["--cases-per-function", str(args.cases_per_function)]
    if args.no_edge_cases:
        argv.append("--no-edge-cases")
    if getattr(args, "no_fixture_providers", False):
        argv.append("--no-fixture-providers")
    elif selected_providers is not None:
        for label in selected_providers:
            argv.extend(("--fixture-provider", label))
    if args.only_va is not None:
        argv += ["--only-va", args.only_va]
    if args.live_call_closure:
        argv.append("--live-call-closure")
    if getattr(args, "live_vector_state", False):
        argv.append("--live-vector-state")
    if synth_domain:
        argv += ["--synth-domain", "--synth-domain-dir", str(artifact_dir / "synth-domain")]
        if getattr(args, "synth_domain_version", "t1773-synth-v1") != "t1773-synth-v1":
            argv += ["--synth-domain-version", args.synth_domain_version]
        for pin in getattr(args, "synth_domain_pin", None) or ():
            argv += ["--synth-domain-pin", pin]
    if fp_scalar:
        argv += ["--vector-mode", "fp-scalar-v1", "--fp-build-record", str(fp_record)]
    for boundary in args.live_call_boundary:
        argv += ["--live-call-boundary", boundary]
    for patch in args.live_memory_dword:
        argv += ["--live-memory-dword", patch]
    for patch in args.live_stack_dword:
        argv += ["--live-stack-dword", patch]
    if guarded_selected:
        witness = build_witness_subject(args)
        argv += ["--guarded-jumps", "--witness-subject", str(witness)]
        if getattr(args, "guarded_sweep_version", 1) != 1:
            argv += ["--guarded-sweep-version", str(args.guarded_sweep_version)]
        argv += ["--witness-draft", str(witness_draft(args))]
    return harness_cli.main(argv)


def witness_draft(args: argparse.Namespace) -> Path:
    """The draft file that registers the proved root (its switch is the witness subject)."""
    if args.only_va is None:
        raise SystemExit("the guarded-jump contract proves one root: pass --only-va")
    va = int(args.only_va, 16)
    sources = {r.source for r in scan_directory(args.game_dir) if r.va == va}
    if len(sources) != 1:
        raise SystemExit(f"expected exactly one source registering 0x{va:08X}, found {sources}")
    return args.game_dir / sources.pop()


def build_witness_subject(args: argparse.Namespace) -> Path:
    """T1576: the same draft compiled at -O0 by clang with source coverage, linked into a
    witness driver (HARNESS_WITNESS). Shares the weak-wired lifted objects of the main subject."""
    from .build import PROJECT_WARNINGS
    from .fp_build import FP_COMPILE_FLAGS

    work: Path = args.work_dir
    fp_scalar = getattr(args, "vector_mode", "legacy") == "fp-scalar-v1"
    flags = ("-g", "-fprofile-instr-generate", "-fcoverage-mapping")
    objects = compile_objects(
        args.game_dir,
        work / "game-objs-witness",
        opt_level="-O0",
        cc="clang",
        extra_flags=(*flags, *(FP_COMPILE_FLAGS if fp_scalar else ())),
        warnings=tuple(w for w in PROJECT_WARNINGS if w != "-Werror"),
    )
    subject = work / "subject-witness-O0"
    run_script(
        HARNESS_DIR / "build_subject.sh",
        [str(work / "stub-weak" / "obj"), str(HARNESS_DIR / "runtime_min.c"), str(subject)],
        {
            "GEN_DIR": str(args.gen_dir),
            "EXTRA_OBJS": " ".join(str(path) for path in objects),
            "REPL_INCLUDE_DIR": str(args.game_dir),
            "REPL_SHA": repl_digest(args.game_dir)[:16],
            "REPL_CFLAGS": "-O0 witness",
            "CC": "clang",
            "WITNESS": "1",
        },
    )
    print(f"witness subject: {subject}")
    return subject


def prove_x87_v2(args: argparse.Namespace, subject: Path, artifact_dir: Path) -> int:
    """T1510: the model-arbitrated-x87-v2 proof of the registry x87 roots (see x87_v2_prove)."""
    from . import x87_v2_prove

    if args.only_va is None or int(args.only_va, 16) not in x87_v2_prove.ROOTS:
        print(
            "model-arbitrated-x87-v2 proves one authentic x87 root: pass --only-va", file=sys.stderr
        )
        return 2
    return x87_v2_prove.run(
        xbe=args.xbe,
        subject_binary=subject,
        image_path=args.work_dir / "guest-x87v2.img",
        out_path=artifact_dir / "x87-v2-proof.json",
        seed=args.seed,
        cases=args.cases_per_function if args.cases_per_function is not None else 600,
        roots=(int(args.only_va, 16),),
        opt_level=args.opt_level,
        game_dir=args.game_dir,
        ack=args.x87_arbiter_ack,
    )


def prove_codewrite_v2(args: argparse.Namespace, subject: Path, artifact_dir: Path) -> int:
    """T1508: the faithful-codewrite-arbiter-v2 proof of a code-write root (codewrite_v2_prove)."""
    from . import codewrite_tx_prove, codewrite_v2_prove

    if args.only_va is not None and int(args.only_va, 16) in codewrite_tx_prove.ROOTS:
        return codewrite_tx_prove.run(
            xbe=args.xbe,
            subject_binary=subject,
            image_path=args.work_dir / "guest-codewrite-v2.img",
            out_path=artifact_dir / "codewrite-v2-proof.json",
            seed=args.seed,
            cases=args.cases_per_function if args.cases_per_function is not None else 600,
            root=int(args.only_va, 16),
            opt_level=args.opt_level,
            game_dir=args.game_dir,
            ack=args.codewrite_arbiter_ack,
            authentic_dir=Path(os.environ["TSFP_CODEWRITE_AUTHENTIC_DIR"])
            if os.environ.get("TSFP_CODEWRITE_AUTHENTIC_DIR")
            else None,
        )
    if args.only_va is None or int(args.only_va, 16) not in codewrite_v2_prove.ROOTS:
        print(
            "faithful-codewrite-arbiter-v2 proves one authentic code-write root: pass --only-va",
            file=sys.stderr,
        )
        return 2
    return codewrite_v2_prove.run(
        xbe=args.xbe,
        subject_binary=subject,
        image_path=args.work_dir / "guest-codewrite-v2.img",
        out_path=artifact_dir / "codewrite-v2-proof.json",
        seed=args.seed,
        cases=args.cases_per_function if args.cases_per_function is not None else 600,
        root=int(args.only_va, 16),
        opt_level=args.opt_level,
        game_dir=args.game_dir,
        ack=args.codewrite_arbiter_ack,
        authentic_dir=Path(os.environ["TSFP_CODEWRITE_AUTHENTIC_DIR"])
        if os.environ.get("TSFP_CODEWRITE_AUTHENTIC_DIR")
        else None,
    )


def cmd_audit(args: argparse.Namespace) -> int:
    from tools.harness.image import build_guest_image

    from . import audit as audit_module

    manifest_path = args.manifest
    if not manifest_path.exists():
        print(f"run `manifest` first: {manifest_path} is missing", file=sys.stderr)
        return 1
    import json

    from .manifest import entry_from_json, manifest_sha

    document = json.loads(manifest_path.read_text(encoding="utf-8"))
    entries = [entry_from_json(record) for record in document["functions"]]
    if document["manifest_sha"] != manifest_sha(entries, repl_digest(args.game_dir)):
        print(
            "refusing stale manifest: rebuild it from the current hand-written sources",
            file=sys.stderr,
        )
        return 1
    selected = getattr(args, "only_va", None)
    if selected is not None:
        entries = [entry for entry in entries if entry.va == int(selected, 16)]
        if not entries:
            print("selected replacement is absent from the manifest", file=sys.stderr)
            return 1
    image = build_guest_image(args.xbe)
    from .bridge import BridgeExemption

    bridge = BridgeExemption.open(args.xbe, args.gen_dir, args.game_dir)
    from .straddle_corpus import data_ranges

    # no readable XBE (a test double image): the rule stays off, the conservative answer
    ranges = [(low, high) for _, low, high in data_ranges(args.xbe)] if args.xbe.exists() else []
    audits = audit_module.audit_all(
        entries,
        args.gen_dir,
        image,
        data_ranges=ranges,
        bridge=bridge,
        functions=getattr(args, "functions", None),
    )
    audit_module.write_audit(args.out, audits, manifest_sha=document["manifest_sha"])
    ineligible = 0
    for item in audits:
        print(
            f"{item.va:#010x} sites={item.direct_sites} tail={item.tail_jumps} "
            f"data_refs={item.data_references} safe={item.safe} unsafe={item.unsafe} "
            f"unresolved={item.unresolved}"
        )
        ineligible += not item.eligible
    return 1 if ineligible else 0


COMMANDS = {
    "scan": cmd_scan,
    "wire": cmd_wire,
    "manifest": cmd_manifest,
    "build-subject": cmd_build_subject,
    "prove": cmd_prove,
    "audit": cmd_audit,
}


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return COMMANDS[args.command](args)
    except (ScanError, WiringError, BuildError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
