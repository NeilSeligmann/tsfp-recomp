# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic vertex programs for validation: a directed set and a seeded random set.

Nothing here derives from the user's executable, so it can be committed and run in CI.
The title's own programs never use `LOG`, `LIT` or `DPH` and exercise operand shapes in a
narrow band, so the translator is also checked on programs built to cover every opcode
and every operand feature (swizzle, negate, write masks, the three register files,
relative constants, dual issue, MAC and ILU writing the same temporary).

`random_programs` draws instructions field by field under the rules the translator and
the interpreter both support. It never generates a constant-file write, a reserved
output address, an undefined opcode or an output written by a NOP unit, because those are
the declared unsupported cases (`translate.Unsupported`).
"""

from __future__ import annotations

import argparse
import random
from pathlib import Path

from tools.nv2a import isa

VALID_OUTPUTS = (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12)
SWIZZLES = (0x1B, 0x00, 0x55, 0xAA, 0xFF, 0xE4, 0x93, 0xC6, 0x1A, 0x2D)
#: Constant window the relative programs read: hardware constants 96..103 plus A0.
RELATIVE_BASE = 96


def _headed(words: list[tuple[int, int, int, int]]) -> bytes:
    header = (len(words) << 16 | isa.HEADER_VERSION).to_bytes(4, "little")
    return header + b"".join(b"".join(w.to_bytes(4, "little") for w in i) for i in words)


def pack(instructions: list[tuple[int, int, int, int]]) -> bytes:
    """Headed program bytes. The last instruction gets the FINAL bit."""
    words = list(instructions)
    first, second, third, fourth = words[-1]
    words[-1] = (first, second, third, fourth | 1)
    return _headed(words)


def _source(prefix: str, mux: int, index: int, swizzle: int, negate: int) -> dict[str, int]:
    fields = {f"{prefix}_mux": mux, f"{prefix}_swizzle": swizzle, f"{prefix}_neg": negate}
    if prefix == "c":
        fields["c_temp_low"], fields["c_temp_high"] = index & 3, index >> 2
    else:
        fields[f"{prefix}_temp"] = index
    return fields


def _instruction(
    *,
    mac: int = 0,
    ilu: int = 0,
    a: tuple[int, int, int, int] = (isa.MUX_INPUT, 0, 0x1B, 0),
    b: tuple[int, int, int, int] = (isa.MUX_CONST, 0, 0x1B, 0),
    c: tuple[int, int, int, int] = (isa.MUX_TEMP, 0, 0x1B, 0),
    input_index: int = 0,
    const: int = 96,
    relative: bool = False,
    temp_out: int = 0,
    mac_mask: int = 0,
    ilu_mask: int = 0,
    out_address: int = 0,
    out_mask: int = 0,
    out_is_ilu: bool = False,
) -> tuple[int, int, int, int]:
    fields = {
        "mac": mac,
        "ilu": ilu,
        "input": input_index,
        "const": const,
        "relative": int(relative),
        "temp_out": temp_out,
        "mac_mask": mac_mask,
        "ilu_mask": ilu_mask,
        "out_mask": out_mask,
        "out_address": out_address,
        "out_to_output": 1,
        "out_is_ilu": int(out_is_ilu),
    }
    for prefix, source in (("a", a), ("b", b), ("c", c)):
        fields.update(_source(prefix, *source))
    return isa.encode_instruction(**fields)


def directed_programs() -> dict[str, bytes]:
    """One program per opcode and operand feature, each with a visible result in o0."""
    programs: dict[str, bytes] = {}
    prelude = _instruction(
        mac=1,
        a=(isa.MUX_INPUT, 0, 0x1B, 0),
        input_index=2,
        temp_out=2,
        mac_mask=0xF,
    )
    for mac in range(1, 13):
        name = isa.MAC_NAMES[mac]
        for variant, (a_swizzle, b_swizzle, negate) in enumerate(
            ((0x1B, 0x1B, 0), (0xE4, 0x93, 1), (0xAA, 0x1A, 2))
        ):
            body = _instruction(
                mac=mac,
                a=(isa.MUX_INPUT, 0, a_swizzle, negate & 1),
                b=(isa.MUX_CONST, 0, b_swizzle, negate >> 1),
                c=(isa.MUX_TEMP, 2, 0x1B, 0),
                input_index=1,
                const=100,
                temp_out=1,
                mac_mask=0xF,
            )
            show = _instruction(
                ilu=1,
                c=(isa.MUX_TEMP, 1, 0x1B, 0),
                out_mask=0xF,
                out_is_ilu=True,
            )
            programs[f"mac_{name.lower()}_{variant}"] = pack([prelude, body, show])
    for ilu in range(1, 8):
        name = isa.ILU_NAMES[ilu]
        for variant, (swizzle, negate, mask) in enumerate(
            ((0x00, 0, 0xF), (0xFF, 1, 0x8), (0x1B, 0, 0xE), (0x55, 0, 0x1))
        ):
            body = _instruction(
                ilu=ilu,
                c=(isa.MUX_INPUT, 0, swizzle, negate),
                input_index=1,
                temp_out=3,
                ilu_mask=mask,
            )
            show = _instruction(
                mac=1,
                a=(isa.MUX_TEMP, 3, 0x1B, 0),
                out_mask=0xF,
            )
            programs[f"ilu_{name.lower()}_{variant}"] = pack([body, show])
    # ARL then relative constants through both units, window 96..103 plus the loaded A0.
    load = _instruction(mac=isa.MAC_ARL, a=(isa.MUX_INPUT, 0, 0x00, 0), input_index=0)
    programs["arl_relative_mov"] = pack(
        [
            load,
            _instruction(
                ilu=1,
                c=(isa.MUX_CONST, 0, 0x1B, 0),
                const=RELATIVE_BASE,
                relative=True,
                out_mask=0xF,
                out_is_ilu=True,
            ),
        ]
    )
    programs["arl_relative_dp4"] = pack(
        [
            load,
            _instruction(
                mac=7,
                a=(isa.MUX_INPUT, 0, 0x1B, 0),
                b=(isa.MUX_CONST, 0, 0x1B, 0),
                input_index=1,
                const=RELATIVE_BASE + 2,
                relative=True,
                out_mask=0xF,
            ),
        ]
    )
    # MAC and ILU in one instruction, writing different components of one temporary, the
    # output taking the ILU result, then the temporary shown.
    programs["dual_issue_split_temp"] = pack(
        [
            _instruction(
                mac=2,
                ilu=2,
                a=(isa.MUX_INPUT, 0, 0x1B, 0),
                b=(isa.MUX_CONST, 0, 0x1B, 0),
                c=(isa.MUX_INPUT, 0, 0xFF, 0),
                input_index=1,
                const=97,
                temp_out=4,
                mac_mask=0x7,
                ilu_mask=0x8,
                out_mask=0xF,
                out_is_ilu=True,
            ),
            _instruction(
                mac=1,
                a=(isa.MUX_TEMP, 4, 0x1B, 0),
                out_address=3,
                out_mask=0xF,
            ),
        ]
    )
    # Operands read the temporary the SAME instruction rewrites.
    programs["read_before_write"] = pack(
        [
            prelude,
            _instruction(
                mac=3,
                ilu=3,
                a=(isa.MUX_TEMP, 2, 0x1B, 0),
                c=(isa.MUX_TEMP, 2, 0x00, 0),
                temp_out=2,
                mac_mask=0xF,
                ilu_mask=0x0,
                out_mask=0xF,
                out_is_ilu=True,
            ),
        ]
    )
    # FINAL on the first of two instructions: the second must never run.
    stop = _instruction(mac=1, a=(isa.MUX_INPUT, 0, 0x1B, 0), input_index=1, out_mask=0xF)
    never = _instruction(mac=1, a=(isa.MUX_INPUT, 0, 0x1B, 0), input_index=2, out_mask=0xF)
    programs["final_before_the_last_instruction"] = _headed(
        [(stop[0], stop[1], stop[2], stop[3] | 1), never]
    )
    return programs


#: ILU opcodes whose results are only approximate in hardware (EXP, LOG z need 2^-11 only).
APPROXIMATE_ILU = (5, 6, 7)


def _random_instruction(rng: random.Random, *, approximate: bool) -> tuple[int, int, int, int]:
    mac = rng.choice([0, *range(1, 14)])
    ilu = rng.choice([0, *(n for n in range(1, 8) if approximate or n not in APPROXIMATE_ILU)])
    if mac == 0 and ilu == 0:
        mac = rng.randrange(1, 13)

    def source() -> tuple[int, int, int, int]:
        return (
            rng.choice((isa.MUX_TEMP, isa.MUX_INPUT, isa.MUX_CONST)),
            rng.randrange(16),
            rng.choice(SWIZZLES) if rng.random() < 0.8 else rng.randrange(256),
            rng.randrange(2),
        )

    mac_writes = mac not in (0, isa.MAC_ARL)
    out_is_ilu = ilu != 0 and (not mac_writes or rng.random() < 0.5)
    return _instruction(
        mac=mac,
        ilu=ilu,
        a=source(),
        b=source(),
        c=source(),
        input_index=rng.randrange(1, 4),
        const=rng.randrange(RELATIVE_BASE, RELATIVE_BASE + 8),
        relative=rng.random() < 0.25,
        temp_out=rng.randrange(8),
        mac_mask=rng.randrange(16) if mac_writes and rng.random() < 0.6 else 0,
        ilu_mask=rng.randrange(16) if ilu and rng.random() < 0.6 else 0,
        out_address=rng.choice(VALID_OUTPUTS),
        out_mask=rng.randrange(16) if (out_is_ilu or mac_writes) and rng.random() < 0.7 else 0,
        out_is_ilu=out_is_ilu,
    )


def random_programs(
    seed: int, count: int, length: int = 8, *, approximate: bool = False
) -> dict[str, bytes]:
    """`count` seeded random programs of `length` instructions, plus an ARL up front so
    relative reads move. Each ends by copying r0..r3 to outputs so temporaries show.

    EXP, LOG and LIT are left out unless `approximate`: their z results are only specified
    to 2^-11, so a chain that feeds one into a reciprocal is ill conditioned and a numeric
    comparison of the whole program says nothing (MEASURED, see docs/vertex-translator.md).
    They are covered one at a time by `directed_programs`.
    """
    rng = random.Random(seed)
    programs: dict[str, bytes] = {}
    for number in range(count):
        body = [_random_instruction(rng, approximate=approximate) for _ in range(length)]
        if rng.random() < 0.5:
            body.insert(0, _instruction(mac=isa.MAC_ARL, a=(isa.MUX_INPUT, 0, 0x00, 0)))
        for temp, address in enumerate((9, 10, 11, 12)):
            body.append(
                _instruction(
                    ilu=1,
                    c=(isa.MUX_TEMP, temp, 0x1B, 0),
                    out_address=address,
                    out_mask=0xF,
                    out_is_ilu=True,
                )
            )
        programs[f"random_{seed}_{number:04d}"] = pack(body)
    return programs


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Write the synthetic programs as .bin files.")
    parser.add_argument("--out", type=Path, default=Path("generated/shaders/directed"))
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--random", type=int, default=200)
    parser.add_argument(
        "--approximate", action="store_true", help="let random programs use EXP, LOG and LIT"
    )
    args = parser.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)
    programs = {
        **directed_programs(),
        **random_programs(args.seed, args.random, approximate=args.approximate),
    }
    for name, data in programs.items():
        (args.out / f"{name}.bin").write_bytes(data)
    print(f"{len(programs)} programs written to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
