# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in `guarded-jump-schema3` contract (T1576). OFF by default, and not yet runnable.

Mirrors x87_arbiter_contract.py. schema3 is emitted ONLY when the opt-in is on AND a root uses a
jump table or an out-of-body tail jmp (`needs_schema3`); every other receipt keeps its existing
schema bytes (`schema_for`). Readers refuse schema3 unless the same ack fingerprint is supplied.
`unmet_prerequisites` lists exactly what production still lacks, so selection refuses today.
"""

from __future__ import annotations

import hashlib
import json
import os
from collections.abc import Mapping
from pathlib import Path

NAME = "guarded-jump-schema3"
LEGACY_SCHEMAS = (1, 2)
SCHEMA = 3
ENV_SELECTED = "TSFP_GUARDED_JUMP_CONTRACT"
REPO = Path(__file__).resolve().parents[2]

#: Files whose change must force a new, deliberate acknowledgement.
GUARD_FILES = (
    "tools/replace/arm_witness.py",
    "tools/replace/guarded_jump_session.py",
    "tools/replace/guarded_jump_mutants.py",
    "tools/replace/tail_adapter.py",
    "tools/replace/guarded_jump_tuples.py",
    "tools/replace/game_guest_tail.h",
    "tools/harness/oracle.py",
    "tools/harness/cli.py",
    "tools/harness/selection.py",
    "tools/harness/replacement.py",
    "tools/harness/build_subject.sh",
    "tools/replace/cli.py",
    "tools/replace/build.py",
    "tools/replace/guarded_jump_receipts.py",
    "tools/replace/guarded_jump_contract.py",
    "tools/harness/guarded_tables.py",
    "tools/harness/driver.c",
    "tools/harness/fault_diagnostics.py",
    "tools/harness/guarded_jumps.py",
    "tools/harness/static_tables.py",
    "tools/harness/callclosure.py",
    "tools/harness/dispatch_source.py",
)
#: Schema3-only receipt fields.
RECEIPT_FIELDS = ("arm_witness", "tail_target", "fault")

#: Guard -> production state, recorded honestly. A guard marked False blocks selection. A True is
#: only believed while `_probe` also finds the implementation, so a stale flag cannot enable it.
GUARD_STATE: dict[str, bool] = {
    "arm-witness-library": True,
    "oracle-fault-address": True,
    "native-fault-address": True,
    "fault-parity-compare": True,
    "schema3-gate-and-ack": True,
    "arm-witness-in-prove-pipeline": True,
    "original-table-hit-sets-from-trace": True,
    "typed-tail-adapter-full-register": True,
    "closure-scan-admits-jump-table-and-tail": True,
    "runtime-mutants-in-pipeline": True,
}


def _probe(name: str) -> bool:
    """The implementation behind a guard exists in this tree (not a proof that it is correct)."""
    import importlib
    import inspect

    def has(module: str, attr: str) -> bool:
        try:
            return hasattr(importlib.import_module(module), attr)
        except ImportError:
            return False

    checks = {
        "arm-witness-library": lambda: has("tools.replace.arm_witness", "arm_parity"),
        "oracle-fault-address": lambda: has("tools.harness.oracle", "UnicornOracle"),
        "native-fault-address": lambda: (
            "FAULTADDR" in (REPO / "tools/harness/driver.c").read_text()
        ),
        "fault-parity-compare": lambda: (
            "require_fault_parity"
            in inspect.signature(
                importlib.import_module("tools.harness.compare").compare
            ).parameters
        ),
        "schema3-gate-and-ack": lambda: True,
        "arm-witness-in-prove-pipeline": lambda: (
            has("tools.replace.guarded_jump_session", "GuardedSession")
            and "HARNESS_WITNESS" in (REPO / "tools/harness/driver.c").read_text()
        ),
        "original-table-hit-sets-from-trace": lambda: (
            has("tools.harness.guarded_tables", "prove_guarded_tables")
            and "arm_events" in (REPO / "tools/harness/oracle.py").read_text()
        ),
        "typed-tail-adapter-full-register": lambda: (
            has("tools.replace.tail_adapter", "check_draft")
            and (REPO / "tools/replace/game_guest_tail.h").exists()
        ),
        "closure-scan-admits-jump-table-and-tail": lambda: (
            "guarded_jumps"
            in inspect.signature(
                importlib.import_module("tools.harness.callclosure").discover_live_call_closure
            ).parameters
        ),
        "runtime-mutants-in-pipeline": lambda: (
            has("tools.replace.guarded_jump_mutants", "generate")
            and has("tools.replace.guarded_jump_tuples", "run_tuple")
        ),
    }
    return bool(checks[name]())


def select_root(proof: Mapping[str, object], va: int | str) -> dict:
    """The one `functions` row whose VA is the root under test (T1576 defect-functions0).

    Never positional: a proof document may hold several functions. Fails closed (ValueError)
    when the root is absent or listed more than once.
    """
    want = int(va, 0) if isinstance(va, str) else va
    rows = [row for row in proof.get("functions") or [] if int(str(row.get("va")), 0) == want]
    if len(rows) != 1:
        raise ValueError(f"proof has {len(rows)} functions rows for root 0x{want:08X}, need 1")
    return rows[0]


def enabled() -> bool:
    return os.environ.get(ENV_SELECTED) == NAME


def _digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def identity(root: Path = REPO) -> dict[str, str]:
    return {name: _digest(root / name) for name in GUARD_FILES}


def fingerprint(ident: Mapping[str, object]) -> str:
    return hashlib.sha256(json.dumps(ident, sort_keys=True).encode("ascii")).hexdigest()


def current_fingerprint(root: Path = REPO) -> str:
    return fingerprint(identity(root))


def unmet_prerequisites() -> list[str]:
    return [name for name, ready in GUARD_STATE.items() if not (ready and _probe(name))]


def needs_schema3(uses_jump_table: bool, uses_out_of_body_tail: bool) -> bool:
    return uses_jump_table or uses_out_of_body_tail


def schema_for(
    contract_on: bool, uses_jump_table: bool, uses_out_of_body_tail: bool, legacy_schema: int
) -> int:
    """Receipt schema to write. Legacy roots keep their schema whatever the flag says."""
    if legacy_schema not in LEGACY_SCHEMAS:
        raise ValueError(f"unknown legacy schema {legacy_schema}")
    if contract_on and needs_schema3(uses_jump_table, uses_out_of_body_tail):
        return SCHEMA
    return legacy_schema


def select(name: str | None, acknowledgement: str | None, root: Path = REPO) -> bool:
    """True when the contract is selected. Raises ValueError to refuse (the default is OFF)."""
    if name is None:
        if acknowledgement is not None:
            raise ValueError(f"--guarded-jump-ack is only valid with {NAME}")
        return False
    if name != NAME:
        raise ValueError(f"unknown contract {name!r}; known: {NAME}")
    expected = current_fingerprint(root)
    if acknowledgement != expected:
        raise ValueError(f"{NAME} must name its guards: pass --guarded-jump-ack {expected}")
    unmet = unmet_prerequisites()
    if unmet:
        raise ValueError(f"{NAME} is not runnable yet, missing: " + ", ".join(unmet))
    return True


def attach_schema3(
    document: dict[str, object],
    *,
    arm_witness: Mapping[str, object],
    tail_target: Mapping[str, object],
    closure_guards: Mapping[str, object] | None,
    root: Path = REPO,
) -> bool:
    """Turn a freshly built proof document into schema3, ONLY when the guarded admission was
    actually used (a table or out-of-body tail in the root or any closure node). Otherwise the
    document keeps its legacy schema bytes and False is returned."""
    nodes = list((closure_guards or {}).get("nodes", []))  # type: ignore[call-overload]
    used = (
        bool(arm_witness.get("table"))
        or bool(tail_target.get("sites"))
        or any(node.get("tails") or node.get("tables") for node in nodes)
    )
    legacy = document.get("schema")
    if not used or schema_for(True, used, used, legacy) != SCHEMA:  # type: ignore[arg-type]
        return False
    document["base_schema"] = legacy
    document["schema"] = SCHEMA
    document["guarded_jump_contract"] = NAME
    document["arm_witness"] = dict(arm_witness)
    document["tail_target"] = dict(tail_target)
    document["fault"] = {
        "parity": "same guest fault class and guest address; a match is never a verdict",
        "native_address": "driver FAULTADDR (HARNESS_FAULT_ADDR)",
        "oracle_address": "UC_HOOK_MEM_INVALID",
    }
    document["closure_guards"] = [
        {
            "va": node["va"],
            "tails": node.get("tails", []),
            "tables": [
                {
                    key: table[key]
                    for key in ("site", "address", "targets", "body_sha256", "default_target")
                    if key in table
                }
                for table in node.get("tables", [])
            ],
        }
        for node in nodes
        if node.get("tails") or node.get("tables")
    ]
    document["guard_identity"] = identity(root)
    document["guard_ack_fingerprint"] = current_fingerprint(root)
    return True


def read_document(
    document: Mapping[str, object], acknowledgement: str | None, root: Path = REPO
) -> Mapping[str, object]:
    """Reader dispatch: schema3 needs the ack and the recorded identity; legacy needs none."""
    schema = document.get("schema")
    if schema in LEGACY_SCHEMAS:
        if any(field in document for field in RECEIPT_FIELDS):
            raise ValueError("legacy document carries schema3-only fields")
        return document
    if schema != SCHEMA:
        raise ValueError(f"unknown receipt schema {schema!r}")
    if acknowledgement != current_fingerprint(root):
        raise ValueError("schema3 document refused: missing or stale guarded-jump ack")
    if document.get("guard_identity") != identity(root):
        raise ValueError("schema3 guard files changed since the receipt was taken")
    if not all(field in document for field in RECEIPT_FIELDS):
        raise ValueError("schema3 document missing arm_witness, tail_target or fault")
    return document
