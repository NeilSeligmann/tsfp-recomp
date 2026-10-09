# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in `model-arbitrated-x87-v2` proof contract (T1510). OFF by default.

Separately versioned from the inherited `inherited-limited-x87-v1` x87 contract in
`proof_contract.py`, which this module neither imports for mutation nor changes: every existing
gate, receipt and replay stays byte-identical. v2 differs in what the original side is trusted
for. v1 trusts Unicorn's x87 results as far as they are modelled (TOP only). v2 replaces the
Unicorn result of each modelled instruction with an independent Intel-SDM-derived model
(`tools/harness/x87_*_model.py`, evidence INFERRED, not hardware) and compares the COMPLETE raw
x87 state (80-bit stack slots, tags, control, status) strictly.

It cannot be selected silently: it needs the exact contract name AND an acknowledgement equal
to the fingerprint of the arbiter files named in `arbiter_identity()`, so a changed model needs
a changed, deliberate acknowledgement. Selecting it today still refuses (`unmet_prerequisites`)
because the registry-shaped x87 replacement ABI and the checked-return helper do not exist.
"""

from __future__ import annotations

import hashlib
import json
import os
from collections.abc import Mapping
from pathlib import Path

V1_NAME = "inherited-limited-x87-v1"
V2_NAME = "model-arbitrated-x87-v2"
CONTRACT_NAMES = (V1_NAME, V2_NAME)
DEFAULT_NAME = V1_NAME
EVIDENCE_CLASS = "x87-model-arbitrated"
EVIDENCE_LABEL = "INFERRED"

REPO = Path(__file__).resolve().parents[2]
#: SDM-derived models that replace Unicorn's result of the instruction they cover.
MODEL_FILES = (
    "tools/harness/x87_arith_model.py",
    "tools/harness/x87_fprem_model.py",
    "tools/harness/x87_store_model.py",
)
#: The harness pieces that apply the models and compare the raw state.
ARBITER_HARNESS_FILES = (
    "tools/harness/x87_root_compare.py",
    "tools/harness/x87_root_oracle.py",
    "tools/harness/x87_state.py",
)
#: Per-root arbiter extension (T1510, 3C9DE8). The FPREM loop root needs the incomplete-reduction
#: model, its own oracle and its own case stream. Those files are NOT part of the base identity, so
#: the fingerprint (and every receipt) of the four roots admitted earlier is unchanged; they join
#: the identity only when the extension root is selected (`extension_va`).
ROOT_EXTENSIONS: dict[int, tuple[str, ...]] = {
    0x3C9DE8: (
        "tools/harness/x87_fprem_full_model.py",
        "tools/harness/x87_fprem_root_oracle.py",
        "docs/evidence/t1510/fprem_root_cases.py",
    ),
}
#: Instructions the arbiter models; anything else keeps the fail-closed refusal.
MODELLED_INSTRUCTIONS = ("FLD m32", "FMUL m32", "FSTP m32", "FXCH st1", "FPREM", "FSTP st1")
#: Driver READY capability the v2 proof needs from the subject.
REQUIRED_DRIVER_CAPABILITY = "x87raw=1"
REPLACEMENT_ABI_MARKER = "src/game/x87_replace.h"
#: Set by `tools.replace prove` for the v2 proof so every child process (manifest, build,
#: mutate) compiles the roots for the raw-state backend. Never set by a default build.
ENV_SELECTED = "TSFP_X87_CONTRACT"
#: Proof builds compile the roots against the raw-state backend (x87_replace.h).
COMPILE_FLAGS = ("-DTSFP_X87_BACKEND_RAW=1",)

V2_DESCRIPTION: dict[str, object] = {
    "name": V2_NAME,
    "supersedes": None,
    "default": False,
    "arbiter": "intel-sdm-model-replaces-unicorn-x87-results",
    "modelled_instructions": list(MODELLED_INSTRUCTIONS),
    "state_compared": "full raw x87: 8 raw80 physical slots, tags, control, status (strict)",
    "fail_closed": "unmodelled instruction, unmasked exception or out-of-domain state refuses",
    "narrowing": "scratch eax/ecx/edx and the dead stack below entry esp only, as declared",
    "driver_capability": REQUIRED_DRIVER_CAPABILITY,
    "evidence_class": EVIDENCE_CLASS,
    "evidence_label": EVIDENCE_LABEL,
}


def replace_enabled() -> bool:
    return os.environ.get(ENV_SELECTED) == V2_NAME


def _digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extension_va_for(only_va: str | None) -> int | None:
    """The extension root selected by `--only-va`, or None for the four original roots."""
    if only_va is None:
        return None
    va = int(only_va, 16)
    return va if va in ROOT_EXTENSIONS else None


def arbiter_identity(root: Path = REPO, extension_va: int | None = None) -> dict[str, object]:
    """Sorted per-file sha256 of every model and harness file the arbiter consists of."""
    files = {name: _digest(root / name) for name in (*MODEL_FILES, *ARBITER_HARNESS_FILES)}
    identity: dict[str, object] = {
        "models": {n: files[n] for n in MODEL_FILES},
        "harness": {n: files[n] for n in ARBITER_HARNESS_FILES},
    }
    if extension_va is not None:
        if extension_va not in ROOT_EXTENSIONS:
            raise ValueError(f"no arbiter extension for root {extension_va:X}")
        identity["extension"] = {
            "root": f"{extension_va:X}",
            "files": {n: _digest(root / n) for n in ROOT_EXTENSIONS[extension_va]},
        }
    return identity


def fingerprint(identity: Mapping[str, object]) -> str:
    return hashlib.sha256(json.dumps(identity, sort_keys=True).encode("ascii")).hexdigest()


def current_fingerprint(root: Path = REPO, extension_va: int | None = None) -> str:
    return fingerprint(arbiter_identity(root, extension_va))


def unmet_prerequisites(root: Path = REPO) -> list[str]:
    """Reasons the production path cannot honour v2 yet. Empty only when it can."""
    reasons = []
    if not (root / REPLACEMENT_ABI_MARKER).is_file():
        reasons.append(
            f"no x87 replacement ABI ({REPLACEMENT_ABI_MARKER} absent): a registry replacement "
            "cannot leave its result on the guest x87 stack or produce the raw state"
        )
    return reasons


def select(
    name: str | None,
    acknowledgement: str | None,
    root: Path = REPO,
    extension_va: int | None = None,
) -> str:
    """Resolve the requested x87 comparison contract. Raises `ValueError` to refuse.

    None or v1 returns v1 and requires no acknowledgement (the default is untouched). v2 needs
    the exact name, the current arbiter fingerprint and every prerequisite met."""
    if name is None or name == V1_NAME:
        if acknowledgement is not None:
            raise ValueError("--x87-arbiter-ack is only valid with " + V2_NAME)
        return V1_NAME
    if name != V2_NAME:
        raise ValueError(f"unknown x87 contract {name!r}; known: {', '.join(CONTRACT_NAMES)}")
    expected = current_fingerprint(root, extension_va)
    if acknowledgement != expected:
        raise ValueError(
            f"{V2_NAME} must name its arbiter: pass --x87-arbiter-ack {expected} "
            "(fingerprint of the SDM model and comparator files)"
        )
    unmet = unmet_prerequisites(root)
    if unmet:
        raise ValueError(f"{V2_NAME} is not runnable yet: " + "; ".join(unmet))
    return V2_NAME


def receipt_block(root: Path = REPO, extension_va: int | None = None) -> dict[str, object]:
    """The block a v2 proof receipt must carry: contract, arbiter identity and fingerprint."""
    identity = arbiter_identity(root, extension_va)
    return {
        "contract": V2_NAME,
        "description": dict(V2_DESCRIPTION),
        "arbiter": identity,
        "arbiter_fingerprint": fingerprint(identity),
    }


def validate_receipt_block(block: object, root: Path = REPO) -> None:
    """Replay check. Refuses a missing, altered, v1-shaped or stale (model changed) block."""
    if not isinstance(block, dict) or set(block) != {
        "contract",
        "description",
        "arbiter",
        "arbiter_fingerprint",
    }:
        raise ValueError("missing or malformed x87 v2 receipt block")
    if block["contract"] != V2_NAME:
        raise ValueError("x87 v2 receipt block names another contract")
    if json.dumps(block["description"], sort_keys=True) != json.dumps(
        V2_DESCRIPTION, sort_keys=True
    ):
        raise ValueError("x87 v2 description differs from the predeclared contract")
    if block["arbiter_fingerprint"] != fingerprint(block["arbiter"]):  # type: ignore[arg-type]
        raise ValueError("x87 v2 arbiter fingerprint does not match its recorded identity")
    recorded = block["arbiter"]
    extension = recorded.get("extension") if isinstance(recorded, dict) else None
    extension_va = int(extension["root"], 16) if isinstance(extension, dict) else None
    if block["arbiter"] != arbiter_identity(root, extension_va):
        raise ValueError("x87 v2 arbiter files changed since the receipt was taken")


def reject_in_v1_document(document: object) -> None:
    """A v1/legacy document must never carry v2 provenance (no mixing)."""
    text = json.dumps(document, sort_keys=True)
    if V2_NAME in text:
        raise ValueError("v2 x87 provenance inside a document that does not select it")
