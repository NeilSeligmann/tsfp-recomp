"""TSFP-LOCAL (patch 05): non-returning-function analysis.

Upstream has no notion of `noreturn`, so the bytes after a call to a fatal
error or throw helper are decoded as code. In the void(void) lifting model the
caller then returns normally and executes whatever that padding became.

These pin the analysis contract. The important half is the NEGATIVE cases:
the proof must be one-sided, answering "returns" at every uncertainty, because
a false positive truncates a real function.

Driven directly against FunctionDetector with stub instructions, in the same
style as the adjacent test_function_end.py.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector, Function  # noqa: E402


class _Insn:
    def __init__(self, addr, size, mnemonic="mov", target=None, call=None,
                 is_ret=False, is_jump=False, is_cond_jump=False,
                 jump_table=None):
        self.address = addr
        self.size = size
        self.end_address = addr + size
        self.mnemonic = mnemonic
        self.jump_target = target
        self.call_target = call
        self.jump_table = jump_table
        self.is_ret = is_ret
        self.is_jump = is_jump
        self.is_cond_jump = is_cond_jump
        self.is_call = call is not None or mnemonic == "call"
        self.is_branch = is_jump or is_cond_jump


class _Engine:
    def __init__(self, insns):
        self._insns = list(insns)

    def get_instructions_in_range(self, lo, hi):
        return [i for i in self._insns if lo <= i.address < hi]


def _detector(funcs, insns):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns)
    det.functions = {f.start: f for f in funcs}
    det._noreturn = set()
    det._noreturn_seeds = set()
    det.detect_noreturn = True
    return det


def _func(start, end, name=None):
    return Function(start=start, end=end, name=name or f"sub_{start:08X}")


class NoreturnBaseCasesTest(unittest.TestCase):

    def test_a_function_that_jumps_to_itself_never_returns(self):
        # The infinite-loop panic: `jmp $`. No ret, no exit.
        det = _detector([_func(0x1000, 0x1002)],
                        [_Insn(0x1000, 2, "jmp", target=0x1000, is_jump=True)])
        self.assertEqual(det._analyse_noreturn(), 1)
        self.assertTrue(det.functions[0x1000].noreturn)
        self.assertEqual(det.functions[0x1000].noreturn_reason,
                         "no reachable ret")

    def test_a_ud2_stub_never_returns(self):
        det = _detector([_func(0x1000, 0x1002)], [_Insn(0x1000, 2, "ud2")])
        self.assertEqual(det._analyse_noreturn(), 1)

    def test_a_plain_function_returns(self):
        det = _detector([_func(0x1000, 0x1001)],
                        [_Insn(0x1000, 1, "ret", is_ret=True)])
        self.assertEqual(det._analyse_noreturn(), 0)
        self.assertFalse(det.functions[0x1000].noreturn)

    def test_a_ret_behind_a_conditional_branch_still_counts(self):
        #   1000 jne 1005
        #   1002 jmp 1000      (back edge)
        #   1005 ret
        det = _detector(
            [_func(0x1000, 0x1006)],
            [_Insn(0x1000, 2, "jne", target=0x1005, is_cond_jump=True),
             _Insn(0x1002, 3, "jmp", target=0x1000, is_jump=True),
             _Insn(0x1005, 1, "ret", is_ret=True)])
        self.assertEqual(det._analyse_noreturn(), 0)


class NoreturnPropagationTest(unittest.TestCase):

    def test_a_call_to_a_noreturn_function_propagates(self):
        # sub_2000 is `jmp $`. sub_1000 calls it and has no other exit, so
        # sub_1000 cannot return either. One fixpoint round apart.
        det = _detector(
            [_func(0x1000, 0x1005), _func(0x2000, 0x2002)],
            [_Insn(0x1000, 5, "call", call=0x2000),
             _Insn(0x2000, 2, "jmp", target=0x2000, is_jump=True)])
        self.assertEqual(det._analyse_noreturn(), 2)
        self.assertTrue(det.functions[0x1000].noreturn)
        self.assertIn("propagated", det.functions[0x1000].noreturn_reason)

    def test_a_tail_jmp_to_a_noreturn_function_propagates(self):
        det = _detector(
            [_func(0x1000, 0x1005), _func(0x2000, 0x2002)],
            [_Insn(0x1000, 5, "jmp", target=0x2000, is_jump=True),
             _Insn(0x2000, 2, "jmp", target=0x2000, is_jump=True)])
        self.assertEqual(det._analyse_noreturn(), 2)

    def test_a_caller_with_its_own_ret_still_returns(self):
        # The discriminating case: calling a noreturn function does NOT make
        # the caller noreturn if another path reaches a ret.
        #   1000 jne 1008
        #   1002 call 2000     (noreturn)
        #   1008 ret
        det = _detector(
            [_func(0x1000, 0x1009), _func(0x2000, 0x2002)],
            [_Insn(0x1000, 2, "jne", target=0x1008, is_cond_jump=True),
             _Insn(0x1002, 5, "call", call=0x2000),
             _Insn(0x1008, 1, "ret", is_ret=True),
             _Insn(0x2000, 2, "jmp", target=0x2000, is_jump=True)])
        self.assertEqual(det._analyse_noreturn(), 1)
        self.assertFalse(det.functions[0x1000].noreturn)

    def test_a_seed_propagates_without_being_provable(self):
        # A kernel import cannot be proved structurally -- it is not a decoded
        # function -- so seeding is the only route. The seed must still only
        # count when it is a known function.
        det = _detector(
            [_func(0x1000, 0x1005), _func(0x2000, 0x2001)],
            [_Insn(0x1000, 5, "call", call=0x2000),
             _Insn(0x2000, 1, "ret", is_ret=True)])
        self.assertEqual(det._analyse_noreturn(seeds={0x2000}), 2)
        self.assertEqual(det.functions[0x2000].noreturn_reason, "seeded")


class NoreturnIsOneSidedTest(unittest.TestCase):
    """Every uncertainty must answer "returns". These are the false-positive
    guards, and they are the reason the analysis is safe to act on."""

    def test_an_indirect_jmp_is_assumed_to_return(self):
        det = _detector([_func(0x1000, 0x1002)],
                        [_Insn(0x1000, 2, "jmp", target=None, is_jump=True)])
        self.assertEqual(det._analyse_noreturn(), 0)

    def test_a_recovered_jump_table_is_assumed_to_return(self):
        # The arm set is data-driven and may be truncated at the function
        # bound, so an epilogue could sit outside what we can see.
        det = _detector([_func(0x1000, 0x1007)],
                        [_Insn(0x1000, 7, "jmp", target=None, is_jump=True,
                               jump_table=0x9000)])
        self.assertEqual(det._analyse_noreturn(), 0)

    def test_an_undecodable_body_is_assumed_to_return(self):
        # No instruction at the entry at all: data misclassified as a
        # function must not be declared non-returning, because its CALLERS
        # would then be truncated.
        det = _detector([_func(0x1000, 0x1010)], [])
        self.assertEqual(det._analyse_noreturn(), 0)

    def test_an_indirect_call_is_assumed_to_return(self):
        det = _detector([_func(0x1000, 0x1002)],
                        [_Insn(0x1000, 2, "call", call=None)])
        self.assertEqual(det._analyse_noreturn(), 0)

    def test_a_tail_jmp_to_an_unknown_function_is_assumed_to_return(self):
        det = _detector([_func(0x1000, 0x1005)],
                        [_Insn(0x1000, 5, "jmp", target=0x9000, is_jump=True)])
        self.assertEqual(det._analyse_noreturn(), 0)

    def test_falling_out_of_the_recovered_bounds_is_assumed_to_return(self):
        # A body whose last instruction runs off the end: the real ret may be
        # just past a mis-detected bound.
        det = _detector([_func(0x1000, 0x1002)], [_Insn(0x1000, 2, "mov")])
        self.assertEqual(det._analyse_noreturn(), 0)


class FunctionEndStopsAtANoreturnCallTest(unittest.TestCase):

    def _end(self, insns, noreturn, start=0x1000, upper=0x1100):
        det = FunctionDetector.__new__(FunctionDetector)

        class _E:
            def __init__(self, ins):
                self.by_addr = {i.address: i for i in ins}
                self.jump_tables = {}

            def get_instruction(self, addr):
                return self.by_addr.get(addr)

            def jump_table_entries(self, tbl):
                return []

        det.engine = _E(insns)
        det._noreturn = set(noreturn)
        return det._find_function_end(start, None, upper)

    def test_the_sweep_stops_after_a_call_that_never_returns(self):
        #   1000 call 2000     (noreturn)
        #   1005 mov           <- padding or the next function, NOT code here
        insns = [_Insn(0x1000, 5, "call", call=0x2000),
                 _Insn(0x1005, 5, "mov")]
        self.assertEqual(self._end(insns, {0x2000}), 0x1005)
        # and without the analysis it ran straight on, which is the bug
        self.assertEqual(self._end(insns, set()), 0x100A)

    def test_a_branch_target_past_the_call_is_still_decoded(self):
        # The guard that keeps this safe. Something inside the function
        # branches past the noreturn call, so those bytes ARE code and the
        # sweep must not stop short of them.
        #   1000 jne 1005
        #   1002 call 2000     (noreturn)
        #   1005 ret
        insns = [_Insn(0x1000, 2, "jne", target=0x1005, is_cond_jump=True),
                 _Insn(0x1002, 3, "call", call=0x2000),
                 _Insn(0x1005, 1, "ret", is_ret=True)]
        self.assertEqual(self._end(insns, {0x2000}), 0x1006)

    def test_a_call_to_an_ordinary_function_does_not_stop_the_sweep(self):
        insns = [_Insn(0x1000, 5, "call", call=0x2000),
                 _Insn(0x1005, 1, "ret", is_ret=True)]
        self.assertEqual(self._end(insns, {0x3000}), 0x1006)


if __name__ == "__main__":
    unittest.main()
