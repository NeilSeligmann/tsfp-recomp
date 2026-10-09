"""TSFP-LOCAL (patch 18): a shift whose masked count is zero keeps EFLAGS.

x86 masks a shift count to five bits and leaves EFLAGS untouched when the
masked count is zero. The lifter tracked the shift as the flag owner
unconditionally, so `and cl, 0x1F; sar eax, cl` reached with ECX=0x20 answered
a following flag read from the UNSHIFTED result (ZF=0) where the hardware
preserves the AND's flags (ZF=1). The T16 flags channel measured exactly this,
twice, in the CRT 64-bit shift helpers (`sub_003CAA90`, `sub_003CAD70`), and a
jcc after such a shift would mis-resolve the same way.

Patch 18 makes the owner RUNTIME-CONDITIONAL for a register count: a
pre-statement parks the previous owner's ZF/SF in _fa/_fas when the masked
count is zero, and the shift's own result snapshot runs only when it is not.
An immediate count that masks to zero keeps the previous owner statically.
Everything else lifts byte-identically to before.

Two kinds of pinning here:

* EMISSION SHAPE: where the conditional owner appears, and the four cases
  that must stay byte-identical (immediate nonzero count, no tracked owner,
  an owner whose probes cannot answer, a destination aliasing ECX).
* EXECUTION: the measured divergence itself, compiled and run. The lifted
  `and cl, 0x1F; sar eax, cl; sete dl` is swept against an x86 reference over
  counts 0..0x47 including 0x20, and the negative control re-lifts with the
  patch disabled (the plan hook removed, which IS the mutation) and requires
  the sweep to fail at ECX=0x20 specifically.
"""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from . import lifter as lifter_mod
from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _insn(addr, mnemonic, ops, size=2):
    insn = Instruction(addr, size, mnemonic, "", "90")
    insn.operands = ops
    return insn


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(value):
    return Operand(type="imm", imm=value)


def _lift(insns, needs_cf=False):
    lifter = Lifter()
    lifter.needs_cf = needs_cf
    return lift_basic_block(lifter, BasicBlock(start=0, instructions=insns))


_AND_CL = _insn(0, "and", [_reg("cl"), _imm(0x1F)])


class ConditionalOwnerEmissionTest(unittest.TestCase):
    def test_variable_sar_after_and_emits_the_conditional_owner(self):
        stmts, state = _lift([
            _insn(0, "and", [_reg("cl"), _imm(0x1F)]),
            _insn(2, "sar", [_reg("eax"), _reg("cl")]),
        ])
        pre = [s for s in stmts if "zero-count sar keeps and's ZF/SF" in s]
        self.assertEqual(len(pre), 1, stmts)
        # The pre-statement runs only when the masked count is zero and
        # encodes the AND's ZF/SF into the snapshot the shift owner reads.
        self.assertIn("if (!((LO8(ecx)) & 31u))", pre[0])
        self.assertIn("(_fa == 0)) ? 0u", pre[0])
        self.assertIn("0x80000000u : 1u", pre[0])
        self.assertIn("_fas = (int32_t)_fa;", pre[0])
        # The shift's own snapshot is conditional on a nonzero count.
        guarded = [s for s in stmts
                   if s.startswith("if (((LO8(ecx)) & 31u)) {")
                   and "sar result" in s]
        self.assertEqual(len(guarded), 1, stmts)
        # The shift still owns the flags for everything downstream.
        self.assertEqual(state[0], "sar")

    def test_the_pre_statement_comes_before_the_shift_write(self):
        stmts, _ = _lift([
            _insn(0, "and", [_reg("cl"), _imm(0x1F)]),
            _insn(2, "shr", [_reg("eax"), _reg("cl")]),
        ])
        pre = next(i for i, s in enumerate(stmts) if "zero-count" in s)
        write = next(i for i, s in enumerate(stmts)
                     if s.startswith("eax = eax >>"))
        self.assertLess(pre, write, stmts)

    def test_a_flag_read_after_the_shift_still_reads_the_snapshot(self):
        stmts, _ = _lift([
            _insn(0, "and", [_reg("cl"), _imm(0x1F)]),
            _insn(2, "shr", [_reg("eax"), _reg("cl")]),
            _insn(4, "sete", [_reg("dl")]),
        ])
        sete = [s for s in stmts if "/* sete */" in s]
        self.assertEqual(len(sete), 1, stmts)
        self.assertIn("(_fa == 0)", sete[0])

    def test_shld_variable_count_gets_the_pre_statement_only(self):
        stmts, _ = _lift([
            _insn(0, "cmp", [_reg("eax"), _reg("ebx")]),
            _insn(2, "shld", [_reg("eax"), _reg("edx"), _reg("cl")]),
        ])
        pre = [s for s in stmts if "zero-count shld keeps cmp's ZF/SF" in s]
        self.assertEqual(len(pre), 1, stmts)
        # shld's snapshot already lives inside its own zero-count guard, so
        # the body is the pre-patch body.
        body = [s for s in stmts if "/* shld */" in s]
        self.assertEqual(len(body), 1, stmts)
        self.assertIn("if (_c && _c < 32u)", body[0])

    def test_sal_is_tracked_as_shl(self):
        _, state = _lift([
            _insn(0, "and", [_reg("cl"), _imm(0x1F)]),
            _insn(2, "sal", [_reg("eax"), _reg("cl")]),
        ])
        self.assertEqual(state[0], "shl")


class StaticZeroCountTest(unittest.TestCase):
    def test_immediate_zero_keeps_the_previous_owner(self):
        stmts, state = _lift([
            _insn(0, "cmp", [_reg("eax"), _reg("ebx")]),
            _insn(2, "shl", [_reg("eax"), _imm(0)]),
            _insn(4, "sete", [_reg("dl")]),
        ])
        self.assertEqual(state[0], "cmp")
        self.assertNotIn("shift result", " ".join(stmts))
        sete = [s for s in stmts if "/* sete */" in s]
        self.assertEqual(len(sete), 1, stmts)
        self.assertIn("CMP_EQ(_fa, _fb)", sete[0])

    def test_immediate_32_masks_to_zero_too(self):
        _, state = _lift([
            _insn(0, "cmp", [_reg("eax"), _reg("ebx")]),
            _insn(2, "shr", [_reg("eax"), _imm(32)]),
        ])
        self.assertEqual(state[0], "cmp")


class UnchangedCasesTest(unittest.TestCase):
    """The four fallbacks must emit exactly the pre-patch statements."""

    def _assert_plain(self, stmts, state, kind="sar"):
        self.assertNotIn("zero-count", " ".join(stmts))
        plain = [s for s in stmts
                 if s.startswith("_fa = ") and f"{kind} result" in s]
        self.assertEqual(len(plain), 1, stmts)
        self.assertEqual(state[0], kind if kind != "shift" else "shl")

    def test_immediate_nonzero_count(self):
        stmts, state = _lift([
            _insn(0, "and", [_reg("cl"), _imm(0x1F)]),
            _insn(2, "sar", [_reg("eax"), _imm(2)]),
        ])
        self._assert_plain(stmts, state)

    def test_no_tracked_owner(self):
        stmts, state = _lift([_insn(0, "sar", [_reg("eax"), _reg("cl")])])
        self._assert_plain(stmts, state)

    def test_an_owner_whose_probes_cannot_answer(self):
        # imul leaves ZF and SF architecturally undefined; its je/js probes
        # refuse, so the shift cannot inherit anything and lifts as before.
        stmts, state = _lift([
            _insn(0, "imul", [_reg("eax"), _reg("ebx")]),
            _insn(3, "sar", [_reg("eax"), _reg("cl")]),
        ])
        self._assert_plain(stmts, state)

    def test_a_destination_aliasing_ecx(self):
        # The guarded snapshot re-reads the count AFTER the write; with the
        # destination inside ECX it would read the shifted count instead.
        stmts, state = _lift([
            _insn(0, "and", [_reg("cl"), _imm(0x1F)]),
            _insn(2, "shr", [_reg("ecx"), _reg("cl")]),
        ])
        self.assertNotIn("zero-count", " ".join(stmts))
        plain = [s for s in stmts
                 if s.startswith("_fa = ") and "shift result" in s]
        self.assertEqual(len(plain), 1, stmts)
        self.assertEqual(state[0], "shr")


# ── the measured divergence, compiled and run ──────────────────────────────

_PRELUDE = """#include <stdint.h>
#include <stdio.h>
static uint32_t eax, ecx, edx;
static int _flags = 0;
static uint32_t _fa, _fb;
static int32_t _fas, _fbs;
#define LO8(v) ((uint8_t)(v))
#define SET_LO8(r, v) ((r) = ((r) & 0xFFFFFF00u) | ((uint32_t)(v) & 0xFFu))
"""

_HARNESS = """
static uint32_t subject(uint32_t a, uint32_t c)
{
    eax = a; ecx = c; edx = 0;
    _fa = _fb = 0; _fas = _fbs = 0; (void)_flags; (void)_fb; (void)_fbs;
@BODY@
    return LO8(edx);
}

/* x86: `and cl, 0x1F` sets ZF from the masked count; a shift by a masked
 * count of zero preserves those flags, otherwise ZF comes from the shifted
 * result. */
static uint32_t reference(uint32_t a, uint32_t c)
{
    uint32_t masked = c & 0xFFu & 0x1Fu;
    int zf = (masked == 0);
    if (masked != 0)
        zf = ((uint32_t)((int32_t)a >> masked) == 0);
    return zf ? 1u : 0u;
}

int main(void)
{
    static const uint32_t vals[] = { 0x00000000u, 0x00000001u, 0x80000000u,
                                     0xFFFFFFFFu, 0x12345678u, 0xDEADBEEFu };
    int failures = 0;
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++)
        for (uint32_t c = 0; c <= 0x47u; c++) {
            uint32_t got = subject(vals[i], c);
            uint32_t want = reference(vals[i], c);
            if (got != want) {
                failures++;
                if (failures <= 12)
                    printf("a=0x%08X c=0x%02X got %u want %u\\n",
                           vals[i], c, got, want);
            }
        }
    return failures ? 1 : 0;
}
"""


def _lifted_sequence_c(patched):
    """Lift `and cl, 0x1F; sar eax, cl; sete dl`, patched or reverted.

    `patched=False` re-lifts with _shift_zero_count_plan answering None for
    everything, which makes lift_basic_block take the pre-patch path -- the
    exact mutation "remove the conditional owner".
    """
    insns = [
        _insn(0, "and", [_reg("cl"), _imm(0x1F)]),
        _insn(3, "sar", [_reg("eax"), _reg("cl")]),
        _insn(5, "sete", [_reg("dl")]),
    ]
    if patched:
        stmts, _ = _lift(insns)
    else:
        with mock.patch.object(lifter_mod, "_shift_zero_count_plan",
                               lambda *a, **k: None):
            stmts, _ = _lift(insns)
    return "\n".join("    " + s for s in stmts)


def _build_and_run(source):
    cc = shutil.which("clang") or shutil.which("gcc") or shutil.which("cc")
    if not cc:
        raise unittest.SkipTest("C compiler unavailable")
    with tempfile.TemporaryDirectory() as temp:
        src = Path(temp) / "test.c"
        src.write_text(source)
        exe = Path(temp) / "test.exe"
        built = subprocess.run([cc, "-O2", str(src), "-o", str(exe)],
                               capture_output=True, text=True)
        assert built.returncode == 0, built.stderr + "\n" + source
        return subprocess.run([str(exe)], capture_output=True, text=True)


class MeasuredDivergenceTest(unittest.TestCase):
    def test_the_lifted_sequence_matches_x86_at_every_count(self):
        ran = _build_and_run(_PRELUDE + _HARNESS.replace(
            "@BODY@", _lifted_sequence_c(patched=True)))
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_negative_control_the_unconditional_owner_is_caught(self):
        """The mutation: lift with the plan removed, as before patch 18.

        The sweep must fail, and fail at ECX=0x20 -- the input Unicorn
        measured the two CRT helpers diverging at.
        """
        ran = _build_and_run(_PRELUDE + _HARNESS.replace(
            "@BODY@", _lifted_sequence_c(patched=False)))
        self.assertEqual(ran.returncode, 1,
                         "the pre-patch lift should disagree with x86:\n"
                         + ran.stdout + ran.stderr)
        self.assertIn(" c=0x20 ", ran.stdout, ran.stdout)


if __name__ == "__main__":
    unittest.main()
