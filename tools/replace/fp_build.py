# SPDX-License-Identifier: GPL-3.0-or-later
"""T1620 fp-scalar-v1: build-flag guard for the subject that runs the replacement.

A scalar-float proof only means something if the replacement is compiled with IEEE-754
single-precision semantics: no fast-math, no contraction into fused multiply-add, no x87
excess precision. This module

* defines the extra flags the fp mode adds (`-ffp-contract=off -fno-fast-math`),
* rejects any flag or link input that would move the subject to another FP domain
  (`-ffast-math`, `-Ofast`, `crtfastmath.o`, `-march=native`, `-mfma`, `-mfpmath=387`, ...),
* disassembles the compiled replacement objects and fails on a fused multiply-add or an x87
  instruction,
* compiles every vector source (it references `g_xmm`) again at production-like flags
  (`-std=c11` at -O0, -O2, -O3, the CMake default dialect) and scans that too, and
* records everything in a JSON record that the harness embeds in the proof receipt.

    python -m tools.replace.fp_build --game-dir DIR --objects OBJ... --opt-flag -O0 --out REC.json
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from collections.abc import Sequence
from pathlib import Path

#: flags the fp-scalar-v1 subject compile adds on top of the project flags
FP_COMPILE_FLAGS = ("-ffp-contract=off", "-fno-fast-math")
#: substrings that are never allowed in a compile or link command of the fp mode
FORBIDDEN = (
    "-ffast-math",
    "-Ofast",
    "-funsafe-math-optimizations",
    "-ffinite-math-only",
    "-fassociative-math",
    "-freciprocal-math",
    "-fno-signed-zeros",
    "-fno-trapping-math",
    "-ffp-contract=fast",
    "-ffp-contract=on",
    "-march=native",
    "-mtune=native",
    "-mfma",
    "-mavx",
    "-mfpmath=387",
    "-mno-sse",
    "-m80387",
    "-mfused-madd",
    "crtfastmath",
)
#: production-like dialect/optimisation pairs the vector sources are re-compiled at
PRODUCTION_FLAGS: tuple[tuple[str, ...], ...] = (
    ("-std=c11", "-O0"),
    ("-std=c11", "-O2"),
    ("-std=c11", "-O3"),
)
FMA = re.compile(r"\bv?f(?:n?m(?:add|sub)|m(?:addsub|subadd))\d*[sp][sd]\b")
X87 = re.compile(
    r"\b(?:fld|fst|fstp|fild|fist|fistp|fadd|faddp|fsub|fsubp|fsubr|fmul|fmulp|fdiv|fdivp|fdivr"
    r"|fxch|fchs|fabs|fsqrt|fcom|fcomp|fcomi|fcomip|fucom|fucomp|fucomi|fucomip|fnstcw|fldcw"
    r"|fnstsw|fprem|frndint|fscale|fyl2x)\w*\b"
)


def check_flags(argv: Sequence[str]) -> list[str]:
    """Violations in one compile/link argument vector (empty when clean)."""
    return [
        f"forbidden {bad!r} in {' '.join(argv)}" for bad in FORBIDDEN if any(bad in a for a in argv)
    ]


def scan_disassembly(text: str) -> dict[str, int]:
    """Count fused multiply-add and x87 mnemonics in `objdump -d` text."""
    fma = x87 = 0
    for line in text.splitlines():
        fields = line.split("\t")
        instruction = fields[-1].strip() if len(fields) >= 2 else ""
        if not instruction:
            continue
        mnemonic = instruction.split()[0]
        if FMA.fullmatch(mnemonic):
            fma += 1
        elif X87.fullmatch(mnemonic):
            x87 += 1
    return {"fma": fma, "x87": x87}


def _objdump(path: Path) -> str:
    completed = subprocess.run(
        ["objdump", "-d", "--no-show-raw-insn", str(path)],
        capture_output=True,
        text=True,
        timeout=300,
        check=True,
    )
    return completed.stdout


def vector_sources(game_dir: Path) -> list[Path]:
    """Sources that touch the vector TLS slots (the admitted replacements)."""
    return [
        path
        for path in sorted(game_dir.glob("*.c"))
        if "g_xmm" in path.read_text(encoding="utf-8", errors="replace")
    ]


def build_record(
    game_dir: Path,
    objects: Sequence[Path],
    opt_flag: str,
    *,
    cc: str = "cc",
    compile_flags: Sequence[str] = (),
) -> dict[str, object]:
    """Check `compile_flags`, scan the subject `objects`, re-compile at production flags."""
    failures = check_flags(list(compile_flags))
    # the subject link line and driver compile live in build_subject.sh, and a CFLAGS/LDFLAGS
    # environment would silently reach the compiler wrapper: scan both for fast-math inputs
    script = Path(__file__).resolve().parents[1] / "harness" / "build_subject.sh"
    failures += check_flags(script.read_text(encoding="utf-8").split())
    environment = ("CC", "CFLAGS", "LDFLAGS", "CPPFLAGS", "REPL_CFLAGS")
    failures += check_flags([os.environ.get(name, "") for name in environment])
    counts = {"fma": 0, "x87": 0}
    scanned = 0
    # Only the vector bodies are claimed float-faithful. Other replacements may legitimately use
    # long double (x87), so their objects are not scanned.
    stems = {path.stem for path in vector_sources(game_dir)}
    for obj in objects:
        if obj.stem not in stems:
            continue
        found = scan_disassembly(_objdump(obj))
        counts["fma"] += found["fma"]
        counts["x87"] += found["x87"]
        scanned += 1
    production: list[dict[str, object]] = []
    sources = vector_sources(game_dir)
    with tempfile.TemporaryDirectory(prefix="fp-build-") as scratch:
        for flags in PRODUCTION_FLAGS:
            totals = {"fma": 0, "x87": 0}
            command_flags = [*flags, "-fPIE", *FP_COMPILE_FLAGS, f"-I{game_dir}"]
            failures += check_flags(command_flags)
            for source in sources:
                obj = Path(scratch) / f"{source.stem}{''.join(flags)}.o"
                completed = subprocess.run(
                    [cc, *command_flags, "-c", str(source), "-o", str(obj)],
                    capture_output=True,
                    text=True,
                    timeout=600,
                    check=False,
                )
                if completed.returncode != 0:
                    failures.append(f"production compile failed {source.name} {' '.join(flags)}")
                    continue
                found = scan_disassembly(_objdump(obj))
                totals["fma"] += found["fma"]
                totals["x87"] += found["x87"]
            production.append({"flags": list(flags), "sources": len(sources), **totals})
    for row in production:
        if row["fma"] or row["x87"]:
            failures.append(f"production flags {row['flags']}: fma={row['fma']} x87={row['x87']}")
    if counts["fma"] or counts["x87"]:
        failures.append(f"subject objects: fma={counts['fma']} x87={counts['x87']}")
    return {
        "mode": "fp-scalar-v1",
        "compiler": cc,
        "subject_flags": ["-std=c11", opt_flag, "-fPIE", *FP_COMPILE_FLAGS],
        "link": "-pie (no crtfastmath, no -ffast-math)",
        "driver_flags": "tools/harness/build_subject.sh: -O0 -std=gnu11 -fno-strict-aliasing "
        "SSE2 baseline (no -march, no FMA possible)",
        "subject_objects_scanned": scanned,
        "subject_scan": counts,
        "production": production,
        "game_dir_sha256": hashlib.sha256(
            b"".join(p.read_bytes() for p in sorted(game_dir.glob("*.c")))
        ).hexdigest(),
        "failures": failures,
        "passed": not failures,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--game-dir", type=Path, required=True)
    parser.add_argument("--objects", type=Path, nargs="*", default=[])
    parser.add_argument("--opt-flag", default="-O0")
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args(argv)
    record = build_record(
        args.game_dir,
        args.objects,
        args.opt_flag,
        cc=args.cc,
        compile_flags=["-std=c11", args.opt_flag, "-fPIE", *FP_COMPILE_FLAGS],
    )
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({k: record[k] for k in ("passed", "failures")}))
    return 0 if record["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
