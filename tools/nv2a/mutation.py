# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501  (mutation patterns are literal source text and cannot be wrapped)
"""Mutation test of the translator, the interpreter and the validation itself.

A mutation is one textual change to `translate.py` or `interp.py`. It is applied to a COPY
of the package in a scratch directory (never to the checkout, so concurrent work is not
disturbed and no stale `.pyc` can hide a mutation), the numeric validation is run against
the copy, and the mutant is KILLED when the validation reports a failure. A survivor is
either a gap in the validation or an equivalent mutant, and the report says which survived
so each can be judged by hand.

If the translator and the interpreter shared a bug the validation would prove nothing. This
is the check that it can fail: a wrong opcode semantic injected into either side must
surface as a mismatch.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

PACKAGE = Path("tools/nv2a")
TIMEOUT_SECONDS = 1800


@dataclass(frozen=True)
class Mutation:
    label: str
    target: str  # file name inside tools/nv2a
    old: str
    new: str
    #: `--output-init` the validation runs with (T561). The "nv" mutants need it, a mutation of
    #: the "nv" start value is invisible to a run that never selects it.
    output_init: str = "zero"


def _t(label: str, old: str, new: str) -> Mutation:
    return Mutation(f"translate: {label}", "translate.py", old, new)


def _i(label: str, old: str, new: str) -> Mutation:
    return Mutation(f"interp: {label}", "interp.py", old, new)


def _tn(label: str, old: str, new: str) -> Mutation:
    """A translator mutation judged by a validation run with `--output-init nv` (T561)."""
    return Mutation(f"translate: {label}", "translate.py", old, new, "nv")


def _in(label: str, old: str, new: str) -> Mutation:
    """An interpreter mutation judged by a validation run with `--output-init nv` (T561)."""
    return Mutation(f"interp: {label}", "interp.py", old, new, "nv")


MUTATIONS: tuple[Mutation, ...] = (
    _t(
        "DP3 gains the w term",
        'src_a.z * src_b.z)",\n    "DPH"',
        'src_a.z * src_b.z + src_a.w * src_b.w)",\n    "DPH"',
    ),
    _t(
        "DP4 drops the w term",
        'src_a.z * src_b.z + src_a.w * src_b.w)",\n    "DST"',
        'src_a.z * src_b.z)",\n    "DST"',
    ),
    _t(
        "DPH adds a.w instead of b.w",
        "src_a.z * src_b.z + src_b.w)",
        "src_a.z * src_b.z + src_a.w)",
    ),
    _t("ADD reads B instead of C", '"ADD": "src_a + src_c"', '"ADD": "src_a + src_b"'),
    _t("MAD drops the addend", '"MAD": "src_a * src_b + src_c"', '"MAD": "src_a * src_b"'),
    _t("MUL becomes ADD", '"MUL": "src_a * src_b"', '"MUL": "src_a + src_b"'),
    _t("MOV reads B", '"MOV": "src_a",', '"MOV": "src_b",'),
    _t(
        "DST y uses a.y * b.z",
        "src_a.y * src_b.y, src_a.z, src_b.w",
        "src_a.y * src_b.z, src_a.z, src_b.w",
    ),
    _t(
        "DST w takes a.w",
        "src_a.y * src_b.y, src_a.z, src_b.w",
        "src_a.y * src_b.y, src_a.z, src_a.w",
    ),
    _t(
        "SLT becomes SLE",
        '"SLT": "vec4(lessThan(src_a, src_b))"',
        '"SLT": "vec4(lessThanEqual(src_a, src_b))"',
    ),
    _t(
        "SGE becomes GT",
        '"SGE": "vec4(greaterThanEqual(src_a, src_b))"',
        '"SGE": "vec4(greaterThan(src_a, src_b))"',
    ),
    _t(
        "MIN picks the larger",
        '"MIN": "mix(src_b, src_a, lessThan(src_a, src_b))"',
        '"MIN": "mix(src_b, src_a, greaterThan(src_a, src_b))"',
    ),
    _t(
        "MAX picks the smaller",
        '"MAX": "mix(src_b, src_a, greaterThanEqual(src_a, src_b))"',
        '"MAX": "mix(src_b, src_a, lessThan(src_a, src_b))"',
    ),
    _t("ILU MOV reads x only", '"MOV": ("src_c", None)', '"MOV": ("vec4(src_c.x)", None)'),
    _t(
        "RCP uses the y component",
        '"RCP": ("vec4(nv_rcp(src_c.x))"',
        '"RCP": ("vec4(nv_rcp(src_c.y))"',
    ),
    _t(
        "RCP of zero is +inf always",
        "(floatBitsToUint(x) & 0x80000000u) | 0x7f800000u",
        "0x7f800000u",
    ),
    _t("RCC lower clamp raised", "uintBitsToFloat(@MIN@)", "uintBitsToFloat(0x3f800000u)"),
    _t(
        "RCC drops the sign",
        "return uintBitsToFloat(floatBitsToUint(magnitude) | sign_bit);",
        "return magnitude;",
    ),
    _t(
        "RSQ drops the absolute value",
        "float magnitude = abs(x);\n    if (magnitude == 0.0) {\n        return NV_INF;",
        "float magnitude = x;\n    if (magnitude == 0.0) {\n        return NV_INF;",
    ),
    _t("EXP y is x - ceil", "x - whole, exp2(x)", "x - ceil(x), exp2(x)"),
    _t("EXP z uses the floor", "x - whole, exp2(x), 1.0", "x - whole, exp2(whole), 1.0"),
    _t("EXP w is 0", "exp2(x), 1.0);\n}", "exp2(x), 0.0);\n}"),
    _t("LOG exponent off by one", "float(exponent - 1) - bias", "float(exponent) - bias"),
    _t("LOG mantissa unscaled", "fraction * 2.0", "fraction"),
    _t("LOG z is the natural log", "log2(magnitude) - bias", "log(magnitude) - bias"),
    _t("LIT diffuse not clamped", "if (diffuse < 0.0) {\n        diffuse = 0.0;\n    }", ""),
    _t(
        "LIT drops the zero power rule",
        "} else if (power == 0.0) {\n            term = 1.0;",
        "} else if (power == 12345.0) {\n            term = 1.0;",
    ),
    _t(
        "LIT z ignores the diffuse test",
        "if (diffuse > 0.0) {\n        // [NV] z",
        "if (diffuse > -1.0) {\n        // [NV] z",
    ),
    _t("ARL rounds to nearest", 'ARL_ROUNDING = "floor"', 'ARL_ROUNDING = "nearest"'),
    _t(
        "relative offset subtracts",
        'f"nv_c_at({decoded.const} + a0)"',
        'f"nv_c_at({decoded.const} - a0)"',
    ),
    _t("relative read bound too low", "index >= @CONSTANT_COUNT@", "index >= 100"),
    _t("negate ignored", 'return f"-{base}" if source.negate else base', "return base"),
    _t(
        "write mask bit order reversed",
        '((8, "x"), (4, "y"), (2, "z"), (1, "w"))',
        '((1, "x"), (2, "y"), (4, "z"), (8, "w"))',
    ),
    _t(
        "MAC temporary mask loses a component",
        '_write(target, decoded.mac_mask, "mac_result")',
        '_write(target, decoded.mac_mask & 7, "mac_result")',
    ),
    _t(
        "output sourced from the wrong unit",
        'result = "ilu_result" if decoded.out_is_ilu else "mac_result"',
        'result = "mac_result" if decoded.out_is_ilu else "ilu_result"',
    ),
    _t(
        "input register index shifted",
        'base = f"v[{decoded.input}]"',
        'base = f"v[{decoded.input} + 1]"',
    ),
    _t(
        "constant index shifted", 'base = f"c[{decoded.const}]"', 'base = f"c[{decoded.const} + 1]"'
    ),
    _t(
        "operand B reads the A swizzle",
        "_swizzle_text(source.swizzle)",
        "_swizzle_text(decoded.a.swizzle)",
    ),
    _t(
        "FINAL ignored",
        "        if decoded.final:\n            if translated",
        "        if False:\n            if translated",
    ),
    _i(
        "DP3 sums four terms",
        'return [_dot(a, b, 3, model)] * 4\n    if name == "DPH"',
        'return [_dot(a, b, 4, model)] * 4\n    if name == "DPH"',
    ),
    _i(
        "MAD multiplies by C",
        "_add(_mul(a[i], b[i], model), c[i])",
        "_add(_mul(a[i], c[i], model), c[i])",
    ),
    _i(
        "SLT uses less-equal",
        "_less(a[i], b[i], model) else 0.0",
        "_less(a[i], b[i], model) or a[i] == b[i] else 0.0",
    ),
    _i(
        "ARL truncates",
        "max(INT32_MIN, min(INT32_MAX, math.floor(value)))",
        "max(INT32_MIN, min(INT32_MAX, math.trunc(value)))",
    ),
    _i(
        "RSQ drops the absolute value",
        "magnitude = abs(value)\n    if magnitude == 0.0:\n        return math.inf\n    if magnitude == math.inf:\n        return 0.0\n    return _arith(1.0 / math.sqrt(magnitude))",
        "magnitude = value\n    if magnitude == 0.0:\n        return math.inf\n    if magnitude == math.inf:\n        return 0.0\n    return _arith(1.0 / math.sqrt(abs(magnitude)) if magnitude > 0 else -1.0 / math.sqrt(abs(magnitude)))",
    ),
    _i(
        "RCC clamp swapped",
        "magnitude = min(max(abs(result), RCC_MIN), RCC_MAX)",
        "magnitude = min(max(abs(result), RCC_MAX), RCC_MIN)",
    ),
    _i("DST y takes a.y only", "_mul(a[1], b[1], model), a[2], b[3]]", "a[1], a[2], b[3]]"),
    _i("write mask bit order reversed", "(mask >> (3 - component)) & 1", "(mask >> component) & 1"),
    _i(
        "relative offset ignored",
        "inst.xfctx_addr + (address if inst.xfctx_indexed else 0)",
        "inst.xfctx_addr",
    ),
    _i(
        "negate ignored",
        "return [-value for value in swizzled] if source.neg else swizzled",
        "return swizzled",
    ),
    _i(
        "output routed from the wrong unit",
        "routed = ilu_result if inst.out_is_sca else mac_result",
        "routed = mac_result if inst.out_is_sca else ilu_result",
    ),
    _i(
        "EXP y wrong",
        "_arith(value - floor), _arith(2.0**value), 1.0]",
        "_arith(floor - value), _arith(2.0**value), 1.0]",
    ),
    _i("LOG exponent off by one", "exponent -= 1", "exponent -= 0"),
    _i(
        "LIT power clamp wrong",
        "if power < -LIT_POWER_LIMIT:\n        power = -LIT_POWER_LIMIT",
        "if power < -1.0:\n        power = -LIT_POWER_LIMIT",
    ),
    _i(
        "MIN compares the wrong way",
        "a[i] if a[i] < b[i] else b[i]",
        "a[i] if a[i] > b[i] else b[i]",
    ),
    _tn(
        "nv output start value loses its w",
        '"nv": "vec4(0.0, 0.0, 0.0, 1.0)",',
        '"nv": "vec4(0.0)",',
    ),
    _tn(
        "nv output start value puts the one in z",
        '"nv": "vec4(0.0, 0.0, 0.0, 1.0)",',
        '"nv": "vec4(0.0, 0.0, 1.0, 0.0)",',
    ),
    _tn(
        "compute form ignores the output start option",
        "o[i] = {_OUTPUT_INIT_MARK};",
        "o[i] = vec4(0.0);",
    ),
    _tn(
        "compute form starts the outputs at one",
        "_COMPUTE_MAIN.replace(_OUTPUT_INIT_MARK, output_init_text(output_init))",
        '_COMPUTE_MAIN.replace(_OUTPUT_INIT_MARK, "vec4(1.0)")',
    ),
    _in(
        "nv output start value loses its w",
        '"nv": (0.0, 0.0, 0.0, 1.0),',
        '"nv": (0.0, 0.0, 0.0, 0.0),',
    ),
    _in(
        "output start value ignores the option",
        "outputs = [list(OUTPUT_INITS[output_init]) for _ in range(NUM_OUTPUTS)]",
        'outputs = [list(OUTPUT_INITS["zero"]) for _ in range(NUM_OUTPUTS)]',
    ),
    _in(
        "only o0 takes the output start value",
        "outputs = [list(OUTPUT_INITS[output_init]) for _ in range(NUM_OUTPUTS)]",
        "outputs = [list(OUTPUT_INITS[output_init] if _ == 0 else (0.0,) * 4) for _ in range(NUM_OUTPUTS)]",
    ),
    _i(
        "ILU reads C after the MAC write",
        "        if mac_result is not None:\n            _write_masked(temps[inst.dst], mac_result, inst.dst_wm_vec)\n",
        "        if mac_result is not None:\n            _write_masked(temps[inst.dst], mac_result, inst.dst_wm_vec)\n            c = read(inst.src2, inst)\n            ilu_result = _ilu(ilu_name, c, model)\n",
    ),
)


def apply(mutation: Mutation, source: Path, destination: Path) -> None:
    """Copy `tools/` pieces needed to run the validation into `destination` and mutate."""
    (destination / "tools").mkdir(parents=True)
    shutil.copy2("tools/__init__.py", destination / "tools" / "__init__.py")
    shutil.copytree(
        source, destination / "tools" / "nv2a", ignore=shutil.ignore_patterns("__pycache__")
    )
    target = destination / "tools" / "nv2a" / mutation.target
    text = target.read_text()
    if text.count(mutation.old) != 1:
        raise ValueError(f"{mutation.label}: pattern occurs {text.count(mutation.old)} times")
    target.write_text(text.replace(mutation.old, mutation.new))
    # The runner binary and the corpus are shared and read-only for the mutant.
    (destination / "generated").symlink_to(Path("generated").resolve())


def run_one(mutation: Mutation, programs: list[str], vectors: int, seed: int) -> dict[str, object]:
    with tempfile.TemporaryDirectory(prefix="nv2a-mutant-") as scratch:
        root = Path(scratch)
        try:
            apply(mutation, PACKAGE, root)
        except ValueError as error:
            return {"label": mutation.label, "outcome": "not-applied", "detail": str(error)}
        command = [
            sys.executable,
            "-B",
            "-m",
            "tools.nv2a.validate",
            "--programs",
            *[str(Path(p).resolve()) for p in programs],
            "--vectors",
            str(vectors),
            "--seed",
            str(seed),
            "--work",
            str(root / "work"),
            "--model",
            "ieee",
            "--examples",
            "1",
            "--output-init",
            mutation.output_init,
        ]
        try:
            completed = subprocess.run(
                command,
                cwd=root,
                env={
                    "PYTHONPATH": str(root),
                    "PATH": "/usr/bin:/bin:/usr/local/bin",
                    "PYTHONDONTWRITEBYTECODE": "1",
                },
                capture_output=True,
                text=True,
                timeout=TIMEOUT_SECONDS,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return {"label": mutation.label, "outcome": "timeout", "detail": ""}
        outcome = {0: "SURVIVED", 1: "killed"}.get(completed.returncode, "error")
        tail = (
            completed.stdout.strip().splitlines()[:3]
            if outcome != "error"
            else completed.stderr.strip().splitlines()[-3:]
        )
        return {"label": mutation.label, "outcome": outcome, "detail": " | ".join(tail)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--programs", nargs="+", required=True, help="directories of *.bin")
    parser.add_argument("--vectors", type=int, default=8)
    parser.add_argument("--seed", type=int, default=21)
    parser.add_argument("--workers", type=int, default=6)
    parser.add_argument("--only", help="run only mutations whose label contains this text")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)
    chosen = [m for m in MUTATIONS if not args.only or args.only in m.label]
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        results = list(
            pool.map(lambda m: run_one(m, args.programs, args.vectors, args.seed), chosen)
        )
    for result in results:
        print(f"{result['outcome']:11} {result['label']}")
    survived = [r for r in results if r["outcome"] != "killed"]
    print(f"{len(results) - len(survived)} of {len(results)} killed, {len(survived)} not killed")
    if args.json:
        args.json.write_text(json.dumps(results, indent=1) + "\n")
    return 1 if survived else 0


if __name__ == "__main__":
    raise SystemExit(main())
