# SPDX-License-Identifier: GPL-3.0-or-later
"""Reference interpreter for NV2A (Kelvin, NV20 family) vertex-program microcode.

Written clean-room as the oracle a GLSL translator is validated against, so it is
scalar and obvious rather than fast.  float32 is emulated by computing in a Python
double and rounding to float32 after every operation.

Sources, and what each one settles
----------------------------------
[E]  envytools docs/hw/graph/xf/isa.rst (Kelvin column): the 92-bit word layout, the
     source sub-fields, the swizzle encoding and the MAC and ILU opcode numbering.
     It states no instruction semantics.
[NV] GL_NV_vertex_program, section 2.14.1.10.1 to 2.14.1.10.17 and 2.14.1.11:
     per-instruction pseudo-code, special cases, relative addressing, float rules.
[11] GL_NV_vertex_program1_1: DPH (2.14.1.10.18) and RCC (2.14.1.10.19).

Temporary 12. The title's programs read r12 and never write it (MEASURED), always after
writing the same components of o0 (the position). The xemu source, reference only, states
that R12 mirrors oPos. This model aliases temp 12 to o0, reads and writes both. [NV] and
[E] say nothing about it: [NV] lists 12 temporaries r0..r11.

Choices where the sources are silent or contradict the brief (all UNSETTLED on real
hardware, none is measured):
  * [NV] says the vertex result registers start at (0,0,0,1) but this model starts
    them at 0 as instructed.
  * [NV] says there are 12 temporaries.  The 4-bit field reaches r15 so 16 exist here.
  * A scalar op reads the swizzled C operand's x component.
  * ADD reads operands A and C (per the brief).  [E] only says the vector unit uses
    "sources #0, #1, and maybe #2", which does not say which pair ADD uses.
  * A constant read outside 0..191 returns vec4(0).  [NV] says 0 for reads outside
    0..95, which is the same rule over a different file size.
  * A unit that is NOP, and ARL's vector result, produce no value, so a destination
    write that selects them writes nothing.
  * Output writes to o[16..255] and constant writes to c[192..255] are ignored.
  * ARL: floor of the source x, NaN gives 0, infinities saturate to the int32 range.
    [NV] says a signed 32-bit register, [E] suggests 9 bits "or maybe larger".
  * A source mux of 0 reads vec4(0).
  * Summation order of DP3/DP4/DPH is left to right with a float32 rounding per op.
  * MIN and MAX use the literal [NV] pseudo-code (plain IEEE compares), so
    MIN(-0, +0) = +0 and MIN(NaN, x) = x.  Only SLT and SGE use the [NV] total order.
  * EXP: y is 0 when x overflows or underflows (literal reading of [NV]).  The z
    approximation is replaced by the exact 2^s rounded to float32.  LOG z is the
    exact log2 rounded to float32.  LOG(NaN) and EXP(NaN) give NaN, with w = 1.
  * MAD is NOT fused: the product is rounded to float32 before the add.
  * Rounding is to nearest even as instructed.  [NV] 2.14.1.11 says round to zero.
  * [NV] 2.14.1.11 says 0 * x = +0 for every x including inf and NaN is "recommended",
    denormal inputs are treated as 0, SLT/SGE order -0 below +0 and NaN outside the
    infinities, and arithmetic on NaN yields +NaN.  `SPEC_MODEL` implements all of
    that, `IEEE_MODEL` implements plain IEEE instead.  The default is `SPEC_MODEL`.
"""

from __future__ import annotations

import argparse
import ctypes
import math
import struct
from collections import Counter
from collections.abc import Sequence
from dataclasses import dataclass

MAC_NAMES = (
    "NOP",
    "MOV",
    "MUL",
    "ADD",
    "MAD",
    "DP3",
    "DPH",
    "DP4",
    "DST",
    "MIN",
    "MAX",
    "SLT",
    "SGE",
    "ARL",
)
ILU_NAMES = ("NOP", "MOV", "RCP", "RCC", "RSQ", "EXP", "LOG", "LIT")
HEADER_LOW = 0x2078
INSTRUCTION_BYTES = 16
NUM_INPUTS = 16
NUM_CONSTANTS = 192
NUM_TEMPS = 16
#: Temporary 12 is a mirror of the position output o0 (MEASURED consistency in the title's
#: programs, and stated by the xemu source, which is reference only). See the notes.
POSITION_MIRROR_TEMP = 12
NUM_OUTPUTS = 16

#: Initial value of every output register (T561). "zero" is the default. "nv" is (0, 0, 0, 1),
#: the xboxdevwiki NV2A/Vertex_Shader page's "XYZ=0 and W=1.0" (INFERRED, a web source, no
#: hardware run). Same names as `tools.nv2a.translate.OUTPUT_INITS`.
OUTPUT_INITS: dict[str, tuple[float, float, float, float]] = {
    "zero": (0.0, 0.0, 0.0, 0.0),
    "nv": (0.0, 0.0, 0.0, 1.0),
}

MUX_NONE, MUX_TEMP, MUX_INPUT, MUX_CONST = 0, 1, 2, 3

# Magnitude limits of RCC, the IEEE bit patterns 0x5F800000 and 0x1F800000 in [11].
RCC_MAX = 2.0**64
RCC_MIN = 2.0**-64
LIT_POWER_LIMIT = 128.0 - 1.0 / 256.0
MIN_NORMAL = 2.0**-126
INT32_MIN = -(2**31)
INT32_MAX = 2**31 - 1


class InterpError(Exception):
    """Undecodable program or malformed run arguments."""


@dataclass(frozen=True)
class Model:
    """The arithmetic corner cases that [NV] 2.14.1.11 specifies against IEEE."""

    zero_times_anything_is_zero: bool = True
    flush_denormal_inputs: bool = True
    total_order_compare: bool = True


SPEC_MODEL = Model()
IEEE_MODEL = Model(False, False, False)


@dataclass(frozen=True)
class Source:
    neg: int
    swizzle: int
    reg: int
    mux: int


@dataclass(frozen=True)
class Instruction:
    end: int
    xfctx_indexed: int
    out_is_sca: int
    out_addr: int
    out_target: int
    out_wm: int
    dst_wm_sca: int
    dst: int
    dst_wm_vec: int
    src2: Source
    src1: Source
    src0: Source
    ibuf_addr: int
    xfctx_addr: int
    op_vec: int
    op_sca: int


@dataclass(frozen=True)
class TraceStep:
    """One executed instruction: the decoded fields, the three source operands as read (after
    swizzle and negate) and the MAC and ILU results (None when the unit made no value)."""

    index: int
    inst: Instruction
    a: Vec
    b: Vec
    c: Vec
    mac_result: Vec | None
    ilu_result: Vec | None


@dataclass
class InterpResult:
    outputs: list[list[float]]
    a0: int
    constants: list[list[float]]
    executed: Counter[str]


def _decode_source(bits: int) -> Source:
    return Source(
        neg=(bits >> 14) & 1,
        swizzle=(bits >> 6) & 0xFF,
        reg=(bits >> 2) & 0xF,
        mux=bits & 3,
    )


def decode(words: Sequence[int]) -> Instruction:
    """Decode four little-endian dwords into an Instruction.

    Dword 0 is unused.  The 92-bit envytools word is dwords 1..3 with its bit n in
    dword 3 - n // 32 at bit n % 32, so dword 3 holds bits 0..31.
    """
    if len(words) != 4:
        raise InterpError(f"an instruction is 4 dwords, got {len(words)}")
    for word in words:
        if not 0 <= word <= 0xFFFFFFFF:
            raise InterpError(f"dword out of range: {word!r}")
    wide = words[3] | (words[2] << 32) | (words[1] << 64)

    def field(low: int, high: int) -> int:
        return (wide >> low) & ((1 << (high - low + 1)) - 1)

    op_vec = field(85, 88)
    if op_vec >= len(MAC_NAMES):
        raise InterpError(f"undefined MAC opcode {op_vec}")
    return Instruction(
        end=field(0, 0),
        xfctx_indexed=field(1, 1),
        out_is_sca=field(2, 2),
        out_addr=field(3, 10),
        out_target=field(11, 11),
        out_wm=field(12, 15),
        dst_wm_sca=field(16, 19),
        dst=field(20, 23),
        dst_wm_vec=field(24, 27),
        src2=_decode_source(field(28, 42)),
        src1=_decode_source(field(43, 57)),
        src0=_decode_source(field(58, 72)),
        ibuf_addr=field(73, 76),
        xfctx_addr=field(77, 84),
        op_vec=op_vec,
        op_sca=field(89, 91),
    )


def decode_program(program: bytes) -> list[Instruction]:
    """Decode a headed program or a bare run of 16-byte instructions."""
    size = len(program)
    if size % INSTRUCTION_BYTES == 4:
        (header,) = struct.unpack_from("<I", program, 0)
        count = (size - 4) // INSTRUCTION_BYTES
        if header & 0xFFFF != HEADER_LOW or header >> 16 != count:
            raise InterpError(f"bad program header {header:#010x} for {count} instructions")
        body = program[4:]
    elif size % INSTRUCTION_BYTES == 0:
        body = program
    else:
        raise InterpError(f"program length {size} is neither 16n nor 16n+4")
    return [
        decode(struct.unpack_from("<4I", body, offset))
        for offset in range(0, len(body), INSTRUCTION_BYTES)
    ]


# float32 helpers ---------------------------------------------------------------


def _f32(value: float) -> float:
    """Round a double to float32 (nearest even), overflowing to inf without raising."""
    return ctypes.c_float(value).value


def _arith(value: float) -> float:
    """Round an arithmetic result.  Per [NV] a NaN result is the canonical +NaN."""
    rounded = _f32(value)
    return math.nan if rounded != rounded else rounded


def _flush(value: float) -> float:
    """Treat a denormal as a zero of the same sign."""
    return math.copysign(0.0, value) if 0.0 < abs(value) < MIN_NORMAL else value


def _mul(left: float, right: float, model: Model) -> float:
    if model.zero_times_anything_is_zero and (left == 0.0 or right == 0.0):
        return 0.0
    return _arith(left * right)


def _add(left: float, right: float) -> float:
    return _arith(left + right)


def _order_key(value: float) -> tuple[int, float, int]:
    """[NV] total order: -NaN < -Inf < ... < -0 < +0 < ... < +Inf < +NaN."""
    if value != value:
        return (0, 0.0, 0) if math.copysign(1.0, value) < 0.0 else (2, 0.0, 0)
    return (1, value, 0 if math.copysign(1.0, value) < 0.0 else 1)


def _less(left: float, right: float, model: Model) -> bool:
    if model.total_order_compare:
        return _order_key(left) < _order_key(right)
    return left < right


def _greater_equal(left: float, right: float, model: Model) -> bool:
    if model.total_order_compare:
        return _order_key(left) >= _order_key(right)
    return left >= right


def _floor_to_int32(value: float) -> int:
    if value != value:
        return 0
    if value == math.inf:
        return INT32_MAX
    if value == -math.inf:
        return INT32_MIN
    return max(INT32_MIN, min(INT32_MAX, math.floor(value)))


# MAC unit ----------------------------------------------------------------------

Vec = list[float]


def _dot(left: Vec, right: Vec, count: int, model: Model) -> float:
    total = _mul(left[0], right[0], model)
    for index in range(1, count):
        total = _add(total, _mul(left[index], right[index], model))
    return total


def _mac(name: str, a: Vec, b: Vec, c: Vec, model: Model) -> Vec | None:
    """The vector result, or None for NOP and ARL (which has no vector result)."""
    if name == "MOV":
        return list(a)
    if name == "MUL":
        return [_mul(a[i], b[i], model) for i in range(4)]
    if name == "ADD":
        return [_add(a[i], c[i]) for i in range(4)]
    if name == "MAD":
        return [_add(_mul(a[i], b[i], model), c[i]) for i in range(4)]
    if name == "DP3":
        return [_dot(a, b, 3, model)] * 4
    if name == "DPH":
        return [_add(_dot(a, b, 3, model), b[3])] * 4
    if name == "DP4":
        return [_dot(a, b, 4, model)] * 4
    if name == "DST":
        return [1.0, _mul(a[1], b[1], model), a[2], b[3]]
    if name == "MIN":
        return [a[i] if a[i] < b[i] else b[i] for i in range(4)]
    if name == "MAX":
        return [a[i] if a[i] >= b[i] else b[i] for i in range(4)]
    if name == "SLT":
        return [1.0 if _less(a[i], b[i], model) else 0.0 for i in range(4)]
    if name == "SGE":
        return [1.0 if _greater_equal(a[i], b[i], model) else 0.0 for i in range(4)]
    return None


# ILU unit ----------------------------------------------------------------------


def _reciprocal(value: float) -> float:
    # [NV] demands RCP(1.0) == 1.0 exactly, which correctly rounded division already gives.
    if value == 0.0:
        return math.copysign(math.inf, value)
    if value != value:
        return math.nan
    return _arith(1.0 / value)


def _rcc(value: float) -> float:
    result = _reciprocal(value)
    if result != result:
        return result
    magnitude = min(max(abs(result), RCC_MIN), RCC_MAX)
    return math.copysign(magnitude, result)


def _rsq(value: float) -> float:
    if value != value:
        return math.nan
    magnitude = abs(value)
    if magnitude == 0.0:
        return math.inf
    if magnitude == math.inf:
        return 0.0
    return _arith(1.0 / math.sqrt(magnitude))


def _exp(value: float) -> Vec:
    if value != value:
        return [math.nan, math.nan, math.nan, 1.0]
    if value == -math.inf:
        return [0.0, 0.0, 0.0, 1.0]
    if value == math.inf:
        return [math.inf, 0.0, math.inf, 1.0]
    floor = math.floor(value)
    exponent = max(-300, min(300, floor))
    integer_part = _f32(math.ldexp(1.0, exponent))
    if integer_part != math.inf and 0.0 < integer_part < MIN_NORMAL:
        integer_part = 0.0
    if integer_part == math.inf:
        return [math.inf, 0.0, math.inf, 1.0]
    if integer_part == 0.0:
        return [0.0, 0.0, 0.0, 1.0]
    return [integer_part, _arith(value - floor), _arith(2.0**value), 1.0]


def _log(value: float) -> Vec:
    if value != value:
        return [math.nan, math.nan, math.nan, 1.0]
    magnitude = abs(value)
    if magnitude == 0.0:
        return [-math.inf, 1.0, -math.inf, 1.0]
    if magnitude == math.inf:
        return [math.inf, 1.0, math.inf, 1.0]
    fraction, exponent = math.frexp(magnitude)
    mantissa = fraction * 2.0
    exponent -= 1
    return [float(exponent), mantissa, _arith(exponent + math.log2(mantissa)), 1.0]


def _lit_power(specular: float, power: float) -> float:
    """specular ** power as the spec's EXP(w * LOG(y)) chain, computed exactly.

    [NV] LIT: 0.0 times anything is 0.0, so any base to the power 0.0 is 1.0, whatever the
    arithmetic model says about 0 * inf. The EXP and LOG approximations are only required
    to 2^-11, so the exact result is the reference.
    """
    if power != power or specular != specular:
        return math.nan
    if power == 0.0:
        return 1.0
    if specular == 0.0:
        return 0.0 if power > 0.0 else math.inf
    if specular == math.inf:
        return math.inf if power > 0.0 else 0.0
    return _exp(power * math.log2(specular))[2]


def _lit(c: Vec, model: Model) -> Vec:
    diffuse, specular, power = c[0], c[1], c[3]
    if power < -LIT_POWER_LIMIT:
        power = -LIT_POWER_LIMIT
    elif power > LIT_POWER_LIMIT:
        power = LIT_POWER_LIMIT
    if diffuse < 0.0:
        diffuse = 0.0
    if specular < 0.0:
        specular = 0.0
    specular_term = _lit_power(specular, power) if diffuse > 0.0 else 0.0
    return [1.0, diffuse, specular_term, 1.0]


def _ilu(name: str, c: Vec, model: Model) -> Vec | None:
    scalar = c[0]
    if name == "MOV":
        return list(c)
    if name == "RCP":
        return [_reciprocal(scalar)] * 4
    if name == "RCC":
        return [_rcc(scalar)] * 4
    if name == "RSQ":
        return [_rsq(scalar)] * 4
    if name == "EXP":
        return _exp(scalar)
    if name == "LOG":
        return _log(scalar)
    if name == "LIT":
        return _lit(c, model)
    return None


# Execution ---------------------------------------------------------------------


def _check_vectors(label: str, vectors: Sequence[Sequence[float]], count: int) -> list[Vec]:
    if len(vectors) != count or any(len(vector) != 4 for vector in vectors):
        raise InterpError(f"{label} must be {count} vectors of 4 floats")
    return [[_f32(float(value)) for value in vector] for vector in vectors]


def _write_masked(target: Vec, value: Vec, mask: int) -> None:
    """Write components under a 4-bit mask where x is bit 3 and w is bit 0."""
    for component in range(4):
        if (mask >> (3 - component)) & 1:
            target[component] = value[component]


def run(
    program: bytes,
    inputs: Sequence[Sequence[float]],
    constants: Sequence[Sequence[float]],
    *,
    a0: int = 0,
    model: Model = SPEC_MODEL,
    trace: list[TraceStep] | None = None,
    output_init: str = "zero",
) -> InterpResult:
    """Execute a vertex program once and return outputs, A0, the constant file and a count.

    `trace`, when given, receives one `TraceStep` per executed instruction (T100b).
    `output_init` names the initial value of every output (`OUTPUT_INITS`, T561)."""
    instructions = decode_program(program)
    input_file = _check_vectors("inputs", inputs, NUM_INPUTS)
    constant_file = _check_vectors("constants", constants, NUM_CONSTANTS)
    temps = [[0.0] * 4 for _ in range(NUM_TEMPS)]
    if output_init not in OUTPUT_INITS:
        raise InterpError(f"output_init must be one of {sorted(OUTPUT_INITS)}, got {output_init!r}")
    outputs = [list(OUTPUT_INITS[output_init]) for _ in range(NUM_OUTPUTS)]
    temps[POSITION_MIRROR_TEMP] = outputs[0]
    executed: Counter[str] = Counter()
    address = a0

    def read(source: Source, inst: Instruction) -> Vec:
        if source.mux == MUX_TEMP:
            raw = temps[source.reg]
        elif source.mux == MUX_INPUT:
            raw = input_file[inst.ibuf_addr]
        elif source.mux == MUX_CONST:
            index = inst.xfctx_addr + (address if inst.xfctx_indexed else 0)
            raw = constant_file[index] if 0 <= index < NUM_CONSTANTS else [0.0] * 4
        else:
            raw = [0.0] * 4
        if model.flush_denormal_inputs:
            raw = [_flush(value) for value in raw]
        # Component i comes from the pair at bits (7 - 2i, 6 - 2i), x in the high pair.
        swizzled = [raw[(source.swizzle >> (6 - 2 * i)) & 3] for i in range(4)]
        return [-value for value in swizzled] if source.neg else swizzled

    for inst in instructions:
        a, b, c = read(inst.src0, inst), read(inst.src1, inst), read(inst.src2, inst)
        mac_name = MAC_NAMES[inst.op_vec]
        ilu_name = ILU_NAMES[inst.op_sca]
        mac_result = _mac(mac_name, a, b, c, model)
        ilu_result = _ilu(ilu_name, c, model)
        new_address = _floor_to_int32(a[0]) if mac_name == "ARL" else None

        # xemu vsh-prog.c: paired ILU writes R1 regardless of the encoded temp destination;
        # a paired MAC temp write to R1 is suppressed. Both units still read their sources
        # before either write, and output/constant routing is independent of these temp rules.
        paired = inst.op_vec != 0 and inst.op_sca != 0
        if mac_result is not None and not (paired and inst.dst == 1):
            _write_masked(temps[inst.dst], mac_result, inst.dst_wm_vec)
        if ilu_result is not None:
            _write_masked(temps[1 if paired else inst.dst], ilu_result, inst.dst_wm_sca)
        routed = ilu_result if inst.out_is_sca else mac_result
        if inst.out_wm and routed is not None:
            if inst.out_target == 1 and inst.out_addr < NUM_OUTPUTS:
                _write_masked(outputs[inst.out_addr], routed, inst.out_wm)
            elif inst.out_target == 0 and inst.out_addr < NUM_CONSTANTS:
                _write_masked(constant_file[inst.out_addr], routed, inst.out_wm)
        if new_address is not None:
            address = new_address

        if trace is not None:
            trace.append(TraceStep(len(trace), inst, a, b, c, mac_result, ilu_result))
        if mac_name != "NOP":
            executed[mac_name] += 1
        if ilu_name != "NOP":
            executed["ILU_MOV" if ilu_name == "MOV" else ilu_name] += 1
        if inst.end:
            break

    return InterpResult(outputs=outputs, a0=address, constants=constant_file, executed=executed)


def _parse_vectors(items: Sequence[str], limit: int, label: str) -> dict[int, Vec]:
    parsed: dict[int, Vec] = {}
    for item in items:
        index_text, _, values_text = item.partition(":")
        values = [float(part) for part in values_text.split(",")]
        if not 0 <= int(index_text) < limit or len(values) != 4:
            raise InterpError(f"bad {label} {item!r}, want INDEX:x,y,z,w with INDEX < {limit}")
        parsed[int(index_text)] = values
    return parsed


def main() -> None:
    parser = argparse.ArgumentParser(description="Run a Kelvin vertex program on given registers.")
    parser.add_argument("hex_program", help="program bytes as hex, headed or bare")
    parser.add_argument("--input", action="append", default=[], help="vN as N:x,y,z,w")
    parser.add_argument("--const", action="append", default=[], help="hardware cN as N:x,y,z,w")
    parser.add_argument("--a0", type=int, default=0, help="initial A0")
    parser.add_argument("--ieee", action="store_true", help="plain IEEE corner cases")
    parser.add_argument(
        "--output-init",
        choices=sorted(OUTPUT_INITS),
        default="zero",
        help="initial value of the outputs: zero, or nv = (0, 0, 0, 1) (T561, INFERRED)",
    )
    args = parser.parse_args()
    input_file = [[0.0] * 4 for _ in range(NUM_INPUTS)]
    constant_file = [[0.0] * 4 for _ in range(NUM_CONSTANTS)]
    for index, vector in _parse_vectors(args.input, NUM_INPUTS, "input").items():
        input_file[index] = vector
    for index, vector in _parse_vectors(args.const, NUM_CONSTANTS, "const").items():
        constant_file[index] = vector
    result = run(
        bytes.fromhex(args.hex_program),
        input_file,
        constant_file,
        a0=args.a0,
        model=IEEE_MODEL if args.ieee else SPEC_MODEL,
        output_init=args.output_init,
    )
    for index, vector in enumerate(result.outputs):
        print(f"o{index}", vector)
    print("a0", result.a0)
    print("executed", dict(result.executed))


if __name__ == "__main__":
    main()
