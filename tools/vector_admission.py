# SPDX-License-Identifier: GPL-3.0-or-later
"""Explicit bounded vector reports; never changes pilot selection or admits replacements."""

from __future__ import annotations

import argparse
import hashlib
import json
import signal
import sys
from collections.abc import Callable, Mapping
from dataclasses import asdict
from pathlib import Path

from tools.harness.callclosure import LiveClosureError, discover_live_call_closure
from tools.harness.callstub import _decoder
from tools.harness.provenance import tree_digest
from tools.harness.selection import instruction_reason
from tools.harness.vector import vector_state_instruction
from tools.replace.audit import FunctionAudit
from tools.replace.manifest import ManifestEntry
from tools.replace.scan import CONVENTIONS

MAX_ROOTS = 32


def corpus_roots(document: object) -> tuple[int, ...]:
    if not isinstance(document, dict) or not isinstance(document.get("roots"), list):
        raise ValueError("corpus requires explicit roots")
    rows = document["roots"]
    if not 1 <= len(rows) <= MAX_ROOTS:
        raise ValueError("corpus must contain1..32 roots")
    roots = tuple(int(row["va"], 16) for row in rows)
    if len(set(roots)) != len(roots) or any(not 0 <= va <= 0xFFFFFFFF for va in roots):
        raise ValueError("duplicate or invalid root")
    return roots


def contracts(document: object, roots: tuple[int, ...]) -> dict[int, ManifestEntry]:
    if not isinstance(document, dict) or set(document) != {"contracts"}:
        raise ValueError("ABI document requires contracts")
    result = {}
    for row in document["contracts"]:
        required = {"va", "convention", "stack_args", "returns", "scratch", "review"}
        if not isinstance(row, dict) or set(row) not in (
            required,
            required | {"register_inputs"},
        ):
            raise ValueError("malformed reviewed ABI")
        va = int(row["va"], 16)
        if va not in roots or va in result:
            raise ValueError("duplicate or out-of-corpus ABI")
        if not isinstance(row["review"], str) or not row["review"].strip():
            raise ValueError("ABI review reference required")
        if (
            row["convention"] not in CONVENTIONS
            or type(row["stack_args"]) is not int
            or not 0 <= row["stack_args"] <= 8
            or row["returns"] not in {"u32", "void"}
            or not isinstance(row["scratch"], list)
            or len(set(row["scratch"])) != len(row["scratch"])
            or not set(row["scratch"]) <= {"eax", "ecx", "edx"}
        ):
            raise ValueError("unsupported ABI contract")
        result[va] = ManifestEntry(
            va,
            "reviewed-vector-hypothesis",
            row["convention"],
            row["stack_args"],
            "eax" if row["returns"] == "u32" else "void",
            tuple(row["scratch"]),
            row["review"],
            tuple(row["register_inputs"]) if "register_inputs" in row else None,
        )
    return result


def audit_ready(audit: FunctionAudit | None, root: int) -> bool:
    return bool(
        audit is not None
        and audit.va == root
        and audit.eligible
        and audit.direct_sites > 0
        and not audit.tail_jumps
        and not audit.data_references
        and not audit.unresolved
        and len(audit.flags) == audit.direct_sites
        and len({flag.return_va for flag in audit.flags}) == audit.direct_sites
    )


def inspect_root(
    root: int,
    *,
    sizes: dict[int, int],
    names: dict[int, str],
    read_code: Callable[[int, int], bytes],
    game_member: bool | None,
    abi: ManifestEntry | None = None,
    audit: FunctionAudit | None = None,
    expected_body_sha256: str | None = None,
) -> dict[str, object]:
    """Static readiness only. A ready row still needs linked audit and actual proofs."""
    row: dict[str, object] = {"va": f"0x{root:08x}", "production_admitted": False}
    size = sizes.get(root)
    if size is None:
        return {**row, "blocker": "unknown-root-bound"}
    body = read_code(root, size)
    row["body_sha256"] = hashlib.sha256(body).hexdigest()
    if expected_body_sha256 is not None and row["body_sha256"] != expected_body_sha256:
        return {**row, "blocker": "original-body-pin-mismatch"}
    try:
        closure = discover_live_call_closure(
            root, sizes=sizes, names=names, read_code=read_code, allow_vector=True
        )
    except LiveClosureError as exc:
        return {**row, "blocker": "unsupported-closure", "detail": str(exc)}
    row["closure"] = [asdict(node) for node in closure.nodes]
    has_vector = False
    decoder = _decoder()
    for node in closure.nodes:
        code = read_code(node.va, node.size)
        if hashlib.sha256(code).hexdigest() != node.sha256:
            return {**row, "blocker": "closure-bytes-changed"}
        for insn in decoder.disasm(code, node.va):
            if instruction_reason(insn.mnemonic, insn.op_str) == "x87":
                return {
                    **row,
                    "blocker": "x87-full-state-unavailable",
                    "detail": f"0x{insn.address:08x} {insn.mnemonic}",
                }
            has_vector |= vector_state_instruction(insn)
    if not has_vector:
        return {**row, "blocker": "no-vector-in-closure"}
    if game_member is not True:
        return {**row, "blocker": "game-membership-unconfirmed"}
    if abi is None or abi.va != root:
        return {**row, "blocker": "ABI-not-reviewed"}
    row["abi_hypothesis"] = abi.as_json()
    if not audit_ready(audit, root):
        return {
            **row,
            "blocker": "caller-audit-not-ready",
            "caller_audit": asdict(audit) if audit is not None else None,
        }
    return {
        **row,
        "blocker": None,
        "caller_audit": asdict(audit),
        "status": "static-ready-requires-linked-audit-and-fixed-proofs",
    }


def output_path(path: Path) -> Path:
    resolved = path.resolve()
    if not resolved.is_relative_to(Path("tmp").resolve()):
        raise ValueError("reports must stay under isolated tmp outputs")
    return resolved


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--gen-dir", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--abi-contracts", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=int, default=60, choices=range(1, 61))
    return parser


def dependency_pins(tool_root: Path | None = None) -> dict[str, object]:
    """Pin complete Python tool membership, decoder package and actual loaded native library."""
    import capstone

    root = tool_root if tool_root is not None else Path(__file__).resolve().parent
    tool_sources = {
        str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(root.rglob("*.py"))
    }
    package_root = Path(capstone.__file__).resolve().parent
    package_sources = {
        str(path.relative_to(package_root)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in sorted(package_root.rglob("*"))
        if path.is_file() and "__pycache__" not in path.parts and path.suffix != ".pyc"
    }
    library = Path(capstone._cs._name).resolve()
    if not library.is_file():
        raise ValueError("loaded Capstone native library identity unavailable")
    native_stat = library.stat()
    return {
        "tools_python_membership_sha256": tool_sources,
        "capstone_version": capstone.__version__,
        "capstone_engine_version": list(capstone.cs_version()),
        "capstone_package": str(package_root),
        "capstone_package_membership_sha256": package_sources,
        "capstone_native": {
            "path": str(library),
            "sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
            "device": native_stat.st_dev,
            "inode": native_stat.st_ino,
            "size": native_stat.st_size,
        },
        "python": {
            "executable": sys.executable,
            "version": sys.version,
            "sha256": hashlib.sha256(Path(sys.executable).read_bytes()).hexdigest(),
        },
    }


def save_receipt(destination: Path, state: dict[str, object]) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    pending = destination.with_suffix(destination.suffix + ".pending")
    pending.write_text(json.dumps(state, indent=2) + "\n")
    pending.replace(destination)


def _run(
    args: argparse.Namespace, destination: Path, state: dict[str, object]
) -> dict[str, object]:
    corpus = json.loads(args.corpus.read_text())
    roots = corpus_roots(corpus)
    abi_document = (
        json.loads(args.abi_contracts.read_text()) if args.abi_contracts else {"contracts": []}
    )
    entries = contracts(abi_document, roots)
    from tools.harness.image import build_guest_image
    from tools.name_additions import World
    from tools.replace.audit import audit_all
    from tools.replace.straddle_corpus import data_ranges

    xbe = args.source_root / "build/default.xbe"
    functions = args.source_root / "generated/retail/functions.csv"
    pins = [
        xbe,
        functions,
        args.corpus,
        Path(__file__),
        args.source_root / "generated/retail/flirt_names.csv",
        args.source_root / "tools/data/function_overrides.csv",
        args.source_root / "tools/data/function_additions.csv",
        args.source_root / "tools/data/function_names.csv",
        Path("tools/harness/vector.py"),
        Path("tools/harness/callclosure.py"),
        Path("tools/harness/callstub.py"),
        Path("tools/harness/selection.py"),
        Path("tools/replace/audit.py"),
        Path("tools/name_additions.py"),
    ]
    if args.abi_contracts:
        pins.append(args.abi_contracts)
    state["phase"] = "pinning-inputs"
    save_receipt(destination, state)
    source_pins = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in pins}
    state["source_pins"] = source_pins
    dependencies = dependency_pins()
    state["dependencies"] = dependencies
    save_receipt(destination, state)
    for key, path in [("xbe_sha256", xbe), ("functions_sha256", functions)]:
        if corpus.get(key) != source_pins[str(path)]:
            raise ValueError(f"authenticated corpus {key} missing or mismatched")
    gen_sha = tree_digest(args.gen_dir)
    state["gen_tree_sha256"] = gen_sha
    state["phase"] = "loading-original-and-auditing"
    save_receipt(destination, state)
    world = World(args.source_root)
    # Same production ranges and CSV source; no bridges, no budget overrides.
    audits = (
        audit_all(
            entries.values(),
            args.gen_dir,
            build_guest_image(xbe),
            data_ranges=[(lo, hi) for _, lo, hi in data_ranges(xbe)],
            functions=functions,
        )
        if entries
        else []
    )
    by_va = {item.va: item for item in audits}
    specs: Mapping[int, dict] = {int(row["va"], 16): row for row in corpus["roots"]}
    rows = []
    for va in roots:
        state["phase"] = "inspecting-root"
        state["active_root"] = f"0x{va:08x}"
        save_receipt(destination, state)
        rows.append(
            inspect_root(
                va,
                sizes=world.size,
                names=world.names,
                read_code=world.read,
                game_member=va not in world.library and world.text_lo <= va < world.text_hi,
                abi=entries.get(va),
                audit=by_va.get(va),
                expected_body_sha256=specs[va].get("original_body_sha256"),
            )
        )
        state["roots"] = rows
        save_receipt(destination, state)
    state["phase"] = "verifying-input-stability"
    save_receipt(destination, state)
    if (
        source_pins != {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in pins}
        or gen_sha != tree_digest(args.gen_dir)
        or dependencies != dependency_pins()
    ):
        raise ValueError("source inputs changed during report")
    report = {
        "schema": 1,
        "mode": "opt-in-static-vector-report-not-admission",
        "source_pins": source_pins,
        "dependencies": dependencies,
        "status": "complete",
        "phase": "complete",
        "gen_tree_sha256": gen_sha,
        "audit_policy": "production audit_all; code_ranges=(); original data ranges; no bridges",
        "abi_contracts": abi_document,
        "roots": rows,
    }
    save_receipt(destination, report)
    return report


def run(args: argparse.Namespace) -> dict[str, object]:
    destination = output_path(args.out)
    if destination.exists():
        raise ValueError("receipt already exists; retain previous attempt and choose a new output")
    state: dict[str, object] = {
        "schema": 1,
        "mode": "opt-in-static-vector-report-not-admission",
        "status": "running",
        "phase": "preflight",
        "roots": [],
        "timeout_seconds": args.timeout_seconds,
    }
    save_receipt(destination, state)

    def expired(signum: int, frame: object) -> None:
        raise TimeoutError("bounded report deadline exceeded; no automatic retry")

    previous = signal.signal(signal.SIGALRM, expired)
    signal.alarm(args.timeout_seconds)
    try:
        return _run(args, destination, state)
    except Exception as exc:
        signal.alarm(0)
        state["status"] = "timeout" if isinstance(exc, TimeoutError) else "failed"
        state["error"] = {"class": type(exc).__name__, "message": str(exc)}
        save_receipt(destination, state)
        raise
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, previous)


def main() -> int:
    run(build_parser().parse_args())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
