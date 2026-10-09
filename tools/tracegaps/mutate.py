# SPDX-License-Identifier: GPL-3.0-or-later
# ruff: noqa: E501
"""Mutation sweep for `tools.tracegaps`: apply one textual defect at a time, run the tests.

    python -m tools.tracegaps.mutate                 # logic mutants, synthetic tests only
    python -m tools.tracegaps.mutate --real          # also the image-bound mutants (needs the XBE)
    python -m tools.tracegaps.mutate --only flow-only-keeps-literal

Each mutation swaps one exact string. The `old` string must occur exactly once or the run
reports ANCHOR-DRIFT, because a drifted anchor that silently changes nothing looks exactly
like a mutant the tests killed. The package under test is COPIED into a scratch tree whose
`tools/` is symlinks to the real one except `tracegaps`, so the repository is never modified.

Two sets. LOGIC mutants live in code the synthetic tests exercise and are run with the
`real_image` tests deselected. REAL mutants live in the retail-specific tables and the
analysis that only the image reaches, and are run with ONLY the `real_image` tests (about
half a minute each); they are skipped, with a message, when the image is missing.

Outcomes: KILLED, SURVIVED (a test is missing), INVALID (did not import), ANCHOR-DRIFT. The
exit status is nonzero unless every selected mutation was KILLED.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
XBE = ROOT / "tmp/oxm-extract/retail/default.xbe"


@dataclass(frozen=True)
class Mutation:
    identifier: str
    file: str
    old: str
    new: str
    why: str
    real: bool = False
    #: The test file whose synthetic (or `real_image`) tests must kill the mutant.
    suite: str = "tests/test_tracegaps.py"


MUTATIONS = [
    # ---- value sets
    Mutation(
        "valueset-only-drops-literal-when-open",
        "flow.py",
        "if value in self.values or self.unbounded:",
        "if value in self.values:",
        "an unbounded value restricted to a literal comes out empty",
    ),
    Mutation(
        "valueset-without-keeps-value",
        "flow.py",
        "return ValueSet(self.values - {value}, self.unbounded)",
        "return ValueSet(self.values, self.unbounded)",
        "the not-equal edge no longer removes the literal",
    ),
    Mutation(
        "valueset-cap-off-by-one",
        "flow.py",
        "if len(merged) > MAX_VALUES:",
        "if len(merged) >= MAX_VALUES:",
        "a set of exactly the cap is called unbounded",
    ),
    Mutation(
        "valueset-cap-removed",
        "flow.py",
        "if len(merged) > MAX_VALUES:",
        "if False:",
        "a runaway set is never reported unbounded",
    ),
    Mutation(
        "valueset-map-unwrapped",
        "flow.py",
        "frozenset(function(v) & MASK32 for v in self.values)",
        "frozenset(function(v) for v in self.values)",
        "arithmetic escapes 32 bits",
    ),
    Mutation(
        "valueset-reasons-lost",
        "flow.py",
        "reasons = tuple(dict.fromkeys(self.unbounded + other.unbounded))",
        "reasons = self.unbounded",
        "an unbounded part of the right operand is dropped",
    ),
    Mutation(
        "substitute-ignores-entry",
        "flow.py",
        "and int(match.group(2), 16) == entry",
        "and True",
        "an argument of another function is replaced",
    ),
    Mutation(
        "substitute-ignores-number",
        "flow.py",
        "and int(match.group(1)) in arguments",
        "and True",
        "a missing argument raises or is replaced by the wrong one",
    ),
    # ---- resolver
    Mutation(
        "edge-equal-flipped",
        "flow.py",
        'equal_edge = (edge.kind == "taken") == (jcc.mnemonic == "je")',
        'equal_edge = (edge.kind == "taken") != (jcc.mnemonic == "je")',
        "the equal and not-equal edges swap",
    ),
    Mutation(
        "edge-test-constant",
        "flow.py",
        "            constant = 0\n",
        "            constant = 1\n",
        "`test r, r` is read as a compare with one",
    ),
    Mutation(
        "edge-byte-register-allowed",
        "flow.py",
        "            if first.size != 4:\n                return None\n",
        "",
        "a byte compare refines the whole register",
    ),
    Mutation(
        "edge-rewrite-between-ignored",
        "flow.py",
        'if kind == "reg" and self.writes_family(between, str(location)):',
        "if False:",
        "a register rewritten after the compare is still refined",
    ),
    Mutation(
        "edge-only-je-jne",
        "flow.py",
        'if jcc.mnemonic not in ("je", "jne") or capstone.CS_GRP_JUMP not in jcc.groups:',
        "if capstone.CS_GRP_JUMP not in jcc.groups:",
        "every conditional jump is read as an equality",
    ),
    Mutation(
        "sbb-idiom-requires-neg",
        "flow.py",
        'and previous.mnemonic == "neg"',
        "and True",
        "any sbb r, r is treated as the neg idiom",
    ),
    Mutation(
        "setcc-partial-removed",
        "flow.py",
        'mnemonic.startswith("set")',
        "False",
        "setcc is unbounded again",
    ),
    Mutation(
        "or-minus-one-removed",
        "flow.py",
        "and (ops[1].imm & MASK32) == MASK32",
        "and False",
        "or r, -1 depends on r again",
    ),
    Mutation(
        "and-zero-removed",
        "flow.py",
        "and (ops[1].imm & MASK32) == 0",
        "and False",
        "and r, 0 depends on r again",
    ),
    Mutation(
        "inc-dec-swapped",
        "flow.py",
        'inner.map(lambda v: v + 1 if mnemonic == "inc" else v - 1)',
        'inner.map(lambda v: v - 1 if mnemonic == "inc" else v + 1)',
        "inc and dec swap",
    ),
    Mutation(
        "shift-left-wrong-direction",
        "flow.py",
        '"shl": lambda a, b: a << (b & 31),',
        '"shl": lambda a, b: a >> (b & 31),',
        "shl shifts right",
    ),
    Mutation(
        "sub-reversed",
        "flow.py",
        '"sub": lambda a, b: a - b,',
        '"sub": lambda a, b: b - a,',
        "sub reverses its operands",
    ),
    Mutation(
        "lea-scale-ignored",
        "flow.py",
        "(a + b * scale) & MASK32",
        "(a + b) & MASK32",
        "lea ignores the index scale",
    ),
    Mutation(
        "call-clobbers-nothing",
        "flow.py",
        'if source.mnemonic == "call" and kind == "reg" and str(location) in VOLATILE:',
        "if False:",
        "a register survives a call",
    ),
    Mutation(
        "cycle-not-reported",
        "flow.py",
        'return open_set(f"loop-carried value of {location} at {insn.address:#x}")',
        "return ValueSet()",
        "a loop-carried value looks empty instead of unbounded",
    ),
    # ---- stack model and slots
    Mutation(
        "push-has-no-effect",
        "flow.py",
        '        if mnemonic == "push":\n            return -4\n',
        '        if mnemonic == "push":\n            return 0\n',
        "the stack depth ignores pushes",
    ),
    Mutation(
        "callee-pop-ignored",
        "flow.py",
        "return insn.operands[0].imm if insn.operands else 0",
        "return 0",
        "a stdcall callee's pop is not modelled",
    ),
    Mutation(
        "delta-conflict-unseen",
        "flow.py",
        "elif (\n                    self.delta[target] != after\n                    and after is not None\n                    and self.delta[target] is not None\n                ):",
        "elif False:",
        "two depths into one instruction go unreported",
    ),
    Mutation(
        "lost-delta-always-reported",
        "flow.py",
        "if after is None and not self._leads_to_return(address):",
        "if after is None:",
        "leave straight into ret is reported as a lost model",
    ),
    Mutation(
        "push-store-offset",
        "flow.py",
        "return None if delta is None else delta - 4",
        "return None if delta is None else delta",
        "a push is stored one slot off",
    ),
    Mutation(
        "address-taken-ignored",
        "flow.py",
        "if self.frame_offset(address, insn.operands[1]) == frame:",
        "if False:",
        "a slot whose address escaped is trusted",
    ),
    Mutation(
        "incoming-argument-number",
        "flow.py",
        'reason = f"incoming argument {location // 4} of {self.entry:#x}"',
        'reason = f"incoming argument {location // 4 + 1} of {self.entry:#x}"',
        "the argument number is off by one",
    ),
    # ---- switch tables
    Mutation(
        "table-ja-count",
        "flow.py",
        "return (right.imm & MASK32) + 1",
        "return right.imm & MASK32",
        "a ja-bounded table loses its last entry",
    ),
    Mutation(
        "table-jae-count",
        "flow.py",
        "return right.imm & MASK32\n",
        "return (right.imm & MASK32) + 1\n",
        "a jae-bounded table gains an entry",
    ),
    Mutation(
        "table-byte-count",
        "flow.py",
        "count = max(item[0] for item in raw if item is not None) + 1",
        "count = max(item[0] for item in raw if item is not None)",
        "an index-table switch loses its last arm",
    ),
    Mutation(
        "table-scale-unchecked",
        "flow.py",
        "if mem.base != 0 or mem.index == 0 or mem.scale != 4:",
        "if mem.base != 0 or mem.index == 0:",
        "a non-dword-indexed jump is read as a table",
    ),
    # ---- call arguments, entries, reachability
    Mutation(
        "call-arguments-cross-merge",
        "flow.py",
        "            if len(self.preds.get(cursor.address, [])) > 1:\n                break\n",
        "",
        "pushes from different paths are mixed",
    ),
    Mutation(
        "call-arguments-order",
        "flow.py",
        "values = [self.operand_value(push, push.operands[0]) for push in pushes]",
        "values = [self.operand_value(push, push.operands[0]) for push in reversed(pushes)]",
        "argument 1 and argument N swap",
    ),
    Mutation(
        "reaches-ignores-avoiding",
        "flow.py",
        "if successor not in seen and successor not in blocked:",
        "if successor not in seen:",
        "a dominating arm is not noticed",
    ),
    Mutation(
        "incoming-reads-include-before",
        "flow.py",
        "scope = {a for a in self.insns if a != before and self.reaches(a, before)}",
        "scope = {a for a in self.insns if self.reaches(a, before)}",
        "the join's own reads count as before it",
    ),
    Mutation(
        "entry-needs-padding-only",
        "code.py",
        "return insn.address in targets or self._looks_like_prologue(position)",
        "return insn.address in targets",
        "a function with no direct caller is missed",
    ),
    Mutation(
        "entry-nearest-not-first",
        "flow.py",
        "            flow = self.at_entry(candidate)\n            if address in flow.insns:\n                return flow\n",
        "            flow = self.at_entry(candidate)\n            if address in flow.insns and count >= 1:\n                return flow\n",
        "the nearest reaching entry is skipped",
    ),
    # ---- code queries
    Mutation(
        "callers-include-tail-jumps",
        "code.py",
        'return [t for t in self.transfers().get(target, []) if t.mnemonic == "call"]',
        "return list(self.transfers().get(target, []))",
        "tail jumps are counted as callers",
    ),
    Mutation(
        "tail-jumps-are-calls",
        "code.py",
        'return [t for t in self.transfers().get(target, []) if t.mnemonic == "jmp"]',
        'return [t for t in self.transfers().get(target, []) if t.mnemonic == "call"]',
        "tail jumps report calls",
    ),
    Mutation(
        "dword-hit-code-flag",
        "code.py",
        "hits.append((section.name, address, self._inside_instruction(address)))",
        "hits.append((section.name, address, False))",
        "a hit inside an instruction is reported as a stored pointer",
    ),
    Mutation(
        "covering-end-inclusive",
        "code.py",
        "return insn if insn.address <= address < insn.address + insn.size else None",
        "return insn if insn.address <= address <= insn.address + insn.size else None",
        "an address one past an instruction is inside it",
    ),
    # ---- emulation
    Mutation(
        "runner-never-stops",
        "emulate.py",
        "        uc.emu_stop()\n",
        "        pass\n",
        "the run continues past the stop",
    ),
    Mutation(
        "runner-registers-not-seeded",
        "emulate.py",
        "            uc.reg_write(_REGISTERS[name], value & 0xFFFFFFFF)",
        "            pass",
        "seeded registers are ignored",
    ),
    Mutation(
        "runner-memory-not-seeded",
        "emulate.py",
        '            uc.mem_write(address, (word & 0xFFFFFFFF).to_bytes(4, "little"))',
        "            pass",
        "seeded scratch memory is ignored",
    ),
    Mutation(
        "runner-arguments-order",
        "emulate.py",
        "frame = [SENTINEL, *[value & 0xFFFFFFFF for value in arguments]]",
        "frame = [SENTINEL, *[value & 0xFFFFFFFF for value in reversed(arguments)]]",
        "arguments are pushed in the wrong order",
    ),
    # ---- create event census
    Mutation(
        "ce-name-domain-nonzero",
        "createevent.py",
        "domain = sorted({0 if value == 0 else symbol(number) for value in domain})",
        "domain = sorted({0 for value in domain})",
        "a possibly non-NULL name is always run as NULL",
    ),
    Mutation(
        "ce-open-argument-only-symbol",
        "createevent.py",
        "domain |= {0, symbol(number)}",
        "domain |= {symbol(number)}",
        "an open argument is never run as zero",
    ),
    Mutation(
        "ce-kernel-word-offset",
        "createevent.py",
        "(label, capture.words[2 + position])",
        "(label, capture.words[1 + position])",
        "the kernel arguments are read one word early",
    ),
    Mutation(
        "ce-null-test",
        "createevent.py",
        'found.append(Outcome(combination, "NULL" if oa == 0 else "non-NULL", kernel))',
        'found.append(Outcome(combination, "non-NULL" if oa == 0 else "NULL", kernel))',
        "ObjectAttributes NULL and non-NULL swap",
    ),
    Mutation(
        "ce-thunk-ordinal-mask",
        "createevent.py",
        "return value & ~ORDINAL_FLAG",
        "return value",
        "the ordinal flag is left on",
    ),
    Mutation(
        "ce-slot-refs-partition",
        "createevent.py",
        "outside = [insn.address for insn in slot_refs if insn.address not in wrapper.insns]",
        "outside = []",
        "a second kernel caller goes unreported",
    ),
    Mutation(
        "ce-pointer-hits-all",
        "createevent.py",
        "pointer_hits=[(name, address) for name, address, in_code in hits if not in_code],",
        "pointer_hits=[],",
        "a stored wrapper address goes unreported",
    ),
    # ---- dispatch
    Mutation(
        "dispatch-chain-end",
        "vertexkeys.py",
        'address > start and insn.mnemonic == "call"',
        'address > start and insn.mnemonic == "ret"',
        "the else arm is not found",
    ),
    Mutation(
        "dispatch-arm-entries",
        "vertexkeys.py",
        'and flow.insns[a].mnemonic != "jmp"',
        'and flow.insns[a].mnemonic == "jmp"',
        "conditional arms are not found",
    ),
    Mutation(
        "dispatch-probe-short",
        "vertexkeys.py",
        "TYPE_PROBE = (*range(-2, 0x41), 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF)",
        "TYPE_PROBE = (0, 1)",
        "most types are never probed",
    ),
    Mutation(
        "dispatch-dominance",
        "vertexkeys.py",
        "if flow.reaches(stop, site) and not flow.reaches(flow.entry, site, avoiding={stop}):",
        "if flow.reaches(stop, site):",
        "a site reachable around its arm is attributed to it",
    ),
    # ---- image-bound mutants (real set)
    Mutation(
        "real-event-thunk-slot",
        "createevent.py",
        "thunk_slot=0x475898,",
        "thunk_slot=0x475899,",
        "the NtCreateEvent slot is wrong",
        real=True,
    ),
    Mutation(
        "real-event-entry",
        "createevent.py",
        "entry=0x37FF30,",
        "entry=0x37FF31,",
        "the wrapper entry is wrong",
        real=True,
    ),
    Mutation(
        "real-mutex-entry",
        "createevent.py",
        "entry=0x37FFB1,",
        "entry=0x37FFB2,",
        "the mutex wrapper entry is wrong",
        real=True,
    ),
    Mutation(
        "real-name-argument",
        "createevent.py",
        "name_argument=4,",
        "name_argument=1,",
        "the wrong argument is treated as the name",
        real=True,
    ),
    Mutation(
        "real-key-argument",
        "vertexkeys.py",
        "BATCHERS = (Batcher(0x18EA0, 4, 3), Batcher(0x18FF0, 3, 2))",
        "BATCHERS = (Batcher(0x18EA0, 4, 2), Batcher(0x18FF0, 3, 2))",
        "the batcher key is read from the wrong argument",
        real=True,
    ),
    Mutation(
        "real-second-batcher-argument",
        "vertexkeys.py",
        "Batcher(0x18FF0, 3, 2))",
        "Batcher(0x18FF0, 3, 3))",
        "0x18FF0's key is read from its count",
        real=True,
    ),
    Mutation(
        "real-record-pointer",
        "vertexkeys.py",
        "RECORD_POINTER = 0x55FB74",
        "RECORD_POINTER = 0x55FB78",
        "the recorders are found through another global",
        real=True,
    ),
    Mutation(
        "real-field-key",
        "vertexkeys.py",
        "FIELD_KEY = 0x08",
        "FIELD_KEY = 0x0C",
        "the key field is the flags field",
        real=True,
    ),
    Mutation(
        "real-field-type",
        "vertexkeys.py",
        "FIELD_TYPE = 0x14",
        "FIELD_TYPE = 0x18",
        "the type field is the stream field",
        real=True,
    ),
    Mutation(
        "real-join-ignores-function",
        "vertexkeys.py",
        "for i in code.absolute_operand_refs(RECORD_MODE) if i.address in flow.insns",
        "for i in code.absolute_operand_refs(RECORD_MODE) if True",
        "a record-mode reference in another function can become the key join",
        real=True,
    ),
    Mutation(
        "real-flow-check-instruction-count",
        "vertexkeys.py",
        "len(graph.insns),",
        "0,",
        "a function's graph size is no longer reported",
        real=True,
    ),
    Mutation(
        "real-exact-requires-bounded",
        "vertexkeys.py",
        "if all(value.bounded and value.values for value in needed):",
        "if False:",
        "no caller is run exactly",
        real=True,
    ),
    Mutation(
        "real-else-types-inverted",
        "vertexkeys.py",
        "frozenset(v for v in all_types.values if v not in explicit_union)",
        "frozenset(v for v in all_types.values if v in explicit_union)",
        "else-arm types are the arm types",
        real=True,
    ),
    Mutation(
        "real-replay-skip-not-applied",
        "vertexkeys.py",
        "n: wrapper.argument_union(n, skip=replay_only)",
        "n: wrapper.argument_union(n)",
        "the replay's open type argument leaks into the recorders' types",
        real=True,
    ),
    Mutation(
        "real-recorder-window",
        "vertexkeys.py",
        "        for _ in range(4):\n            if cursor is None:\n                break\n            cops = cursor.operands",
        "        for _ in range(1):\n            if cursor is None:\n                break\n            cops = cursor.operands",
        "stores a few instructions after the pointer load are missed",
        real=True,
    ),
    Mutation(
        "real-closure-no-feedback",
        "vertexkeys.py",
        "        if produced_keys <= keys.values and produced_flags <= flags.values:\n            break\n",
        "        break\n",
        "the replay's outputs are never fed back",
        real=True,
    ),
    Mutation(
        "real-mask-pixel",
        "vertexkeys.py",
        '"pixel": RETAIL.pixel_builder.mask}',
        '"pixel": 0xFFFFFFFF}',
        "the pixel mask is not applied to the keys",
        real=True,
    ),
    Mutation(
        "reach-callers-ignore-interior-jumps",
        "reach.py",
        "for address in sorted(self.body(callee) | {callee}):",
        "for address in [callee]:",
        "a jmp into the middle of a member no longer joins the closure",
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "reach-callers-only-entry-calls",
        "reach.py",
        "for transfer in transfers.get(address, []):",
        'for transfer in [t for t in transfers.get(address, []) if t.mnemonic == "call"]:',
        "tail jumps are not callers",
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "reach-taking-kinds-lose-data",
        "reach.py",
        'TAKING_KINDS = frozenset({"immediate", "displacement", "data", "table"})',
        'TAKING_KINDS = frozenset({"immediate", "displacement", "table"})',
        "a stored pointer no longer counts as taking the address",
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "reach-unaligned-data-is-a-pointer",
        "reach.py",
        'hits.append(Hit(section, location, "data" if aligned else "straddle", aligned))',
        'hits.append(Hit(section, location, "data", aligned))',
        "an unaligned run of bytes is called a pointer",
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "reach-branch-is-an-immediate",
        "reach.py",
        '                    kind = "branch"',
        '                    kind = "immediate"',
        "a relative branch distance counts as taking the address",
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "reach-pointer-run-never",
        "reach.py",
        "        if location % 4:\n            return False\n        image = self.code.image",
        "        return False\n        image = self.code.image",
        "a switch table decoded as code is dismissed as coincidence",
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "reach-resolution-ignored",
        "reach.py",
        "                    callees.update(resolutions[site.address])",
        "                    pass",
        "a resolved indirect site adds no target",
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "reach-switch-arms-dropped",
        "reach.py",
        "                    pending.extend(self._table_targets(insn))",
        "                    pass",
        "case arms of a switch are not part of the body",
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "reach-imports-are-blind",
        "reach.py",
        '                if site.kind == "import":',
        "                if False:",
        "a kernel import call is treated as an unresolved site",
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "reach-orphans-not-owned",
        "reach.py",
        "            self._orphans.add(insn.address)\n            for address in self.body(insn.address):\n                owners[address].append(insn.address)\n",
        "            self._orphans.add(insn.address)\n",
        "code no call target floods has no owner and its callers vanish",
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "barrier-overlap-ignored",
        "threadreach.py",
        "    overlap = sorted(set(direct.parent) & set(backward.members))",
        "    overlap: list[int] = []",
        "a thread that calls into the closure is called barred",
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "barrier-sink-needs-no-push",
        "threadreach.py",
        '    if insn is None or insn.mnemonic != "push":',
        "    if insn is None:",
        "a store of the address next to the wrapper call counts as passing it",
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "barrier-sink-ignores-unbounded",
        "threadreach.py",
        "                    and candidate.start.bounded\n",
        "",
        "an unbounded start routine value explains a taken address",
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "barrier-sink-any-start",
        "threadreach.py",
        "if candidate.start.values == frozenset({address})",
        "if candidate.start.values",
        "any wrapper site explains any taken address",
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "real-metadata-sections-empty",
        "reach.py",
        'METADATA_SECTIONS = frozenset({".XTLID"})',
        "METADATA_SECTIONS: frozenset[str] = frozenset()",
        "the library identification table counts as an address-taking pointer table",
        real=True,
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "real-verdict-ignores-sinks",
        "threadreach.py",
        "if self.getter.barred and self.spawner.barred and self.sinks_ok:",
        "if self.getter.barred and self.spawner.barred:",
        "the verdict no longer needs the pointer sinks to be the kernel and the trampoline",
        real=True,
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "real-getter-address",
        "threadreach.py",
        "GETTER = 0x22030",
        "GETTER = 0x22031",
        "the barrier is computed for a function that is not the getter",
        real=True,
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "real-start-argument-ignored",
        "threadreach.py",
        "    direct = graph.closure([start])",
        "    direct = graph.closure([START])",
        "the study of another start routine silently analyses the network polling thread",
        real=True,
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "real-start-address-taking-ignored",
        "threadreach.py",
        "graph.address_taking(graph.address_hits([start]).get(start, []))",
        "graph.address_taking(graph.address_hits([START]).get(START, []))",
        "the report names where the network thread's address is taken for every start routine",
        real=True,
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "real-guarded-words-not-scanned",
        "threadreach.py",
        "            if operand.type == capstone.x86.X86_OP_MEM and disp in found:",
        "            if operand.type == capstone.x86.X86_OP_MEM and disp in found and False:",
        "no reference to the counter or the worker's state words is ever reported",
        real=True,
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "real-wait-calls-not-read",
        "threadreach.py",
        '        if insn is None or insn.mnemonic != "call" or insn.op_str != hex(WAIT_WRAPPER):',
        "        if insn is None:",
        "every instruction of the start routine is read as the wait wrapper call",
        real=True,
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "real-spawner-skips-the-owner-start",
        "threadreach.py",
        "if site.start.values and site.start.values <= reader_starts",
        "if site.start.values and site.start.values <= {0x156CB0}",
        "the CRT start routine's creation site is not among the reader thread creators",
        real=True,
        suite="tests/test_tracegaps_reach.py",
    ),
    Mutation(
        "gateregion-zero-condition-prunes-nothing",
        "gateregion.py",
        '        return mnemonic in ("jne", "jnz")',
        "        return False",
        "the gate's flag test no longer prunes the early return",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-nonzero-condition-prunes-the-wrong-branch",
        "gateregion.py",
        '    return mnemonic in ("je", "jz")',
        '    return mnemonic in ("jne", "jnz")',
        "the running test prunes the branch it takes",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-counter-operand-ignored",
        "gateregion.py",
        "                if disp == COUNTER:\n                    region.counter_touches.append(address)",
        "                if False:\n                    region.counter_touches.append(address)",
        "a load of the vblank counter on the path is not noticed",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-counter-immediate-ignored",
        "gateregion.py",
        "if op.type == cs_x86.X86_OP_IMM and (op.imm & 0xFFFFFFFF) == COUNTER:",
        "if op.type == cs_x86.X86_OP_IMM and (op.imm & 0xFFFFFFFF) == COUNTER + 1:",
        "the address of the counter taken as an immediate is not noticed",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-computed-operand-ignored",
        "gateregion.py",
        "            else:\n                region.computed_operands.append(address)",
        "            else:\n                pass",
        "an access through a base register, which could alias the counter, is not reported",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-return-ignored",
        "gateregion.py",
        "            region.returns.append(address)",
        "            pass",
        "a path that returns to the caller without a call is not reported",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-indirect-call-ignored",
        "gateregion.py",
        '            else:\n                region.indirect.append(address)\n            continue\n        if mnemonic in ("ret"',
        '            else:\n                pass\n            continue\n        if mnemonic in ("ret"',
        "an indirect call on the path is not reported",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-walk-continues-past-a-call",
        "gateregion.py",
        '                region.indirect.append(address)\n            continue\n        if mnemonic in ("ret"',
        '                region.indirect.append(address)\n            stack.append((follow, frozenset(), None))\n            continue\n        if mnemonic in ("ret"',
        "the walk goes on after the first call, so a later call is attributed to the path",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-test-state-not-carried",
        "gateregion.py",
        "stack.append((follow, frozenset(next_loaded.items()), tested))",
        "stack.append((follow, frozenset(next_loaded.items()), None))",
        "the compare is forgotten before its branch",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-overwritten-register-keeps-the-flag",
        "gateregion.py",
        "                next_loaded.pop(register, None)",
        "                pass",
        "a register that was reloaded still counts as holding the flag",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-report-ignores-the-pruned-count",
        "gateregion.py",
        "            and len(region.pruned) == len(ENTRY_CONDITIONS)\n",
        "            and True\n",
        "a region with an unpruned entry condition counts as closed",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-report-ignores-returns",
        "gateregion.py",
        "            and not region.returns\n",
        "            and True\n",
        "a region that can return counts as closed",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-wrapper-ignores-unresolved-calls",
        "gateregion.py",
        "not self.wait_reaches_getter and self.wait_unresolved == 0 and self.wait_functions > 0",
        "not self.wait_reaches_getter and self.wait_functions > 0",
        "a wait wrapper with an unresolved indirect call counts as closed",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-counter-roles-are-a-subset",
        "gateregion.py",
        "        return bool(self.counter_refs) and {role for _, role in self.counter_refs} == {",
        "        return bool(self.counter_refs) and {role for _, role in self.counter_refs} <= {",
        "a counter with a reader but no writer named counts as fully accounted",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-isolation-ignores-overlap",
        "gateregion.py",
        "            and not any(self.thread_overlap.values())\n",
        "            and True\n",
        "another thread's closure inside the writers' callers does not break the isolation",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "index-scan-skips-unaligned-windows",
        "code.py",
        "            for offset in range(4):\n",
        "            for offset in range(1):\n",
        "the one-pass scan misses every hit that is not 4-aligned (straddles, overlapping windows)",
        suite="tests/test_tracegaps_index.py",
    ),
    Mutation(
        "index-scan-drops-the-last-dword",
        "code.py",
        "count = (len(body) - offset) // 4\n",
        "count = (len(body) - offset) // 4 - 1\n",
        "the last four bytes of a section are never tested",
        suite="tests/test_tracegaps_index.py",
    ),
    Mutation(
        "index-scan-hits-are-not-ordered",
        "code.py",
        "                for position in sorted(offsets):\n",
        "                for position in offsets:\n",
        "the hits of a value come out offset by offset instead of by address",
        suite="tests/test_tracegaps_index.py",
    ),
    Mutation(
        "index-scan-records-every-window",
        "code.py",
        "                    if word in wanted:\n",
        "                    if word:\n",
        "values nobody asked for are in the answer",
        suite="tests/test_tracegaps_index.py",
    ),
    Mutation(
        "index-scan-loses-the-inside-flag",
        "code.py",
        "                        (section.name, address, self._inside_instruction(address))\n",
        "                        (section.name, address, False)\n",
        "every hit reads as outside decoded code, so no immediate or displacement is classified",
        suite="tests/test_tracegaps_index.py",
    ),
    Mutation(
        "index-address-hits-asks-for-the-first-address-only",
        "reach.py",
        "self._classify_hits(wanted, self.code.dword_hits_many(wanted).get)",
        "self._classify_hits(wanted, self.code.dword_hits_many(wanted[:1]).get)",
        "the barrier finds the hits of one address and no other",
        suite="tests/test_tracegaps_index.py",
    ),
    Mutation(
        "index-address-hits-reorders-the-callers-addresses",
        "reach.py",
        "wanted = list(dict.fromkeys(addresses))",
        "wanted = sorted(set(addresses))",
        "the result no longer follows the order the caller asked in",
        suite="tests/test_tracegaps_index.py",
    ),
    Mutation(
        "gateregion-isolation-ignores-the-running-barrier",
        "gateregion.py",
        "            and self.running_barred\n",
        "            and True\n",
        "an open barrier over the running flag writers does not break the isolation",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-running-barrier-needs-the-starts-even-without-a-residue",
        "gateregion.py",
        "(not self.running_unexplained or self.starts_closed)",
        "(self.starts_closed)",
        "an empty residue of address-taking no longer bars the running flag writers by itself",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-running-barrier-ignores-unattributed",
        "gateregion.py",
        "            and not self.running_unattributed\n",
        "            and True\n",
        "a caller instruction no function owns does not open the barrier",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-running-barrier-bars-an-empty-closure",
        "gateregion.py",
        "and self.running_members > 0",
        "and self.running_members >= 0",
        "a callers closure that found nothing counts as barred",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-overlap-keeps-only-the-last-barrier",
        "gateregion.py",
        "        members |= set(item.backward.members)\n",
        "        members = set(item.backward.members)\n",
        "a thread inside the callers of the first flag writers is not reported",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-running-barrier-uses-the-gate-writers",
        "gateregion.py",
        "running = threadreach.barrier(graph, tuple(sorted(running_writers)), first, sites)",
        "running = threadreach.barrier(graph, tuple(sorted(writer_functions)), first, sites)",
        "the running flag barrier runs over the wrong writers",
        real=True,
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-running-taken-functions-not-counted",
        "gateregion.py",
        "running_taken_functions=len(classification.entries) + len(classification.interiors),",
        "running_taken_functions=0,",
        "the report hides how many functions of the title are address-taken",
        real=True,
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-barrier-ignores-the-residue-when-starts-close",
        "gateregion.py",
        "            and (not self.running_unexplained or self.starts_closed)\n",
        "            and True\n",
        "an address-taking residue no closed start excuses does not open the barrier",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-barrier-needs-no-closed-start-for-a-residue",
        "gateregion.py",
        "(not self.running_unexplained or self.starts_closed)",
        "(not self.running_unexplained or bool(self.starts))",
        "a start list that is not closed excuses the residue",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-starts-closed-needs-only-one-start",
        "gateregion.py",
        "return bool(self.starts) and all(verdict.closed for verdict in self.starts.values())",
        "return bool(self.starts) and any(verdict.closed for verdict in self.starts.values())",
        "one closed start excuses the others",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-starts-closed-accepts-no-start",
        "gateregion.py",
        "return bool(self.starts) and all(verdict.closed for verdict in self.starts.values())",
        "return all(verdict.closed for verdict in self.starts.values())",
        "an empty list of starts counts as closed",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-start-closes-with-unresolved-sites",
        "gateregion.py",
        "            and not self.unresolved_sites\n",
        "            and True\n",
        "a start with an unresolved indirect site counts as closed",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-start-closes-with-an-unexplained-callback",
        "gateregion.py",
        "            and not self.callback_imports\n",
        "            and True\n",
        "a callback service call with no proven null routine does not keep the start open",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-start-closes-with-a-writer",
        "gateregion.py",
        "            and not self.writers_reached\n",
        "            and True\n",
        "a start whose saturated closure holds a writer counts as closed",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-start-closes-when-empty",
        "gateregion.py",
        "            self.functions > 0\n",
        "            self.functions >= 0\n",
        "an empty closure counts as closed",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-writers-reached-not-reported",
        "gateregion.py",
        "writers_reached=tuple(saturated.reaches(writers)),",
        "writers_reached=(),",
        "a writer inside a saturated closure is not reported",
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-poll-claims-dropped",
        "gateregion.py",
        'claims = {"network poll": threadreach.poll_claims(graph)}',
        "claims = {}",
        "the T424 registrations no longer resolve the poll thread sites",
        real=True,
        suite="tests/test_gateregion.py",
    ),
    Mutation(
        "gateregion-residue-ignores-the-rules",
        "gateregion.py",
        "running_unexplained = collections.Counter(hit.kind for _, hit in classification.residue)",
        "running_unexplained = collections.Counter(hit.kind for _, hit in running.unexplained)",
        "the report prints the occurrences before the T603 rules as unexplained",
        real=True,
        suite="tests/test_gateregion.py",
    ),
]


# T603 mutants live in their own modules (tuples in the field order of `Mutation`).
from tools.tracegaps import (  # noqa: E402
    mutants_addresstaking,
    mutants_pools,
    mutants_saturate,
    mutants_vtable_pairing,
)

MUTATIONS += [
    Mutation(*fields)
    for fields in (
        *mutants_addresstaking.MUTANTS,
        *mutants_saturate.MUTANTS,
        *mutants_pools.MUTANTS,
        *mutants_vtable_pairing.MUTANTS,
    )
]


def run_one(mutation: Mutation, timeout: float) -> str:
    source = (ROOT / "tools/tracegaps" / mutation.file).read_text()
    if source.count(mutation.old) != 1:
        return f"ANCHOR-DRIFT ({source.count(mutation.old)} occurrences)"
    with tempfile.TemporaryDirectory(prefix="tracegaps-mutant-") as scratch_name:
        scratch = Path(scratch_name)
        (scratch / "tools").mkdir()
        for entry in (ROOT / "tools").iterdir():
            if entry.name not in ("tracegaps", "__pycache__"):
                (scratch / "tools" / entry.name).symlink_to(entry)
        shutil.copytree(
            ROOT / "tools/tracegaps",
            scratch / "tools/tracegaps",
            ignore=shutil.ignore_patterns("__pycache__"),
        )
        (scratch / "tools/tracegaps" / mutation.file).write_text(
            source.replace(mutation.old, mutation.new)
        )
        (scratch / "tests").mkdir()
        for name in {"tests/test_tracegaps.py", mutation.suite}:
            shutil.copy(ROOT / name, scratch / name)
        shutil.copy(ROOT / "pyproject.toml", scratch / "pyproject.toml")
        if mutation.real:
            (scratch / "tmp").mkdir()
            (scratch / "tmp/oxm-extract").symlink_to((ROOT / "tmp/oxm-extract").resolve())
        environment = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        environment.pop("PYTHONPATH", None)
        command = [
            sys.executable,
            "-m",
            "pytest",
            mutation.suite,
            "-q",
            "-x",
            "-k",
            "real_image" if mutation.real else "not real_image",
            "-p",
            "no:cacheprovider",
        ]
        try:
            result = subprocess.run(
                command,
                cwd=scratch,
                env=environment,
                capture_output=True,
                text=True,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            return "KILLED (timeout)"
    tail = result.stdout[-400:]
    if result.returncode == 0:
        return "SURVIVED"
    if "ImportError" in result.stdout or "SyntaxError" in result.stdout or "no tests ran" in tail:
        return "INVALID " + tail.strip().splitlines()[-1]
    return "KILLED"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--only", help="run one mutation by id")
    parser.add_argument("--real", action="store_true", help="include the image-bound mutants")
    parser.add_argument("--list", action="store_true", help="list ids and exit")
    parser.add_argument("--timeout", type=float, default=600.0, help="seconds per mutant")
    args = parser.parse_args(argv)
    if len({m.identifier for m in MUTATIONS}) != len(MUTATIONS):
        print("duplicate mutation ids", file=sys.stderr)
        return 2
    chosen = [
        m
        for m in MUTATIONS
        if args.only in (None, m.identifier) and (args.real or args.only or not m.real)
    ]
    if not chosen:
        print(f"no mutation named {args.only!r}", file=sys.stderr)
        return 2
    if any(m.real for m in chosen) and not XBE.exists():
        print(f"skipping image-bound mutants: no image at {XBE.relative_to(ROOT)}", file=sys.stderr)
        chosen = [m for m in chosen if not m.real]
        if not chosen:
            return 0
    if args.list:
        for mutation in chosen:
            print(mutation.identifier)
        return 0
    bad = 0
    for mutation in chosen:
        outcome = run_one(mutation, args.timeout)
        print(f"{outcome:<14} {mutation.identifier}: {mutation.why}", flush=True)
        bad += not outcome.startswith("KILLED")
    print(f"{len(chosen) - bad} of {len(chosen)} mutations killed")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
