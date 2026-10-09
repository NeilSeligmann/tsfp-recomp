# SPDX-License-Identifier: GPL-3.0-or-later
"""Random VALID combiner configurations, to exercise the whole field space.

The title uses a small corner of it: no mux, no bias, no scale of 4, two of the eight input
mappings in the final combiner. A translator validated only on those could be wrong
everywhere else, so validation also runs on configurations drawn from the full space the
decoder accepts. Every draw satisfies the decoder's own rules (`config.decode` is applied
to the encoded block, so an invalid draw fails loudly instead of being quietly skipped).
"""

from __future__ import annotations

import random

from tools.nv2a_combiner import config as cfg

_SOURCES = sorted(cfg.GENERAL_SOURCES)
_DESTINATIONS = [0, 0, 12, 12, 13, 13, 4, 5, 8, 9, 10, 11]
_FINAL_SOURCES = sorted(cfg.FINAL_SOURCES)
_STAGE0_MODES = (0, 1, 2, 3, 4, 5)
_LATER_MODES = (0, 1, 2, 3, 4, 5, cfg.TEX_DEPENDENT_AR, cfg.TEX_DEPENDENT_GB, cfg.TEX_DOT_PRODUCT)


def _input(rng: random.Random, sources: list[int]) -> cfg.Input:
    return cfg.Input(rng.choice(sources), rng.random() < 0.4, rng.randrange(8))


def _output(rng: random.Random, *, rgb: bool, mux: bool) -> cfg.Output:
    scale_code = rng.randrange(4)
    bias = scale_code < 2 and rng.random() < 0.3
    # Two outputs to one register is an error in the specification, so keep them apart.
    chosen: list[int] = []
    for _ in range(3):
        while True:
            destination = rng.choice(_DESTINATIONS)
            if destination == 0 or destination not in chosen:
                break
        chosen.append(destination)
    return cfg.Output(
        cd_dst=chosen[0],
        ab_dst=chosen[1],
        sum_dst=chosen[2],
        cd_dot=rgb and rng.random() < 0.2,
        ab_dot=rgb and rng.random() < 0.2,
        mux=mux and rng.random() < 0.25,
        bias=bias,
        scale_code=scale_code,
        blue_to_alpha_cd=rgb and rng.random() < 0.2,
        blue_to_alpha_ab=rgb and rng.random() < 0.2,
    )


def _half(rng: random.Random, *, rgb: bool, mux: bool) -> cfg.Half:
    inputs = tuple(_input(rng, _SOURCES) for _ in range(4))
    return cfg.Half((inputs[0], inputs[1], inputs[2], inputs[3]), _output(rng, rgb=rgb, mux=mux))


def random_block(rng: random.Random, *, bounded: bool = False) -> bytes:
    """A 240-byte block drawn from the decoder's accepted space.

    `bounded` leaves out the two features the precision bound cannot cover, a mux and a
    pass-through texture coordinate.
    """
    count = rng.randint(1, 8)
    each0 = rng.random() < 0.5
    each1 = rng.random() < 0.5
    stages = tuple(
        cfg.Stage(
            _half(rng, rgb=True, mux=not bounded),
            _half(rng, rgb=False, mux=not bounded),
            factor0=index if each0 else 0,
            factor1=index if each1 else 0,
        )
        for index in range(count)
    )
    final_inputs = [_input(rng, _FINAL_SOURCES) for _ in range(7)]
    # E and F make the product, so they cannot read it.
    for position in (4, 5):
        while final_inputs[position].register == cfg.REG_PROD:
            final_inputs[position] = _input(rng, _FINAL_SOURCES)
    final = cfg.Final(
        *final_inputs,
        clamp_sum=rng.random() < 0.5,
        complement_v1=rng.random() < 0.3,
        complement_r0=rng.random() < 0.3,
    )
    modes = [rng.choice(_STAGE0_MODES)]
    modes += [rng.choice(_LATER_MODES) for _ in range(1, 3)]
    # Dot-product stages exist only at stages 1 and 2 on the hardware.
    modes.append(rng.choice([mode for mode in _LATER_MODES if mode != cfg.TEX_DOT_PRODUCT]))
    if rng.random() < 0.5:
        modes[2] = cfg.TEX_DOT_PRODUCT
        modes[3] = cfg.TEX_DOT_ST
    if bounded:
        modes = [cfg.TEX_2D_PROJECTIVE if mode == cfg.TEX_PASS_THROUGH else mode for mode in modes]
    other = 0
    for stage in (2, 3):
        other |= rng.randrange(stage) << (16 + 4 * (stage - 2))
    dot_mapping = sum(rng.randrange(2) << (4 * nibble) for nibble in range(3))
    config = cfg.Config(
        stages=stages,
        final=final,
        mux_msb=rng.random() < 0.7,
        texture_modes=(modes[0], modes[1], modes[2], modes[3]),
        dot_mapping=dot_mapping,
        other_input=other,
    )
    return cfg.encode(config)


def random_config(rng: random.Random, *, bounded: bool = False) -> cfg.Config:
    return cfg.decode(random_block(rng, bounded=bounded))
