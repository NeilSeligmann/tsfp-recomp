"""T1573 mutation check: each guard of the implicit es: string-op test must be load-bearing."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

TARGET = Path("tools/call_draft_lists.py")
MUTANTS = {
    "prefix-guard": ("if item.prefix[1] != 0:", "if False:"),
    "mnemonic-guard": ("item.mnemonic.split()[-1] in {", "item.mnemonic.split()[-1] not in {"),
    "always-refuse": ("and not _implicit_es_string_operand_only(item)", "and True"),
    "never-refuse": (
        'if ":" in item.op_str and not _implicit_es_string_operand_only(item)',
        "if False",
    ),
}


def main() -> int:
    argparse.ArgumentParser(description=__doc__).parse_args()
    original = TARGET.read_text()
    survived = 0
    try:
        for name, (old, new) in MUTANTS.items():
            if old not in original:
                print(name, "ANCHOR-DRIFT")
                survived += 1
                continue
            TARGET.write_text(original.replace(old, new, 1))
            run = subprocess.run(
                [sys.executable, "-m", "pytest", "-q", "-x", "tests/test_call_draft_lists.py"],
                capture_output=True,
                timeout=120,
            )
            killed = run.returncode != 0
            survived += not killed
            print(name, "KILLED" if killed else "SURVIVED")
    finally:
        TARGET.write_text(original)
    return 1 if survived else 0


if __name__ == "__main__":
    raise SystemExit(main())
