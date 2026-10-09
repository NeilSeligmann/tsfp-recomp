# SPDX-License-Identifier: GPL-3.0-or-later
"""NV2A vertex-program microcode to GLSL 4.50 (Vulkan), offline.

Why GLSL text: the programs are translated offline, the source is diffable and
mutation-testable, and `glslangValidator` is the project's toolchain.

Shape of the output. `Translation.body` is a self-contained fragment: constants, the
helpers the program needs, and `void nv2a_program()`. It reads and writes three
module-level arrays the wrapper declares before it: `vec4 v[16]` (inputs), `vec4 c[192]`
(constants, HARDWARE index: the D3D8 register c[n] is hardware index n + 96, which is the
host's concern) and `vec4 o[16]` (outputs by NV2A output ADDRESS). Temporaries r0..r15
and the address register `a0` are locals of the function and start at zero. Both
wrappers (`compute_test_source`, `vertex_shader_source`) call the same function.

Per instruction every source is read into a local first, then the MAC and ILU results are
computed from those locals, then all writes happen, so a MAC and an ILU in one instruction
both see the old register values. Results are `precise` so no driver fuses a mad.

Sources for the semantics, cited by tag in the notes and comments:

  [D3D] Microsoft Learn, "Vertex Shader Instructions" pages for vs_1_1 (add ... rsq, lit,
        dst, exp/expp, log/logp, mov, frc, slt, sge, min, max, the address and constant
        register pages). These are the DirectX 9 SDK reference pages for the vs_1_1 set.
  [NVD8] NVIDIA "Introduction to DX8 Vertex Shaders" (Huddy): the expp and log result
        quartets, and the pairing `MOV A0.x` with the GL `ARL A0.x`.
  [NV]  GL_NV_vertex_program, for EXP, LOG and LIT special cases ONLY. The translator was
        first written from [D3D] alone and the numeric validator disagreed with the
        interpreter (derived from [NV]) on overflow, underflow, zero and NaN inputs of
        those three. They were aligned to [NV], the vendor's own statement for this
        hardware family. See docs/vertex-translator.md.
  [ASM] Measured behaviour of the title's own assembler (which unit and swizzle each D3D
        instruction becomes).

Hardware reads that no source in that set settles are marked UNSOURCED or UNSETTLED in
`Translation.notes` and in the comments below.

Output position. Xbox D3D8 vertex programs commonly end with
`rcc r1.x, r12.w; mad oPos.xyz, r12, r1.x, c[-37]`, so they do the perspective divide
and the viewport transform THEMSELVES. INFERRED: NV2A `oPos` is therefore not a Vulkan
clip-space position. The vertex shader emits it RAW into `gl_Position` and leaves the
conversion to the renderer.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from dataclasses import dataclass, field
from pathlib import Path

from tools.nv2a import isa, viewport

#: ARL conversion. "floor" is the hardware reading (the opcode is the NV address register
#: load), "nearest" is what the Microsoft vs_1_1 pages state. UNSETTLED between them.
ARL_ROUNDING = "floor"

#: ARL clamps the source to this magnitude first so the float to int conversion is always
#: defined. With const <= 255 any |a0| beyond it already reads outside the 192 constants,
#: so the clamp never changes a result.
ARL_CLAMP = 1024.0

#: Largest power LIT accepts [D3D lit page].
LIT_MAX_POWER = "127.9961"

#: RCC clamp range as exact float bit patterns: 2^-64 and 2^64. UNSOURCED in the D3D set
#: (vs_1_1 has no rcc): the textbook "finite magnitude range, sign kept".
RCC_MIN_BITS = "0x1f800000u"
RCC_MAX_BITS = "0x5f800000u"

OUTPUT_ARRAY_SIZE = isa.OUTPUT_COUNT
CONSTANT_COUNT = isa.CONSTANT_COUNT

INPUT_FLOATS = (isa.INPUT_COUNT + CONSTANT_COUNT) * 4
OUTPUT_FLOATS = OUTPUT_ARRAY_SIZE * 4

#: Initial value of every output register `o[i]` before the program runs (T561). "zero" is the
#: long-standing default (both the translator and `tools/nv2a/interp.py` start at 0). "nv" is
#: (0, 0, 0, 1): the xboxdevwiki NV2A/Vertex_Shader page says the outputs start "XYZ=0 and
#: W=1.0" (a web source, INFERRED, no hardware run). It differs from "zero" in w only: for an
#: output the program leaves w unwritten (`oD0.w` of an xyz-only colour, `oT.w` of an xy-only
#: coordinate, `oPos.w`), and for a program that reads an output back (r12 mirrors o0). The
#: option is shared by `compute_test_source` and `vertex_shader_source`, and by the
#: interpreter (`interp.run(output_init=)`), so the numeric validation can compare like with like.
OUTPUT_INITS: dict[str, str] = {
    "zero": "vec4(0.0)",
    "nv": "vec4(0.0, 0.0, 0.0, 1.0)",
}
DEFAULT_OUTPUT_INIT = "zero"


def output_init_text(name: str) -> str:
    """The GLSL initialiser of option `name`, ValueError for an unknown option."""
    if name not in OUTPUT_INITS:
        raise ValueError(f"output_init must be one of {sorted(OUTPUT_INITS)}, got {name!r}")
    return OUTPUT_INITS[name]


#: Fixed varying locations of the vertex shader (oPos and oPts are builtins).
VARYINGS: dict[int, tuple[str, int, str]] = {
    3: ("oD0", 0, "vec4"),
    4: ("oD1", 1, "vec4"),
    5: ("oFog", 2, "float"),
    9: ("oT0", 3, "vec4"),
    10: ("oT1", 4, "vec4"),
    11: ("oT2", 5, "vec4"),
    12: ("oT3", 6, "vec4"),
    7: ("oB0", 7, "vec4"),
    8: ("oB1", 8, "vec4"),
}

# (bit, letter), x is the HIGH bit of the 4-bit write mask.
#: Temporary 12 mirrors o0 (the position output).
POSITION_MIRROR_TEMP = 12
_MASK_BITS = ((8, "x"), (4, "y"), (2, "z"), (1, "w"))


class Unsupported(Exception):
    """The program cannot be translated. `code` says why, for coverage counting."""

    CODES = ("undecodable", "undefined-opcode", "constant-file-write", "reserved-output", "other")

    def __init__(self, code: str, message: str = "") -> None:
        if code not in self.CODES:
            raise ValueError(f"unknown Unsupported code {code!r}")
        super().__init__(f"{code}: {message}" if message else code)
        self.code = code


@dataclass
class Translation:
    body: str
    name: str
    opcodes: Counter[str]
    #: output address -> OR of the written component masks (x=8, y=4, z=2, w=1)
    outputs_written: dict[int, int]
    relative_reads: bool
    uses_a0: bool
    #: instructions translated, up to and including the first one with the FINAL bit
    instruction_count: int
    notes: list[str]
    #: input registers the program reads, ascending
    inputs_read: tuple[int, ...] = field(default=())
    #: viewport shape of the oPos writes (tools/nv2a/viewport.py): full, z-only or none
    viewport: str = "none"


_ARL_CLAMPED = f"clamp(x, -{ARL_CLAMP:.1f}, {ARL_CLAMP:.1f})"
_ARL_ROUND_EXPRESSION = (
    f"floor({_ARL_CLAMPED})" if ARL_ROUNDING == "floor" else f"floor({_ARL_CLAMPED} + 0.5)"
)

# ---------------------------------------------------------------------------------------
# GLSL helper library. Each entry is (dependencies, text). Emitted in this order, only the
# ones a program needs.

_HELPERS: dict[str, tuple[tuple[str, ...], str]] = {
    "NV_INF": (
        (),
        "#define NV_INF uintBitsToFloat(0x7f800000u)",
    ),
    "NV_FLT_MAX": (
        (),
        "const float NV_FLT_MAX = 3.4028235e38;",
    ),
    "nv_c_at": (
        (),
        """\
vec4 nv_c_at(int index) {
    // CHOICE: out of range reads are zero. Hardware behaviour UNSETTLED.
    if (index < 0 || index >= @CONSTANT_COUNT@) {
        return vec4(0.0);
    }
    return c[index];
}""".replace("@CONSTANT_COUNT@", str(CONSTANT_COUNT)),
    ),
    "nv_arl": (
        (),
        """\
int nv_arl(float x) {
    // CHOICE: NaN gives 0, and the magnitude is clamped so the int conversion is defined.
    if (x != x) {
        return 0;
    }
    return int(@ROUND@);
}""".replace("@ROUND@", _ARL_ROUND_EXPRESSION),
    ),
    "nv_rcp": (
        ("NV_INF",),
        """\
float nv_rcp(float x) {
    // x == 0 gives infinity with the sign of the zero: UNSETTLED, D3D gives FLT_MAX or +inf.
    if (x == 0.0) {
        return uintBitsToFloat((floatBitsToUint(x) & 0x80000000u) | 0x7f800000u);
    }
    return 1.0 / x;
}""",
    ),
    "nv_rcc": (
        ("nv_rcp",),
        """\
#define NV_RCC_MIN uintBitsToFloat(@MIN@)
#define NV_RCC_MAX uintBitsToFloat(@MAX@)
float nv_rcc(float x) {
    float reciprocal = nv_rcp(x);
    if (reciprocal != reciprocal) {
        return reciprocal;
    }
    uint sign_bit = floatBitsToUint(reciprocal) & 0x80000000u;
    float magnitude = abs(reciprocal);
    if (magnitude > NV_RCC_MAX) {
        magnitude = NV_RCC_MAX;
    } else if (magnitude < NV_RCC_MIN) {
        magnitude = NV_RCC_MIN;
    }
    return uintBitsToFloat(floatBitsToUint(magnitude) | sign_bit);
}""".replace("@MIN@", RCC_MIN_BITS).replace("@MAX@", RCC_MAX_BITS),
    ),
    "nv_rsq": (
        ("NV_INF",),
        """\
float nv_rsq(float x) {
    float magnitude = abs(x);
    if (magnitude == 0.0) {
        return NV_INF;
    }
    return inversesqrt(magnitude);
}""",
    ),
    "nv_exp": (
        ("NV_INF",),
        """\
vec4 nv_exp(float x) {
    // [NV] EXP: -inf or underflow gives (0, 0, 0, 1), +inf or overflow gives (inf, 0, inf, 1).
    if (x != x) {
        return vec4(x, x, x, 1.0);
    }
    if (x == -NV_INF) {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    float whole = floor(x);
    if (x == NV_INF || whole > 127.0) {
        return vec4(NV_INF, 0.0, NV_INF, 1.0);
    }
    if (whole < -126.0) {
        return vec4(0.0, 0.0, 0.0, 1.0);
    }
    return vec4(exp2(whole), x - whole, exp2(x), 1.0);
}""",
    ),
    "nv_log": (
        ("NV_INF",),
        """\
vec4 nv_log(float x) {
    // [NV] LOG: 0 gives (-inf, 1, -inf, 1), +-inf gives (inf, 1, inf, 1). D3D says -FLT_MAX at 0.
    float magnitude = abs(x);
    if (x != x) {
        return vec4(x, x, x, 1.0);
    }
    if (magnitude == 0.0) {
        return vec4(-NV_INF, 1.0, -NV_INF, 1.0);
    }
    if (magnitude == NV_INF) {
        return vec4(NV_INF, 1.0, NV_INF, 1.0);
    }
    // A denormal is scaled by 2^24 first: frexp and log2 of a denormal are not dependable.
    float bias = 0.0;
    if (magnitude < 1.17549435e-38) {
        magnitude *= 16777216.0;
        bias = 24.0;
    }
    int exponent;
    float fraction = frexp(magnitude, exponent);
    return vec4(float(exponent - 1) - bias, fraction * 2.0, log2(magnitude) - bias, 1.0);
}""",
    ),
    "nv_lit": (
        ("NV_INF",),
        """\
vec4 nv_lit(vec4 s) {
    float diffuse = s.x;
    float specular = s.y;
    float power = s.w;
    if (power < -@MAX@) {
        power = -@MAX@;
    } else if (power > @MAX@) {
        power = @MAX@;
    }
    if (diffuse < 0.0) {
        diffuse = 0.0;
    }
    if (specular < 0.0) {
        specular = 0.0;
    }
    float term = 0.0;
    if (diffuse > 0.0) {
        // [NV] z = EXP(w * LOG(y)), and 0.0 times anything is 0.0 so power 0 gives 1.
        if (power != power || specular != specular) {
            term = power + specular;
        } else if (power == 0.0) {
            term = 1.0;
        } else if (specular == 0.0) {
            term = (power > 0.0) ? 0.0 : NV_INF;
        } else if (specular == NV_INF) {
            term = (power > 0.0) ? NV_INF : 0.0;
        } else {
            term = pow(specular, power);
        }
    }
    return vec4(1.0, diffuse, term, 1.0);
}""".replace("@MAX@", LIT_MAX_POWER),
    ),
}

_MAC_EXPRESSIONS = {
    "MOV": "src_a",
    "MUL": "src_a * src_b",
    "ADD": "src_a + src_c",
    "MAD": "src_a * src_b + src_c",
    "DP3": "vec4(src_a.x * src_b.x + src_a.y * src_b.y + src_a.z * src_b.z)",
    "DPH": "vec4(src_a.x * src_b.x + src_a.y * src_b.y + src_a.z * src_b.z + src_b.w)",
    "DP4": "vec4(src_a.x * src_b.x + src_a.y * src_b.y + src_a.z * src_b.z + src_a.w * src_b.w)",
    "DST": "vec4(1.0, src_a.y * src_b.y, src_a.z, src_b.w)",
    "MIN": "mix(src_b, src_a, lessThan(src_a, src_b))",
    "MAX": "mix(src_b, src_a, greaterThanEqual(src_a, src_b))",
    "SLT": "vec4(lessThan(src_a, src_b))",
    "SGE": "vec4(greaterThanEqual(src_a, src_b))",
}

# ILU: (expression, helper). The scalar unit reads component x of the swizzled C.
_ILU_EXPRESSIONS = {
    "MOV": ("src_c", None),
    "RCP": ("vec4(nv_rcp(src_c.x))", "nv_rcp"),
    "RCC": ("vec4(nv_rcc(src_c.x))", "nv_rcc"),
    "RSQ": ("vec4(nv_rsq(src_c.x))", "nv_rsq"),
    "EXP": ("nv_exp(src_c.x)", "nv_exp"),
    "LOG": ("nv_log(src_c.x)", "nv_log"),
    "LIT": ("nv_lit(src_c)", "nv_lit"),
}

_NOTES = {
    "relative": (
        "relative constant reads c[const + a0] outside 0..191 return vec4(0.0): a CHOICE, the "
        "hardware behaviour is UNSETTLED (the D3D docs also say out of range reads are zero)"
    ),
    "arl": (
        "ARL converts with %s after clamping to +-%d, NaN gives 0: UNSETTLED, the Microsoft "
        "pages say round to nearest, the hardware reading is the NV address register load"
    ),
    "rcp": (
        "RCP/RCC of +-0 gives +-infinity by the sign of the zero: UNSETTLED, the D3D page "
        "pseudo-code says FLT_MAX and its text says infinity"
    ),
    "rsq": "RSQ takes |x|, RSQ(0) is +infinity [D3D rsq page], FLT_MAX in its pseudo-code",
    "rcc": (
        "RCC is UNSOURCED (vs_1_1 has no rcc): reciprocal with the magnitude clamped to "
        "[2^-64, 2^64], sign kept"
    ),
    "dph": "DPH is UNSOURCED beyond its name: (a.x*b.x + a.y*b.y + a.z*b.z + b.w)",
    "log": (
        "LOG is (exponent, mantissa, log2|x|, 1) [NVIDIA DX8 deck]. LOG(0) is (-inf, 1, -inf, 1) "
        "after the NV spec: the D3D log page says -FLT_MAX for z, a disagreement the validator "
        "found and that is UNSETTLED on hardware"
    ),
    "exp": (
        "EXP is (2^floor(x), x - floor(x), 2^x, 1) [D3D expp section, NVIDIA DX8 deck] with the "
        "NV spec overflow and underflow cases (inf, 0, inf, 1) and (0, 0, 0, 1)"
    ),
    "lit": (
        "LIT reads x, y and w of C, power clamped to +-127.9961 [D3D lit page]. The NV spec "
        "cases for NaN and for a zero power or base were taken over after the validator found "
        "the D3D reading differs there"
    ),
    "minmax": (
        "MIN/MAX/SLT/SGE use explicit comparisons: with a NaN operand MIN and MAX give the "
        "B operand, SLT and SGE give 0 (D3D pseudo-code reading)"
    ),
    "position": (
        "oPos is emitted RAW (INFERRED: the title's programs do their own perspective divide "
        "and viewport transform, so it may not be a Vulkan clip-space position)"
    ),
    "mirror": (
        "temporary r12 is a mirror of the position output o0: MEASURED that the title reads "
        "r12 and never writes it, after writing the same components of oPos, and a reference "
        "emulator states it"
    ),
    "mux-none": "an operand the unit reads has no register file selected: it reads as zero",
    "overlap": (
        "MAC and ILU write the same temporary component: the ILU result is written last, UNSETTLED"
    ),
    "after-final": "%d instruction(s) after the FINAL instruction were not translated",
    "no-final": "no instruction carries the FINAL bit, the whole program was translated",
    "constant-range": "an absolute constant index is >= 192: it reads as zero",
}


def _swizzle_text(swizzle: tuple[int, int, int, int]) -> str:
    return "".join("xyzw"[index] for index in swizzle)


def _mask_text(mask: int) -> str:
    return "".join(letter for bit, letter in _MASK_BITS if mask & bit)


def _temporary(number: int, state: _State) -> str:
    """The GLSL name of temporary `number`. r12 is a mirror of the position output o[0]."""
    if number == POSITION_MIRROR_TEMP:
        state.notes.add(_NOTES["mirror"])
        return "o[0]"
    state.temporaries.add(number)
    return f"r{number}"


def _write(destination: str, mask: int, source: str) -> str:
    if mask == 0xF:
        return f"{destination} = {source};"
    letters = _mask_text(mask)
    return f"{destination}.{letters} = {source}.{letters};"


class _State:
    """What the translation of one program has collected so far."""

    def __init__(self) -> None:
        self.temporaries: set[int] = set()
        self.inputs: set[int] = set()
        self.helpers: set[str] = set()
        self.notes: set[str] = set()
        self.opcodes: Counter[str] = Counter()
        self.outputs: dict[int, int] = {}
        self.relative_reads = False
        self.uses_a0 = False


def _operand(decoded: isa.Decoded, which: str, state: _State) -> str:
    source = decoded.source(which)
    if source.mux == isa.MUX_NONE:
        state.notes.add(_NOTES["mux-none"])
        return "vec4(0.0)"
    if source.mux == isa.MUX_TEMP:
        base = _temporary(source.temp, state)
    elif source.mux == isa.MUX_INPUT:
        state.inputs.add(decoded.input)
        base = f"v[{decoded.input}]"
    elif decoded.relative:
        state.relative_reads = True
        state.uses_a0 = True
        state.helpers.add("nv_c_at")
        base = f"nv_c_at({decoded.const} + a0)"
    elif decoded.const >= CONSTANT_COUNT:
        state.notes.add(_NOTES["constant-range"])
        base = "vec4(0.0)"
    else:
        base = f"c[{decoded.const}]"
    swizzle = _swizzle_text(source.swizzle)
    if swizzle != "xyzw":
        base = f"{base}.{swizzle}"
    return f"-{base}" if source.negate else base


def _check_instruction(index: int, decoded: isa.Decoded) -> None:
    if decoded.mac >= len(isa.MAC_NAMES):
        raise Unsupported("undefined-opcode", f"instruction {index}: MAC opcode {decoded.mac}")
    if decoded.out_mask:
        if not decoded.out_to_output:
            raise Unsupported("constant-file-write", f"instruction {index}")
        if decoded.out_address not in isa.OUTPUT_NAMES:
            raise Unsupported(
                "reserved-output", f"instruction {index}: output address {decoded.out_address}"
            )


def _instruction_text(index: int, decoded: isa.Decoded, state: _State) -> list[str]:
    mac, ilu = decoded.mac_name, decoded.ilu_name
    arl = mac == "ARL"
    has_mac = mac not in ("NOP", "ARL")
    has_ilu = ilu != "NOP"
    if mac == "NOP" and decoded.mac_mask:
        raise Unsupported("other", f"instruction {index}: MAC write mask with a NOP MAC")
    if not has_ilu and decoded.ilu_mask:
        raise Unsupported("other", f"instruction {index}: ILU write mask with a NOP ILU")
    if decoded.out_mask:
        unit = ilu if decoded.out_is_ilu else mac
        if unit in ("NOP", "ARL"):
            raise Unsupported("other", f"instruction {index}: output written from {unit}")
    if not (arl or has_mac or has_ilu):
        return []

    for name in (mac, ilu):
        if name != "NOP":
            state.opcodes[name] += 1
    reads = isa.MAC_READS[mac] + ("C" if has_ilu else "")
    lines = [f"    // {index}: " + " + ".join(n for n in (mac, ilu) if n != "NOP"), "    {"]
    for which in "ABC":
        if which in reads:
            lines.append(f"        vec4 src_{which.lower()} = {_operand(decoded, which, state)};")

    if arl:
        state.uses_a0 = True
        state.helpers.add("nv_arl")
        state.notes.add(_NOTES["arl"] % (ARL_ROUNDING, ARL_CLAMP))
        lines.append("        a0 = nv_arl(src_a.x);")
    if has_mac:
        lines.append(f"        precise vec4 mac_result = {_MAC_EXPRESSIONS[mac]};")
        if mac in ("MIN", "MAX", "SLT", "SGE"):
            state.notes.add(_NOTES["minmax"])
        if mac == "DPH":
            state.notes.add(_NOTES["dph"])
    if has_ilu:
        expression, helper = _ILU_EXPRESSIONS[ilu]
        lines.append(f"        precise vec4 ilu_result = {expression};")
        if helper:
            state.helpers.add(helper)
        for tag, names in (
            ("rcp", ("RCP", "RCC")),
            ("rsq", ("RSQ",)),
            ("rcc", ("RCC",)),
            ("log", ("LOG",)),
            ("exp", ("EXP",)),
            ("lit", ("LIT",)),
        ):
            if ilu in names:
                state.notes.add(_NOTES[tag])

    # xemu vsh-prog.c paired-unit routing: ILU targets R1, and MAC temp writes to R1
    # are suppressed. The encoded destination remains the unpaired destination. An ARL
    # MAC also counts as non-NOP for pairing, although it writes A0 rather than a temp.
    paired = decoded.mac != 0 and decoded.ilu != 0
    if has_mac and decoded.mac_mask and not (paired and decoded.temp_out == 1):
        target = _temporary(decoded.temp_out, state)
        lines.append("        " + _write(target, decoded.mac_mask, "mac_result"))
    if has_ilu and decoded.ilu_mask:
        target = _temporary(1 if paired else decoded.temp_out, state)
        lines.append("        " + _write(target, decoded.ilu_mask, "ilu_result"))
    if decoded.out_mask:
        address = decoded.out_address
        result = "ilu_result" if decoded.out_is_ilu else "mac_result"
        lines.append("        " + _write(f"o[{address}]", decoded.out_mask, result))
        state.outputs[address] = state.outputs.get(address, 0) | decoded.out_mask
        if address == 0:
            state.notes.add(_NOTES["position"])
    lines.append("    }")
    return lines


def _closure(helpers: set[str]) -> list[str]:
    needed = set(helpers)
    pending = list(helpers)
    while pending:
        for dependency in _HELPERS[pending.pop()][0]:
            if dependency not in needed:
                needed.add(dependency)
                pending.append(dependency)
    return [name for name in _HELPERS if name in needed]


def _identifier_comment(name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]", "_", name)


def translate(data: bytes, *, name: str = "program") -> Translation:
    """Translate NV2A vertex-program microcode (headed or bare) to a GLSL function body."""
    try:
        program = isa.decode_program(data)
    except isa.DecodeError as error:
        raise Unsupported("undecodable", str(error)) from error
    if not program:
        raise Unsupported("undecodable", "no instructions")

    state = _State()
    statements: list[str] = []
    translated = 0
    for index, decoded in enumerate(program):
        _check_instruction(index, decoded)
        statements.extend(_instruction_text(index, decoded, state))
        translated = index + 1
        if decoded.final:
            if translated < len(program):
                state.notes.add(_NOTES["after-final"] % (len(program) - translated))
            break
    else:
        state.notes.add(_NOTES["no-final"])
    if state.relative_reads:
        state.notes.add(_NOTES["relative"])

    lines = [f"// NV2A vertex program {_identifier_comment(name)}: {translated} instruction(s)"]
    for helper in _closure(state.helpers):
        lines.append(_HELPERS[helper][1])
    lines.append("void nv2a_program() {")
    if state.uses_a0:
        lines.append("    int a0 = 0;")
    for temporary in sorted(state.temporaries):
        lines.append(f"    vec4 r{temporary} = vec4(0.0);")
    lines.extend(statements)
    lines.append("}")
    return Translation(
        body="\n".join(lines) + "\n",
        name=name,
        opcodes=state.opcodes,
        outputs_written=dict(sorted(state.outputs.items())),
        relative_reads=state.relative_reads,
        uses_a0=state.uses_a0,
        instruction_count=translated,
        notes=sorted(state.notes),
        inputs_read=tuple(sorted(state.inputs)),
        viewport=viewport.classify(data),
    )


_COMPUTE_HEADER = f"""\
#version 450
#extension GL_EXT_spirv_intrinsics : enable
spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], capabilities = [4466], 4461, 32);
layout(local_size_x = 64) in;
layout(push_constant) uniform Push {{ uint count; }};
layout(std430, binding = 0) readonly buffer In {{ float data[]; }};
layout(std430, binding = 1) writeonly buffer Out {{ float result[]; }};
vec4 v[{isa.INPUT_COUNT}];
vec4 c[{CONSTANT_COUNT}];
vec4 o[{OUTPUT_ARRAY_SIZE}];
"""

_OUTPUT_INIT_MARK = "@OUTPUT_INIT@"

_COMPUTE_MAIN = f"""\
void main() {{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= count) {{
        return;
    }}
    uint input_base = gid * {INPUT_FLOATS}u;
    for (int i = 0; i < {isa.INPUT_COUNT}; ++i) {{
        uint at = input_base + uint(i) * 4u;
        v[i] = vec4(data[at], data[at + 1u], data[at + 2u], data[at + 3u]);
    }}
    for (int i = 0; i < {CONSTANT_COUNT}; ++i) {{
        uint at = input_base + {isa.INPUT_COUNT * 4}u + uint(i) * 4u;
        c[i] = vec4(data[at], data[at + 1u], data[at + 2u], data[at + 3u]);
    }}
    for (int i = 0; i < {OUTPUT_ARRAY_SIZE}; ++i) {{
        o[i] = {_OUTPUT_INIT_MARK};
    }}
    nv2a_program();
    uint output_base = gid * {OUTPUT_FLOATS}u;
    for (int i = 0; i < {OUTPUT_ARRAY_SIZE}; ++i) {{
        uint at = output_base + uint(i) * 4u;
        result[at] = o[i].x;
        result[at + 1u] = o[i].y;
        result[at + 2u] = o[i].z;
        result[at + 3u] = o[i].w;
    }}
}}
"""


def compute_test_source(t: Translation, *, output_init: str = DEFAULT_OUTPUT_INIT) -> str:
    """A complete GLSL 450 compute shader: one invocation runs the program on one record.

    Input record at data[gid * 832]: v0..v15 then c0..c191, 4 floats each. Output record at
    result[gid * 64]: o0..o15 by NV2A output address. Temporaries and A0 start at zero, the
    outputs at `output_init` ("zero" by default, "nv" is (0, 0, 0, 1), T561 INFERRED, see
    `OUTPUT_INITS`). The execution mode below asks for IEEE signed zero, inf and NaN
    preservation (SPIR-V SignedZeroInfNanPreserve, width 32) so the explicit special cases
    hold.
    """
    main = _COMPUTE_MAIN.replace(_OUTPUT_INIT_MARK, output_init_text(output_init))
    return f"{_COMPUTE_HEADER}\n{t.body}\n{main}"


#: The compute form's float-controls mode (SPIR-V SignedZeroInfNanPreserve, width 32) as GLSL
#: lines, for `vertex_shader_source(float_controls=True)`. The device needs
#: `shaderSignedZeroInfNanPreserveFloat32` (RADV and llvmpipe report it).
FLOAT_CONTROLS_LINES = (
    "#extension GL_EXT_spirv_intrinsics : enable\n"
    'spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], capabilities = [4466], '
    "4461, 32);\n"
)


def vertex_shader_source(
    t: Translation,
    *,
    undo_viewport: bool = False,
    float_controls: bool = False,
    window_to_clip: bool = False,
    output_init: str = DEFAULT_OUTPUT_INIT,
    live_raster: bool = False,
    live_fog: bool = False,
) -> str:
    """A complete GLSL 450 vertex shader for the renderer.

    `float_controls` (T100a, off by default so the emitted text is unchanged) adds the compute
    form's SignedZeroInfNanPreserve mode: without it the explicit NaN and infinity tests
    (`x != x`, the RCP and LOG special cases) are undefined behaviour in Vulkan, and 6 static
    programs measurably differ from the compute form (docs/vertex-translator.md 7.6).

    `output_init` (T561, "zero" by default so the emitted text is unchanged) is the initial value
    of every `o[i]`, the same option as `compute_test_source`: "nv" starts them at (0, 0, 0, 1),
    INFERRED from the xboxdevwiki NV2A vertex shader page, see `OUTPUT_INITS`.

    Declares only what the program reads and writes. Constants are the hardware file
    (index n + 96 for D3D8 c[n]). oPos goes RAW into gl_Position, INFERRED not to be a
    Vulkan clip-space position, the renderer converts, or `undo_viewport` (T96, INFERRED, see
    tools/nv2a/viewport.py) inverts the program's own c58/c59 transform. `window_to_clip`
    (T560, INFERRED, off by default) also converts x and y of the programs the inverse leaves
    alone, which hand the rasterizer window coordinates. oPts goes to gl_PointSize (x).
    """
    if live_fog and not live_raster:
        raise Unsupported("live fog requires the versioned live raster profile")
    init_text = output_init_text(output_init)
    lines = ["#version 450"]
    if float_controls:
        lines.extend(FLOAT_CONTROLS_LINES.splitlines())
    for number in t.inputs_read:
        lines.append(f"layout(location = {number}) in vec4 v{number};")
    raster_uniform = " vec4 live_raster;" if live_raster else ""
    if live_fog:
        raster_uniform += " vec4 live_fog;"
    lines.append(
        f"layout(std140, set = 0, binding = 0) uniform Constants {{ "
        f"vec4 c[{CONSTANT_COUNT}];{raster_uniform} }};"
    )
    for address in sorted(set(t.outputs_written) | ({5} if live_fog else set())):
        if address in VARYINGS:
            varying, location, kind = VARYINGS[address]
            lines.append(f"layout(location = {location}) out {kind} {varying};")
    lines.append(f"vec4 v[{isa.INPUT_COUNT}];")
    lines.append(f"vec4 o[{OUTPUT_ARRAY_SIZE}];")
    lines.append("")
    position = (
        viewport.live_raster_position()
        if live_raster
        else viewport.glsl_position(
            t.viewport, undo_viewport=undo_viewport, window_to_clip=window_to_clip
        )
    )
    if position is not None:
        lines.append(viewport.GLSL_HELPER)
    if live_fog:
        from tools.nv2a.fog import GLSL as fog_glsl

        lines.append(fog_glsl)
    lines.append(t.body)
    lines.append("void main() {")
    lines.append(f"    for (int i = 0; i < {isa.INPUT_COUNT}; ++i) {{")
    lines.append("        v[i] = vec4(0.0);")
    lines.append("    }")
    lines.append(f"    for (int i = 0; i < {OUTPUT_ARRAY_SIZE}; ++i) {{")
    lines.append(f"        o[i] = {init_text};")
    lines.append("    }")
    for number in t.inputs_read:
        lines.append(f"    v[{number}] = v{number};")
    lines.append("    nv2a_program();")
    for address in sorted(t.outputs_written):
        if address == 0 and position is not None:
            lines.append(position)
        elif address == 0:
            lines.append("    gl_Position = o[0];")
        elif address == 6:
            if not live_raster:
                lines.append("    gl_PointSize = o[6].x;")
        elif address == 5 and live_fog:
            pass  # Generated fog below replaces the raw programmable distance.
        else:
            varying, _, kind = VARYINGS[address]
            suffix = ".x" if kind == "float" else ""
            lines.append(f"    {varying} = o[{address}]{suffix};")
    if live_fog:
        lines.append("    oFog = live_fog_factor(o[5].x, live_fog);")
    if live_raster:
        lines.append("    gl_PointSize = live_raster.w;")
    lines.append("}")
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="python -m tools.nv2a.translate",
        description="Translate an NV2A vertex program (headed or bare .bin) to GLSL 4.50.",
    )
    parser.add_argument("file", type=Path, help="microcode file")
    parser.add_argument("--out", type=Path, help="output file (.comp or .vert), default stdout")
    parser.add_argument(
        "--stage",
        choices=("compute", "vertex"),
        help="shader stage, default from the --out suffix, else compute",
    )
    parser.add_argument(
        "--undo-viewport",
        action="store_true",
        help="vertex stage: invert the program's own c58/c59 viewport transform (T96, INFERRED)",
    )
    parser.add_argument(
        "--window-to-clip",
        action="store_true",
        help="vertex stage: convert x and y of window coordinate programs with c58/c59 "
        "(T560, INFERRED)",
    )
    parser.add_argument(
        "--output-init",
        choices=sorted(OUTPUT_INITS),
        default=DEFAULT_OUTPUT_INIT,
        help="initial value of the output registers: zero, or nv = (0, 0, 0, 1) (T561, INFERRED)",
    )
    args = parser.parse_args(argv)
    stage = args.stage
    if stage is None:
        stage = "vertex" if args.out is not None and args.out.suffix == ".vert" else "compute"
    try:
        translation = translate(args.file.read_bytes(), name=args.file.stem)
    except Unsupported as error:
        print(f"unsupported: {error}", file=sys.stderr)
        return 2
    source = (
        vertex_shader_source(
            translation,
            undo_viewport=args.undo_viewport,
            window_to_clip=args.window_to_clip,
            output_init=args.output_init,
        )
        if stage == "vertex"
        else compute_test_source(translation, output_init=args.output_init)
    )
    for note in translation.notes:
        print(f"note: {note}", file=sys.stderr)
    if args.out is None:
        sys.stdout.write(source)
    else:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(source)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
