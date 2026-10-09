# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutation-test the VALIDATION, not the translator.

Each mutation edits ONE expression in `glsl.py`, `reference.py` or `config.py`, runs the
checks in a fresh interpreter, and restores the file. A mutation the checks do not fail on
is a hole in the validation. The report says WHICH check caught each one, because the
checks see different things:

  corpus   the title's own configurations, translated, run on lavapipe, compared with the
           reference model. It cannot see a feature the title never uses.
  random   random valid configurations through the same comparison. Sees every feature.
  probe    route A: decode + reference model against the Direct3D meaning of source
           constructs assembled by the title's assembler. The only check that sees a defect
           in `config.py`, which the translator and the model SHARE, so a GPU-versus-model
           comparison agrees with itself about it.

The mutated file is always the one imported: the child runs with bytecode writing off AND
every `__pycache__` under the package is deleted first, because a same-size edit inside
one second can otherwise reuse a stale `.pyc` and report a mutation killed by old code.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

PACKAGE = Path(__file__).resolve().parent


@dataclass(frozen=True)
class Mutation:
    name: str
    file: str
    old: str
    new: str


MUTATIONS: tuple[Mutation, ...] = (
    # Translator: input mappings.
    Mutation(
        "glsl EXPAND_NORMAL offset",
        "glsl.py",
        'f"(2.0 * max({expression}, 0.0) - 1.0)"',
        'f"(2.0 * max({expression}, 0.0) - 0.5)"',
    ),
    Mutation(
        "glsl EXPAND_NEGATE equals EXPAND_NORMAL",
        "glsl.py",
        'f"(1.0 - 2.0 * max({expression}, 0.0))"',
        'f"(2.0 * max({expression}, 0.0) - 1.0)"',
    ),
    Mutation(
        "glsl HALF_BIAS_NEGATE sign",
        "glsl.py",
        'f"(0.5 - max({expression}, 0.0))"',
        'f"(-0.5 - max({expression}, 0.0))"',
    ),
    Mutation(
        "glsl UNSIGNED_INVERT unclamped",
        "glsl.py",
        'f"(1.0 - clamp({expression}, 0.0, 1.0))"',
        'f"(1.0 - {expression})"',
    ),
    Mutation(
        "glsl SIGNED_NEGATE dropped",
        "glsl.py",
        'return f"(-{expression})"',
        "return expression",
    ),
    Mutation(
        "glsl UNSIGNED_IDENTITY lets negatives through",
        "glsl.py",
        'return f"max({expression}, 0.0)"',
        "return expression",
    ),
    # Translator: scale, bias, clamp.
    Mutation(
        "glsl scale x4 emitted as x2",
        "glsl.py",
        "* {literal(o.scale)}, -1.0, 1.0);",
        "* {literal(min(o.scale, 2.0))}, -1.0, 1.0);",
    ),
    Mutation(
        "glsl scale x1/2 emitted as x1",
        "glsl.py",
        "* {literal(o.scale)}, -1.0, 1.0);",
        "* {literal(max(o.scale, 1.0))}, -1.0, 1.0);",
    ),
    Mutation(
        "glsl bias 0.5 becomes 0.25",
        "glsl.py",
        'bias = "0.5" if o.bias else "0.0"',
        'bias = "0.25" if o.bias else "0.0"',
    ),
    Mutation(
        "glsl output clamp low edge 0",
        "glsl.py",
        "* {literal(o.scale)}, -1.0, 1.0);",
        "* {literal(o.scale)}, 0.0, 1.0);",
    ),
    # Translator: mux, dot, blue-to-alpha, destinations.
    Mutation(
        "glsl mux operands swapped",
        "glsl.py",
        "? {tag}_cd : {tag}_ab;",
        "? {tag}_ab : {tag}_cd;",
    ),
    Mutation("glsl mux threshold", "glsl.py", '"r0.a >= 0.5"', '"r0.a >= 0.25"'),
    Mutation("glsl mux lsb scale", "glsl.py", "* 255.0 + 0.5) & 1u", "* 256.0 + 0.5) & 1u"),
    Mutation(
        "glsl AB dot becomes product",
        "glsl.py",
        'f"vec3(dot({names[0]}, {names[1]}))" if o.ab_dot',
        'f"{names[0]} * {names[1]}" if o.ab_dot',
    ),
    Mutation(
        "glsl blue-to-alpha reads green",
        "glsl.py",
        'writes.append(f"{register}.a = {value}.b;")',
        'writes.append(f"{register}.a = {value}.g;")',
    ),
    Mutation(
        "glsl AB output goes to the CD register",
        "glsl.py",
        '("ab", o.ab_dst, o.blue_to_alpha_ab),',
        '("ab", o.cd_dst, o.blue_to_alpha_ab),',
    ),
    Mutation(
        "glsl alpha component usage swapped",
        "glsl.py",
        'channels = ".a" if operand.alpha else ".b"',
        'channels = ".b" if operand.alpha else ".a"',
    ),
    Mutation(
        "glsl factor0 read from factor1",
        "glsl.py",
        'else f"f.factor0[{stage.factor0}]"',
        'else f"f.factor1[{stage.factor0}]"',
    ),
    Mutation(
        "glsl initial spare0 alpha convention",
        "glsl.py",
        'initial = "t0.a" if config.texture_modes[0] != cfg.TEX_NONE else "1.0"',
        'initial = "t0.a" if config.texture_modes[0] != cfg.TEX_NONE else "0.0"',
    ),
    # Translator: final combiner and texture stages.
    Mutation(
        "glsl final combiner drops (1-A)",
        "glsl.py",
        "fd + (1.0 - fa) * fc + fa * fb",
        "fd + fa * fc + fa * fb",
    ),
    Mutation(
        "glsl final sum clamp inverted", "glsl.py", "if final.clamp_sum:", "if not final.clamp_sum:"
    ),
    Mutation(
        "glsl projective divide by z",
        "glsl.py",
        "{coord}.xy / {coord}.w, 0.0",
        "{coord}.xy / {coord}.z, 0.0",
    ),
    Mutation(
        "glsl dependent GB reads RG",
        "glsl.py",
        "vec3(t{previous}.gb, 0.0)",
        "vec3(t{previous}.rg, 0.0)",
    ),
    Mutation(
        "glsl signed dot mapping offset",
        "glsl.py",
        "* 255.0 - 128.0) / 127.0",
        "* 255.0 - 127.5) / 127.5",
    ),
    Mutation(
        "glsl dependent AR reads RA",
        "glsl.py",
        "vec3(t{previous}.ar, 0.0)",
        "vec3(t{previous}.ra, 0.0)",
    ),
    Mutation(
        "glsl complement flag on the wrong register",
        "glsl.py",
        'first = "(1.0 - clamp(r0.rgb, 0.0, 1.0))" if final.complement_r0',
        'first = "(1.0 - clamp(r0.rgb, 0.0, 1.0))" if final.complement_v1',
    ),
    Mutation(
        "glsl per-stage constant index ignored",
        "glsl.py",
        'else f"f.factor1[{stage.factor1}]"',
        'else "f.factor1[0]"',
    ),
    # Reference model.
    Mutation(
        "reference EXPAND_NORMAL offset",
        "reference.py",
        "return 2.0 * max(0.0, value) - 1.0",
        "return 2.0 * max(0.0, value) - 0.5",
    ),
    Mutation(
        "reference scale before bias",
        "reference.py",
        "(value - (0.5 if bias else 0.0)) * scale",
        "value * scale - (0.5 if bias else 0.0)",
    ),
    Mutation(
        "reference mux direction", "reference.py", "take_cd = alpha >= 0.5", "take_cd = alpha < 0.5"
    ),
    Mutation(
        "reference final equation",
        "reference.py",
        "d[i] + (1.0 - a[i]) * c[i] + a[i] * b[i]",
        "d[i] + a[i] * c[i] + (1.0 - a[i]) * b[i]",
    ),
    Mutation(
        "reference clamps the sum register never",
        "reference.py",
        "if final.clamp_sum:",
        "if not final.clamp_sum:",
    ),
    Mutation(
        "reference LSB select reads the second bit",
        "reference.py",
        "take_cd = (int(level + 0.5) & 1) == 1",
        "take_cd = (int(level + 0.5) & 2) == 2",
    ),
    # Shared decode, invisible to a translator-versus-model comparison.
    Mutation(
        "config mux select bit is bit 9",
        "config.py",
        "CONTROL_MUX_MSB_BIT = 1 << 8",
        "CONTROL_MUX_MSB_BIT = 1 << 9",
    ),
    Mutation(
        "config sum clamp flag is bit 6",
        "config.py",
        "FINAL_CLAMP_SUM = 1 << 7",
        "FINAL_CLAMP_SUM = 1 << 6",
    ),
    Mutation(
        "config factor mode bit is bit 13",
        "config.py",
        "CONTROL_FACTOR0_EACH_BIT = 1 << 12",
        "CONTROL_FACTOR0_EACH_BIT = 1 << 13",
    ),
    Mutation(
        "config AB and CD dot bits swapped",
        "config.py",
        "OUT_CD_DOT = 1 << 12\nOUT_AB_DOT = 1 << 13",
        "OUT_CD_DOT = 1 << 13\nOUT_AB_DOT = 1 << 12",
    ),
    Mutation(
        "config scale table x1/2 becomes x1/4",
        "config.py",
        "OUT_SCALE_FACTOR = (1.0, 2.0, 4.0, 0.5)",
        "OUT_SCALE_FACTOR = (1.0, 2.0, 4.0, 0.25)",
    ),
    Mutation(
        "config EXPAND_NORMAL and HALF_BIAS_NORMAL codes swapped",
        "config.py",
        "MAP_EXPAND_NORMAL = 2\nMAP_EXPAND_NEGATE = 3\nMAP_HALF_BIAS_NORMAL = 4",
        "MAP_EXPAND_NORMAL = 4\nMAP_EXPAND_NEGATE = 3\nMAP_HALF_BIAS_NORMAL = 2",
    ),
    Mutation(
        "config operand byte order reversed",
        "config.py",
        "for shift in (24, 16, 8, 0):",
        "for shift in (0, 8, 16, 24):",
    ),
    Mutation(
        "config bias bit is bit 16",
        "config.py",
        "bias=bool(operation & 1),",
        "bias=bool(operation & 2),",
    ),
    Mutation(
        "config AB destination field is CD",
        "config.py",
        "cd_dst=word & 0xF,\n        ab_dst=(word >> 4) & 0xF,",
        "cd_dst=(word >> 4) & 0xF,\n        ab_dst=word & 0xF,",
    ),
)


def file_digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def clear_bytecode() -> None:
    for directory in PACKAGE.rglob("__pycache__"):
        shutil.rmtree(directory, ignore_errors=True)


def run_child(arguments: list[str]) -> tuple[int, str]:
    environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    completed = subprocess.run(
        [sys.executable, "-m", "tools.nv2a_combiner.cli", *arguments],
        capture_output=True,
        text=True,
        env=environment,
        timeout=1500,
    )
    return completed.returncode, completed.stdout.strip().splitlines()[
        -1
    ] if completed.stdout else ""


def collect_blocks(xbe: Path, out: Path) -> Path:
    """Write the title's 195 distinct blocks, concatenated, for the child processes."""
    from tools.nv2a_combiner import corpus
    from tools.nv2a_combiner.cli import default_addresses
    from tools.shaderscan.image import Image

    addresses = default_addresses()
    image = Image.load(xbe)
    static = corpus.static_definitions(image, addresses.setter)
    generated = corpus.generated_definitions(xbe, image, addresses)
    blocks = sorted(set(static.values()) | set(generated.blocks.values()))
    path = out / "blocks.bin"
    path.write_bytes(b"".join(blocks))
    return path


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Mutation-test the combiner validation. Each mutation is applied, checked "
        "in a fresh interpreter, and restored."
    )
    parser.add_argument("--xbe", type=Path, help="enables the corpus and probe checks")
    parser.add_argument("--out", type=Path, default=Path("generated/shaders/combiner"))
    parser.add_argument("--random-configs", type=int, default=100)
    parser.add_argument("--fragments", type=int, default=100)
    parser.add_argument("--only", help="substring of a mutation name")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args(argv)

    args.out.mkdir(parents=True, exist_ok=True)
    blocks = collect_blocks(args.xbe, args.out) if args.xbe else None
    check = [
        "check",
        "--random-configs",
        str(args.random_configs),
        "--fragments",
        str(args.fragments),
    ]
    check += ["--out", str(args.out / "mutation")]
    if blocks:
        check += ["--blocks", str(blocks)]

    clear_bytecode()
    code, line = run_child(check)
    print(f"baseline check: exit {code} {line}")
    if code != 0:
        print("the unmutated tree fails its own check, so mutation results would mean nothing")
        return 2
    if args.xbe:
        code, line = run_child(["probe", str(args.xbe)])
        print(f"baseline probe: exit {code} {line}")
        if code != 0:
            return 2

    rows = []
    survivors = 0
    for mutation in MUTATIONS:
        if args.only and args.only not in mutation.name:
            continue
        path = PACKAGE / mutation.file
        original = path.read_text()
        before = file_digest(path)
        if original.count(mutation.old) != 1:
            print(f"SKIP {mutation.name}: pattern found {original.count(mutation.old)} times")
            rows.append({"name": mutation.name, "status": "pattern"})
            continue
        try:
            path.write_text(original.replace(mutation.old, mutation.new))
            clear_bytecode()
            gpu_code, gpu_line = run_child(check)
            caught = []
            try:
                summary = json.loads(gpu_line)
            except json.JSONDecodeError:
                summary = {}
            for label in ("corpus", "random"):
                if gpu_code != 0 and summary.get(label, {}).get("failing"):
                    caught.append(label)
            if gpu_code != 0 and not summary:
                caught.append("crash")
            if args.xbe:
                probe_code, _ = run_child(["probe", str(args.xbe)])
                if probe_code != 0:
                    caught.append("probe")
        finally:
            path.write_text(original)
            clear_bytecode()
        if file_digest(path) != before:
            raise RuntimeError(f"{mutation.file} was not restored")
        status = "killed" if caught else "SURVIVED"
        survivors += 0 if caught else 1
        print(f"{status:8s} {mutation.name}: {', '.join(caught) or 'nothing caught it'}")
        rows.append({"name": mutation.name, "status": status, "caught_by": caught})
    print(f"{len(rows) - survivors} of {len(rows)} mutations killed, {survivors} survived")
    if args.json:
        args.json.write_text(json.dumps(rows, indent=2) + "\n")
    return 1 if survivors else 0


if __name__ == "__main__":
    raise SystemExit(main())
