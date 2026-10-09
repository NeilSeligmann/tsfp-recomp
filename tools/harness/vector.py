# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in raw vector state and deliberately narrow legacy SSE closure admission."""

from __future__ import annotations

import hashlib
from dataclasses import replace

from .model import FP_SCALAR_MODE, FP_SCALAR_MXCSR, FP_SCALAR_STATUS_MASK, Case

# These legacy forms do not convert floating values or consult/update MXCSR.
VECTOR_MOVES = frozenset({"movups", "movss", "movlps", "movhps", "movhlps", "movlhps"})
VECTOR_BITS = frozenset({"xorps", "andps", "andnps", "orps"})


def vector_state_instruction(insn: object) -> bool:
    """Catch implicit vector/control-state opcodes as well as named operands."""
    from capstone import x86_const as x86

    groups = {
        getattr(x86, "X86_GRP_" + name, -1)
        for name in (
            "MMX",
            "3DNOW",
            "SSE1",
            "SSE2",
            "SSE3",
            "SSSE3",
            "SSE41",
            "SSE42",
            "SSE4A",
            "AVX",
            "AVX2",
            "AVX512",
            "F16C",
            "FMA",
            "FMA4",
            "XOP",
        )
    }
    mnemonic = insn.mnemonic.lower()
    return bool(groups.intersection(insn.groups)) or mnemonic in {
        "emms",
        "femms",
        "ldmxcsr",
        "stmxcsr",
        "fxsave",
        "fxrstor",
        "fxsave64",
        "fxrstor64",
        "xsave",
        "xsave64",
        "xsaveopt",
        "xsaveopt64",
        "xsaves",
        "xsaves64",
        "xsavec",
        "xsavec64",
        "xrstor",
        "xrstor64",
        "xrstors",
        "xrstors64",
    }


def supported_vector_instruction(insn: object) -> bool:
    """Accept exact operand forms, never a mnemonic prefix or an MMX overload."""
    from capstone.x86_const import X86_OP_MEM, X86_OP_REG

    mnemonic = insn.mnemonic.lower()
    operands = insn.operands
    if mnemonic not in VECTOR_MOVES | VECTOR_BITS or len(operands) != 2:
        return False

    def vector(op: object) -> bool:
        return op.type == X86_OP_REG and insn.reg_name(op.reg) in {f"xmm{i}" for i in range(8)}

    left, right = operands
    if mnemonic in VECTOR_BITS:
        return vector(left) and vector(right)
    if mnemonic in {"movhlps", "movlhps"}:
        return vector(left) and vector(right)
    if mnemonic in {"movlps", "movhps"}:
        return (vector(left) and right.type == X86_OP_MEM) or (
            left.type == X86_OP_MEM and vector(right)
        )
    return (vector(left) and (vector(right) or right.type == X86_OP_MEM)) or (
        left.type == X86_OP_MEM and vector(right)
    )


def attach_vector_state(case: Case) -> Case:
    """Stable independent stream: all128 bits/all8 registers vary by seed/index/VA."""
    values = tuple(
        int.from_bytes(
            hashlib.sha256(
                f"vector-v1:{case.seed}:{case.index}:{case.va}:{i}".encode("ascii")
            ).digest()[:16],
            "little",
        )
        for i in range(8)
    )
    # All exception masks set; vary rounding, FTZ and status bits even though
    # the admitted subset must preserve the entire word, including these bits.
    control = 0x1F80 | ((case.index & 3) << 13) | ((case.index & 4) << 13)
    control |= case.index & 0x3F  # DAZ stays clear in the conservative legacy-SSE domain.
    return replace(case, xmm=values, mxcsr=control)


# ---------------------------------------------------------------------------------------
# T1620 fp-scalar-v1: scalar single-precision arithmetic (opt-in, additive to the legacy mode)
#
# Admission is by matrix row (tests/test_vector_scalar_native_matrix.py): the Unicorn oracle
# reproduces the host CPU bit for bit at MXCSR 0x1F80 for these instructions, except two
# different NaN operands for add/sub/mul (counted non-verdict, see the tripwire). The step 2b
# rows (divss, cvttss2si, register movaps) are green in the matrix but not admitted yet.

#: canonical (mandatory prefix + opcode) bytes per admitted mnemonic.
VECTOR_SCALAR_FP_ENCODING: dict[str, bytes] = {
    "addss": b"\xf3\x0f\x58",
    "subss": b"\xf3\x0f\x5c",
    "mulss": b"\xf3\x0f\x59",
    "comiss": b"\x0f\x2f",
    "ucomiss": b"\x0f\x2e",
    "cvtsi2ss": b"\xf3\x0f\x2a",
    # T1622 step 2b (matrix rows green): divss, cvttss2si, register-only movaps (load form)
    "divss": b"\xf3\x0f\x5e",
    "cvttss2si": b"\xf3\x0f\x2c",
    "movaps": b"\x0f\x28",
}
VECTOR_SCALAR_FP = frozenset(VECTOR_SCALAR_FP_ENCODING)
#: the step 2b additions (admitted by T1622, listed in VECTOR_SCALAR_FP).
VECTOR_SCALAR_FP_STEP2B = frozenset({"divss", "cvttss2si", "movaps"})
#: mnemonics whose operands are two floats (the NaN-pair tripwire and operand classes apply).
FP_TWO_OPERAND = frozenset({"addss", "subss", "mulss", "divss", "comiss", "ucomiss"})
XMM_NAMES = frozenset(f"xmm{i}" for i in range(8))
GPR32_NAMES = frozenset({"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"})


def supported_scalar_fp_instruction(insn: object) -> bool:
    """Exact fp-scalar-v1 forms: xmm0-7 destination, xmm or dword memory source (r32/m32 for
    cvtsi2ss), canonical encoding with no extra prefix (no segment, operand or address size,
    rep or lock). Never a mnemonic prefix match."""
    from capstone.x86_const import X86_OP_MEM, X86_OP_REG

    mnemonic = insn.mnemonic.lower()
    encoding = VECTOR_SCALAR_FP_ENCODING.get(mnemonic)
    operands = insn.operands
    if encoding is None or len(operands) != 2:
        return False
    if bytes(insn.bytes[: len(encoding)]) != encoding:
        return False
    left, right = operands
    if mnemonic == "cvttss2si":  # r32 <- xmm or m32 (a float source, an integer destination)
        if left.type != X86_OP_REG or insn.reg_name(left.reg) not in GPR32_NAMES:
            return False
        if right.type == X86_OP_MEM:
            return right.mem.segment == 0 and right.size == 4
        return right.type == X86_OP_REG and insn.reg_name(right.reg) in XMM_NAMES
    if left.type != X86_OP_REG or insn.reg_name(left.reg) not in XMM_NAMES:
        return False
    if mnemonic == "movaps":  # register to register only, memory forms stay refused
        return right.type == X86_OP_REG and insn.reg_name(right.reg) in XMM_NAMES
    if right.type == X86_OP_MEM:
        return right.size == 4 and right.mem.segment == 0
    if right.type != X86_OP_REG:
        return False
    name = insn.reg_name(right.reg)
    if mnemonic == "cvtsi2ss":
        return name in GPR32_NAMES
    return name in XMM_NAMES


def instruction_supported(insn: object, mode: str = "") -> bool:
    """Per-mode instruction whitelist: the legacy 10 mnemonics, plus fp-scalar-v1 in that mode."""
    if supported_vector_instruction(insn):
        return True
    return mode == FP_SCALAR_MODE and supported_scalar_fp_instruction(insn)


def status_bits(case: Case) -> int:
    """Sticky MXCSR status bits of a case: an independent hash stream, half the cases at 0.

    Replaces the legacy `case.index & 0x3F` preset, which correlates with the case index.
    """
    stream = hashlib.sha256(
        f"{FP_SCALAR_MODE}:status:{case.seed}:{case.index}:{case.va}".encode("ascii")
    ).digest()
    return 0 if stream[0] & 1 else stream[1] & FP_SCALAR_STATUS_MASK


#: operand class weights (percent). "const" falls back to "normal" without harvested constants.
LANE_CLASSES: tuple[tuple[str, int], ...] = (
    ("normal", 28),
    ("int", 10),
    ("zero", 8),
    ("denormal", 8),
    ("inf", 5),
    ("limit", 6),
    ("alias", 8),
    ("neighbour", 6),
    ("tie", 5),
    ("nan", 4),
    ("const", 6),
    ("raw", 6),
)
#: the single NaN pattern a case may hold (quiet NaN with sign set). No signalling NaN is seeded.
CASE_NAN = 0xFFC00000


def is_nan_bits(bits: int) -> bool:
    return (bits & 0x7F800000) == 0x7F800000 and (bits & 0x007FFFFF) != 0


def _float_bits(value: float) -> int:
    import struct

    return struct.unpack("<I", struct.pack("<f", value))[0]


class LaneSampler:
    """Deterministic float-class sampler: sha256(seed, index, va, mode, key) decides everything."""

    def __init__(self, case: Case, constants: tuple[int, ...] = ()) -> None:
        self.case = case
        self.constants = tuple(constants)
        #: values produced so far in this case, the pool for alias/neighbour/tie classes
        self.history: list[int] = []
        self.classes: list[str] = []

    def _stream(self, key: str) -> bytes:
        c = self.case
        return hashlib.sha256(
            f"{FP_SCALAR_MODE}:{c.seed}:{c.index}:{c.va}:{key}".encode("ascii")
        ).digest()

    def sample(self, key: str, remember: bool = True) -> tuple[str, int]:
        stream = self._stream(key)
        roll = stream[0] % 100
        name = LANE_CLASSES[-1][0]
        total = 0
        for label, weight in LANE_CLASSES:
            total += weight
            if roll < total:
                name = label
                break
        word = int.from_bytes(stream[1:9], "little")
        sign = (word & 1) << 31
        pool = self.history
        if name in ("alias", "neighbour", "tie") and not pool:
            name = "normal"
        if name == "const" and not self.constants:
            name = "normal"
        if name == "normal":
            exponent = 127 - 12 + (word >> 1) % 25
            bits = sign | exponent << 23 | (word >> 8) & 0x7FFFFF
        elif name == "int":
            bits = _float_bits(float(1 + (word >> 1) % 16)) | sign
        elif name == "zero":
            bits = sign
        elif name == "denormal":
            bits = sign | 1 + (word >> 1) % 0x7FFFFF
        elif name == "inf":
            bits = sign | 0x7F800000
        elif name == "limit":
            bits = sign | (0x7F7FFFFF, 0x7F7FFFFE, 0x00800000, 0x00800001)[(word >> 1) % 4]
        elif name == "alias":
            bits = pool[(word >> 1) % len(pool)]
        elif name == "tie":
            bits = pool[(word >> 1) % len(pool)] ^ 0x80000000
        elif name == "neighbour":
            base = pool[(word >> 1) % len(pool)]
            bits = (base + (1 if (word >> 40) & 1 else -1)) & 0xFFFFFFFF
            if is_nan_bits(bits) or is_nan_bits(base):
                bits = base
        elif name == "nan":
            bits = CASE_NAN
        elif name == "const":
            constant = self.constants[(word >> 1) % len(self.constants)]
            bits = (
                constant,
                (constant + 1) & 0xFFFFFFFF,
                (constant - 1) & 0xFFFFFFFF,
                constant ^ 0x80000000,
            )[(word >> 40) % 4]
        else:  # raw bits, NaN patterns folded into the single case-wide quiet NaN
            bits = (word >> 1) & 0xFFFFFFFF
        if is_nan_bits(bits):
            bits = CASE_NAN
        if remember:
            self.history.append(bits)
            self.classes.append(name)
        return name, bits


def attach_scalar_fp_state(case: Case, constants: tuple[int, ...] = ()) -> Case:
    """fp-scalar-v1 state: eight xmm registers of float-class lanes, MXCSR control word fixed.

    Control word 0x1F80 always (the mode never varies rounding, FTZ or DAZ). Sticky status bits
    come from an independent hash stream. At most one NaN pattern per case, no signalling NaN.
    `constants` are harvested .rdata float bit patterns (c, c+-1 ulp, -c are seeded).
    """
    sampler = LaneSampler(case, constants)
    registers = []
    for reg in range(8):
        value = 0
        for lane in range(4):
            _, bits = sampler.sample(f"xmm{reg}.{lane}")
            value |= bits << (32 * lane)
        registers.append(value)
    return replace(
        case,
        xmm=tuple(registers),
        mxcsr=FP_SCALAR_MXCSR | status_bits(case),
        vector_mode=FP_SCALAR_MODE,
    )
