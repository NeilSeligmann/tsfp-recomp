"""Mutation check of the T757 fail-closed rules in `tools/replace/audit.py`.

Each mutant disables or weakens one new check by an exact text replacement. The run applies
it to a scratch copy of the repository tree's audit module, runs the T757 test files and
requires a failure (the mutant is killed). A mutant whose anchor text is missing is an error,
so a drifted anchor cannot masquerade as a kill.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

AUDIT = Path("tools/replace/audit.py")
TESTS = (
    "tests/test_replace_stack_clean.py",
    "tests/test_replace_return_stack_review.py",
    "tests/test_replace_climb.py",
    "tests/test_replace_table_walker.py",
    "tests/test_t766_caller_audit_review.py",
    "tests/test_t779_interior_dword.py",
)

# (name, exact text in audit.py, replacement)
MUTANTS: tuple[tuple[str, str, str], ...] = (
    (
        "ret-balance",
        'if delta != 0:\n                    return dirty(f"ret at',
        'if False:\n                    return dirty(f"ret at',
    ),
    ("ret-n-agree", "if len(pops) > 1:", "if False:"),
    (
        "store-via-register",
        'return "store through a register that may alias the stack"',
        'return ""',
    ),
    ("store-above-slot", "if offset + operand.size > 0:", "if False:"),
    (
        "indexed-store",
        'if index:\n            return "indexed',
        'if False:\n            return "indexed',
    ),
    (
        "absolute-store",
        'if base == 0:\n            return ""',
        'if base == -1:\n            return ""',
    ),
    (
        "indirect-call",
        'if not direct:\n                    return dirty(f"indirect call',
        'if False:\n                    return dirty(f"indirect call',
    ),
    (
        "indirect-jump",
        'if not direct:\n                    return dirty(f"indirect jump',
        'if False:\n                    return dirty(f"indirect jump',
    ),
    ("esp-written", "if written & stack_regs:", "if False:"),
    (
        "register-store",
        'return "store through a register that may alias the stack"',
        'return ""',
    ),
    (
        "mov-esp",
        "if not isinstance(frame, int) or source.type != capstone.CS_OP_REG:",
        "if False:",
    ),
    (
        "pop-memory",
        'if operands[0].type != capstone.CS_OP_REG:\n                    return dirty(f"pop to',
        'if False:\n                    return dirty(f"pop to',
    ),
    (
        "unmodelled",
        'if name in _UNMODELLED_WRITERS or insn.mnemonic in _TRAPS or "int" in groups:',
        "if False:",
    ),
    ("recursion-pop", "if result.clean and result.pop != 0:", "if False:"),
    ("offset-bound", "if abs(delta) > MAX_CLEAN_OFFSET:", "if False:"),
    ("state-cap", "if len(visited) >= MAX_CLEAN_STATES:", "if False:"),
    ("prune", "if prune:\n                # R10:", "if False:\n                # R10:"),
    (
        "nested-ret-n",
        "(following, delta + sub.pop, after, frozenset(), taint, slack, saved, remaining)",
        "(following, delta, after, frozenset(), taint, slack, saved, remaining)",
    ),
    (
        "call-forgets-ebp",
        "after = frame if kept else None",
        "after = frame",
    ),
    (
        "pop-ebp-forgets",
        "frame = _ENTRY_EBP if saved == delta and not slack and width == 4 else None",
        "frame = frame",
    ),
    ("leave-offset", "delta, slack = frame + width, 0", "delta, slack = frame, 0"),
    ("mov-ebp-esp", "frame = delta if isesp and not slack else None", "frame = None"),
    ("store-is-write-only", "or not operand.access & 2:", "or False:"),
    (
        "callee-gate",
        "            body = pruned_audits[remaining].clean(target, True)\n"
        "            if not body.clean:",
        "            body = pruned_audits[remaining].clean(target, True)\n            if False:",
    ),
    ("frame-gate", "elif not (body := stack_audit.clean(frame, False)).clean:", "elif False:"),
    ("interior-tail-source", "if source != frame:", "if False:"),
    (
        "interior-tail-bound",
        "if target >= end:\n                break",
        "if False:\n                break",
    ),
    ("interior-call", "if frame < target < end:", "if False:"),
    ("interior-gate", "        if outside:\n", "        if False:\n"),
    # T779: a 4 byte copy of an interior address of the climbed body
    (
        "copy-gate",
        "        return self._interior_copy(frame, end)\n",
        '        return ""\n',
    ),
    (
        "copy-low",
        "_dwords_in_range(self._image.data, frame + 1, top - 1)",
        "_dwords_in_range(self._image.data, frame + 8, top - 1)",
    ),
    (
        "copy-high",
        "_dwords_in_range(self._image.data, frame + 1, top - 1)",
        "_dwords_in_range(self._image.data, frame + 1, top - 2)",
    ),
    (
        "copy-empty",
        '        if not found:\n            return ""\n',
        '        if True:\n            return ""\n',
    ),
    (
        "copy-entry-only",
        "_dwords_in_range(self._image.data, frame + 1, top - 1)",
        "_dwords_in_range(self._image.data, frame + 1, frame + 1)",
    ),
    ("copy-top", "top = min(end, self._image.base + len(self._image.data))", "top = frame + 2"),
    # T768: pointer provenance from a complete caller set
    ("pointer-tracking", "tracked = self._discharge is not None", "tracked = True"),
    (
        "origin-call-forgets",
        "(following, delta + sub.pop, after, frozenset(), taint, slack, saved, remaining)",
        "(following, delta + sub.pop, after, origin_pairs, taint, slack, saved, remaining)",
    ),
    (
        "origin-write-forgets",
        'origins.pop(_FULL_REGISTER.get(insn.reg_name(written) or "", ""), None)',
        "None",
    ),
    ("origin-copy", "origins[_FULL_REGISTER[copy[0]]] = copy[1]", "pass"),
    (
        "need-index",
        "if tracked and label is not None and not operand.mem.index:",
        "if tracked and label is not None:",
    ),
    ("need-high", "max(old[1], low + operand.size)", "old[1]"),
    ("need-low", "min(old[0], low)", "old[0]"),
    (
        "need-discharge",
        "            if why:\n                return dirty(",
        "            if False:\n                return dirty(",
    ),
    (
        "push-over-slot",
        ") and delta > 0:\n                # the pushed",
        ") and False:\n                # the pushed",
    ),
    (
        "call-over-slot",
        'if delta > 0:\n                    return dirty(f"call pushes',
        'if False:\n                    return dirty(f"call pushes',
    ),
    ("proof-entry", "if slot >= len(entries) or entries[slot] != entry:", "if False:"),
    (
        "proof-complete",
        'if callers is None:\n            return f"caller set of',
        'if False:\n            return f"caller set of',
    ),
    ("proof-direct-sites", "if not self._index.sites.get(entry):", "if False:"),
    (
        "proof-owner",
        'if owner is None:\n            return "owner of the call',
        'if False:\n            return "owner of the call',
    ),
    (
        "proof-interior",
        'if outside:\n            return f"{_hex(owner)} has an interior',
        'if False:\n            return f"{_hex(owner)} has an interior',
    ),
    ("proof-indirect-jump", "if first is None or first.type != 2:", "if False:"),
    ("proof-linear-end", "if address != end and end != 1 << 32:", "if False:"),
    (
        "proof-decodes",
        'if insn is None:\n                return f"{_hex(owner)} does not decode',
        'if False:\n                return f"{_hex(owner)} does not decode',
    ),
    ("proof-sweep-error", "if isinstance(body, str):", "if False:"),
    ("proof-call-found", 'if call < 0 or insns[call].mnemonic != "call":', "if False:"),
    ("proof-call-target", "or operand.imm & 0xFFFFFFFF != entry:", "or False:"),
    ("proof-call-label", "if insns[call].address in labels:", "if False:"),
    (
        "proof-run-label",
        'if insn.address in labels:\n                return f"{_hex(insn.address)} is a jump',
        'if False:\n                return f"{_hex(insn.address)} is a jump',
    ),
    (
        "proof-call-between",
        'if "call" in groups or "int" in groups or insn.mnemonic in _TRAPS:',
        'if "int" in groups or insn.mnemonic in _TRAPS:',
    ),
    (
        "proof-trap-between",
        'if "call" in groups or "int" in groups or insn.mnemonic in _TRAPS:',
        'if "call" in groups:',
    ),
    (
        "proof-mov",
        'insn.mnemonic != "mov"\n            or len(operands) != 2',
        "False\n            or len(operands) != 2",
    ),
    ("proof-width", "or operands[0].size != 4", "or False"),
    ("proof-immediate", "or operands[1].type != 2  # capstone.CS_OP_IMM", "or False"),
    ("proof-image-low", "if value + low < self._image.base or", "if False or"),
    ("proof-image-high", "or value + high > self._image.base + len(", "or False and len("),
)


def run_tests(root: Path) -> bool:
    """True when the T757 tests pass in `root`."""
    done = subprocess.run(
        [sys.executable, "-m", "pytest", "-q", "-x", "-p", "no:cacheprovider", *TESTS],
        cwd=root,
        capture_output=True,
        text=True,
        timeout=300,
        check=False,
    )
    return done.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", help="run only the mutant with this name")
    args = parser.parse_args()
    original = AUDIT.read_text(encoding="utf-8")
    if not run_tests(Path(".")):
        print("baseline T757 tests fail, nothing to mutate")
        return 2
    survived = []
    try:
        for name, anchor, mutant in MUTANTS:
            if args.only and name != args.only:
                continue
            if original.count(anchor) < 1:
                print(f"{name}: ANCHOR MISSING")
                return 2
            AUDIT.write_text(original.replace(anchor, mutant, 1), encoding="utf-8")
            killed = not run_tests(Path("."))
            print(f"{name}: {'killed' if killed else 'SURVIVED'}")
            if not killed:
                survived.append(name)
    finally:
        AUDIT.write_text(original, encoding="utf-8")
    print(f"{len(survived)} survived: {survived}")
    return 1 if survived else 0


if __name__ == "__main__":
    raise SystemExit(main())
