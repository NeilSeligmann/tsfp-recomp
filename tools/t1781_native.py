# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent original/native controls and compiled source mutants for T1781 group A.

No proof tuple is executed. The frozen original-derived domain provides inputs, and
expected registers plus the whole changed guest memory come only from retail
instructions under Unicorn. Per root the controls are:

* up to 16 clean ordinary states (`derive(...).make_case(20261001, n)`),
* for roots with `rep movs/stos`, each ordinary state also at DF=0 and DF=1,
* up to 12 relocated-stack alias states (as T1782: the root's real reads/writes land on
  its argument, local and saved-register slots),
* explicit bulk saved-slot alias fixtures (table reused from
  `docs/data/t1781-synth/bulk_alias.py`), where a bulk store overwrites the PUSHed
  register before the POP (the T1479 POP-alias lesson).

Mutants are REAL textual mutations of the frozen draft's GAME_REPLACE_EXACT body
(never of the bridge), compiled with the same strict flags. A mutant that fails to
compile is recorded and the next rule is tried. A survivor is reported, never hidden.
Native execution runs in a forked child so a wild mutant cannot kill the tool.
"""

from __future__ import annotations

import argparse
import ast
import ctypes
import difflib
import hashlib
import json
import multiprocessing
import re
import struct
import subprocess
from collections.abc import Iterator
from dataclasses import replace
from multiprocessing.connection import Connection
from pathlib import Path
from typing import Any

import capstone

from tools.harness.image import GuestImage, build_guest_image
from tools.harness.model import REG_NAMES, Case, ExecResult
from tools.harness.oracle import UnicornOracle
from tools.harness.seeding import GUEST_LO, GUEST_SPAN, SENTINEL
from tools.harness.synth_domain import derive

DATA = Path("docs/data/t1781-synth")
BRIDGE = Path("tests/c/t1781/native_bridge.c")
WORK = Path("tmp/native-t1781")
SEED = 20261001
#: Roots with `rep movs/stos`: the direction flag matters, so each ordinary state runs at both.
REP_ROOTS = (0x38F30, 0x38F50, 0x55EC0, 0x69D40, 0xB0590, 0xC2E30, 0xD6920)
#: Bulk writers whose store can reach their own PUSHed register slot.
BULK_ROOTS = (0x38F30, 0x38F50, 0x69D40, 0xB0590, 0xC2E30, 0xD6920)
#: Root that calls into the lifted closure and cannot be linked standalone.
CALL_ROOTS = (0x70E40,)
CHILD_TIMEOUT = 180.0


def build(sources: list[Path], opt: int, dest: Path) -> Path:
    """Compile bridge + registry + sources with the strict flags into one shared object."""
    cmd = ["cc", "-std=c11", f"-O{opt}", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC"]
    cmd += ["-Isrc/game", str(BRIDGE), "src/game/game_registry.c"]
    for source in sources:
        cmd += ["-x", "c", str(source)]
    cmd += ["-o", str(dest)]
    subprocess.run(cmd, check=True, capture_output=True, timeout=180)
    return dest


def check(lib: ctypes.CDLL, image: bytes, case: Case, result: ExecResult) -> list[str]:
    """Run one native case and compare all 8 GPRs and the whole guest memory."""
    ptr = lib.native_memory()
    initial = bytearray(image)
    for address, data in case.patches:
        initial[address - GUEST_LO : address - GUEST_LO + len(data)] = data
    initial_bytes = bytes(initial)
    ctypes.memmove(ptr + GUEST_LO, initial_bytes, len(initial_bytes))
    regs = (ctypes.c_uint32 * 8)(*case.regs)
    if lib.native_execute(case.va, regs, case.df) != 0:
        return ["native adapter not registered"]
    problems = []
    if tuple(regs) != result.regs:
        problems.append(f"registers native={list(regs)} original={list(result.regs)}")
    for address, value in result.writes.items():
        initial[address - GUEST_LO] = value
    actual = ctypes.string_at(ptr + GUEST_LO, GUEST_SPAN)
    if actual != bytes(initial):
        offsets = [
            i + GUEST_LO for i, (a, b) in enumerate(zip(actual, initial, strict=True)) if a != b
        ]
        problems.append(f"memory {len(offsets)} different bytes; first={offsets[:12]}")
    return problems


Test = tuple[Case, ExecResult, str]


def _child(conn: Connection, lib_path: str, image: bytes, tests: list[Test], stop: bool) -> None:
    lib = ctypes.CDLL(str(Path(lib_path).resolve()))
    lib.native_memory.restype = ctypes.c_void_p
    lib.native_execute.argtypes = [ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.c_int]
    for position, (case, result, _kind) in enumerate(tests):
        problems = check(lib, image, case, result)
        conn.send((position, problems))
        if problems and stop:
            break
    conn.send(None)
    conn.close()


def run_checks(
    lib_path: Path, image: bytes, tests: list[Test], *, stop: bool = False
) -> tuple[dict[int, list[str]], str | None]:
    """Run `tests` in a forked child. Returns per-test problems and a crash/hang note."""
    context = multiprocessing.get_context("fork")
    reader, writer = context.Pipe(duplex=False)
    process = context.Process(target=_child, args=(writer, str(lib_path), image, tests, stop))
    process.start()
    writer.close()
    found: dict[int, list[str]] = {}
    note = None
    while True:
        if not reader.poll(CHILD_TIMEOUT):
            note = "native hang (timeout)"
            process.kill()
            break
        try:
            message = reader.recv()
        except EOFError:
            process.join(10)
            note = f"native crash exitcode={process.exitcode}"
            break
        if message is None:
            break
        found[message[0]] = message[1]
    process.join(10)
    if process.is_alive():
        process.kill()
        process.join(10)
    reader.close()
    return found, note


def saved_slots(image: GuestImage, case: Case, result: ExecResult) -> list[dict[str, Any]]:
    """Metadata for each executed `push reg`: entry value, final slot value, overwrite, loads."""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    offset = 0
    slots = []
    for insn in md.disasm(image.code_at(case.va, case.size), case.va):
        if insn.address not in result.reach.covered_vas:
            continue
        if insn.mnemonic == "sub" and insn.op_str.startswith("esp, "):
            offset -= int(insn.op_str.split(", ")[1], 0)
        if insn.mnemonic == "add" and insn.op_str.startswith("esp, "):
            offset += int(insn.op_str.split(", ")[1], 0)
        if insn.mnemonic == "push":
            offset -= 4
            reg = insn.op_str
            if reg in REG_NAMES:
                address = case.esp + offset
                value = case.regs[REG_NAMES.index(reg)]
                initial = bytearray(image.data)
                for a, data in case.patches:
                    initial[a - GUEST_LO : a - GUEST_LO + len(data)] = data
                final = bytes(
                    result.writes.get(address + i, initial[address + i - GUEST_LO])
                    for i in range(4)
                )
                slot_value = int.from_bytes(final, "little")
                slots.append(
                    dict(
                        reg=reg,
                        address=hex(address),
                        entry=value,
                        final=slot_value,
                        final_slot=slot_value,
                        exit=result.regs[REG_NAMES.index(reg)],
                        overwritten=slot_value != value,
                        saved_slot_load_count=sum(
                            a <= address + 3 and address < a + n for a, n, _ in result.reach.loads
                        ),
                    )
                )
        if insn.mnemonic == "pop":
            offset += 4
        if insn.mnemonic == "ret":
            break
    return slots


def bulk_fixtures() -> list[tuple[int, int, list[int], list[tuple[int, int]], Any]]:
    """The fixture table of bulk_alias.py, read from its source (single point of truth)."""
    tree = ast.parse((DATA / "bulk_alias.py").read_text())
    for node in ast.walk(tree):
        if (
            isinstance(node, ast.Assign)
            and isinstance(node.targets[0], ast.Name)
            and node.targets[0].id == "fixtures"
        ):
            return ast.literal_eval(node.value)
    raise RuntimeError("fixtures table not found in bulk_alias.py")


def bulk_case(row: dict[str, Any], fixture: tuple[Any, ...]) -> Case:
    va, stack, args, stores, fill = fixture
    patches = [(address, struct.pack("<I", value)) for address, value in stores]
    if fill is not None:
        patches.append((fill[0], b"\xab\xcd\xef\x12" * (fill[1] // 4)))
    frame = struct.pack("<I", SENTINEL) + b"".join(struct.pack("<I", a) for a in args)
    patches.append((stack, frame))
    regs = (0xAABBCCDD, 0x55667788, 0x12345678, 0x11223344, stack)
    regs += (0x22334455, 0x33445566, 0x44556677)
    if va == 0x69D40:
        # Same valid +0x3C alias as bulk_alias.py (the sentinel trial faulted at 0x69D77).
        regs = regs[:-1] + (0x910000,)
    return Case(SEED, 0, va, row["size"], regs, 0, tuple(patches))


def controls(
    image: GuestImage, oracle: UnicornOracle, row: dict[str, Any]
) -> tuple[list[Test], list[str]]:
    """All control states for one root, plus notes about states that could not be made."""
    va = int(row["va"], 16)
    notes: list[str] = []
    domain = derive(va, row["size"], image)
    ordinary: list[Test] = []
    for ordinal in range(domain.case_count()):
        case = domain.make_case(SEED, ordinal)
        result = oracle.run(case)
        if not result.faulted:
            ordinary.append((case, result, "ordinary"))
        if len(ordinary) == 16:
            break
    if not ordinary:
        return [], [f"{va:x}: no clean ordinary controls"]
    tests = list(ordinary)
    if va in REP_ROOTS:
        skipped = 0
        for case, _, _ in ordinary:
            for df in (0, 1):
                if df == case.df:
                    continue
                variant = replace(case, df=df)
                result = oracle.run(variant)
                if result.faulted:
                    skipped += 1
                    continue
                tests.append((variant, result, f"ordinary-df{df}"))
        if skipped:
            notes.append(f"{skipped} DF variants faulted in the original and were skipped")
    aliases: list[Test] = []
    seen = set()
    for case, result, _ in ordinary:
        candidates = sorted(set(result.writes) | {a for a, _, _ in result.reach.loads})
        candidates = [
            a for a in candidates if abs(a - case.esp) > 0x200 and GUEST_LO + 0x100 <= a < 0xF00000
        ]
        writes = sorted(a for a in result.writes if abs(a - case.esp) > 0x200)
        ends = [a for i, a in enumerate(writes) if i == len(writes) - 1 or writes[i + 1] != a + 1]
        if len(candidates) > 40:
            candidates = candidates[:20] + candidates[-20:]
        candidates = list(dict.fromkeys(ends + candidates))
        for address in candidates:
            for delta in (-4, 4, 8, 12, 16, 20):
                stack = (address + delta) & ~3
                if stack in seen:
                    continue
                seen.add(stack)
                # Copy the root's actual arguments into the relocated frame. Patch last
                # so the domain prefill cannot erase the return slot.
                initial = bytearray(image.data)
                for a, data in case.patches:
                    initial[a - GUEST_LO : a - GUEST_LO + len(data)] = data
                count = max(row["abi"]["stack_args"], 16)
                args = bytes(initial[case.esp + 4 - GUEST_LO : case.esp + 4 + 4 * count - GUEST_LO])
                regs = list(case.regs)
                regs[4] = stack
                frame = struct.pack("<I", SENTINEL) + args
                aliased = replace(case, regs=tuple(regs), patches=case.patches + ((stack, frame),))
                original = oracle.run(aliased)
                if original.faulted or original.regs[4] != stack + 4:
                    continue
                aliases.append((aliased, original, "relocated-stack-alias"))
                if len(aliases) >= 48:
                    break
            if len(aliases) >= 48:
                break
        if len(aliases) >= 48:
            break
    if not aliases:
        notes.append("no valid relocated-stack alias state")
    aliases.sort(
        key=lambda item: sum(slot["overwritten"] for slot in saved_slots(image, item[0], item[1])),
        reverse=True,
    )
    tests += aliases[:12]
    for fixture in bulk_fixtures():
        if fixture[0] != va:
            continue
        case = bulk_case(row, fixture)
        result = oracle.run(case)
        if result.faulted or result.regs[4] != case.regs[4] + 4:
            notes.append(f"bulk fixture faulted or did not return: {result.fault}")
            continue
        tests.append((case, result, "bulk-saved-slot-alias"))
    return tests, notes


# ---- source mutation -------------------------------------------------------------------

ADDRESS_CALL = re.compile(
    r"\b(?:guest_read32|guest_write32|synth_read(?:8|16|32)|synth_write\d+|synth_push)\s*\("
)
HEX_LITERAL = re.compile(r"\b0[xX][0-9A-Fa-f]+u\b")
DEC_LITERAL = re.compile(r"(?<![\w.])\d+u\b")
COMPARISON = re.compile(r"(?<![<>\-=!])(>=|<=|>|<)(?![<>=])")
BITOP = re.compile(r"(?<![|&])([|&])(=?)(?![|&])")
EAX_FLIP = re.compile(r"\bg_eax = ([^;]*);")
REG_FLIP = re.compile(r"\bg_(?:eax|ecx|edx|ebx|esi|edi|ebp) = ([^;]*);")


def call_arguments(body: str) -> Iterator[tuple[int, int]]:
    """Spans (start, end) of the balanced argument list of each address-bearing call."""
    for match in ADDRESS_CALL.finditer(body):
        depth, position = 1, match.end()
        while position < len(body) and depth:
            depth += {"(": 1, ")": -1}.get(body[position], 0)
            position += 1
        yield match.end(), position - 1


def rule_hex_displacement(body: str) -> str | None:
    for start, end in call_arguments(body):
        literal = HEX_LITERAL.search(body, start, end)
        if literal:
            text = literal.group()
            digits = text[2:-1]
            value = f"{int(digits, 16) + 4:0{len(digits)}X}"
            return body[: literal.start()] + f"0x{value}u" + body[literal.end() :]
    return None


def rule_decimal_displacement(body: str) -> str | None:
    for start, end in call_arguments(body):
        literal = DEC_LITERAL.search(body, start, end)
        if literal:
            value = int(literal.group()[:-1]) + 4
            return body[: literal.start()] + f"{value}u" + body[literal.end() :]
    return None


def rule_comparison(body: str) -> str | None:
    flipped = {">=": "<", "<=": ">", ">": "<=", "<": ">="}
    for match in COMPARISON.finditer(body):
        return body[: match.start()] + flipped[match.group()] + body[match.end() :]
    return None


def rule_bitop(body: str) -> str | None:
    for match in BITOP.finditer(body):
        before = body[: match.start()].rstrip()
        if not before or not (before[-1].isalnum() or before[-1] in ")_]"):
            continue  # unary address-of
        swap = "&" if match.group(1) == "|" else "|"
        return body[: match.start()] + swap + match.group(2) + body[match.end() :]
    return None


def _flip_last(pattern: re.Pattern[str], body: str) -> str | None:
    matches = list(pattern.finditer(body))
    if not matches:
        return None
    last = matches[-1]
    expression = last.group(1)
    start = last.start(1)
    return body[:start] + f"({expression}) ^ 1u" + body[last.end(1) :]


def rule_eax_xor(body: str) -> str | None:
    return _flip_last(EAX_FLIP, body)


def rule_register_xor(body: str) -> str | None:
    return _flip_last(REG_FLIP, body)


RULES = (
    ("hex-displacement+4", rule_hex_displacement),
    ("flip-comparison", rule_comparison),
    ("flip-bitop", rule_bitop),
    ("final-eax-xor-1", rule_eax_xor),
    # Not in the owner priority list: only reached when all four fail or survive.
    ("decimal-displacement+4", rule_decimal_displacement),
    ("final-register-xor-1", rule_register_xor),
)


def mutants(source: str) -> Iterator[tuple[str, str, str]]:
    """(rule, mutated source, unified diff) for each applicable rule, in priority order.

    Only the text from GAME_REPLACE_EXACT onward is mutated: the shared helpers are not.
    """
    marker = source.index("GAME_REPLACE_EXACT(")
    head, body = source[:marker], source[marker:]
    for name, rule in RULES:
        mutated = rule(body)
        if mutated is None or mutated == body:
            continue
        diff = "".join(
            difflib.unified_diff(
                body.splitlines(keepends=True),
                mutated.splitlines(keepends=True),
                "draft",
                "mutant",
                n=0,
            )
        )
        yield name, head + mutated, diff


def mutant_receipt(
    va: int, source: Path, opt: int, image: bytes, tests: list[Test]
) -> dict[str, Any]:
    """Compile mutants rule by rule. The first compiled mutant is the primary one."""
    attempts: list[dict[str, Any]] = []
    for name, text, diff in mutants(source.read_text()):
        path = WORK / f"mutant-{va:08x}-{name}.c"
        path.write_text(text)
        record: dict[str, Any] = {"rule": name, "diff": diff}
        try:
            lib = build([path], opt, WORK / f"mutant-{va:08x}-o{opt}-{name}.so")
        except subprocess.CalledProcessError as error:
            record["compiled"] = False
            record["compile_error"] = error.stderr.decode(errors="replace")[:400]
            attempts.append(record)
            continue
        found, note = run_checks(lib, image, tests, stop=True)
        failing = {k: v for k, v in found.items() if v}
        record["compiled"] = True
        record["killed"] = bool(failing) or note is not None
        record["kill_note"] = note
        record["first_kill"] = next(iter(failing.items()), None)
        attempts.append(record)
        if record["killed"]:
            break
    compiled = [a for a in attempts if a["compiled"]]
    return {
        "attempts": attempts,
        "primary": compiled[0] if compiled else None,
        "primary_killed": bool(compiled) and compiled[0]["killed"],
        "killed_by_any": any(a["killed"] for a in compiled),
    }


# ---- production test emitter -----------------------------------------------------------

#: Common C support, emitted once per translation unit (guarded so several labels can be
#: #included into one test binary). Memory model = the native bridge: a zeroed 16 MiB guest
#: window holds only the embedded initial bytes; every written byte is checked against the
#: original, and any byte that is not in the expected write set must be unchanged.
C_COMMON = r"""#ifndef T1781_COMMON_DEFINED
#define T1781_COMMON_DEFINED
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef T1781_EMBEDDED
__thread uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi, g_fs_base;
__thread int g_df;
ptrdiff_t g_xbox_mem_offset;
static unsigned checks, failures;
extern game_replacement __start_game_replacements[] __attribute__((weak));
extern game_replacement __stop_game_replacements[] __attribute__((weak));
const game_replacement *game_replacement_table(size_t *count)
{
    if (__start_game_replacements == NULL || __stop_game_replacements == NULL) {
        *count = 0u;
        return NULL;
    }
    *count = (size_t)(__stop_game_replacements - __start_game_replacements);
    return __start_game_replacements;
}
#endif
#define T1781_SPAN 0x01010000u
typedef struct t1781_state {
    const char *kind;
    int df;
    uint32_t regs[8];   /* eax ecx edx ebx esp ebp esi edi at entry */
    uint32_t expect[8]; /* the same registers after the original returned */
    const char *init;   /* records: initial bytes (retail data read or seeded by the state) */
    const char *writes; /* records: bytes the original changed, with their final values */
} t1781_state;
static unsigned char t1781_mem[T1781_SPAN];
static unsigned char t1781_want[T1781_SPAN];
static const char t1781_names[8][4] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};

static unsigned t1781_nibble(char digit)
{
    return (unsigned)(digit <= '9' ? digit - '0' : digit - 'a' + 10);
}

static uint32_t t1781_byte(const char **cursor)
{
    uint32_t value = (t1781_nibble((*cursor)[0]) << 4) | t1781_nibble((*cursor)[1]);
    *cursor += 2;
    return value;
}

static uint32_t t1781_word(const char **cursor)
{
    uint32_t value = 0u;
    for (unsigned shift = 0u; shift < 32u; shift += 8u) value |= t1781_byte(cursor) << shift;
    return value;
}

/* Record: addr(4) len(4, bit 31 = fill) then one byte (fill) or len bytes. Little endian.
 * clear=0 stores into the selected buffers, clear=1 zeroes the same spans again. */
static void t1781_apply(const char *cursor, int to_mem, int to_want, int clear)
{
    while (*cursor != '\0') {
        uint32_t address = t1781_word(&cursor);
        uint32_t word = t1781_word(&cursor);
        uint32_t length = word & 0x7FFFFFFFu;
        int fill = (word & 0x80000000u) != 0u;
        uint32_t value = fill ? t1781_byte(&cursor) : 0u;
        if (address + length > T1781_SPAN) abort();
        for (uint32_t i = 0u; i < length; ++i) {
            unsigned char byte = clear ? 0u : (unsigned char)(fill ? value : t1781_byte(&cursor));
            if (clear && !fill) cursor += 2;
            if (to_mem) t1781_mem[address + i] = byte;
            if (to_want) t1781_want[address + i] = byte;
        }
    }
}

static void t1781_run(uint32_t va, const t1781_state *states, size_t count)
{
    size_t entries = 0u;
    const game_replacement *table = game_replacement_table(&entries);
    const game_replacement *entry = NULL;
    for (size_t i = 0u; i < entries; ++i) if (table[i].va == va) entry = &table[i];
    ++checks;
    if (entry == NULL) {
        ++failures;
        fprintf(stderr, "T1781 %08X: not registered\n", (unsigned)va);
        return;
    }
    for (size_t n = 0u; n < count; ++n) {
        const t1781_state *state = &states[n];
        int ok = 1;
        t1781_apply(state->init, 1, 1, 0);
        t1781_apply(state->writes, 0, 1, 0);
        g_eax = state->regs[0]; g_ecx = state->regs[1]; g_edx = state->regs[2];
        g_ebx = state->regs[3]; g_esp = state->regs[4]; g_ebp = state->regs[5];
        g_esi = state->regs[6]; g_edi = state->regs[7]; g_df = state->df;
        entry->adapter();
        const uint32_t got[8] = {g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi};
        for (unsigned r = 0u; r < 8u; ++r) {
            ++checks;
            if (got[r] != state->expect[r]) {
                ok = 0;
                ++failures;
                fprintf(stderr, "T1781 %08X state %zu (%s): %s %08X != %08X\n", (unsigned)va, n,
                        state->kind, t1781_names[r], (unsigned)got[r], (unsigned)state->expect[r]);
            }
        }
        ++checks;
        if (memcmp(t1781_mem, t1781_want, T1781_SPAN) != 0) {
            unsigned long different = 0ul, first = 0ul;
            for (unsigned long i = 0ul; i < T1781_SPAN; ++i) {
                if (t1781_mem[i] != t1781_want[i] && different++ == 0ul) first = i;
            }
            ok = 0;
            ++failures;
            fprintf(stderr, "T1781 %08X state %zu (%s): %lu bytes differ, first at %08lX "
                    "(%02X != %02X)\n", (unsigned)va, n, state->kind, different, first,
                    (unsigned)t1781_mem[first], (unsigned)t1781_want[first]);
        }
        if (ok) {
            t1781_apply(state->init, 1, 1, 1);
            t1781_apply(state->writes, 1, 1, 1);
        } else {
            memset(t1781_mem, 0, sizeof t1781_mem);
            memset(t1781_want, 0, sizeof t1781_want);
        }
    }
}
#endif
"""

FILL_RUN = 12


class StoreOracle(UnicornOracle):
    """Oracle that also remembers every store, including same-value ones.

    `ExecResult.writes` omits a store of the value already present. The embedded test only
    carries the bytes a state touched, so a replacement that makes the same store would
    look like a change from 0. Seeding the stored-to bytes with their retail value removes
    that false alarm. Behaviour (results, loads, writes) is identical to the base oracle.
    """

    def __init__(self, *args: Any, **kwargs: Any) -> None:
        self._stores: list[tuple[int, int]] = []
        self._kept: dict[int, tuple[ExecResult, tuple[tuple[int, int], ...]]] = {}
        super().__init__(*args, **kwargs)

    def _on_write(
        self, uc: Any, access: int, address: int, size: int, value: int, data: Any
    ) -> None:
        self._stores.append((address & 0xFFFFFFFF, size))
        super()._on_write(uc, access, address, size, value, data)

    def run(self, case: Case, *args: Any, **kwargs: Any) -> ExecResult:
        self._stores = []
        result = super().run(case, *args, **kwargs)
        self._kept[id(result)] = (result, tuple(self._stores))
        return result

    def stores_of(self, result: ExecResult) -> tuple[tuple[int, int], ...]:
        return self._kept[id(result)][1]


def encode_records(spans: list[tuple[int, bytes]]) -> str:
    """Hex text of `addr(4) len(4, bit 31 = fill) payload` records, runs of >=12 as fills."""
    out = bytearray()
    for address, data in spans:
        position = 0
        literal_start = 0

        def flush(end: int, base: int = address, data: bytes = data) -> None:
            nonlocal literal_start
            if end > literal_start:
                out.extend(struct.pack("<II", base + literal_start, end - literal_start))
                out.extend(data[literal_start:end])
            literal_start = end

        while position < len(data):
            run = position
            while run < len(data) and data[run] == data[position]:
                run += 1
            if run - position >= FILL_RUN:
                flush(position)
                out.extend(struct.pack("<II", address + position, (run - position) | 0x80000000))
                out.append(data[position])
                literal_start = run
            position = run
        flush(len(data))
    return out.hex()


def initial_spans(
    image: bytes, case: Case, result: ExecResult, stores: tuple[tuple[int, int], ...]
) -> list[tuple[int, bytes]]:
    """Initial bytes the state seeds, the original read or stored to, merged into spans."""
    initial = bytearray(image)
    intervals = []
    for address, data in case.patches:
        initial[address - GUEST_LO : address - GUEST_LO + len(data)] = data
        intervals.append((address, address + len(data)))
    intervals += [(a, a + n) for a, n, _ in result.reach.loads]
    intervals += [(a, a + n) for a, n in stores]
    merged: list[list[int]] = []
    for start, end in sorted(intervals):
        start, end = max(start, GUEST_LO), min(end, GUEST_LO + GUEST_SPAN)
        if start >= end:
            continue
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return [(a, bytes(initial[a - GUEST_LO : b - GUEST_LO])) for a, b in merged]


def write_spans(result: ExecResult) -> list[tuple[int, bytes]]:
    spans: list[tuple[int, bytearray]] = []
    for address in sorted(result.writes):
        if spans and spans[-1][0] + len(spans[-1][1]) == address:
            spans[-1][1].append(result.writes[address])
        else:
            spans.append((address, bytearray([result.writes[address]])))
    return [(a, bytes(d)) for a, d in spans]


def c_string(text: str, width: int = 100) -> str:
    chunks = [text[i : i + width] for i in range(0, len(text), width)] or [""]
    return "\n        ".join(f'"{chunk}"' for chunk in chunks)


def emit_state(image: bytes, test: Test, stores: tuple[tuple[int, int], ...]) -> str:
    case, result, kind = test
    regs = ", ".join(f"0x{r:08X}u" for r in case.regs)
    expect = ", ".join(f"0x{r:08X}u" for r in result.regs)
    init = c_string(encode_records(initial_spans(image, case, result, stores)))
    writes = c_string(encode_records(write_spans(result)))
    return (
        f'    {{"{kind}", {case.df}, {{{regs}}},\n     {{{expect}}},\n'
        f"     {init},\n        {writes}}}"
    )


def emit_test(
    label: str, rows: list[dict[str, Any]], image: GuestImage, oracle: StoreOracle, dest: Path
) -> dict[str, Any]:
    """Write the production test for `rows` and return a summary of what it embeds."""
    if not re.fullmatch(r"[A-Za-z0-9_]+", label):
        raise SystemExit(f"--label must be [A-Za-z0-9_]+: {label!r}")
    body: list[str] = []
    summary: dict[str, Any] = {}
    calls = []
    xbe = hashlib.sha256(Path("build/default.xbe").read_bytes()).hexdigest()
    for row in rows:
        va = int(row["va"], 16)
        tests, notes = controls(image, oracle, row)
        if not tests:
            raise SystemExit(f"{va:08X}: no controls ({notes})")
        name = f"t1781_{label}_{va:08X}"
        states = ",\n".join(
            emit_state(image.data, test, oracle.stores_of(test[1])) for test in tests
        )
        body.append(f"static const t1781_state {name}[] = {{\n{states}\n}};")
        calls.append(f"    t1781_run(0x{va:08X}u, {name}, sizeof {name} / sizeof {name}[0]);")
        summary[f"{va:08X}"] = {
            "states": len(tests),
            "kinds": {k: sum(t[2] == k for t in tests) for k in sorted({t[2] for t in tests})},
            "notes": notes,
        }
    vas = ", ".join(f"0x{int(r['va'], 16):08X}" for r in rows)
    text = f"""/* SPDX-License-Identifier: GPL-3.0-or-later
 * T1781 native controls for {len(rows)} synthesized EXACT roots (label {label}).
 * Roots: {vas}.
 * Generated by `tools/t1781_native.py --emit-test`, do not edit. Initial registers, memory,
 * expected registers and expected changed bytes come ONLY from the retail instructions
 * under Unicorn (seed {SEED}, 16 ordinary states per root, DF variants for rep roots,
 * relocated-stack alias states and bulk saved-slot alias fixtures). Source xbe sha256
 * {xbe}. Initial bytes are the seeded patches plus the retail bytes the root read or stored to.
 * Each state checks all 8 registers, every written byte, and that all other bytes are
 * unchanged. Build standalone, or define T1781_EMBEDDED and call test_t1781_{label}().
 */
#include "game_replace.h"
{C_COMMON}
{chr(10).join(body)}

void test_t1781_{label}(void)
{{
    const ptrdiff_t saved_offset = g_xbox_mem_offset;
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)t1781_mem;
{chr(10).join(calls)}
    g_xbox_mem_offset = saved_offset;
}}
#ifndef T1781_EMBEDDED
int main(void)
{{
    test_t1781_{label}();
    printf("%u checks %u failures\\n", checks, failures);
    return failures != 0u ? 1 : 0;
}}
#endif
"""
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_text(text)
    return summary


# ---- driver ----------------------------------------------------------------------------


def parse_vas(path: Path) -> list[int]:
    text = path.read_text()
    try:
        items = json.loads(text)
        if isinstance(items, dict):
            items = list(items)
    except json.JSONDecodeError:
        items = [w for line in text.splitlines() for w in line.split("#")[0].split()]
    return [int(str(item), 16) for item in items]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--va", action="append", default=[], help="root VA (hex), repeatable")
    parser.add_argument("--admitted", type=Path, help="file listing root VAs (text or JSON)")
    parser.add_argument("--opt", action="append", type=int, help="-O level, repeatable")
    parser.add_argument("--mutants", action="store_true", help="also compile and run mutants")
    parser.add_argument("--out", type=Path, help="JSON receipt path (required unless --emit-test)")
    parser.add_argument(
        "--emit-test", type=Path, help="write the embedded production C test for the roots here"
    )
    parser.add_argument("--label", help="test label for --emit-test, e.g. list1475_001")
    args = parser.parse_args()
    if args.emit_test is None and args.out is None:
        parser.error("--out is required unless --emit-test is given")
    if args.emit_test is not None and not args.label:
        parser.error("--emit-test needs --label")
    rows = json.loads((DATA / "candidate-inputs.json").read_text())
    if args.admitted:
        wanted = parse_vas(args.admitted)
    elif args.va:
        wanted = [int(v, 16) for v in args.va]
    else:
        wanted = [
            int(r["va"], 16)
            for r in rows
            if (DATA / "drafts" / f"{int(r['va'], 16):08x}.c.txt").exists()
        ]
    wanted = [v for v in wanted if v not in CALL_ROOTS]
    rows = [r for r in rows if int(r["va"], 16) in wanted]
    if args.emit_test is not None:
        wanted_order = {v: i for i, v in enumerate(sorted(wanted))}
        rows.sort(key=lambda r: wanted_order[int(r["va"], 16)])
        image = build_guest_image(Path("build/default.xbe"))
        oracle = StoreOracle(image.data, max_insns=200000, record_loads=True)
        summary = emit_test(args.label, rows, image, oracle, args.emit_test)
        lines = len(args.emit_test.read_text().splitlines())
        print(f"wrote {args.emit_test} ({len(rows)} roots, {lines} lines)")
        for va, info in summary.items():
            print(va, info["states"], info["kinds"], info["notes"])
        return 0
    sources = {int(r["va"], 16): DATA / "drafts" / f"{int(r['va'], 16):08x}.c.txt" for r in rows}
    WORK.mkdir(parents=True, exist_ok=True)
    image = build_guest_image(Path("build/default.xbe"))
    oracle = UnicornOracle(image.data, max_insns=200000, record_loads=True)
    prepared = {r["va"]: controls(image, oracle, r) for r in rows}
    records = []
    failed = False
    for opt in args.opt or (0, 2, 3):
        lib = build(list(sources.values()), opt, WORK / f"native-o{opt}.so")
        for row in rows:
            va = int(row["va"], 16)
            tests, notes = prepared[row["va"]]
            issues: list[Any] = []
            found, note = run_checks(lib, image.data, tests) if tests else ({}, None)
            if not tests:
                issues.append("no controls")
            if note:
                issues.append(note)
            for position, problems in sorted(found.items()):
                if problems:
                    case, _, kind = tests[position]
                    issues.append(
                        {
                            "seed": case.seed,
                            "index": case.index,
                            "esp": hex(case.esp),
                            "df": case.df,
                            "kind": kind,
                            "problems": problems,
                        }
                    )
            alias_states = [
                {
                    "kind": k,
                    "esp": hex(c.esp),
                    "df": c.df,
                    "index": c.index,
                    "original_regs": list(r.regs),
                    "entry_regs": list(c.regs),
                    "case_sha256": hashlib.sha256(repr(c).encode()).hexdigest(),
                    "saved_slots": saved_slots(image, c, r),
                    "writes_sha256": hashlib.sha256(
                        json.dumps(r.writes, sort_keys=True).encode()
                    ).hexdigest(),
                }
                for c, r, k in tests
                if k.endswith("alias")
            ]
            overwritten = any(s["overwritten"] for a in alias_states for s in a["saved_slots"])
            receipt: dict[str, Any] = {
                "va": row["va"],
                "opt": opt,
                "ordinary": sum(k == "ordinary" for _, _, k in tests),
                "ordinary_df": sum(k.startswith("ordinary-df") for _, _, k in tests),
                "alias": len(alias_states),
                "bulk_fixture": sum(k == "bulk-saved-slot-alias" for _, _, k in tests),
                "controls": len(tests),
                "saved_slot_overwritten_alias": overwritten,
                "bulk_root": va in BULK_ROOTS,
                "notes": notes,
                "issues": issues,
                "alias_states": alias_states,
            }
            if va in BULK_ROOTS and not overwritten:
                receipt["alias_requirement_unmet"] = True
            if args.mutants and opt in (0, 3):
                receipt["mutant"] = mutant_receipt(va, sources[va], opt, image.data, tests)
                if not receipt["mutant"]["primary_killed"]:
                    receipt["mutant_survived_or_missing"] = True
            records.append(receipt)
            failed |= bool(issues) or bool(receipt.get("alias_requirement_unmet"))
            failed |= bool(receipt.get("mutant_survived_or_missing"))
            args.out.write_text(json.dumps(records, indent=1) + "\n")
            mutant = receipt.get("mutant")
            summary = "-"
            if mutant is not None:
                primary = mutant["primary"]
                verdict = "killed" if mutant["primary_killed"] else "SURVIVED"
                summary = f"{primary['rule']}:{verdict}" if primary else "no-mutant"
            print(
                row["va"],
                f"O{opt}",
                "FAIL" if issues else "PASS",
                f"controls={len(tests)}",
                f"alias_overwritten={overwritten}",
                f"mutant={summary}",
                issues[:1],
                flush=True,
            )
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
