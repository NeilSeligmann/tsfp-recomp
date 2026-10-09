# SPDX-License-Identifier: GPL-3.0-or-later
"""Schema preflight for T1478 appendix drafts, before any proof tuple.

For each draft: the registration scanner must accept exactly the declared registration
(`GAME_REPLACE_EXACT` or `_EXACT_INPUTS`, VA upper case, return token `u32` or `void`),
the registration must match the census row ABI, and the file must compile with the strict
warning set at O0, O2 and O3. Prints one line per draft and exits 1 on any failure.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from tools.replace.scan import scan_text

LIST = "docs/data/t1772-replacement-census/band-T1478/appendix-previously-mentioned.json"


def check(draft: Path, rows: dict[int, dict]) -> list[str]:
    problems: list[str] = []
    registrations = scan_text(draft.read_text(), draft.name)
    if len(registrations) != 1:
        return [f"{draft.name}: {len(registrations)} registrations, expected 1"]
    reg = registrations[0]
    row = rows.get(reg.va)
    if row is None:
        return [f"{draft.name}: VA {reg.va:08X} not in the appendix"]
    if not reg.exact_registers:
        problems.append("registration is not EXACT")
    if reg.returns not in ("u32", "void"):
        problems.append(f"return token {reg.returns}")
    abi = row["abi"]
    if reg.convention != abi["convention"] or reg.stack_args != abi["stack_args"]:
        problems.append(f"ABI {reg.convention}/{reg.stack_args} != row {abi}")
    wanted = tuple(abi.get("register_inputs") or ()) or None
    if (reg.register_inputs or None) != wanted:
        problems.append(f"register inputs {reg.register_inputs} != {wanted}")
    with tempfile.TemporaryDirectory() as tmp:
        for opt in (0, 2, 3):
            command = ["cc", "-std=c11", f"-O{opt}", "-Wall", "-Wextra", "-Werror", "-fPIE"]
            command += ["-Isrc/game", "-c", "-x", "c", str(draft), "-o", f"{tmp}/o{opt}.o"]
            result = subprocess.run(command, capture_output=True, text=True, timeout=90)
            if result.returncode:
                problems.append(f"O{opt} compile failed: {result.stderr.strip()[:400]}")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("drafts", nargs="+", type=Path)
    parser.add_argument("--list", default=LIST)
    args = parser.parse_args()
    rows = {int(r["va"], 16): r for r in json.loads(Path(args.list).read_text())}
    failed = False
    for draft in args.drafts:
        problems = check(draft, rows)
        print(draft.name, "PASS" if not problems else "FAIL " + "; ".join(problems))
        failed |= bool(problems)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
