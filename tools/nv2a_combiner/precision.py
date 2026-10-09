# SPDX-License-Identifier: GPL-3.0-or-later
"""How far a float shader can sit from the 9-bit fixed-point hardware, in the model.

THE GAP. The NV10 documentation says the combiner computes in 9-bit signed fixed point
(a value range of [-1, 1] on a grid of 1/255 fits in 9 bits) and that the specification is
written as if it were float. A float32 shader keeps full precision between stages. Where
the hardware rounds is NOT documented, so the fixed-point side here is an INFERENCE:
every general-stage result is rounded to a multiple of 1/255 when it is written to a
register, and nothing else is rounded. The rounding is `models.WRITE_EVEN`: nearest, exact ties
to even, with the `models.TIE_GUARD` tie guard (T103c). It used to be Python's `round()` on the
raw float, which decides an exact tie by float noise, see `measure(model=None)`. Two
numbers come out, per configuration:

  bound     a rigorous bound, under that model, on |float - fixed|, by propagating a worst
            case error through the stages (each rounded write adds at most half a step,
            the maps and products amplify by their Lipschitz constants, an output scale
            multiplies). None when a mux can flip its selection, because then the two
            models may pick different operands and the difference is not small.
  measured  the largest difference seen over random fragments whose inputs are on the
            8-bit grid. A measurement, not a proof, and never above the bound when one
            exists (that is checked).

Neither number says anything about the real hardware's rounding. They say how much a float
translation can differ from THIS fixed-point model.
"""

from __future__ import annotations

import random
from dataclasses import dataclass

from tools.nv2a_combiner import config as cfg
from tools.nv2a_combiner import models, reference
from tools.nv2a_combiner.validate import make_case

QUANTUM = 1.0 / 255.0
#: Largest error of one rounded write under `models.WRITE_EVEN`: half a step, plus the tie guard,
#: because a value within the guard of a tie (but not on it) is put on the tie's even side.
HALF_STEP = (0.5 + models.TIE_GUARD) * QUANTUM

#: Lipschitz constant of each input mapping on [0, 1] or [-1, 1].
_MAPPING_GAIN = {0: 1.0, 1: 1.0, 2: 2.0, 3: 2.0, 4: 1.0, 5: 1.0, 6: 1.0, 7: 1.0}


def _operand_error(errors: dict[int, list[float]], operand: cfg.Input) -> float:
    """Error of one mapped operand. Channel 0 is rgb (blue included), channel 1 alpha."""
    return errors[operand.register][1 if operand.alpha else 0] * _MAPPING_GAIN[operand.mapping]


def error_bound(config: cfg.Config) -> float | None:
    """Worst-case |float - fixed| of the final colour under the model, or None (mux)."""
    if cfg.TEX_PASS_THROUGH in config.texture_modes:
        # A texture coordinate is not confined to [-1, 1], so the product bound fails.
        return None
    errors = {register: [0.0, 0.0] for register in range(16)}
    for stage in config.stages:
        updates: dict[tuple[int, int], float] = {}
        for half, is_rgb in ((stage.rgb, True), (stage.alpha, False)):
            out = half.output
            if out.mux:
                return None
            gains = [_operand_error(errors, operand) for operand in half.inputs]
            # |a| and |b| are at most 1, so |ab - a'b'| <= err(a) + err(b). A dot product
            # sums three such products.
            ab = (gains[0] + gains[1]) * (3.0 if out.ab_dot else 1.0)
            cd = (gains[2] + gains[3]) * (3.0 if out.cd_dot else 1.0)
            for amount, destination, blue in (
                (ab, out.ab_dst, out.blue_to_alpha_ab),
                (cd, out.cd_dst, out.blue_to_alpha_cd),
                (ab + cd, out.sum_dst, False),
            ):
                if destination == cfg.REG_ZERO:
                    continue
                written = amount * out.scale + HALF_STEP
                channels = ((0, 1) if blue else (0,)) if is_rgb else (1,)
                for channel in channels:
                    key = (destination, channel)
                    updates[key] = max(updates.get(key, 0.0), written)
        for (register, channel), value in updates.items():
            errors[register][channel] = value
    final = config.final
    errors[cfg.REG_SUM] = [max(errors[cfg.REG_SPARE0][0] + errors[cfg.REG_V1][0], 0.0), 0.0]
    errors[cfg.REG_PROD] = [
        _operand_error(errors, final.e) + _operand_error(errors, final.f),
        0.0,
    ]
    gain = [_operand_error(errors, op) for op in (final.a, final.b, final.c, final.d)]
    # D + (1-A)C + AB has partials of at most 2 (A), 1 (B), 2 (C) and 1 (D) in magnitude.
    rgb = gain[3] + gain[2] * 2.0 + gain[0] * 2.0 + gain[1]
    g_error = errors[final.g.register][1 if final.g.alpha else 0] * _MAPPING_GAIN[final.g.mapping]
    return max(rgb, g_error)


@dataclass
class Gap:
    bound: float | None
    measured: float
    #: Fragments whose 8-bit output differs between the two models, over all fragments.
    output_steps_differ: int
    fragments: int


def output_step(value: float) -> int:
    """The 8-bit step of a colour channel, nearest with ties to even and the tie guard.

    Python's `round()` here would decide an exact half step of the float side by noise."""
    return round(models.grid_round(value, "even") * models.STEPS)


def fixed_point_colour(
    config: cfg.Config, fragment: reference.Fragment, model: models.Model | None = models.WRITE_EVEN
) -> tuple[float, float, float, float]:
    """The final colour with every register write on the 1/255 grid. `model=None` is the
    pre-T103c `quantum=1/255` raw-float rounding, see `measure`."""
    if model is None:
        return reference.evaluate(config, fragment, quantum=QUANTUM)
    return reference.evaluate(config, fragment, model=model)


def measure(
    config: cfg.Config,
    *,
    fragments: int = 200,
    seed: int = 11,
    model: models.Model | None = models.WRITE_EVEN,
) -> Gap:
    """The float model against the fixed-point `model`, over random fragments.

    `model=None` is the pre-T103c behaviour (`quantum=1/255`, `round()` on the raw float, and
    `round()` for the 8-bit steps). It is kept only so the old figures can be reproduced, because
    its exact ties are decided by float noise. Do not use it for anything else."""
    rng = random.Random(seed)
    measured = 0.0
    differ = 0
    step = output_step if model is not None else (lambda value: round(value * 255))
    for _ in range(fragments):
        fragment = make_case(rng).fragment
        ideal = reference.evaluate(config, fragment)
        rounded = fixed_point_colour(config, fragment, model)
        measured = max(measured, *(abs(a - b) for a, b in zip(ideal, rounded, strict=True)))
        if any(step(a) != step(b) for a, b in zip(ideal, rounded, strict=True)):
            differ += 1
    return Gap(error_bound(config), measured, differ, fragments)
