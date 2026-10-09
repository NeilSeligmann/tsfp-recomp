# SPDX-License-Identifier: GPL-3.0-or-later
"""T100b: root-cause the corner floats where a device form differs from the interpreter.

T100 (docs/vertex-translator.md 7.6) left 80 `corner` floats in 31 of the 46 static programs
where the translated shader run on a device differs from the interpreter (RADV: vertex and
compute alike, llvmpipe: the vertex stage only). This module explains each differing float by
hypothesis testing. The IEEE interpreter is the baseline. Each hypothesis is the same
interpreter with ONE arithmetic rule changed (denormal inputs read as zero, denormal results
flushed too, `0 * x = 0`, the NV total order for SLT and SGE, a fused multiply-add). A floating
point value is EXPLAINED by a hypothesis when the hypothesis' interpreter reproduces the
device's value there. The report states, per hypothesis, how many floats it fixes, how many
it breaks that the baseline had right, and what it leaves, so a hypothesis that fixes one float
by luck while breaking hundreds is visible. Each explained float is then attributed to the
first instruction at which the baseline and the hypothesis interpreters diverge and to the
denormal operand lanes at that instruction.

The comparison `validate.classify` ignores NaN payload and sign, signed zero, and the
difference between a denormal and zero. `bit_census` counts exactly those against a
hypothesis so none can hide behind the tolerance.

MEASURED facts are about the device named in the report (RADV, llvmpipe), never NV2A.
`python -m tools.nv2a.corner_classify generated/shaders/corpus/static --device hardware`.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import math
import struct
import sys
import tempfile
from collections import Counter
from collections.abc import Callable, Iterator, Sequence
from dataclasses import dataclass, field
from fractions import Fraction
from pathlib import Path

from tools.nv2a import corners, interp, translate, validate, vertex_device
from tools.nv2a.validate import TestVector, VkRunner

FORM_VERTEX = "vertex"
FORM_COMPUTE = "compute"
FORMS = (FORM_VERTEX, FORM_COMPUTE)

UNEXPLAINED = "unexplained"

# Effect classes: what the device value is, relative to the baseline value.
EFFECT_INF_TO_NAN = "inf_became_nan"
EFFECT_NAN_TO_FINITE = "nan_became_finite"
EFFECT_FINITE_TO_NAN = "finite_became_nan"
EFFECT_INF_TO_FINITE = "inf_became_finite"
EFFECT_FINITE_TO_INF = "finite_became_inf"
EFFECT_FINITE_TO_ZERO = "finite_became_zero"
EFFECT_ZERO_TO_FINITE = "zero_became_finite"
EFFECT_FINITE_SHIFT = "finite_value_changed"
EFFECT_SIGN_FLIP_INF = "inf_sign_flipped"
EFFECT_NONE = "same_under_tolerance"

# Bit census classes: how a float that passes `validate.classify` still differs in bits.
BITS_EXACT = "bit_exact"
BITS_NAN_PAYLOAD = "nan_payload_or_sign"
BITS_SIGNED_ZERO = "signed_zero"
BITS_DENORMAL_VS_ZERO = "denormal_vs_zero"
BITS_ROUNDING = "finite_within_tolerance"
BITS_DIFFERENT = "differs_beyond_tolerance"


@dataclass(frozen=True)
class Hypothesis:
    """The baseline interpreter with one rule changed."""

    name: str
    summary: str
    model: interp.Model
    flush_results: bool = False
    fused_mad: bool = False


IEEE = Hypothesis("ieee", "plain IEEE float32, the baseline", interp.IEEE_MODEL)
DENORMAL_INPUT_FLUSH = Hypothesis(
    "denormal_input_flush",
    "a denormal source operand reads as a zero of the same sign",
    interp.Model(False, True, False),
)
DENORMAL_FLUSH_BOTH = Hypothesis(
    "denormal_flush_inputs_and_results",
    "denormal source operands read as zero and a denormal arithmetic result becomes zero",
    interp.Model(False, True, False),
    flush_results=True,
)
ZERO_TIMES = Hypothesis(
    "zero_times_anything",
    "0 * x = +0 for every x including inf and NaN (NV text)",
    interp.Model(True, False, False),
)
TOTAL_ORDER = Hypothesis(
    "total_order_compare",
    "SLT and SGE order -0 below +0 and NaN outside the infinities (NV text)",
    interp.Model(False, False, True),
)
FUSED_MAD = Hypothesis(
    "fused_mad", "MAD rounds once (a fused multiply-add)", interp.IEEE_MODEL, fused_mad=True
)
SPEC = Hypothesis("spec", "all three NV text rules together", interp.SPEC_MODEL)
HYPOTHESES = (
    IEEE,
    DENORMAL_INPUT_FLUSH,
    DENORMAL_FLUSH_BOTH,
    ZERO_TIMES,
    TOTAL_ORDER,
    FUSED_MAD,
    SPEC,
)


# ------------------------------------------------------------------ the hypothesis interpreter


def _fused_mad(a: Sequence[float], b: Sequence[float], c: Sequence[float]) -> list[float]:
    """a * b + c rounded once, per lane. Non-finite operands take the unfused path (the
    exact rational has no value there). Exact in rationals, then one correctly rounded
    conversion to double and one to float32."""
    lanes = []
    for x, y, z in zip(a, b, c, strict=True):
        if all(math.isfinite(value) for value in (x, y, z)):
            exact = Fraction(x) * Fraction(y) + Fraction(z)
            lanes.append(interp._arith(float(exact)))
        else:
            lanes.append(interp._add(interp._mul(x, y, interp.IEEE_MODEL), z))
    return lanes


@contextlib.contextmanager
def applied(hypothesis: Hypothesis) -> Iterator[interp.Model]:
    """Install the hypothesis' extra rules into the interpreter module, restoring on exit."""
    original_arith, original_mac = interp._arith, interp._mac

    def flushing_arith(value: float) -> float:
        rounded = original_arith(value)
        if rounded == rounded and 0.0 < abs(rounded) < interp.MIN_NORMAL:
            return math.copysign(0.0, rounded)
        return rounded

    def fusing_mac(
        name: str, a: interp.Vec, b: interp.Vec, c: interp.Vec, model: interp.Model
    ) -> interp.Vec | None:
        if name == "MAD":
            return _fused_mad(a, b, c)
        return original_mac(name, a, b, c, model)

    if hypothesis.flush_results:
        interp._arith = flushing_arith
    if hypothesis.fused_mad:
        interp._mac = fusing_mac
    try:
        yield hypothesis.model
    finally:
        interp._arith, interp._mac = original_arith, original_mac


def run_hypothesis(
    hypothesis: Hypothesis,
    program: bytes,
    vector: TestVector,
    trace: list[interp.TraceStep] | None = None,
) -> list[list[float]]:
    """The 16 x 4 outputs of the interpreter under the hypothesis."""
    with applied(hypothesis) as model:
        return interp.run(
            program, vector.inputs, vector.constants, a0=0, model=model, trace=trace
        ).outputs


# ------------------------------------------------------------------------ classification


def _bits(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


def effect_class(baseline: float, device: float) -> str:
    """How the device's value differs from the baseline's, by value alone."""
    if validate.classify(baseline, device) is None:
        return EFFECT_NONE
    base_nan, device_nan = math.isnan(baseline), math.isnan(device)
    if base_nan:
        return EFFECT_NAN_TO_FINITE
    if device_nan:
        return EFFECT_INF_TO_NAN if math.isinf(baseline) else EFFECT_FINITE_TO_NAN
    base_inf, device_inf = math.isinf(baseline), math.isinf(device)
    if base_inf and device_inf:
        return EFFECT_SIGN_FLIP_INF
    if base_inf:
        return EFFECT_INF_TO_FINITE
    if device_inf:
        return EFFECT_FINITE_TO_INF
    if abs(device) < validate.DENORMAL_LIMIT:
        return EFFECT_FINITE_TO_ZERO
    if abs(baseline) < validate.DENORMAL_LIMIT:
        return EFFECT_ZERO_TO_FINITE
    return EFFECT_FINITE_SHIFT


def bit_class(expected: float, actual: float) -> str:
    """How two floats that pass `validate.classify` still differ, bit for bit."""
    if _bits(expected) == _bits(actual):
        return BITS_EXACT
    if validate.classify(expected, actual) is not None:
        return BITS_DIFFERENT
    if math.isnan(expected) and math.isnan(actual):
        return BITS_NAN_PAYLOAD
    if expected == 0.0 and actual == 0.0:
        return BITS_SIGNED_ZERO
    if abs(expected) < validate.DENORMAL_LIMIT and abs(actual) < validate.DENORMAL_LIMIT:
        return BITS_DENORMAL_VS_ZERO
    return BITS_ROUNDING


def bit_census(
    hypothesis: Hypothesis,
    program: bytes,
    vectors: Sequence[TestVector],
    device: Sequence[Sequence[Sequence[float]]],
    wanted: Sequence[vertex_device.Capture],
) -> Counter[str]:
    """Every captured float of every vector, by `bit_class` against the hypothesis."""
    census: Counter[str] = Counter()
    for vector, slots in zip(vectors, device, strict=True):
        outputs = run_hypothesis(hypothesis, program, vector)
        for capture in wanted:
            for component in range(capture.width):
                census[
                    bit_class(
                        outputs[capture.address][component], slots[capture.address][component]
                    )
                ] += 1
    return census


@dataclass(frozen=True)
class Divergence:
    """Where the baseline and a hypothesis first part ways for one output float."""

    #: first instruction whose result differs between the two interpreters
    step: int
    unit: str
    #: denormal operand lanes at that instruction, like "a.x"
    denormal_lanes: tuple[str, ...]
    #: unit of the instruction that finally wrote the output float, and whether that very
    #: instruction's result differs (False: the difference arrived through a temporary)
    writer_unit: str
    writer_diverges: bool


def _unit(step: interp.TraceStep, routed_scalar: bool) -> str:
    inst = step.inst
    return interp.ILU_NAMES[inst.op_sca] if routed_scalar else interp.MAC_NAMES[inst.op_vec]


def first_divergence(
    hypothesis: Hypothesis, program: bytes, vector: TestVector, address: int, component: int
) -> Divergence | None:
    """The first instruction whose result differs between the baseline and the hypothesis, and
    the instruction that wrote output `address`.`component` last. None when no step differs."""
    base: list[interp.TraceStep] = []
    other: list[interp.TraceStep] = []
    run_hypothesis(IEEE, program, vector, base)
    run_hypothesis(hypothesis, program, vector, other)
    writer: tuple[interp.TraceStep, interp.TraceStep] | None = None
    first: tuple[interp.TraceStep, interp.TraceStep] | None = None
    for left, right in zip(base, other, strict=True):
        inst = left.inst
        if first is None and (
            _differs(left.mac_result, right.mac_result)
            or _differs(left.ilu_result, right.ilu_result)
        ):
            first = (left, right)
        writes = (
            inst.out_target == 1
            and inst.out_addr == address
            and (inst.out_wm >> (3 - component)) & 1
            and (left.ilu_result if inst.out_is_sca else left.mac_result) is not None
        )
        if writes:
            writer = (left, right)
    if first is None or writer is None:
        return None
    left, right = first
    scalar = _differs(left.ilu_result, right.ilu_result)
    denormals = tuple(
        f"{label}.{'xyzw'[lane]}"
        for label, operand in (("a", left.a), ("b", left.b), ("c", left.c))
        for lane, value in enumerate(operand)
        if 0.0 < abs(value) < interp.MIN_NORMAL
    )
    last_left, last_right = writer
    routed = bool(last_left.inst.out_is_sca)
    results = (
        (last_left.ilu_result, last_right.ilu_result)
        if routed
        else (last_left.mac_result, last_right.mac_result)
    )
    return Divergence(
        left.index,
        _unit(left, scalar),
        denormals,
        _unit(last_left, routed),
        _differs(*results),
    )


def _differs(left: Sequence[float] | None, right: Sequence[float] | None) -> bool:
    if left is None or right is None:
        return left is not right
    return any(
        not (math.isnan(x) and math.isnan(y)) and _bits(x) != _bits(y)
        for x, y in zip(left, right, strict=True)
    )


@dataclass(frozen=True)
class Finding:
    """One float where the device differs from the IEEE baseline."""

    program: str
    vector: int
    kind: str
    address: int
    component: int
    baseline: float
    device: float
    effect: str
    #: every hypothesis (not the baseline) whose interpreter reproduces the device value here
    explained_by: tuple[str, ...]
    #: the hypothesis the float is attributed to, or UNEXPLAINED
    cause: str
    step: int | None = None
    unit: str = ""
    denormal_lanes: tuple[str, ...] = ()
    writer_unit: str = ""
    writer_diverges: bool = False

    def to_dict(self) -> dict[str, object]:
        return {
            "program": self.program,
            "vector": self.vector,
            "kind": self.kind,
            "address": self.address,
            "component": self.component,
            "baseline_bits": f"0x{_bits(self.baseline):08x}",
            "device_bits": f"0x{_bits(self.device):08x}",
            "effect": self.effect,
            "explained_by": list(self.explained_by),
            "cause": self.cause,
            "step": self.step,
            "unit": self.unit,
            "denormal_lanes": list(self.denormal_lanes),
            "writer_unit": self.writer_unit,
            "writer_diverges": self.writer_diverges,
        }


@dataclass
class HypothesisScore:
    """What one hypothesis does to the compared floats, over every program given."""

    #: baseline-mismatching floats this hypothesis reproduces
    fixes: int = 0
    #: baseline-matching floats this hypothesis breaks
    regressions: int = 0
    #: floats still differing from the device under this hypothesis
    residual: int = 0


@dataclass
class Classification:
    compared: int = 0
    findings: list[Finding] = field(default_factory=list)
    scores: dict[str, HypothesisScore] = field(default_factory=dict)
    #: bit census per hypothesis with the lowest residual
    census: dict[str, Counter[str]] = field(default_factory=dict)
    programs: int = 0


def _mismatch_keys(
    hypothesis: Hypothesis,
    program: bytes,
    vectors: Sequence[TestVector],
    device: Sequence[Sequence[Sequence[float]]],
    wanted: Sequence[vertex_device.Capture],
) -> set[tuple[int, int, int]]:
    """(vector, address, component) of every captured float where the hypothesis' interpreter
    differs from the device under `validate.classify`."""
    keys = set()
    for index, (vector, slots) in enumerate(zip(vectors, device, strict=True)):
        outputs = run_hypothesis(hypothesis, program, vector)
        for capture in wanted:
            for component in range(capture.width):
                if (
                    validate.classify(
                        outputs[capture.address][component], slots[capture.address][component]
                    )
                    is not None
                ):
                    keys.add((index, capture.address, component))
    return keys


@dataclass
class ProgramEvidence:
    """Per program: which hypothesis fails where. Device-free given the device outputs."""

    name: str
    data: bytes
    vectors: Sequence[TestVector]
    device: Sequence[Sequence[Sequence[float]]]
    wanted: Sequence[vertex_device.Capture]


def classify_corpus(
    evidence: Sequence[ProgramEvidence],
    hypotheses: Sequence[Hypothesis] = HYPOTHESES,
) -> Classification:
    """Test every hypothesis on every program and attribute each baseline mismatch.

    The first hypothesis is the baseline. A float's cause is the explaining hypothesis with the
    fewest regressions over the whole set (ties go to list order), so a rule that happens to
    fit one float but breaks others loses to the rule that fits them all."""
    baseline, *others = hypotheses
    result = Classification(programs=len(evidence))
    result.scores = {hypothesis.name: HypothesisScore() for hypothesis in hypotheses}
    pending: list[tuple[ProgramEvidence, tuple[int, int, int], dict[str, bool]]] = []
    for item in evidence:
        result.compared += len(item.vectors) * sum(c.width for c in item.wanted)
        failing = {
            hypothesis.name: _mismatch_keys(
                hypothesis, item.data, item.vectors, item.device, item.wanted
            )
            for hypothesis in hypotheses
        }
        base_keys = failing[baseline.name]
        for hypothesis in hypotheses:
            score = result.scores[hypothesis.name]
            keys = failing[hypothesis.name]
            score.residual += len(keys)
            score.fixes += len(base_keys - keys)
            score.regressions += len(keys - base_keys)
        for key in sorted(base_keys):
            matches = {other.name: key not in failing[other.name] for other in others}
            pending.append((item, key, matches))
    for item, (index, address, component), matches in pending:
        explaining = [other for other in others if matches[other.name]]
        explaining.sort(key=lambda other: result.scores[other.name].regressions)
        cause = explaining[0] if explaining else None
        vector = item.vectors[index]
        outputs = run_hypothesis(baseline, item.data, vector)
        slots = item.device[index]
        divergence = (
            first_divergence(cause, item.data, vector, address, component) if cause else None
        )
        result.findings.append(
            Finding(
                program=item.name,
                vector=index,
                kind=vector.kind,
                address=address,
                component=component,
                baseline=outputs[address][component],
                device=slots[address][component],
                effect=effect_class(outputs[address][component], slots[address][component]),
                explained_by=tuple(other.name for other in explaining),
                cause=cause.name if cause else UNEXPLAINED,
                step=divergence.step if divergence else None,
                unit=divergence.unit if divergence else "",
                denormal_lanes=divergence.denormal_lanes if divergence else (),
                writer_unit=divergence.writer_unit if divergence else "",
                writer_diverges=divergence.writer_diverges if divergence else False,
            )
        )
    if evidence:
        lowest = min(score.residual for score in result.scores.values())
        for hypothesis in hypotheses:
            if result.scores[hypothesis.name].residual == lowest:
                result.census[hypothesis.name] = sum(
                    (
                        bit_census(hypothesis, item.data, item.vectors, item.device, item.wanted)
                        for item in evidence
                    ),
                    Counter(),
                )
    return result


# ------------------------------------------------------------------------------ the device


def without_denormals(vector: TestVector) -> TestVector:
    """The same vector with every denormal input or constant replaced by a zero of its sign.
    The control experiment: a device that flushes and one that preserves must then agree."""

    def clean(value: float) -> float:
        return math.copysign(0.0, value) if 0.0 < abs(value) < interp.MIN_NORMAL else value

    def rows(table: Sequence[Sequence[float]]) -> tuple[validate.Row, ...]:
        return tuple(tuple(clean(value) for value in row) for row in table)  # type: ignore[misc]

    return TestVector(vector.kind, rows(vector.inputs), rows(vector.constants))


def vertex_source_with_denormal_mode(source: str, variant: str) -> str:
    """The float-controlled vertex source with a denormal execution mode added."""
    if variant == corners.VARIANT_DEFAULT:
        return source
    anchor = "capabilities = [4466], 4461, 32);\n"
    if anchor not in source:
        raise ValueError("the vertex source has no float-controls line, pass float_controls=True")
    return source.replace(anchor, anchor + corners._DENORM_LINES[variant] + "\n", 1)


def device_outputs(
    data: bytes,
    name: str,
    vectors: Sequence[TestVector],
    *,
    form: str,
    device: str,
    work: Path,
    variant: str = corners.VARIANT_DEFAULT,
    float_controls: bool = True,
) -> tuple[list[list[list[float]]], tuple[vertex_device.Capture, ...]]:
    """What the device returns for the vertex or compute form over `vectors`, projected to the
    captured components (the rest zero), and the captures."""
    translation = translate.translate(data, name=name)
    wanted = vertex_device.captures(translation)
    if form == FORM_VERTEX:
        source = vertex_source_with_denormal_mode(
            vertex_device.capture_source(translation, float_controls=float_controls), variant
        )
        raw = vertex_device.run_vertex(
            source,
            vectors,
            runner=VkRunner(entry=vertex_device.VKVERT_SOURCE, device=device),
            work=work,
            name=f"classify_vertex_{variant}",
        )
        return [vertex_device.project(slots, wanted) for slots in raw], wanted
    if form != FORM_COMPUTE:
        raise ValueError(f"unknown form {form!r}, expected one of {FORMS}")
    raw = validate.run_glsl_vectors(
        corners.variant_source(translate.compute_test_source(translation), variant),
        vectors,
        work=work,
        runner=VkRunner(device=device),
        name=f"classify_compute_{variant}",
    )
    return [vertex_device.project(slots, wanted) for slots in raw], wanted


def collect_evidence(
    programs: dict[str, bytes],
    *,
    form: str,
    device: str,
    work: Path,
    count: int = 16,
    seed: int = 0,
    kinds: Sequence[str] = (validate.KIND_CORNER,),
    variant: str = corners.VARIANT_DEFAULT,
    float_controls: bool = True,
    transform: Callable[[TestVector], TestVector] | None = None,
) -> list[ProgramEvidence]:
    """Run each program's vectors on the device and package the outputs for classification."""
    evidence = []
    for name, data in programs.items():
        vectors, _ = validate.build_vectors(data, seed, count, list(kinds))
        if transform is not None:
            vectors = [transform(vector) for vector in vectors]
        slots, wanted = device_outputs(
            data,
            name,
            vectors,
            form=form,
            device=device,
            work=work,
            variant=variant,
            float_controls=float_controls,
        )
        evidence.append(ProgramEvidence(name, data, vectors, slots, wanted))
    return evidence


# ------------------------------------------------------------------------------------ CLI


def render(classification: Classification) -> str:
    lines = [
        f"programs: {classification.programs}   floats compared: {classification.compared}",
        f"floats differing from the IEEE baseline: {len(classification.findings)} in "
        f"{len({finding.program for finding in classification.findings})} programs",
        "hypothesis                              fixes regressions residual",
    ]
    for name, score in classification.scores.items():
        lines.append(f"  {name:36} {score.fixes:5} {score.regressions:11} {score.residual:8}")
    for title, key in (
        ("cause", lambda f: f.cause),
        ("effect", lambda f: f.effect),
        ("first diverging unit", lambda f: f.unit or "-"),
        ("unit that wrote the float", lambda f: f.writer_unit or "-"),
        ("writer itself diverges", lambda f: str(f.writer_diverges)),
    ):
        counts = Counter(key(finding) for finding in classification.findings)
        lines.append(f"{title}: {dict(sorted(counts.items(), key=lambda item: -item[1]))}")
    for name, census in classification.census.items():
        lines.append(f"bit census against {name}: {dict(census)}")
    return "\n".join(lines)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.nv2a.corner_classify",
        description=(
            "T100b: explain the corner floats where a device form differs from the interpreter "
            "by testing one arithmetic rule at a time."
        ),
    )
    parser.add_argument("programs", nargs="+", help="microcode .bin files or directories")
    parser.add_argument("--device", default="hardware", help="hardware, radv, llvmpipe ...")
    parser.add_argument("--form", choices=FORMS, default=FORM_VERTEX)
    parser.add_argument("--count", type=int, default=16, help="vectors per kind (default 16)")
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument(
        "--kinds", nargs="+", choices=validate.KINDS, default=[validate.KIND_CORNER]
    )
    parser.add_argument(
        "--denorm",
        choices=corners.VARIANTS,
        default=corners.VARIANT_DEFAULT,
        help="add the DenormFlushToZero (ftz) or DenormPreserve (preserve) execution mode",
    )
    parser.add_argument(
        "--no-float-controls",
        action="store_true",
        help="vertex form without SignedZeroInfNanPreserve, as the renderer emits it",
    )
    parser.add_argument(
        "--strip-denormals",
        action="store_true",
        help="control: replace every denormal input and constant by a signed zero first",
    )
    parser.add_argument("--json", type=Path, help="write the findings here")
    args = parser.parse_args(argv)
    reason = vertex_device.device_unavailable_reason(args.device)
    if reason is not None:
        print(f"unavailable: {reason}", file=sys.stderr)
        return 3
    programs = vertex_device._programs(args.programs)
    if not programs:
        parser.error("no programs given")
    with tempfile.TemporaryDirectory(prefix="nv2a-corner-") as scratch:
        evidence = collect_evidence(
            programs,
            form=args.form,
            device=args.device,
            work=Path(scratch),
            count=args.count,
            seed=args.seed,
            kinds=args.kinds,
            variant=args.denorm,
            float_controls=not args.no_float_controls,
            transform=without_denormals if args.strip_denormals else None,
        )
    classification = classify_corpus(evidence)
    print(
        f"device: {vertex_device.device_name(args.device)}   form: {args.form}   "
        f"denorm mode: {args.denorm}"
    )
    print(render(classification))
    if args.json:
        args.json.write_text(
            json.dumps([finding.to_dict() for finding in classification.findings], indent=1)
        )
    unexplained = [f for f in classification.findings if f.cause == UNEXPLAINED]
    return 1 if unexplained else 0


if __name__ == "__main__":
    raise SystemExit(main())
