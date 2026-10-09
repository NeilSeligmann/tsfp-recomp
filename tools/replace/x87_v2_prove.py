# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end `model-arbitrated-x87-v2` proof of the registry x87 roots (T1510).

Selected by `tools.replace prove --x87-contract model-arbitrated-x87-v2` after the manifest and
the caller resource audit. The SUBJECT is the production-shaped binary `build_subject` produced:
the real driver, the lifted tree with the four roots made weak, and the registry adapters from
`src/game/x87_roots.inc` linked in (so the strong `sub_XXXXXXXX`, the adapter pop and the checked
return are all in the measured path), with the raw x87 runtime behind `x87raw=1`. The REFERENCE is
the original guest bytes in Unicorn with each authenticated x87 instruction replaced by the
independent SDM model (`ArbitratedOracle`). Cases are the predeclared stream of
`docs/evidence/t1510/root_cases.py`. The comparator is `x87_root_compare.compare` with the only
narrowing a register-exact replacement is allowed here: the dead stack below the entry esp (the
original pushes there, the replacement does not). All eight registers, every other write byte and
the complete raw x87 state are strict. One attempt per (root, seed, optimisation) tuple (T1481):
nothing is retried, a subject death or refusal is a recorded non-AGREE outcome.
"""

from __future__ import annotations

import hashlib
import json
import platform
import subprocess
import sys
from collections import Counter
from pathlib import Path

from tools.harness.image import build_guest_image, write_image_file
from tools.harness.model import Case
from tools.harness.subject import SubjectError, SubjectProcess
from tools.harness.x87_fprem_root_oracle import SIZE as FPREM_SIZE
from tools.harness.x87_fprem_root_oracle import FpremRootOracle
from tools.harness.x87_lift import CONTRACTS
from tools.harness.x87_root_compare import Side, compare
from tools.harness.x87_root_oracle import ArbitratedOracle
from tools.replace import x87_arbiter_contract as contract

FPREM_ROOT = 0x3C9DE8
ROOTS = (0xCB3B0, 0x1B7D00, 0x1B7D70, 0x259DF0, FPREM_ROOT)
XBE_SHA = "3cfd001a84fc3e08175d6c4b2e42ebf49577a089b5d0bd87ac724f61a41816bc"
DEAD_STACK_BYTES = 0x100
SCHEMA = 1
REPO = Path(__file__).resolve().parents[2]
SOURCES = ("src/game/x87_replace.h", "src/game/x87_roots.c", "src/game/x87_roots.inc")


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(
    *,
    xbe: Path,
    subject_binary: Path,
    image_path: Path,
    out_path: Path,
    seed: int,
    cases: int,
    roots: tuple[int, ...],
    opt_level: int,
    game_dir: Path,
    ack: str,
    mutant: str | None = None,
) -> int:
    """Run the predeclared case stream. Returns 0 iff every case of every root AGREEs."""
    from docs.evidence.t1510 import fprem_root_cases
    from docs.evidence.t1510.root_cases import generate

    if FPREM_ROOT in roots and roots != (FPREM_ROOT,):
        raise SystemExit("the FPREM root has its own arbiter extension: prove it alone")
    extension = FPREM_ROOT if roots == (FPREM_ROOT,) else None
    if contract.select(contract.V2_NAME, ack, extension_va=extension) != contract.V2_NAME:
        raise SystemExit("v2 contract not selected")
    if _sha(xbe) != XBE_SHA:
        raise SystemExit("original XBE authentication failed")
    if not set(roots) <= set(ROOTS):
        raise SystemExit("not an authentic x87 root")
    image = build_guest_image(xbe)
    write_image_file(image, image_path)
    subject = SubjectProcess(subject_binary, image_path)
    if not subject.supports_raw_x87:
        raise SystemExit("subject does not advertise x87raw=1: the raw backend is not linked")
    provenance = subject.provenance
    arbiter = FpremRootOracle(image.data) if extension else ArbitratedOracle(image.data)
    summary: Counter = Counter()
    sites: Counter = Counter()
    failures: list[dict[str, object]] = []
    replaced: Counter = Counter()
    for root in roots:
        size = FPREM_SIZE if root == FPREM_ROOT else CONTRACTS[root].size
        stream = (
            fprem_root_cases.generate(seed, cases)
            if root == FPREM_ROOT
            else generate(root, seed, cases)
        )
        for item in stream:
            case = Case(seed, item.index, root, size, item.regs, 0, item.patches)
            reference = arbiter.run(case, item.x87)
            raw_case = Case(
                seed,
                item.index,
                root,
                size,
                item.regs,
                0,
                item.patches,
                x87_state=item.x87,
            )
            try:
                result = subject.run(raw_case)
                side = Side(result.regs, result.writes, result.x87_state, result.fault)
                replaced[f"{root:X}:{'yes' if result.replaced else 'NO'}"] += 1
                if not result.replaced and result.fault is None:
                    side = Side(None, None, None, "replacement-not-executed")
            except SubjectError as error:
                side = Side(None, None, None, f"subject-refusal:{error}")
            esp = item.regs[4]
            verdict = compare(
                Side(
                    reference.result.regs,
                    reference.result.writes,
                    reference.x87,
                    reference.result.fault,
                ),
                side,
                unrepresented=reference.unrepresented,
                ignored_write_ranges=(
                    () if root == FPREM_ROOT else ((esp - DEAD_STACK_BYTES, esp),)
                ),
            )
            summary[f"{root:X}:{verdict.outcome}"] += 1
            for event in reference.trace:
                sites[f"{root:X}:{event['op']}@{event['va']:08X}"] += 1
            if not verdict.agree:
                failures.append(
                    {
                        "root": f"{root:X}",
                        "seed": seed,
                        "index": item.index,
                        "outcome": verdict.outcome,
                        "components": list(verdict.components[:40]),
                        "component_count": len(verdict.components),
                    }
                )
    subject_counters = {
        "restarts": subject.restarts,
        "timeouts": subject.timeouts,
        "deaths": subject.deaths,
    }
    subject.close()
    receipt = {
        "schema": SCHEMA,
        "task": "T1510",
        "confidence": "INFERRED (model-arbitrated reference, same host silicon as the checks)",
        "x87_contract": contract.receipt_block(extension_va=extension),
        "xbe_sha256": XBE_SHA,
        "optimization": opt_level,
        "seed": seed,
        "cases_per_root": cases,
        "roots": [f"{root:X}" for root in roots],
        "mutant": mutant,
        "narrowing": {
            "registers": [],
            "dead_stack_bytes_below_entry_esp": 0 if extension else DEAD_STACK_BYTES,
            "x87_state": "strict, complete raw state",
        },
        "sources": {name: _sha(game_dir / Path(name).name) for name in SOURCES},
        "subject": {
            "binary_sha256": hashlib.sha256(subject_binary.read_bytes()).hexdigest(),
            "tree_sha": getattr(provenance, "tree_sha", None),
            "repl_sha": getattr(provenance, "repl_sha", None),
            "counters": subject_counters,
        },
        "compiler": subprocess.run(
            ["cc", "--version"], capture_output=True, text=True, timeout=30
        ).stdout.splitlines()[0],
        "host": platform.platform(),
        "python": sys.version.split()[0],
        "summary": dict(sorted(summary.items())),
        "replacement_executed": dict(sorted(replaced.items())),
        "x87_sites_executed": dict(sorted(sites.items())),
        "failures": failures,
        "all_agree": not failures and bool(summary),
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"summary": receipt["summary"], "failures": len(failures)}, indent=1))
    return 0 if receipt["all_agree"] else 1
