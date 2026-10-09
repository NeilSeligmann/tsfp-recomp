# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure candidate combiner models on a Vulkan device and on the title's static blocks.

T102 (dot-product mapping 1) and T103 (precision model). See `models.py` for the candidates.
What this module establishes, and what it cannot:

  * Each candidate is an executable model in BOTH evaluators (Python and GLSL). Running the
    GLSL form on a device and comparing it with the Python form shows the translation
    implements the candidate, to float32. That is a check of OUR code on that device.
  * Candidate against candidate, on the title's blocks, over synthetic fragments, says how
    much the choice matters. It is a property of the models and the blocks, so it is
    decidable without hardware, and it is the same on any device.
  * Which candidate the NV2A implements is NOT decidable here. A Vulkan device is not an
    NV2A: AMD or llvmpipe float32 arithmetic says nothing about the 9-bit pipeline.

Rates are over synthetic fragments from `validate.make_case` (uniform floats, grid values and
corner values), with no null model and no claim about image error.

    python -m tools.nv2a_combiner.devicemodels tmp/oxm-extract/retail/default.xbe \
        --device hardware --device llvmpipe
"""

from __future__ import annotations

import argparse
import math
import statistics
import struct
import subprocess
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from tools.nv2a_combiner import config as cfg
from tools.nv2a_combiner import models, reference, validate

Vec4 = tuple[float, float, float, float]


# ------------------------------------------------------------------ the title's blocks


def load_static(xbe: Path, setter: int) -> dict[str, cfg.Config]:
    """The distinct, translatable static definitions, named by address of the first copy."""
    from tools.nv2a_combiner import corpus
    from tools.shaderscan.image import Image

    image = Image.load(xbe)
    seen: set[bytes] = set()
    result: dict[str, cfg.Config] = {}
    for address, block in sorted(corpus.static_definitions(image, setter).items()):
        if block in seen:
            continue
        seen.add(block)
        try:
            result[f"s_{address:x}"] = cfg.decode(block)
        except cfg.UnsupportedConfig:
            continue
    return result


def dot_mapping_one_stages(config: cfg.Config) -> int:
    """How many dot-product texture stages use input mapping 1."""
    count = 0
    for stage in range(1, 4):
        if config.texture_modes[stage] in (cfg.TEX_DOT_PRODUCT, cfg.TEX_DOT_ST):
            count += (config.dot_mapping >> (4 * (stage - 1))) & 0xF == 1
    return count


def uses_dot_or_dependent(config: cfg.Config) -> bool:
    return any(
        mode in (cfg.TEX_DOT_PRODUCT, cfg.TEX_DOT_ST, cfg.TEX_DEPENDENT_AR, cfg.TEX_DEPENDENT_GB)
        for mode in config.texture_modes
    )


# ------------------------------------------------------------------ evaluating a model


def model_outputs(
    configs: dict[str, cfg.Config], cases: Sequence[validate.Case], model: models.Model
) -> dict[str, list[Vec4]]:
    """The Python evaluator, one colour per fragment."""
    return {
        name: [reference.evaluate(config, case.fragment, model=model) for case in cases]
        for name, config in configs.items()
    }


def device_outputs(
    configs: dict[str, cfg.Config],
    cases: Sequence[validate.Case],
    model: models.Model,
    *,
    device: str | None,
    directory: Path,
) -> dict[str, list[Vec4]]:
    """The GLSL form on a device, one colour per fragment (fragments a clip plane kills are
    returned as computed, the same as the Python evaluator returns them)."""
    validate.build_runner(directory)
    input_path = directory / "fragments.bin"
    input_path.write_bytes(validate.pack_cases(list(cases)))
    jobs = []
    paths = {}
    for name, config in configs.items():
        spirv = validate.compile_spirv(glsl_for(config, model), directory, f"{model.name}_{name}")
        paths[name] = directory / f"{model.name}_{name}.out"
        jobs.append((spirv, input_path, paths[name], len(cases)))
    validate.run_manifest(directory, jobs, device)
    result = {}
    for name, path in paths.items():
        numbers = struct.unpack(f"<{len(cases) * validate.OUT_FLOATS}f", path.read_bytes())
        result[name] = [
            (numbers[i * 8], numbers[i * 8 + 1], numbers[i * 8 + 2], numbers[i * 8 + 3])
            for i in range(len(cases))
        ]
    return result


def glsl_for(config: cfg.Config, model: models.Model) -> str:
    from tools.nv2a_combiner import glsl

    return glsl.compute_shader(config, model)


def device_name(device: str | None, directory: Path) -> str:
    runner = validate.build_runner(directory)
    import os

    environment = {**os.environ, "VKRUN_DEVICE": device} if device else None
    done = subprocess.run(
        [str(runner), "--info"], capture_output=True, text=True, timeout=60, env=environment
    )
    if done.returncode != 0:
        raise RuntimeError(f"no device for {device!r}: {done.stderr.strip()[:200]}")
    return done.stdout.strip()


# ------------------------------------------------------------------ comparing two models


@dataclass(frozen=True)
class Gap:
    fragments: int
    #: Largest absolute difference of any channel, in full-scale units.
    max_abs: float
    #: Fragments whose 8-bit colour (round(x * 255) per channel) differs at all.
    differ: int
    #: Largest per-channel difference of the 8-bit colour, in steps.
    max_steps: int

    @property
    def rate(self) -> float:
        return self.differ / self.fragments if self.fragments else 0.0


def step_of(value: float) -> int:
    return math.floor(value * 255.0 + 0.5)


def gap(first: Sequence[Vec4], second: Sequence[Vec4]) -> Gap:
    if len(first) != len(second):
        raise ValueError("different fragment counts")
    max_abs = 0.0
    differ = 0
    max_steps = 0
    for a, b in zip(first, second, strict=True):
        max_abs = max(max_abs, *(abs(x - y) for x, y in zip(a, b, strict=True)))
        steps = max(abs(step_of(x) - step_of(y)) for x, y in zip(a, b, strict=True))
        differ += steps > 0
        max_steps = max(max_steps, steps)
    return Gap(len(first), max_abs, differ, max_steps)


def over_a_quarter_step(first: Sequence[Vec4], second: Sequence[Vec4]) -> int:
    """Fragments where the two evaluators differ by more than a quarter of a grid step in some
    channel: a different model, not float32 against float64 (which is below 1e-5)."""
    return sum(
        1
        for a, b in zip(first, second, strict=True)
        if max(abs(x - y) for x, y in zip(a, b, strict=True)) > 0.25 / 255.0
    )


def compare_models(
    reference_outputs: dict[str, list[Vec4]], other_outputs: dict[str, list[Vec4]]
) -> dict[str, Gap]:
    return {name: gap(reference_outputs[name], other_outputs[name]) for name in reference_outputs}


def summarise(gaps: dict[str, Gap]) -> dict[str, float]:
    """Over blocks: median and largest of the per-block rates, and the largest abs gap."""
    rates = [g.rate for g in gaps.values()]
    return {
        "blocks": float(len(gaps)),
        "blocks_differing": float(sum(1 for g in gaps.values() if g.differ)),
        "median_rate": statistics.median(rates) if rates else 0.0,
        "max_rate": max(rates, default=0.0),
        "max_abs": max((g.max_abs for g in gaps.values()), default=0.0),
        "max_steps": float(max((g.max_steps for g in gaps.values()), default=0)),
    }


# ------------------------------------------------------------------ ties


class _TieCounter(reference.Evaluator):
    """Counts the register writes whose pre-rounding value sits on a grid tie."""

    ties = 0
    writes = 0

    def _written(self, value: float) -> float:
        type(self).writes += 1
        type(self).ties += models.on_a_tie(value)
        return super()._written(value)


def tie_rate(config: cfg.Config, cases: Sequence[validate.Case]) -> tuple[int, int]:
    """(ties, writes) over the general-stage register writes of `config`, nearest-even model."""
    counter = type("Counter", (_TieCounter,), {"ties": 0, "writes": 0})
    for case in cases:
        evaluator = counter(config, case.fragment, None, None, models.WRITE_EVEN)
        for stage in config.stages:
            evaluator.stage(stage)
    return counter.ties, counter.writes


# ------------------------------------------------------------------ the dot mapping table


def shipped_unguarded_outputs(
    configs: dict[str, cfg.Config], cases: Sequence[validate.Case]
) -> dict[str, list[Vec4]]:
    """`precision.py`'s model: Python's own round() on the raw float, no tie guard. Where a
    value is a tie in real arithmetic, which side it lands on is float noise."""
    return {
        name: [reference.evaluate(config, case.fragment, quantum=1.0 / 255.0) for case in cases]
        for name, config in configs.items()
    }


def dot_table() -> dict[str, list[float]]:
    """Each mapping's value for every texel byte 0..255, device free and exact in float64."""
    return {
        name: [function(k / 255.0) for k in range(256)]
        for name, (function, _) in models.DOT_MAPPINGS.items()
    }


def dot_table_gap(table: dict[str, list[float]], first: str, second: str) -> tuple[float, int]:
    """(largest |difference|, the texel byte where it happens) between two mappings."""
    differences = [abs(a - b) for a, b in zip(table[first], table[second], strict=True)]
    worst = max(range(256), key=lambda k: differences[k])
    return differences[worst], worst


# ------------------------------------------------------------------ what the device's round does

ROUNDING_SHADER = """#version 450
layout(local_size_x = 64) in;
layout(push_constant) uniform Push { uint count; } pc;
layout(std430, binding = 0) readonly buffer In { float data[]; };
layout(std430, binding = 1) writeonly buffer Out { float result[]; };
void main() {
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= pc.count) return;
    float k = data[gid];
    float tie = k + 0.5;
    // The product of a grid value and one half, scaled back to steps: exactly k/2 in real
    // arithmetic, and an ulp either side of it in float.
    float product = (k / 255.0) * 0.5 * 255.0;
    result[gid * 6u + 0u] = round(tie);
    result[gid * 6u + 1u] = roundEven(tie);
    result[gid * 6u + 2u] = floor(tie + 0.5);
    result[gid * 6u + 3u] = trunc(tie);
    result[gid * 6u + 4u] = product;
    result[gid * 6u + 5u] = round(product);
}
"""


@dataclass(frozen=True)
class RoundingProbe:
    #: round() of k + 0.5 for k in 0..N-1, as returned by the device.
    round_ties: list[float]
    round_even_ties: list[float]
    #: Whether the device's round() agrees with roundEven() on every tie.
    round_is_even: bool
    #: Whether it rounds ties away from zero (floor(x + 0.5) on positives) instead.
    round_is_half_up: bool
    #: (k/255) * 0.5 * 255 against k / 2 for every k in 1..255: how many are not exact.
    inexact_products: int
    products: int
    #: Of the odd k (a true tie), how many land strictly below, exactly on and above k / 2.
    odd_below: int
    odd_exact: int
    odd_above: int


def rounding_probe(directory: Path, device: str | None) -> RoundingProbe:
    runner = validate.build_runner(directory)
    spirv = validate.compile_spirv(ROUNDING_SHADER, directory, "rounding")
    count = 256
    inputs = directory / "rounding.in"
    inputs.write_bytes(struct.pack(f"<{count}f", *[float(k) for k in range(count)]))
    output = directory / "rounding.out"
    manifest = directory / "rounding.txt"
    manifest.write_text(f"{spirv} {inputs} {output} {count} 1 6\n")
    import os

    environment = {**os.environ, "VKRUN_DEVICE": device} if device else None
    done = subprocess.run(
        [str(runner), str(manifest)], capture_output=True, text=True, timeout=120, env=environment
    )
    if done.returncode != 0:
        raise RuntimeError(f"rounding probe failed: {done.stdout}{done.stderr}")
    numbers = struct.unpack(f"<{count * 6}f", output.read_bytes())
    columns = [numbers[i::6] for i in range(6)]
    round_ties, even_ties, half_up = columns[0], columns[1], columns[2]
    products = columns[4]
    odd = [k for k in range(1, count) if k % 2 == 1]
    exact = lambda k: products[k] == k / 2.0  # noqa: E731
    return RoundingProbe(
        round_ties=list(round_ties),
        round_even_ties=list(even_ties),
        round_is_even=list(round_ties) == list(even_ties),
        round_is_half_up=list(round_ties) == list(half_up),
        inexact_products=sum(1 for k in range(1, count) if not exact(k)),
        products=count - 1,
        odd_below=sum(1 for k in odd if products[k] < k / 2.0),
        odd_exact=sum(1 for k in odd if exact(k)),
        odd_above=sum(1 for k in odd if products[k] > k / 2.0),
    )


# ------------------------------------------------------------------ report


def _row(label: str, summary: dict[str, float]) -> str:
    return (
        f"  {label:34s} blocks differing {int(summary['blocks_differing']):2d}/"
        f"{int(summary['blocks']):2d}  median rate {summary['median_rate']:.3f}  "
        f"max rate {summary['max_rate']:.3f}  max abs {summary['max_abs']:.4f}  "
        f"max steps {int(summary['max_steps'])}"
    )


def report(args: argparse.Namespace) -> int:
    from tools.nv2a_combiner.cli import default_addresses

    configs = load_static(args.xbe, default_addresses().setter)
    cases = validate.make_cases(args.seed, args.fragments)
    work = args.work
    print(f"static blocks: {len(configs)} distinct translatable, fragments {args.fragments}")
    dot_blocks = {n: c for n, c in configs.items() if dot_mapping_one_stages(c)}
    texture_blocks = {n: c for n, c in configs.items() if uses_dot_or_dependent(c)}
    print(
        f"blocks using dot mapping 1: {len(dot_blocks)}; blocks with a dot or dependent "
        f"texture stage: {len(texture_blocks)}"
    )

    print("\nDOT MAPPING TABLE (exact, float64, over the 256 texel bytes)")
    table = dot_table()
    for name in table:
        if name != "d3d":
            worst, byte = dot_table_gap(table, "d3d", name)
            print(f"  d3d against {name:13s} largest {worst:.5f} at byte {byte}")

    print("\nPRECISION CANDIDATES against the shipped inferred model (write_even), Python")
    base = model_outputs(configs, cases, models.WRITE_EVEN)
    for model in models.PRECISION_CANDIDATES:
        if model is models.WRITE_EVEN:
            continue
        other = model_outputs(configs, cases, model)
        print(_row(model.name, summarise(compare_models(base, other))))
    unguarded = shipped_unguarded_outputs(configs, cases)
    print(_row("shipped quantum=1/255 (unguarded)", summarise(compare_models(base, unguarded))))
    ties = [tie_rate(config, cases[:100]) for config in configs.values()]
    total_ties = sum(t for t, _ in ties)
    total_writes = sum(w for _, w in ties)
    print(
        f"  grid ties in register writes: {total_ties} of {total_writes} "
        f"({100.0 * total_ties / max(total_writes, 1):.2f}%) over 100 fragments"
    )

    print("\nDOT MAPPING CANDIDATES against d3d, Python, texels on the 8-bit grid")
    base_dot = model_outputs(dot_blocks, cases, models.DOT_CANDIDATES[0])
    for model in models.DOT_CANDIDATES[1:]:
        other = model_outputs(dot_blocks, cases, model)
        print(_row(model.name, summarise(compare_models(base_dot, other))))

    for device in args.device:
        directory = work / device
        directory.mkdir(parents=True, exist_ok=True)
        print(f"\nDEVICE {device}: {device_name(device, directory)}")
        probe = rounding_probe(directory, device)
        print(
            f"  GLSL round() on ties k+0.5: is roundEven {probe.round_is_even}, "
            f"is floor(x+0.5) {probe.round_is_half_up}"
        )
        print(
            f"  (k/255)*0.5*255 inexact for {probe.inexact_products} of {probe.products}; "
            f"true ties (odd k) below/exact/above: "
            f"{probe.odd_below}/{probe.odd_exact}/{probe.odd_above}"
        )
        for model in (*models.PRECISION_CANDIDATES, *models.DOT_CANDIDATES):
            chosen = dot_blocks if model in models.DOT_CANDIDATES else configs
            python = model_outputs(chosen, cases, model)
            gpu = device_outputs(chosen, cases, model, device=device, directory=directory)
            gaps = compare_models(python, gpu)
            flips = sum(over_a_quarter_step(python[n], gpu[n]) for n in gaps)
            worst = max(g.max_abs for g in gaps.values())
            print(
                f"  {model.name:18s} GLSL against Python: model disagreements {flips} of "
                f"{len(chosen) * len(cases)}, largest difference {worst:.2e}"
            )
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("xbe", type=Path)
    parser.add_argument("--device", action="append", default=[], help="VKRUN_DEVICE, repeatable")
    parser.add_argument("--fragments", type=int, default=300)
    parser.add_argument("--seed", type=int, default=5)
    parser.add_argument("--work", type=Path, default=Path("generated/device-models"))
    return report(parser.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
