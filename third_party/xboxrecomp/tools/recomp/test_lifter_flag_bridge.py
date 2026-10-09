"""TSFP-LOCAL (patch 21): flags that cross a call, through an explicit bridge.

The lifter tracks the flag owner per function, so a function that starts with a
`je` on the flags its caller's callee left has no setter and lifts to the
`RECOMP_FLAGS_UNRESOLVED` trap. The CRT acos wrapper is exactly that:

    call 0x3CCE80      ; mov eax,[esp+8]; and eax,7FF00000h; cmp eax,7FF00000h; ... ret
    call 0x3C9CE1      ; push edx; wait; fnstcw [esp]; je ...

`--flag-bridge` names the provider and the consumer. The provider's ret
publishes the modelled CF PF ZF SF OF word and the mask of bits it could answer,
the consumer reads both once at entry and clears the mask. Pinned here:

* DEFAULT-OFF MEANS BYTE-IDENTICAL. With no bridge nothing is emitted.
* THE PROVIDER PUBLISHES AT EVERY RET with its own tracked state, and a ret with
  no tracked setter publishes mask 0 so the consumer traps instead of guessing.
* EVERY CONDITION CODE reads the right bits of the word: the emitted expression
  is evaluated against all 32 combinations of CF PF ZF SF OF and compared with
  the architectural definition written independently here.
* A BIT THE MASK DOES NOT HOLD TRAPS: the macro's guard names the exact bits.
* THE ENTRY STATE applies only to an entry nothing branches back to.
"""

import ast
import itertools
import re
import unittest

from . import config
from .__main__ import _load_flag_bridge
from .lifter import (BRIDGE_SETTER, Lifter, _bridge_condition, _make_condition,
                     lift_basic_block)
from .translator import FunctionTranslator, _incoming_flag_state
from .disasm import BasicBlock, Instruction, Operand

BASE = 0x00011000
PROVIDER = BASE
CONSUMER = BASE + 0x40

# mov eax,[esp+8]; and eax,0x7FF00000; cmp eax,0x7FF00000; je +1; ret;
# mov eax,[esp+8]; ret            (the shape of 0x003CCE80)
PROVIDER_BYTES = bytes.fromhex("8B442408250000F07F3D0000F07F7401C38B442408C3")
# push edx; wait; fnstcw [esp]; je +2; xor eax,eax; ret     (the shape of 0x003C9CE1)
CONSUMER_BYTES = bytes.fromhex("529BD93C24740233C0C3")

_CONFIG_GLOBALS = (
    "_SECTIONS", "SECTIONS", "_configured_from",
    "TEXT_VA_START", "TEXT_VA_END", "RDATA_VA_START", "RDATA_VA_END",
    "DATA_VA_START", "DATA_VA_END", "KERNEL_THUNK_ADDR", "ENTRY_POINT",
)

BITS = {"CF": 0x001, "PF": 0x004, "ZF": 0x040, "SF": 0x080, "OF": 0x800}


def _image():
    image = bytearray(0x80)
    image[0:len(PROVIDER_BYTES)] = PROVIDER_BYTES
    image[0x40:0x40 + len(CONSUMER_BYTES)] = CONSUMER_BYTES
    return bytes(image)


class TranslatedFunctionTest(unittest.TestCase):
    def setUp(self):
        self._saved = {k: getattr(config, k) for k in _CONFIG_GLOBALS}

    def tearDown(self):
        for k, v in self._saved.items():
            setattr(config, k, v)

    def _translate(self, start, providers=(), consumers=()):
        image = _image()
        config._install(
            [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
            entry_point=BASE, kernel_thunk_addr=BASE, origin="flag-bridge-test")
        end = start + (len(PROVIDER_BYTES) if start == PROVIDER else len(CONSUMER_BYTES))
        db = {start: {"start": f"0x{start:08X}", "end": end, "_addr": start,
                      "size": end - start}}
        translator = FunctionTranslator(image, db)
        translator.lifter.flag_bridge_providers = frozenset(providers)
        translator.lifter.flag_bridge_consumers = frozenset(consumers)
        return translator.translate_function(start, db[start])

    def test_default_emits_nothing_for_either_function(self):
        for start in (PROVIDER, CONSUMER):
            code = self._translate(start)
            self.assertNotIn("flag_bridge", code)
            self.assertNotIn("_bridge", code)
        # and the consumer is the trap it was before
        self.assertIn('RECOMP_FLAGS_UNRESOLVED(_flags, "je", 0x00011045u)',
                      self._translate(CONSUMER))

    def test_the_provider_publishes_at_both_rets(self):
        code = self._translate(PROVIDER, providers=[PROVIDER])
        self.assertEqual(code.count("g_flag_bridge_eflags ="), 2, code)
        self.assertEqual(code.count("g_flag_bridge_mask = 0x8C5u"), 2, code)
        for name, bit in BITS.items():
            self.assertEqual(code.count(f"? 0x{bit:03X}u : 0u) /* {name} */"), 2, name)
        # the publish runs before the return of each exit
        for publish in re.finditer(r"g_flag_bridge_mask = 0x8C5u;[^\n]*\n", code):
            self.assertIn("return;", code[publish.end():publish.end() + 400])
        # the consumer machinery is absent from a provider
        self.assertNotIn("_bridge_mask", code.replace("g_flag_bridge_mask", ""))

    def test_a_function_not_listed_does_not_publish(self):
        code = self._translate(CONSUMER, providers=[PROVIDER], consumers=[CONSUMER])
        self.assertNotIn("g_flag_bridge_eflags =", code)

    def test_the_consumer_reads_the_words_once_and_clears_the_mask(self):
        code = self._translate(CONSUMER, consumers=[CONSUMER])
        read = code.index("uint32_t _bridge = g_flag_bridge_eflags, _bridge_mask = g_flag_bridge_mask;")
        clear = code.index("g_flag_bridge_mask = 0u;")
        first_statement = code.index("loc_00011040")
        self.assertLess(read, clear)
        self.assertLess(clear, first_statement)
        self.assertIn('RECOMP_BRIDGE_FLAGS(_bridge_mask, 0x040u, ((_bridge & 0x040u) != 0), '
                      '"je", 0x00011045u)', code)
        self.assertNotIn("RECOMP_FLAGS_UNRESOLVED", code)

    def test_a_provider_with_no_tracked_setter_publishes_mask_zero(self):
        lifter = Lifter()
        lifter.flag_bridge_providers = frozenset({0})
        insns = [_insn(0, "ret", [])]
        stmts, _ = lift_basic_block(lifter, BasicBlock(start=0, instructions=insns))
        publish = [s for s in stmts if "g_flag_bridge" in s]
        self.assertEqual(len(publish), 1)
        self.assertIn("g_flag_bridge_eflags = 0u; g_flag_bridge_mask = 0x000u;", publish[0])
        self.assertIn("no tracked setter", publish[0])


def _insn(addr, mnemonic, ops, size=2):
    insn = Instruction(addr, size, mnemonic, "", "90")
    insn.operands = ops
    return insn


# The architectural definition of each condition code over the five modelled flags.
REFERENCE = {
    "je": lambda f: f["ZF"], "jne": lambda f: not f["ZF"],
    "jb": lambda f: f["CF"], "jae": lambda f: not f["CF"],
    "jbe": lambda f: f["CF"] or f["ZF"], "ja": lambda f: not f["CF"] and not f["ZF"],
    "js": lambda f: f["SF"], "jns": lambda f: not f["SF"],
    "jo": lambda f: f["OF"], "jno": lambda f: not f["OF"],
    "jp": lambda f: f["PF"], "jnp": lambda f: not f["PF"],
    "jl": lambda f: f["SF"] != f["OF"], "jge": lambda f: f["SF"] == f["OF"],
    "jle": lambda f: f["ZF"] or f["SF"] != f["OF"],
    "jg": lambda f: not f["ZF"] and f["SF"] == f["OF"],
}
READS = {
    "je": "ZF", "jne": "ZF", "jb": "CF", "jae": "CF", "jbe": "CF ZF", "ja": "CF ZF",
    "js": "SF", "jns": "SF", "jo": "OF", "jno": "OF", "jp": "PF", "jnp": "PF",
    "jl": "SF OF", "jge": "SF OF", "jle": "ZF SF OF", "jg": "ZF SF OF",
}


def _evaluate(expression, word):
    """Evaluate an emitted C condition over `_bridge` = word.

    The C text is rewritten to a Python expression and walked node by node:
    only names, constants, `&`, comparisons and boolean operators are accepted,
    so nothing but the emitted condition can run.
    """
    text = re.sub(r"!(?!=)", " not ", expression)
    text = text.replace("||", " or ").replace("&&", " and ")
    text = re.sub(r"0x([0-9A-Fa-f]+)u", r"0x\1", text)

    def walk(node):
        if isinstance(node, ast.Expression):
            return walk(node.body)
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return node.value
        if isinstance(node, ast.Name) and node.id == "_bridge":
            return word
        if isinstance(node, ast.BinOp) and isinstance(node.op, ast.BitAnd):
            return walk(node.left) & walk(node.right)
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, ast.Not):
            return not walk(node.operand)
        if isinstance(node, ast.BoolOp):
            values = [walk(v) for v in node.values]
            return all(values) if isinstance(node.op, ast.And) else any(values)
        if isinstance(node, ast.Compare) and len(node.ops) == 1:
            left, right = walk(node.left), walk(node.comparators[0])
            if isinstance(node.ops[0], ast.NotEq):
                return left != right
            if isinstance(node.ops[0], ast.Eq):
                return left == right
        raise AssertionError(f"unexpected node {ast.dump(node)} in {expression}")

    return bool(walk(ast.parse(text.strip(), mode="eval")))


class ConditionCodeTest(unittest.TestCase):
    def test_every_code_matches_the_architectural_definition_on_all_32_words(self):
        self.assertEqual(sorted(REFERENCE), sorted(READS))
        for jcc, reference in REFERENCE.items():
            emitted = _bridge_condition(jcc, 0x1234)
            self.assertIsNotNone(emitted, jcc)
            body = re.fullmatch(
                r"RECOMP_BRIDGE_FLAGS\(_bridge_mask, 0x([0-9A-F]{3})u, (.*), "
                r'"(\w+)", 0x00001234u\)', emitted)
            self.assertIsNotNone(body, emitted)
            need, expression, name = int(body.group(1), 16), body.group(2), body.group(3)
            self.assertEqual(name, jcc)
            expected_need = 0
            for flag in READS[jcc].split():
                expected_need |= BITS[flag]
            self.assertEqual(need, expected_need, jcc)
            for values in itertools.product((False, True), repeat=5):
                flags = dict(zip(BITS, values))
                word = sum(BITS[n] for n, on in flags.items() if on)
                self.assertEqual(_evaluate(expression, word), bool(reference(flags)),
                                 f"{jcc} {flags}")

    def test_the_aliases_agree_with_their_canonical_form(self):
        for alias, canonical in (("jz", "je"), ("jnz", "jne"), ("jc", "jb"), ("jnc", "jae"),
                                 ("jna", "jbe"), ("jnbe", "ja"), ("jnae", "jb"),
                                 ("jnb", "jae"), ("jnge", "jl"), ("jnl", "jge"),
                                 ("jng", "jle"), ("jnle", "jg"), ("jpe", "jp"),
                                 ("jpo", "jnp")):
            a = _bridge_condition(alias, 1).replace(f'"{alias}"', '"x"')
            b = _bridge_condition(canonical, 1).replace(f'"{canonical}"', '"x"')
            self.assertEqual(a, b, alias)

    def test_an_unknown_code_is_not_invented(self):
        self.assertIsNone(_bridge_condition("jecxz", 0))
        self.assertIsNone(_make_condition("jecxz", BRIDGE_SETTER, []))

    def test_setcc_and_cmovcc_read_the_bridge_too(self):
        lifter = Lifter()
        for mnemonic, operands in (("sete", [Operand(type="reg", reg="al")]),
                                   ("cmove", [Operand(type="reg", reg="eax"),
                                              Operand(type="reg", reg="ecx")])):
            insns = [_insn(0x20, mnemonic, operands)]
            stmts, _ = lift_basic_block(
                lifter, BasicBlock(start=0x20, instructions=insns),
                flag_state=(BRIDGE_SETTER, []))
            text = "\n".join(stmts)
            self.assertIn('RECOMP_BRIDGE_FLAGS(_bridge_mask, 0x040u', text)
            self.assertIn("0x00000020u", text)
            self.assertNotIn("RECOMP_FLAGS_UNRESOLVED", text)


class EntryStateTest(unittest.TestCase):
    STATE = (BRIDGE_SETTER, [])

    def test_the_entry_block_gets_the_bridge_state(self):
        self.assertEqual(_incoming_flag_state(set(), {}, True, self.STATE), self.STATE)

    def test_a_back_edge_to_the_entry_withdraws_it(self):
        self.assertIsNone(_incoming_flag_state({0x10}, {0x10: ("cmp", [])}, True, self.STATE))

    def test_no_entry_state_is_the_old_behaviour(self):
        self.assertIsNone(_incoming_flag_state(set(), {}, True))
        self.assertIsNone(_incoming_flag_state(set(), {}, True, None))

    def test_a_later_block_is_unaffected(self):
        self.assertIsNone(_incoming_flag_state(set(), {}, False, self.STATE))
        known = {0x10: ("cmp", []), 0x20: ("cmp", [])}
        self.assertEqual(_incoming_flag_state({0x10, 0x20}, known, False, self.STATE),
                         ("cmp", []))


class ConfigTest(unittest.TestCase):
    def _load(self, tmp_text, db):
        import json
        import os
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "bridge.json")
            with open(path, "w", encoding="utf-8") as handle:
                handle.write(tmp_text)
            return _load_flag_bridge(path, db)

    def test_both_sides_load_as_address_sets(self):
        db = {0x10: {}, 0x20: {}, 0x30: {}}
        providers, consumers = self._load(
            '{"providers": ["0x10"], "consumers": ["0x20", 48]}', db)
        self.assertEqual(providers, frozenset({0x10}))
        self.assertEqual(consumers, frozenset({0x20, 0x30}))

    def test_an_empty_side_is_refused(self):
        with self.assertRaises(SystemExit) as raised:
            self._load('{"providers": [], "consumers": ["0x20"]}', {0x20: {}})
        self.assertIn("no providers", str(raised.exception))
        with self.assertRaises(SystemExit):
            self._load('["0x10"]', {0x10: {}})

    def test_an_address_that_is_not_a_function_is_refused(self):
        with self.assertRaises(SystemExit) as raised:
            self._load('{"providers": ["0x10"], "consumers": ["0x99"]}', {0x10: {}})
        self.assertIn("0x00000099", str(raised.exception))


if __name__ == "__main__":
    unittest.main()
