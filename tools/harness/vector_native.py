# SPDX-License-Identifier: GPL-3.0-or-later
"""T1620: native host SSE stub plus Unicorn single-instruction runner for the fp-scalar matrix.

The fp-scalar-v1 proof mode admits scalar single-precision SSE arithmetic only when the
Unicorn oracle reproduces the host CPU bit for bit. This module executes ONE SSE
instruction on the host CPU (a ctypes/mmap machine-code stub) and on Unicorn with the same
inputs and reports both outputs, so `tests/test_vector_scalar_native_matrix.py` can compare
them. The stub saves xmm0-7, MXCSR in, EFLAGS and EAX, runs the instruction, stores the
results and ALWAYS restores MXCSR to 0x1F80 before returning to Python (QEMU's hardfloat path
and CPython both depend on the host rounding mode).

Instruction bytes are position independent between a 64-bit native stub and a 32-bit Unicorn
guest: the memory form addresses `[rbx+disp32]` / `[ebx+disp32]` with the same ModRM bytes.

    python -m tools.harness.vector_native --report [--pairs N] [--seed S]

prints the per-instruction divergence table used by docs/evidence/t1620/scalar-fp-admission.md.
"""

from __future__ import annotations

import argparse
import ctypes
import mmap
import platform
import random
import struct
import sys
from collections.abc import Iterator
from dataclasses import dataclass

#: Unicorn must be exactly this release: the matrix is evidence about one build.
UNICORN_PIN = "2.1.4"
#: MXCSR the matrix restores after every case and the proof mode pins.
MXCSR_DEFAULT = 0x1F80
#: EFLAGS bits an SSE compare may write (CF PF AF ZF SF OF).
FLAG_MASK = 0x8D5
FLAGS_IN = 0x8D7
STATUS_MASK = 0x3F

# state layout shared by the native stub and the Unicorn runner (byte offsets)
OFF_XMM = 0x00
OFF_MXCSR_IN = 0x80
OFF_MXCSR_OUT = 0x84
OFF_MXCSR_DEF = 0x88
OFF_FLAGS_OUT = 0x8C
OFF_EAX_OUT = 0x90
OFF_ECX_IN = 0x94
OFF_EAX_IN = 0x98
STATE_SIZE = 0xA0
#: operand memory area: two pages, operand dword placed at `operand_offset`.
OPERAND_SIZE = 0x2000


@dataclass(frozen=True)
class Op:
    """One SSE instruction template: prefix, opcode bytes, operand shape."""

    name: str
    prefix: bytes
    opcode: bytes
    #: "xx" dest xmm / src xmm-or-m32, "xr" cvtsi2ss xmm <- r32/m32, "rx" cvttss2si r32 <- xmm/m32,
    #: "xxi" cmpXXss with an immediate predicate.
    shape: str = "xx"
    imm: int | None = None


OPS: dict[str, Op] = {
    op.name: op
    for op in (
        Op("addss", b"\xf3", b"\x0f\x58"),
        Op("subss", b"\xf3", b"\x0f\x5c"),
        Op("mulss", b"\xf3", b"\x0f\x59"),
        Op("divss", b"\xf3", b"\x0f\x5e"),
        Op("comiss", b"", b"\x0f\x2f"),
        Op("ucomiss", b"", b"\x0f\x2e"),
        Op("cvtsi2ss", b"\xf3", b"\x0f\x2a", "xr"),
        Op("cvttss2si", b"\xf3", b"\x0f\x2c", "rx"),
        Op("movaps", b"", b"\x0f\x28"),
        Op("andps", b"", b"\x0f\x54"),
        Op("xorps", b"", b"\x0f\x57"),
        # refused rows, measured for the evidence table only
        Op("sqrtss", b"\xf3", b"\x0f\x51"),
        Op("minss", b"\xf3", b"\x0f\x5d"),
        Op("maxss", b"\xf3", b"\x0f\x5f"),
        Op("cvtss2si", b"\xf3", b"\x0f\x2d", "rx"),
        Op("cvtss2sd", b"\xf3", b"\x0f\x5a"),
        Op("rcpss", b"\xf3", b"\x0f\x53"),
        Op("rsqrtss", b"\xf3", b"\x0f\x52"),
        Op("cmpeqss", b"\xf3", b"\x0f\xc2", "xxi", 0),
        Op("cmpltss", b"\xf3", b"\x0f\xc2", "xxi", 1),
        Op("cmpless", b"\xf3", b"\x0f\xc2", "xxi", 2),
        Op("cmpunordss", b"\xf3", b"\x0f\xc2", "xxi", 3),
    )
}
#: rows the fp-scalar-v1 whitelist admits (step 1) and the later step 2b rows.
ADMITTED_ROWS = ("addss", "subss", "mulss", "comiss", "ucomiss", "cvtsi2ss")
STEP2B_ROWS = ("divss", "cvttss2si", "movaps")
REFUSED_ROWS = (
    "sqrtss",
    "minss",
    "maxss",
    "cvtss2si",
    "cvtss2sd",
    "rcpss",
    "rsqrtss",
    "cmpeqss",
    "cmpltss",
    "cmpless",
    "cmpunordss",
)


def encode(op: Op, dest: int, source: int, *, memory: int | None = None) -> bytes:
    """Machine code for one instruction. `source` is an xmm (or GPR number for cvtsi2ss).

    With `memory` the source is `[ebx+memory]` (mod=10, rm=3). cvtsi2ss register form uses
    ecx-family numbering, cvttss2si writes `dest` as a GPR number.
    """
    if memory is None:
        modrm = 0xC0 | (dest << 3) | source
        tail = b""
    else:
        modrm = 0x80 | (dest << 3) | 3
        tail = struct.pack("<i", memory)
    body = op.prefix + op.opcode + bytes([modrm]) + tail
    if op.imm is not None:
        body += bytes([op.imm])
    return body


def host_supported() -> bool:
    """True on x86-64 Linux where an executable anonymous mapping can be created."""
    if platform.machine() not in ("x86_64", "AMD64") or not sys.platform.startswith("linux"):
        return False
    try:
        page = mmap.mmap(-1, 4096, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
        page.close()
    except (OSError, ValueError):
        return False
    return True


def cpu_model() -> str:
    """Host CPU model string for the evidence record."""
    try:
        with open("/proc/cpuinfo", encoding="ascii", errors="replace") as handle:
            for line in handle:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def _mov(opcode: bytes, modrm_reg: int, disp: int) -> bytes:
    return opcode + bytes([0x80 | (modrm_reg << 3) | 7]) + struct.pack("<I", disp)


def _wrap(instruction: bytes) -> bytes:
    """Prologue and epilogue around the instruction (SysV: rdi=state, rsi=operand area)."""
    code = bytearray()
    code += b"\x53"  # push rbx
    code += b"\x48\x89\xf3"  # mov rbx, rsi
    code += _mov(b"\x0f\xae", 2, OFF_MXCSR_IN)  # ldmxcsr [rdi+d]
    for n in range(8):
        code += _mov(b"\x0f\x10", n, OFF_XMM + 16 * n)  # movups xmm_n, [rdi+d]
    code += _mov(b"\x8b", 0, OFF_EAX_IN)  # mov eax, [rdi+d]
    code += _mov(b"\x8b", 1, OFF_ECX_IN)  # mov ecx, [rdi+d]
    code += b"\x68" + struct.pack("<I", FLAGS_IN) + b"\x9d"  # push imm32; popfq
    code += instruction
    code += b"\x9c\x5a"  # pushfq; pop rdx
    code += _mov(b"\x89", 2, OFF_FLAGS_OUT)  # mov [rdi+d], edx
    code += _mov(b"\x89", 0, OFF_EAX_OUT)  # mov [rdi+d], eax
    code += _mov(b"\x0f\xae", 3, OFF_MXCSR_OUT)  # stmxcsr [rdi+d]
    code += _mov(b"\x0f\xae", 2, OFF_MXCSR_DEF)  # ldmxcsr [rdi+d]  (restore 0x1F80)
    for n in range(8):
        code += _mov(b"\x0f\x11", n, OFF_XMM + 16 * n)  # movups [rdi+d], xmm_n
    code += b"\x5b\xc3"  # pop rbx; ret
    return bytes(code)


class HostStub:
    """Executable ctypes stubs, one per instruction encoding (cached)."""

    def __init__(self) -> None:
        self._stubs: dict[bytes, tuple[mmap.mmap, object]] = {}
        self._state = ctypes.create_string_buffer(STATE_SIZE)
        # page-aligned so a 0xFFE operand offset really crosses a page boundary
        self._operand_map = mmap.mmap(-1, OPERAND_SIZE + 0x1000)
        self._operand = ctypes.c_char.from_buffer(self._operand_map)
        self._probe = self._build(b"", probe=True)

    def _build(self, instruction: bytes, probe: bool = False) -> object:
        if probe:
            # stmxcsr [rdi]; ret
            blob = b"\x0f\xae\x1f\xc3"
        else:
            blob = _wrap(instruction)
        page = mmap.mmap(-1, 4096, prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
        page.write(blob)
        address = ctypes.addressof(ctypes.c_char.from_buffer(page))
        function = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_void_p)(address)
        if not probe:
            self._stubs[instruction] = (page, function)
        else:
            self._probe_page = page
        return function

    def host_mxcsr(self) -> int:
        """Current host MXCSR of the calling thread (stmxcsr)."""
        out = ctypes.c_uint32(0)
        self._probe(ctypes.addressof(out), None)
        return out.value

    def run(
        self,
        instruction: bytes,
        xmm: tuple[int, ...],
        mxcsr: int,
        eax: int = 0,
        ecx: int = 0,
        operand: bytes = b"",
        operand_offset: int = 0,
    ) -> tuple[tuple[int, ...], int, int, int]:
        """Run on the host. Returns (xmm out, mxcsr out, eflags out, eax out)."""
        function = self._stubs.get(instruction)
        function = function[1] if function else self._build(instruction)
        state = bytearray(STATE_SIZE)
        for n, value in enumerate(xmm):
            state[16 * n : 16 * n + 16] = value.to_bytes(16, "little")
        struct.pack_into("<IIII", state, OFF_MXCSR_IN, mxcsr, 0, MXCSR_DEFAULT, 0)
        struct.pack_into("<II", state, OFF_ECX_IN, ecx, eax)
        ctypes.memmove(self._state, bytes(state), STATE_SIZE)
        self._operand_map[0:OPERAND_SIZE] = bytes(OPERAND_SIZE)
        if operand:
            self._operand_map[operand_offset : operand_offset + len(operand)] = operand
        function(ctypes.addressof(self._state), ctypes.addressof(self._operand))
        raw = self._state.raw
        out = tuple(int.from_bytes(raw[16 * n : 16 * n + 16], "little") for n in range(8))
        mxcsr_out = struct.unpack_from("<I", raw, OFF_MXCSR_OUT)[0]
        flags_out = struct.unpack_from("<I", raw, OFF_FLAGS_OUT)[0]
        eax_out = struct.unpack_from("<I", raw, OFF_EAX_OUT)[0]
        return out, mxcsr_out, flags_out, eax_out


class UnicornRunner:
    """Single-instruction Unicorn (32-bit) runner mirroring HostStub.run."""

    CODE_BASE = 0x00100000
    OPERAND_BASE = 0x00300000

    def __init__(self) -> None:
        import unicorn
        from unicorn import x86_const

        if unicorn.__version__ != UNICORN_PIN:
            raise RuntimeError(f"pinned to Unicorn {UNICORN_PIN}, found {unicorn.__version__}")
        self._u = unicorn
        self._x = x86_const
        self._uc = unicorn.Uc(unicorn.UC_ARCH_X86, unicorn.UC_MODE_32)
        self._uc.mem_map(self.CODE_BASE, 0x100000)
        self._uc.mem_map(self.OPERAND_BASE, OPERAND_SIZE + 0x1000)
        self._slot: dict[bytes, int] = {}

    def _address(self, instruction: bytes) -> int:
        address = self._slot.get(instruction)
        if address is None:
            address = self.CODE_BASE + 0x40 * len(self._slot)
            self._uc.mem_write(address, instruction + b"\x90" * 8)
            self._slot[instruction] = address
        return address

    def run(
        self,
        instruction: bytes,
        xmm: tuple[int, ...],
        mxcsr: int,
        eax: int = 0,
        ecx: int = 0,
        operand: bytes = b"",
        operand_offset: int = 0,
    ) -> tuple[tuple[int, ...], int, int, int]:
        uc, x = self._uc, self._x
        address = self._address(instruction)
        for n, value in enumerate(xmm):
            uc.reg_write(getattr(x, f"UC_X86_REG_XMM{n}"), value)
        uc.reg_write(x.UC_X86_REG_MXCSR, mxcsr)
        uc.reg_write(x.UC_X86_REG_EAX, eax)
        uc.reg_write(x.UC_X86_REG_ECX, ecx)
        uc.reg_write(x.UC_X86_REG_EBX, self.OPERAND_BASE)
        uc.reg_write(x.UC_X86_REG_EFLAGS, FLAGS_IN)
        uc.mem_write(self.OPERAND_BASE, bytes(OPERAND_SIZE))
        if operand:
            uc.mem_write(self.OPERAND_BASE + operand_offset, operand)
        uc.emu_start(address, address + len(instruction), count=1)
        out = tuple(uc.reg_read(getattr(x, f"UC_X86_REG_XMM{n}")) for n in range(8))
        return (
            out,
            uc.reg_read(x.UC_X86_REG_MXCSR),
            uc.reg_read(x.UC_X86_REG_EFLAGS),
            uc.reg_read(x.UC_X86_REG_EAX),
        )


# ---------------------------------------------------------------------------------------
# operand grid


def f32(value: float) -> int:
    return struct.unpack("<I", struct.pack("<f", value))[0]


#: scalar operand specials (float32 bit patterns), 26 entries.
SPECIALS: tuple[tuple[str, int], ...] = (
    ("+0", 0x00000000),
    ("-0", 0x80000000),
    ("min_denorm", 0x00000001),
    ("-min_denorm", 0x80000001),
    ("half_denorm", 0x00400000),
    ("max_denorm", 0x007FFFFF),
    ("-max_denorm", 0x807FFFFF),
    ("min_normal", 0x00800000),
    ("-min_normal", 0x80800000),
    ("max", 0x7F7FFFFF),
    ("-max", 0xFF7FFFFF),
    ("+inf", 0x7F800000),
    ("-inf", 0xFF800000),
    ("qnan", 0x7FC00000),
    ("-qnan", 0xFFC00000),
    ("snan", 0x7F800001),
    ("-snan", 0xFF800001),
    ("payload_qnan", 0x7FC12345),
    ("one", 0x3F800000),
    ("-one", 0xBF800000),
    ("one_ulp", 0x3F800001),
    ("1.5", f32(1.5)),
    ("3.0", f32(3.0)),
    ("0.1", f32(0.1)),
    ("2^31", 0x4F000000),
    ("2^-31", 0x30000000),
    ("2^24", 0x4B800000),
    ("-2^31", 0xCF000000),
)
#: filler for the upper 96 bits of each xmm register (checks they are preserved/cleared).
FILLER = 0x0123456789ABCDEFFEDCBA9876543210


def is_nan(bits: int) -> bool:
    return (bits & 0x7F800000) == 0x7F800000 and (bits & 0x007FFFFF) != 0


def lane(dest_bits: int, upper: int = FILLER) -> int:
    """xmm value with `dest_bits` in lane 0 and the filler in lanes 1-3."""
    return (upper & ~0xFFFFFFFF) | dest_bits


def control_words() -> tuple[tuple[str, int], ...]:
    """MXCSR words: default, four rounding modes, FTZ, DAZ, FTZ+DAZ (exceptions masked)."""
    base = 0x1F80
    return (
        ("default", base),
        ("RC=down", base | 1 << 13),
        ("RC=up", base | 2 << 13),
        ("RC=zero", base | 3 << 13),
        ("FTZ", base | 1 << 15),
        ("DAZ", base | 1 << 6),
        ("FTZ+DAZ", base | 1 << 15 | 1 << 6),
    )


@dataclass
class Row:
    """Result cell for one (instruction, form, control word)."""

    instruction: str
    form: str
    control: str
    total: int = 0
    divergent: int = 0
    #: divergent cases whose operands were two different NaN patterns
    two_nan: int = 0
    #: cases where the host set a sticky status bit that Unicorn did not
    sticky_missed: int = 0
    #: cases where Unicorn set a sticky status bit that was not already in the input word
    unicorn_sticky_set: int = 0
    #: cases where Unicorn changed an MXCSR bit outside the status mask
    mxcsr_control_diff: int = 0
    first_divergence: str = ""


FORMS = ("reg-reg", "reg-m32", "dest==src", "m32-unaligned", "m32-pagecross")


#: packed 128-bit forms whose memory operand needs 16-byte alignment: register forms only.
PACKED_ROWS = frozenset({"movaps", "andps", "xorps"})


def _cases(
    op: Op, form: str, pairs: list[tuple[int, int]]
) -> Iterator[tuple[bytes, tuple[int, ...], int, int, bytes, int, int, int]]:
    """Yield (instruction bytes, xmm, eax, ecx, operand, operand_offset, a_bits, b_bits)."""
    if op.name in PACKED_ROWS and form not in ("reg-reg", "dest==src"):
        return
    for a, b in pairs:
        # operand a is the destination lane, b the source
        xmm = [lane(0x3F800000 + n, FILLER ^ (n * 0x1111111100000000)) for n in range(8)]
        offset = 0
        if op.shape == "xr":
            # cvtsi2ss: source is an integer. b_bits interpreted as signed 32-bit integer.
            if form == "reg-reg":
                insn = encode(op, 2, 1)  # cvtsi2ss xmm2, ecx
                ecx = b
                xmm[2] = lane(a, xmm[2])
                yield insn, tuple(xmm), 0, ecx, b"", 0, a, b
                continue
            offset = {"reg-m32": 0, "m32-unaligned": 0x1001, "m32-pagecross": 0xFFE}.get(form)
            if offset is None:
                continue
            insn = encode(op, 2, 0, memory=offset)
            xmm[2] = lane(a, xmm[2])
            yield insn, tuple(xmm), 0, 0, struct.pack("<I", b), offset, a, b
            continue
        if op.shape == "rx":
            if form == "reg-reg":
                insn = encode(op, 0, 3)  # cvttss2si eax, xmm3
                xmm[3] = lane(b, xmm[3])
                yield insn, tuple(xmm), 0xDEADBEEF, 0, b"", 0, a, b
                continue
            offset = {"reg-m32": 0, "m32-unaligned": 0x1001, "m32-pagecross": 0xFFE}.get(form)
            if offset is None:
                continue
            insn = encode(op, 0, 0, memory=offset)
            yield insn, tuple(xmm), 0xDEADBEEF, 0, struct.pack("<I", b), offset, a, b
            continue
        # xx shape
        if form == "reg-reg":
            insn = encode(op, 1, 2)
            xmm[1] = lane(a, xmm[1])
            xmm[2] = lane(b, xmm[2])
            yield insn, tuple(xmm), 0, 0, b"", 0, a, b
        elif form == "dest==src":
            if a != b:
                continue
            insn = encode(op, 4, 4)
            xmm[4] = lane(a, xmm[4])
            yield insn, tuple(xmm), 0, 0, b"", 0, a, b
        else:
            offset = {"reg-m32": 0, "m32-unaligned": 0x1001, "m32-pagecross": 0xFFE}.get(form)
            if offset is None:
                continue
            insn = encode(op, 5, 0, memory=offset)
            xmm[5] = lane(a, xmm[5])
            yield insn, tuple(xmm), 0, 0, struct.pack("<I", b), offset, a, b


def run_row(
    host: HostStub,
    unicorn_runner: UnicornRunner,
    name: str,
    form: str,
    control: tuple[str, int],
    pairs: list[tuple[int, int]],
) -> Row:
    """Compare host and Unicorn over `pairs` for one (instruction, form, control word)."""
    op = OPS[name]
    row = Row(name, form, control[0])
    for insn, xmm, eax, ecx, operand, offset, a, b in _cases(op, form, pairs):
        assert_host_control(host)
        native = host.run(insn, xmm, control[1], eax, ecx, operand, offset)
        emulated = unicorn_runner.run(insn, xmm, control[1], eax, ecx, operand, offset)
        row.total += 1
        n_xmm, n_mxcsr, n_flags, n_eax = native
        u_xmm, u_mxcsr, u_flags, u_eax = emulated
        if (n_mxcsr ^ u_mxcsr) & ~STATUS_MASK & 0xFFFF:
            row.mxcsr_control_diff += 1
        if n_mxcsr & STATUS_MASK & ~u_mxcsr:
            row.sticky_missed += 1
        if u_mxcsr & STATUS_MASK & ~control[1]:
            row.unicorn_sticky_set += 1
        data_equal = (
            n_xmm == u_xmm and (n_flags & FLAG_MASK) == (u_flags & FLAG_MASK) and n_eax == u_eax
        )
        if not data_equal:
            row.divergent += 1
            if is_nan(a) and is_nan(b) and a != b:
                row.two_nan += 1
            if not row.first_divergence:
                row.first_divergence = f"a={a:08x} b={b:08x}"
    return row


def assert_host_control(host: HostStub) -> int:
    """Host MXCSR control bits (everything outside the sticky status bits) must be 0x1F80.

    QEMU's hardfloat path follows the HOST rounding mode, so a polluted host would
    silently move the oracle (measured in the T1620 design). Status bits drift freely.
    """
    value = host.host_mxcsr()
    if value & ~STATUS_MASK & 0xFFFF != MXCSR_DEFAULT:
        raise RuntimeError(f"host MXCSR {value:#06x} control bits drifted from 0x1f80")
    return value


def build_pairs(count: int, seed: int) -> list[tuple[int, int]]:
    """All special x special pairs plus `count` seeded random-bit pairs (deterministic)."""
    bits = [value for _, value in SPECIALS]
    pairs = [(a, b) for a in bits for b in bits]
    rng = random.Random(seed)
    pairs += [(rng.getrandbits(32), rng.getrandbits(32)) for _ in range(count)]
    return pairs


def self_check() -> None:
    """Fail loudly when the host cannot run the matrix honestly."""
    import unicorn

    if unicorn.__version__ != UNICORN_PIN:
        raise RuntimeError(f"Unicorn {unicorn.__version__} != pinned {UNICORN_PIN}")


def report(pairs_count: int, seed: int, instructions: list[str]) -> list[Row]:
    host = HostStub()
    emu = UnicornRunner()
    rows: list[Row] = []
    pairs = build_pairs(pairs_count, seed)
    for name in instructions:
        for form in FORMS:
            for control in control_words():
                if form != "reg-reg" and control[0] != "default":
                    continue
                rows.append(run_row(host, emu, name, form, control, pairs))
    return rows


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--report", action="store_true", help="print the divergence table")
    parser.add_argument("--pairs", type=int, default=300, help="seeded random-bit pairs")
    parser.add_argument("--seed", type=int, default=20261001)
    parser.add_argument("--rows", nargs="*", default=None, help="instruction names (default all)")
    args = parser.parse_args(argv)
    if not host_supported():
        print("SKIP: host is not x86-64 Linux with executable mmap", file=sys.stderr)
        return 77
    self_check()
    print(f"host cpu: {cpu_model()}")
    import unicorn

    print(f"unicorn: {unicorn.__version__}")
    names = args.rows or list(ADMITTED_ROWS + STEP2B_ROWS + REFUSED_ROWS)
    rows = report(args.pairs, args.seed, names)
    print(
        "instruction form control total divergent two_nan sticky_missed unicorn_sticky "
        "mxcsr_ctl_diff first"
    )
    for row in rows:
        print(
            f"{row.instruction} {row.form} {row.control} {row.total} {row.divergent} "
            f"{row.two_nan} {row.sticky_missed} {row.unicorn_sticky_set} "
            f"{row.mxcsr_control_diff} {row.first_divergence}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
