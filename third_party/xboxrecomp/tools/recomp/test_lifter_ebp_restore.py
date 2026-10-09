"""TSFP-LOCAL (patch 10): restoring ebp must unpublish the frame.

`ebp` is a per-function C local; `g_ebp` is the published copy a frameless
callee reads as its caller's frame. `mov ebp, esp` publishes. Before this patch
nothing ever unpublished, so every frame-based function returned leaving g_ebp
pointing at its own dead frame -- the callee-saved-EBP contract held for the
local and not for the published copy.

These tests pin both halves of the contract, and the negative controls pin that
the resync is attached to ebp specifically and not to pops in general.
"""
import unittest

from tools.recomp import config
from tools.recomp.disasm import BasicBlock, Instruction, Operand
from tools.recomp.lifter import Lifter, lift_basic_block
from tools.recomp.translator import FunctionTranslator

#: The marker the patch emits. Asserting on the comment as well as the statement
#: keeps this test from passing on an unrelated `g_ebp = ebp;` such as the
#: publish at `mov ebp, esp` or the re-publish before a call.
RESYNC = "g_ebp = ebp; /* ebp restored: unpublish the dead frame */"


def _lift(*instructions):
    lifted, _ = lift_basic_block(
        Lifter(), BasicBlock(start=0, instructions=list(instructions)))
    return "\n".join(lifted)


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(value):
    return Operand(type="imm", imm=value)


class EbpRestoreTest(unittest.TestCase):
    def test_pop_ebp_unpublishes(self):
        generated = _lift(Instruction(0, 1, "pop", "ebp", "5d", operands=[_reg("ebp")]))

        self.assertIn("POP32(esp, ebp);", generated)
        self.assertIn(RESYNC, generated)
        # Order is load-bearing: the resync must read the restored value.
        self.assertLess(generated.index("POP32(esp, ebp);"),
                        generated.index(RESYNC))

    def test_leave_unpublishes(self):
        generated = _lift(Instruction(0, 1, "leave", "", "c9"))

        self.assertIn("POP32(esp, ebp); /* leave */", generated)
        self.assertIn(RESYNC, generated)

    def test_popad_unpublishes(self):
        generated = _lift(Instruction(0, 1, "popad", "", "61"))

        self.assertIn("POP32(esp, ebp);", generated)
        self.assertIn(RESYNC, generated)
        # After the block, not inside it: the block declares _pa_dead and the
        # resync must not land between a declaration and its use.
        self.assertLess(generated.index("/* popad */"), generated.index(RESYNC))

    def test_enter_publishes_like_mov_ebp_esp(self):
        # enter 0x10, 0 == push ebp; mov ebp, esp; sub esp, 0x10
        generated = _lift(Instruction(0, 4, "enter", "0x10, 0", "c8100000",
                                  operands=[_imm(0x10), _imm(0)]))

        self.assertIn("ebp = esp;", generated)
        self.assertIn("g_ebp = ebp;", generated)
        self.assertIn("g_seh_ebp = ebp;", generated)

    # ── Negative controls ──

    def test_other_register_pops_do_not_touch_g_ebp(self):
        for reg, encoding in (("eax", "58"), ("ebx", "5b"), ("esi", "5e")):
            with self.subTest(reg=reg):
                generated = _lift(Instruction(0, 1, "pop", reg, encoding, operands=[_reg(reg)]))

                self.assertIn(f"POP32(esp, {reg});", generated)
                self.assertNotIn("g_ebp", generated)

    def test_push_ebp_does_not_publish(self):
        # Saving ebp does not change it, so there is nothing to republish. A
        # resync here would publish the *caller's* frame as if it were current.
        generated = _lift(Instruction(0, 1, "push", "ebp", "55", operands=[_reg("ebp")]))

        self.assertIn("PUSH32(esp, ebp);", generated)
        self.assertNotIn("g_ebp", generated)


class EbpRestoreWholeFunctionTest(unittest.TestCase):
    """The contract read end-to-end on a real translated function.

    A classic prologue/epilogue pair must inherit, publish, then unpublish --
    in that order -- so that g_ebp on return is the value the function was
    entered with.
    """

    def test_frame_function_leaves_g_ebp_as_it_found_it(self):
        base = 0x10000
        # push ebp; mov ebp,esp; mov eax,[ebp+8]; pop ebp; ret
        image = bytes.fromhex('558bec8b45085dc3')
        config._install(
            [config.Section('.text', base, len(image), 0, len(image), True)],
            entry_point=base, kernel_thunk_addr=base,
            origin='ebp-restore-test')
        db = {base: {'start': hex(base), 'end': base + len(image),
                     '_addr': base, 'size': len(image)}}
        code = FunctionTranslator(image, db).translate_function(base, db[base])

        inherit = code.index('ebp = g_ebp;')
        publish = code.index('g_ebp = ebp; /* publish frame for frameless')
        unpublish = code.index(RESYNC)
        self.assertLess(inherit, publish)
        self.assertLess(publish, unpublish)
        # And the unpublish is the last thing to touch g_ebp before the ret.
        self.assertLess(unpublish, code.index('return;', unpublish))
