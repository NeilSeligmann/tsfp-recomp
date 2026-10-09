"""TSFP-LOCAL (patches 01 and 02): the loud defaults, pinned and RUN.

Upstream's answer to "I cannot translate this" and "I cannot reconstruct this
condition" was to carry on: log the untranslated instruction and no-op it,
emit the never-assigned `_flags` local for the lost condition, set eax = 0 for
an indirect call that resolves to nothing. All three complete the run and
produce a plausible, wrong answer -- the one failure mode a differential
correctness oracle cannot see, because both sides "ran fine".

These now trap by default and are downgraded by DEFINING A MACRO, which is a
deliberate act recorded in the build rather than a forgotten environment
variable.

A text check alone would be worthless here: the whole claim is about what the
program DOES. So the second class compiles the header both ways and runs it,
and asserts that the default build dies and the downgraded build does not.
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from . import config
from .disasm import Instruction, Operand
from .lifter import Lifter
from .translator import FunctionTranslator

_RUNTIME = Path(__file__).resolve().parents[2] / "templates" / "runtime"
BASE = 0x00010000


def _cc():
    return next(filter(None, map(shutil.which, ("cc", "gcc", "clang"))), None)


def _translate(image):
    """Lift a raw byte image as one function, the way the other tests do."""
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="loud-defaults-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


class EmittedFormTest(unittest.TestCase):

    def test_an_untranslated_instruction_carries_its_va_and_text(self):
        lifter = Lifter()
        insn = Instruction(0x0037FD71, 1, "daa", "", "", operands=[])
        out = " ".join(lifter.lift_instruction(insn))
        self.assertIn('RECOMP_UNIMPL("daa", 0x0037FD71u);', out)
        self.assertEqual(lifter.unimplemented, {"daa": [0x0037FD71]})

    def test_an_unresolved_condition_carries_its_va_and_mnemonic(self):
        # No tracked flag setter, so _make_condition has nothing. Upstream
        # emitted the bare `_flags`, which is declared = 0 and assigned on
        # exactly one path (the rep string compares) -- so on every other
        # path it is a compile-time zero and -O2 deletes the branch outright.
        # nop; jne +2; nop; nop; ret -- nothing owns the flags at the jne.
        code = _translate(b"\x90\x75\x02\x90\x90\xC3")
        self.assertIn('RECOMP_FLAGS_UNRESOLVED(_flags, "jne", 0x00010001u)',
                      code)
        # and the bare read is gone, declaration aside
        self.assertNotIn("if (_flags ", code)

    def test_the_fallback_expression_is_still_threaded_through(self):
        # Passing `_flags` as an argument is what makes
        # -DRECOMP_FLAGS_CONTINUE an exact reproduction of upstream rather
        # than an approximation of it. Without that the downgraded build
        # could not be differentially compared against upstream.
        lifter = Lifter()
        insn = Instruction(0x1000, 3, "setg", "al", "",
                           operands=[Operand(type="reg", reg="al")])
        out = " ".join(lifter.lift_instruction(insn))
        self.assertIn("RECOMP_FLAGS_UNRESOLVED(_flags,", out)

    def test_an_unresolved_cmov_does_not_silently_refuse_to_move(self):
        lifter = Lifter()
        insn = Instruction(0x1000, 3, "cmovl", "eax, ecx", "",
                           operands=[Operand(type="reg", reg="eax"),
                                     Operand(type="reg", reg="ecx")])
        out = " ".join(lifter.lift_instruction(insn))
        self.assertIn('RECOMP_FLAGS_UNRESOLVED(_flags, "cmovl",', out)

    def test_a_function_whose_only_flag_consumer_is_a_loop_declares_flags(self):
        # loope's ZF term falls back the same way a jcc's does, and capstone
        # does not report it as a conditional jump -- so the declaration test
        # missed it and the generated C referenced an undeclared `_flags`.
        code = _translate(b"\x90\xE1\x04\x90\x90\x90\x90\xC3")
        self.assertIn("int _flags = 0;", code)
        self.assertIn('RECOMP_FLAGS_UNRESOLVED(_flags, "loope"', code)


_PROBE = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include "recomp_types.h"

ptrdiff_t g_xbox_mem_offset;

/* The three trap entry points and their logging counterparts, as our host
 * will define them. abort() replaced by exit(42) so the probe's outcome is
 * observable without a signal. */
void recomp_unimpl(const char *t, uint32_t va)
{ (void)t; (void)va; printf("CONTINUED\n"); }
RECOMP_NORETURN void recomp_unimpl_trap(const char *t, uint32_t va)
{ printf("TRAPPED %s 0x%08X\n", t, va); exit(42); }
void recomp_flags_unresolved(const char *cc, uint32_t va)
{ (void)cc; (void)va; printf("CONTINUED\n"); }
RECOMP_NORETURN void recomp_flags_unresolved_trap(const char *cc, uint32_t va)
{ printf("TRAPPED %s 0x%08X\n", cc, va); exit(42); }

int main(void)
{
    int _flags = 0;
    int taken = 0;
    /* exactly what the lifter emits at an unresolved condition */
    if (RECOMP_FLAGS_UNRESOLVED(_flags, "jne", 0x00156CB0u)) taken = 1;
    printf("branch_taken=%d\n", taken);
    /* and at an untranslated instruction */
    RECOMP_UNIMPL("daa", 0x0037FD71u);
    printf("REACHED_END\n");
    return 0;
}
'''


class RuntimeBehaviourTest(unittest.TestCase):
    """Compile the real header and run it. The claim is behavioural."""

    def _run(self, extra):
        cc = _cc()
        if not cc:
            self.skipTest("no C compiler available")
        with tempfile.TemporaryDirectory() as tmp:
            src = os.path.join(tmp, "probe.c")
            exe = os.path.join(tmp, "probe")
            with open(src, "w") as fh:
                fh.write(_PROBE)
            build = subprocess.run(
                [cc, "-std=gnu11", "-O2", "-w", *extra, src, "-o", exe,
                 "-I", str(_RUNTIME)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr[-2000:])
            run = subprocess.run([exe], capture_output=True, text=True)
            return run.returncode, run.stdout

    def test_the_default_build_traps_at_the_unresolved_condition(self):
        rc, out = self._run([])
        self.assertEqual(rc, 42, out)
        self.assertIn("TRAPPED jne 0x00156CB0", out)
        # It stops AT the cause: nothing downstream of it ran.
        self.assertNotIn("branch_taken", out)
        self.assertNotIn("REACHED_END", out)

    def test_the_downgraded_flags_build_reproduces_upstream_exactly(self):
        rc, out = self._run(["-DRECOMP_FLAGS_CONTINUE"])
        self.assertEqual(rc, 42, out)
        # The condition came back false, which is upstream's behaviour...
        self.assertIn("branch_taken=0", out)
        # ...and then the untranslated instruction trapped, because that is a
        # separate switch. The two downgrades are independent on purpose.
        self.assertIn("TRAPPED daa 0x0037FD71", out)

    def test_RECOMP_ALL_CONTINUE_restores_upstream_wholesale(self):
        rc, out = self._run(["-DRECOMP_ALL_CONTINUE"])
        self.assertEqual(rc, 0, out)
        self.assertIn("branch_taken=0", out)
        self.assertIn("REACHED_END", out)

    def test_the_trap_survives_O2(self):
        # The point of making this an opaque call. Upstream's `_flags` read
        # was a compile-time constant 0, so at -O2 the branch was deleted and
        # NOTHING in the binary recorded that a conditional had gone missing.
        # Build at -O2 and confirm the trap still fires.
        rc, out = self._run(["-O2"])
        self.assertEqual(rc, 42, out)
        self.assertIn("TRAPPED", out)


if __name__ == "__main__":
    unittest.main()
