"""TSFP-LOCAL (patch 17): the opt-in EFLAGS publication at every ret.

The lifter models flags lazily -- each jcc reconstructs its condition from the
tracked flag setter -- so there is no flags word for a differential harness to
compare, and flag errors are only ever visible through their effect on a
register or a write. Patch 17 adds an OPT-IN emission: with
`Lifter.publish_eflags` set (the `--publish-eflags` CLI flag), every lifted
`ret` stores two words,

    g_harness_eflags       the assembled flags value
    g_harness_eflags_mask  which bits the model could actually ANSWER here

assembled from the same `_make_condition` probes the jcc forms use, exactly as
`_pushfd_stmt` assembles its pushed word. The mask is the honest half: a bit
the model cannot answer at this exit is left OUT of the mask rather than
published as a guess, and the harness reports it as unmodelled rather than
comparing it.

Two properties are load-bearing and pinned here:

* DEFAULT-OFF MEANS BYTE-IDENTICAL. With the option off (the default), no
  statement and no declaration is emitted anywhere. The whole-tree version of
  this check is a re-lift diffed against the shipped tree; this is the unit
  form.
* THE ENCODING IS THE x86 ONE. CF=0x001, PF=0x004, ZF=0x040, SF=0x080,
  DF=0x400, OF=0x800. A swapped bit would make the harness compare ZF against
  the oracle's SF and report plausible nonsense, so the exact constants are
  asserted per flag, next to the condition that feeds them.
"""
import unittest

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


def _lift(insns, publish=True, needs_cf=False):
    lifter = Lifter()
    lifter.publish_eflags = publish
    lifter.needs_cf = needs_cf
    stmts, _ = lift_basic_block(lifter, BasicBlock(start=0, instructions=insns))
    return stmts


def _cmp_ret(publish=True):
    return _lift(
        [
            _insn(0, "cmp", [_reg("eax"), _reg("ebx")]),
            _insn(2, "ret", []),
        ],
        publish=publish,
    )


def _publish_stmt(stmts):
    found = [s for s in stmts if "g_harness_eflags" in s]
    assert len(found) <= 1, found
    return found[0] if found else None


class DefaultOffTest(unittest.TestCase):
    def test_nothing_is_emitted_with_the_option_off(self):
        self.assertIsNone(_publish_stmt(_cmp_ret(publish=False)))

    def test_the_attribute_defaults_off(self):
        self.assertFalse(Lifter().publish_eflags)


class PublishAtRetTest(unittest.TestCase):
    def test_a_cmp_before_the_ret_publishes_all_five_probeable_flags(self):
        stmt = _publish_stmt(_cmp_ret())
        self.assertIsNotNone(stmt)
        # cmp answers every probe: CF PF ZF SF OF, plus DF always.
        self.assertIn("g_harness_eflags_mask = 0xCC5u", stmt)

    def test_the_bit_encoding_is_the_x86_one_per_flag(self):
        """Each flag's condition must feed ITS OWN x86 bit.

        This is the mutation target: swap any bit constant in
        `_EFLAGS_PROBES` (or reorder its rows against their bits) and this
        fails naming the flag, because the condition text and the bit are
        asserted together.
        """
        stmt = _publish_stmt(_cmp_ret())
        self.assertIn("((CMP_B(_fa, _fb)) ? 0x001u : 0u) /* CF */", stmt)
        self.assertIn("? 0x004u : 0u) /* PF */", stmt)
        self.assertIn("RECOMP_PARITY8", stmt)
        self.assertIn("((CMP_EQ(_fa, _fb)) ? 0x040u : 0u) /* ZF */", stmt)
        self.assertIn("? 0x080u : 0u) /* SF */", stmt)
        self.assertIn("? 0x800u : 0u) /* OF */", stmt)
        self.assertIn("RECOMP_CMP_OF", stmt)
        self.assertIn("(g_df ? 0x400u : 0u)", stmt)

    def test_the_publish_precedes_the_return(self):
        stmts = _cmp_ret()
        publish_at = next(i for i, s in enumerate(stmts) if "g_harness_eflags" in s)
        return_at = next(i for i, s in enumerate(stmts) if "return;" in s)
        self.assertLess(publish_at, return_at)

    def test_no_tracked_setter_publishes_df_only(self):
        stmt = _publish_stmt(_lift([_insn(0, "ret", [])]))
        self.assertIn("g_harness_eflags_mask = 0x400u", stmt)
        self.assertIn("no tracked setter", stmt)

    def test_a_setter_the_model_cannot_fully_answer_shrinks_the_mask(self):
        # shl answers ZF and SF from the snapshot; its OF/PF probes return
        # None, and its CF probe needs the _cf local. Without _cf declared the
        # mask must be exactly DF|ZF|SF -- claiming CF here would either not
        # compile or publish a constant 0 as if it were the model's answer.
        stmts = _lift(
            [
                _insn(0, "shl", [_reg("eax"), _reg("cl")]),
                _insn(2, "ret", []),
            ],
            needs_cf=False,
        )
        stmt = _publish_stmt(stmts)
        self.assertIn("g_harness_eflags_mask = 0x4C0u", stmt)
        self.assertNotIn("_cf", stmt)

    def test_the_cf_probe_returns_once_the_function_declares_cf(self):
        stmts = _lift(
            [
                _insn(0, "shl", [_reg("eax"), _reg("cl")]),
                _insn(2, "ret", []),
            ],
            needs_cf=True,
        )
        stmt = _publish_stmt(stmts)
        self.assertIn("g_harness_eflags_mask = 0x4C1u", stmt)
        self.assertIn("((_cf) ? 0x001u : 0u) /* CF */", stmt)

    def test_every_ret_in_a_block_publishes_with_its_own_flag_state(self):
        # A cmp feeds the first ret; the second ret (unreachable here, but the
        # shape jump-table arms produce) has the same tracked state. Both must
        # carry a publish, one statement each.
        stmts = _lift(
            [
                _insn(0, "cmp", [_reg("eax"), _imm(0)]),
                _insn(2, "ret", []),
                _insn(3, "ret", []),
            ]
        )
        self.assertEqual(sum("g_harness_eflags" in s for s in stmts), 2)

    def test_and_publishes_architectural_constants_for_cf_and_of(self):
        # After and/or/xor the hardware defines CF=0 and OF=0; the probes
        # return constant conditions and both bits belong IN the mask.
        stmts = _lift(
            [
                _insn(0, "and", [_reg("eax"), _reg("ebx")]),
                _insn(2, "ret", []),
            ]
        )
        stmt = _publish_stmt(stmts)
        self.assertIn("((0) ? 0x001u : 0u) /* CF */", stmt)
        self.assertIn("((0) ? 0x800u : 0u) /* OF */", stmt)
        # No PF: the and/or/xor arm of _make_condition has no jp case, so the
        # mask honestly omits 0x004 rather than inventing a parity answer.
        self.assertIn("g_harness_eflags_mask = 0xCC1u", stmt)


if __name__ == "__main__":
    unittest.main()
