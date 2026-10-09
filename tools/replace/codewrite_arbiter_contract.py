# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in `faithful-codewrite-arbiter-v2` proof contract (T1508). OFF by default.

Styled on `x87_arbiter_contract.py` (T1510). Separately versioned from the inherited
`unicorn-per-case-reset-v1` behaviour (the original side is Unicorn with pristine memory plus a
translated-cache flush between cases, T1508 isolation), which this module neither imports for
mutation nor changes: every existing gate, receipt and replay stays byte-identical.

v2 differs in who is trusted for an original that REWRITES ITS OWN CODE inside one case. Unicorn
is measured to deviate there (T1508: `add [eax],al` stores bypass UC_HOOK_MEM_WRITE, in-block SMC
EFLAGS differ, partial-write rule). v2 names as arbiter the byte-accurate interpreter
`docs/evidence/t1508/scratch_family.c` (agrees with Unicorn on all captured authentic cases of the
covered roots, evidence MEASURED against Unicorn, INFERRED against hardware) and requires the
native replacement to be the GUARDED form: a fast path taken only when no store can reach the
window edge, the live bytes of the root or its callee, or the stack frame the callee returns
through, otherwise the exact interpreter (the original's behaviour, including self-modification).

It cannot be selected silently: it needs the exact contract name, an acknowledgement equal to the
fingerprint of the arbiter files named in `arbiter_identity()`, a root covered by the arbiter
and every production prerequisite. Today the production prerequisite is absent (no registry
replacement ABI for the guarded form, `src/game/codewrite_replace.h`), so selecting v2 refuses.
"""

from __future__ import annotations

import hashlib
import json
import os
from collections.abc import Mapping
from pathlib import Path

V1_NAME = "unicorn-per-case-reset-v1"
V2_NAME = "faithful-codewrite-arbiter-v2"
CONTRACT_NAMES = (V1_NAME, V2_NAME)
DEFAULT_NAME = V1_NAME
EVIDENCE_CLASS = "codewrite-arbiter"
EVIDENCE_LABEL = "MEASURED-vs-Unicorn/INFERRED-vs-hardware"

REPO = Path(__file__).resolve().parents[2]
#: The arbiter itself: the interpreter (and its guarded native model) and its predeclared harness.
ARBITER_FILES = (
    "docs/evidence/t1508/scratch_family.c",
    "docs/evidence/t1508/compare_family.py",
)
#: Harness pieces the arbiter comparison is built on.
ARBITER_HARNESS_FILES = (
    "docs/evidence/t1508/compare_root.py",
    "docs/evidence/t1508/compare_prefix.py",
)
#: Roots whose original and callee bytes the arbiter interprets, with the callee pair.
COVERED_ROOTS: dict[int, int] = {0x003BFA40: 0x003BB650, 0x003AA1D0: 0x003A9D20}
#: Byte length of each covered root (the original bytes are authenticated from the image).
COVERED_ROOT_SIZES: dict[int, int] = {0x003BFA40: 43, 0x003AA1D0: 46}
#: Constructor-family roots NOT covered yet, with the reason (never silently treated as covered).
UNCOVERED_ROOTS: dict[int, str] = {
    0x003C97AB: "no captured fixed-matrix cases in the tree, interpreter subset unmeasured",
    0x0040F720: "no captured fixed-matrix cases in the tree, 80-iteration fill loop not in subset",
    0x003C2260: "no captured cases, three guest calls and a 16-byte digest not in subset",
}
# The three roots above are NOT covered by the family arbiter (scratch_family.c); the second pass
# covers them, and four more, with the general arbiter listed in GENERAL_ROOTS below.
#: Second pass (pointer-chasing roots): the general arbiter `scratch_general.c` (the exact
#: interpreter `src/game/codewrite_x86.h` compiled standalone) is the arbiter, validated against
#: Unicorn by `compare_general.py` on the authentic cases of `generate_general_cases.py`.
GENERAL_ARBITER_FILES = (
    "docs/evidence/t1508/scratch_general.c",
    "docs/evidence/t1508/compare_general.py",
    "docs/evidence/t1508/emit_spans.py",
    "src/game/codewrite_x86.h",
)
GENERAL_ROOTS: tuple[int, ...] = (
    0x003C97AB,
    0x0040F720,
    0x003C2260,
    0x0038AEB0,
    0x0038AF20,
    0x0038C010,
    0x003B9BC0,
)
REPLACEMENT_ABI_MARKER = "src/game/codewrite_replace.h"
#: Driver READY capability the v2 proof needs from the subject (HARNESS_CODEWRITE build).
REQUIRED_DRIVER_CAPABILITY = "codewrite=1"
#: Set by `tools.replace prove` for the v2 proof so every child process (manifest, build, mutate)
#: compiles the roots with the proof stop channel. Never set by a default build.
ENV_SELECTED = "TSFP_CODEWRITE_CONTRACT"
#: Proof builds compile the roots against the harness stop channel and the 16 MiB harness window.
COMPILE_FLAGS = ("-DTSFP_CODEWRITE_PROOF=1",)

V2_DESCRIPTION: dict[str, object] = {
    "name": V2_NAME,
    "supersedes": None,
    "default": False,
    "arbiter": "byte-accurate-interpreter-replaces-unicorn-for-within-case-code-writes",
    "covered_roots": sorted(f"{va:08X}" for va in COVERED_ROOTS),
    "native_form": "guarded fast path, exact interpreter fallback",
    "guard": "window edge, live root/callee bytes and callee return frame must not be stored to",
    "state_compared": "8 GPR, EFLAGS, x87, XMM, EIP, fault kind, executed count, 16 MiB memory",
    "fail_closed": "unknown opcode, uncovered root, changed arbiter or missing ABI refuses",
    "narrowing": "none: no register, flag or memory byte is ignored",
    "evidence_class": EVIDENCE_CLASS,
    "evidence_label": EVIDENCE_LABEL,
}


def replace_enabled() -> bool:
    return os.environ.get(ENV_SELECTED) == V2_NAME


def _digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def arbiter_identity(root: Path = REPO) -> dict[str, object]:
    """Sorted per-file sha256 of every file the arbiter consists of."""
    files = {name: _digest(root / name) for name in (*ARBITER_FILES, *ARBITER_HARNESS_FILES)}
    return {
        "arbiter": {n: files[n] for n in ARBITER_FILES},
        "harness": {n: files[n] for n in ARBITER_HARNESS_FILES},
    }


def general_identity(root: Path = REPO) -> dict[str, object]:
    files = {
        name: _digest(root / name) for name in (*GENERAL_ARBITER_FILES, *ARBITER_HARNESS_FILES)
    }
    return {
        "arbiter": {n: files[n] for n in GENERAL_ARBITER_FILES},
        "harness": {n: files[n] for n in ARBITER_HARNESS_FILES},
    }


def general_fingerprint(root: Path = REPO) -> str:
    return fingerprint(general_identity(root))


def fingerprint(identity: Mapping[str, object]) -> str:
    return hashlib.sha256(json.dumps(identity, sort_keys=True).encode("ascii")).hexdigest()


def current_fingerprint(root: Path = REPO) -> str:
    return fingerprint(arbiter_identity(root))


def unmet_prerequisites(root: Path = REPO) -> list[str]:
    """Reasons the production path cannot honour v2 yet. Empty only when it can."""
    reasons = []
    if not (root / REPLACEMENT_ABI_MARKER).is_file():
        reasons.append(
            f"no guarded replacement ABI ({REPLACEMENT_ABI_MARKER} absent): a registry "
            "replacement cannot yet run the guard and fall back to the exact interpreter"
        )
    return reasons


def select(
    name: str | None,
    acknowledgement: str | None,
    root: Path = REPO,
    *,
    root_va: int | None = None,
) -> str:
    """Resolve the requested code-write contract. Raises `ValueError` to refuse.

    None or v1 returns v1 and requires no acknowledgement (the default is untouched). v2 needs
    the exact name, the current arbiter fingerprint, a covered root (when one is named) and every
    prerequisite met."""
    if name is None or name == V1_NAME:
        if acknowledgement is not None:
            raise ValueError("--codewrite-arbiter-ack is only valid with " + V2_NAME)
        return V1_NAME
    if name != V2_NAME:
        raise ValueError(
            f"unknown code-write contract {name!r}; known: {', '.join(CONTRACT_NAMES)}"
        )
    expected = (
        general_fingerprint(root)
        if root_va is not None and root_va in GENERAL_ROOTS
        else current_fingerprint(root)
    )
    if acknowledgement != expected and not (
        root_va is None and acknowledgement == general_fingerprint(root)
    ):
        raise ValueError(
            f"{V2_NAME} must name its arbiter: pass --codewrite-arbiter-ack {expected} "
            "(fingerprint of the interpreter and comparator files)"
        )
    if root_va is not None and root_va not in COVERED_ROOTS and root_va not in GENERAL_ROOTS:
        reason = UNCOVERED_ROOTS.get(root_va, "not a constructor-family root")
        raise ValueError(f"{V2_NAME} does not cover root {root_va:08X}: {reason}")
    unmet = unmet_prerequisites(root)
    if unmet:
        raise ValueError(f"{V2_NAME} is not runnable yet: " + "; ".join(unmet))
    return V2_NAME


def general_receipt_block(root: Path = REPO) -> dict[str, object]:
    """Receipt block of a general-arbiter (second pass) root: same contract, other arbiter."""
    identity = general_identity(root)
    return {
        "contract": V2_NAME,
        "variant": "general",
        "description": dict(V2_DESCRIPTION),
        "arbiter": identity,
        "arbiter_fingerprint": fingerprint(identity),
    }


def receipt_block(root: Path = REPO) -> dict[str, object]:
    """The block a v2 proof receipt must carry: contract, arbiter identity and fingerprint."""
    identity = arbiter_identity(root)
    return {
        "contract": V2_NAME,
        "description": dict(V2_DESCRIPTION),
        "arbiter": identity,
        "arbiter_fingerprint": fingerprint(identity),
    }


def validate_receipt_block(block: object, root: Path = REPO) -> None:
    """Replay check. Refuses a missing, altered, v1-shaped or stale (arbiter changed) block."""
    if isinstance(block, dict) and block.get("variant") == "general":
        if set(block) != {"contract", "variant", "description", "arbiter", "arbiter_fingerprint"}:
            raise ValueError("malformed general code-write v2 receipt block")
        if block["arbiter_fingerprint"] != fingerprint(block["arbiter"]):  # type: ignore[arg-type]
            raise ValueError(
                "code-write v2 arbiter fingerprint does not match its recorded identity"
            )
        if block["arbiter"] != general_identity(root):
            raise ValueError("code-write v2 general arbiter files changed since the receipt")
        return
    if not isinstance(block, dict) or set(block) != {
        "contract",
        "description",
        "arbiter",
        "arbiter_fingerprint",
    }:
        raise ValueError("missing or malformed code-write v2 receipt block")
    if block["contract"] != V2_NAME:
        raise ValueError("code-write v2 receipt block names another contract")
    if json.dumps(block["description"], sort_keys=True) != json.dumps(
        V2_DESCRIPTION, sort_keys=True
    ):
        raise ValueError("code-write v2 description differs from the predeclared contract")
    if block["arbiter_fingerprint"] != fingerprint(block["arbiter"]):  # type: ignore[arg-type]
        raise ValueError("code-write v2 arbiter fingerprint does not match its recorded identity")
    if block["arbiter"] != arbiter_identity(root):
        raise ValueError("code-write v2 arbiter files changed since the receipt was taken")


def reject_in_v1_document(document: object) -> None:
    """A v1/legacy document must never carry v2 provenance (no mixing)."""
    if V2_NAME in json.dumps(document, sort_keys=True):
        raise ValueError("v2 code-write provenance inside a document that does not select it")
