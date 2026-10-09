# SPDX-License-Identifier: GPL-3.0-or-later
"""T724: derive the stand-in sampling unit per vertex program from the MEASURED oT0 range.

The host (`--gpu-replay-draw-dump FILE`) writes one JSON line per replayed draw: the vertex
program digest, whether the combiner samples t0, the assembled vertex attributes and the
constants. This module evaluates the program (`tools.nv2a.interp`, the program bytes come
from a `tools.nv2a.corpus` directory and must hash to the digest) over every vertex of every
sampling draw, reads oT0 x and y, and classifies:

  * normalised  every coordinate within [0, 1]
  * texel       every coordinate within [0, inf) and the largest at least 2
  * refused     anything else (negative, not finite, largest in (1, 2) so wrapping or texels
                cannot be told apart, the program unreadable, not in the corpus, it never
                writes oT0 x and y, or draws of one program disagree)

A program with no sampling draw is not classified and not refused. The result stays INFERRED:
it measures what the translated program writes, not what the title's texture state says.

    python -m tools.nv2a.standin_units --draws draws.jsonl --corpus DIR [--output-init zero]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from collections.abc import Iterable
from dataclasses import dataclass, field
from pathlib import Path

from tools.nv2a import interp

OT0 = 9  # isa.OUTPUT_NAMES
MIN_PREFIX = 8
TEXEL_FLOOR = 2.0  # a largest coordinate below this and above 1 is ambiguous (wrap versus texels)


class UnitError(Exception):
    pass


@dataclass
class Verdict:
    unit: str | None = None  # "normalised", "texel" or None when refused
    reason: str = ""
    draws: int = 0
    low: float = math.inf
    high: float = -math.inf


@dataclass
class Derivation:
    verdicts: dict[str, Verdict] = field(default_factory=dict)

    def lists(self) -> tuple[list[str], list[str]]:
        """`(texel prefixes, normalised prefixes)`: the shortest prefix of 8+ digits that no other
        digest seen shares, so an unclassified program can never match a rule."""
        digests = sorted(self.verdicts)
        texel: list[str] = []
        normalised: list[str] = []
        for digest in digests:
            verdict = self.verdicts[digest]
            if verdict.unit is None:
                continue
            others = [other for other in digests if other != digest]
            length = MIN_PREFIX
            while any(other.startswith(digest[:length]) for other in others) and length < 64:
                length += 1
            (texel if verdict.unit == "texel" else normalised).append(digest[:length])
        return texel, normalised

    def refused(self) -> dict[str, str]:
        return {d: v.reason for d, v in sorted(self.verdicts.items()) if v.unit is None}

    def as_json(self) -> dict[str, object]:
        texel, normalised = self.lists()
        return {
            "inferred": "oT0 range measured through the translated program only",
            "texel": texel,
            "normalised": normalised,
            "programs": {
                digest: {
                    "unit": verdict.unit,
                    "reason": verdict.reason,
                    "sampling_draws": verdict.draws,
                    "oT0_min": None if verdict.draws == 0 else verdict.low,
                    "oT0_max": None if verdict.draws == 0 else verdict.high,
                }
                for digest, verdict in sorted(self.verdicts.items())
            },
        }


def load_program(corpus: Path, digest: str) -> bytes:
    """The headed program bytes of `digest`, which must hash to it."""
    for kind in ("static", "generated"):
        path = corpus / kind / f"{digest}.bin"
        if path.is_file():
            data = path.read_bytes()
            if hashlib.sha256(data).hexdigest() != digest:
                raise UnitError(f"{path} does not hash to its name")
            return data
    raise UnitError("the program is not in the corpus")


def classify_range(low: float, high: float) -> tuple[str | None, str]:
    """`(unit, reason)` for the measured oT0 xy range of one program."""
    if not (math.isfinite(low) and math.isfinite(high)):
        return None, f"oT0 is not finite (min {low}, max {high})"
    if low < 0.0:
        return None, f"oT0 is negative (min {low}): wrap or mirror cannot be told from a unit"
    if high <= 1.0:
        return "normalised", ""
    if high >= TEXEL_FLOOR:
        return "texel", ""
    return None, f"oT0 max {high} lies in (1, {TEXEL_FLOOR}): wrapping and texels are ambiguous"


def writes_ot0_xy(program: bytes) -> bool:
    """Whether some instruction writes oT0 x or y (mask bit 3 is x, bit 2 is y)."""
    return any(
        inst.out_target == 1 and inst.out_addr == OT0 and inst.out_wm & 0b1100
        for inst in interp.decode_program(program)
    )


def oT0_range(program: bytes, draw: dict[str, object], output_init: str) -> tuple[float, float]:
    """Min and max of oT0 x and y over the draw's vertices. A program that never writes oT0 x
    and y is refused: its zero would read as a normalised coordinate."""
    if not writes_ot0_xy(program):
        raise UnitError("the program never writes oT0 x and y")
    count = int(draw["vertices"])  # type: ignore[call-overload]
    attributes = draw["attributes"]
    constants = draw["constants"]
    width = interp.NUM_INPUTS * 4
    if not isinstance(attributes, list) or len(attributes) != count * width:
        raise UnitError("the draw's attribute count does not match its vertex count")
    if not isinstance(constants, list) or len(constants) != interp.NUM_CONSTANTS * 4:
        raise UnitError("the draw's constants are not 192 rows")
    rows = [constants[i : i + 4] for i in range(0, len(constants), 4)]
    low, high = math.inf, -math.inf
    for vertex in range(count):
        flat = attributes[vertex * width : (vertex + 1) * width]
        inputs = [flat[i : i + 4] for i in range(0, width, 4)]
        result = interp.run(program, inputs, rows, output_init=output_init)
        for value in result.outputs[OT0][:2]:
            if not math.isfinite(value):
                return value, value  # min and max would skip a NaN, so it is returned as is
            low, high = min(low, value), max(high, value)
    return low, high


def derive(
    draws: Iterable[dict[str, object]], corpus: Path, output_init: str = "zero"
) -> Derivation:
    """Classify every program that has a sampling draw. Draws that do not sample t0 only register
    the digest (for prefix uniqueness)."""
    result = Derivation()
    programs: dict[str, bytes | str] = {}
    for draw in draws:
        digest = str(draw["digest"])
        verdict = result.verdicts.setdefault(digest, Verdict())
        if not draw.get("samples_t0"):
            continue
        if verdict.unit is None and verdict.reason:
            continue  # already refused
        if digest not in programs:
            try:
                programs[digest] = load_program(corpus, digest)
            except UnitError as error:
                programs[digest] = str(error)
        program = programs[digest]
        if isinstance(program, str):
            verdict.reason = program
            continue
        try:
            low, high = oT0_range(program, draw, output_init)
        except (UnitError, interp.InterpError) as error:
            verdict.unit, verdict.reason = None, str(error)
            programs[digest] = str(error)
            continue
        unit, reason = classify_range(low, high)
        if verdict.draws and unit != verdict.unit:
            other = unit or "unclassifiable"
            unit, reason = None, f"draws of one program disagree ({verdict.unit} versus {other})"
        verdict.draws += 1
        verdict.low, verdict.high = min(verdict.low, low), max(verdict.high, high)
        verdict.unit = unit
        if unit is None:
            verdict.reason = reason or "unclassified"
            programs[digest] = verdict.reason
    for verdict in result.verdicts.values():
        if verdict.reason:
            verdict.unit = None
    return result


def read_draws(path: Path) -> list[dict[str, object]]:
    draws = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip():
            continue
        try:
            draws.append(json.loads(line))
        except json.JSONDecodeError as error:
            raise UnitError(f"{path}:{number} is not JSON: {error}") from error
    if not draws:
        raise UnitError(f"{path} holds no draw")
    return draws


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter
    )
    parser.add_argument(
        "--draws", type=Path, required=True, help="the host's --gpu-replay-draw-dump file"
    )
    parser.add_argument("--corpus", type=Path, required=True, help="a tools.nv2a.corpus directory")
    parser.add_argument("--output-init", choices=sorted(interp.OUTPUT_INITS), default="zero")
    parser.add_argument("--output", type=Path, help="write the derivation JSON here")
    args = parser.parse_args(argv)
    try:
        derivation = derive(read_draws(args.draws), args.corpus, args.output_init)
    except UnitError as error:
        print(f"standin_units: {error}", file=sys.stderr)
        return 2
    text = json.dumps(derivation.as_json(), indent=1)
    if args.output:
        args.output.write_text(text + "\n")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
