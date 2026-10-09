"""
Self-check for where a function ends when a recognised table follows its last jmp.

Run: py -3 tools/disasm/test_table_tail_extent.py

TSFP-LOCAL (patch 15). MSVC parks a switch table AFTER the function that owns
it, behind the last `ret` and sometimes behind two bytes of `mov edi, edi`
alignment. FunctionDetector._find_function_end steps over a table that follows an
unconditional jump, which is right when the arms and the epilogue come after the
table (the CRT memcpy shape) and wrong when nothing does: the walk then lands in
the table's byte map, decodes it as instructions, and runs on into whatever
follows. Two measured consequences on the Xbox build, both against Ghidra's
function bounds:

  - a function that ended correctly now ended 18 to 800 bytes late
  - the `mov edi, edi` pad before the table was taken for a hot-patch prologue
    by _pass_gap_prologues and became a 'function' of its own

Every fixture is hand-assembled and carries its disassembly in a comment.
Nothing here reads a real XBE.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402

BASE = 0x00010000


class _Section:
    def __init__(self, data):
        self.name = ".text"
        self.virtual_addr = BASE
        self.virtual_size = len(data)
        self.executable = True
        self.data = data


class _Image:
    """Minimal duck-typed BinaryImage over one section."""

    def __init__(self, data):
        self._sec = _Section(data)
        self.base_address = BASE
        self.image_size = len(data)

    def get_section_at_va(self, va):
        sec = self._sec
        if sec.virtual_addr <= va < sec.virtual_addr + sec.virtual_size:
            return sec
        return None

    def get_section_data(self, section):
        return section.data

    def read_bytes_at_va(self, va, size):
        off = va - BASE
        if off < 0 or off >= len(self._sec.data):
            return None
        return self._sec.data[off:off + size]

    def read_u32_at_va(self, va):
        raw = self.read_bytes_at_va(va, 4)
        if raw is None or len(raw) < 4:
            return None
        return struct.unpack("<I", raw)[0]


def _dwords(*values):
    return b"".join(struct.pack("<I", v) for v in values)


def _measured(data):
    eng = DisasmEngine(_Image(data))
    eng.linear_sweep(eng.image.get_section_at_va(BASE))
    eng.resync_jump_tables()
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = eng
    return eng, det


# The sub_001CE650 shape: the table follows a direct `jmp` within 16 bytes, and
# every arm is already behind us.
#
#   00: 83 f8 02              cmp   eax, 2
#   03: 77 0e                 ja    0x13
#   05: 0f b6 80 <MAP>        movzx eax, byte ptr [eax + 0x24]
#   0c: ff 24 85 <TABLE>      jmp   [eax*4 + 0x1c]
#   13: e9 <-0x18>            jmp   0x00            the default, back to the top
#   18: c3                    ret                   arm 0
#   19: c3                    ret                   arm 1
#   1a: 90 90                 alignment
#   1c: 2 table dwords        arm 0, arm 1
#   24: 3 map bytes           cmp eax, 2 -> indices 0, 1, 2
#   27: 90 90 90 c3           the next function's bytes
TABLE = BASE + 0x1C
MAP = BASE + 0x24
ARMS = [BASE + 0x18, BASE + 0x19]
END = BASE + 0x2B


def _tail_table(arms=ARMS, lead=b""):
    code = bytes.fromhex("83f802" "770e") + bytes.fromhex("0fb680") + struct.pack("<I", MAP)
    code += bytes.fromhex("ff2485") + struct.pack("<I", TABLE)
    code += bytes.fromhex("e9") + struct.pack("<i", -0x18)
    code += b"\xc3\xc3" + b"\x90\x90"
    code += _dwords(*arms) + b"\x00\x01\x00"
    code += b"\x90\x90\x90\xc3"
    assert len(code) == 0x2B, hex(len(code))
    return code


def test_the_function_ends_at_its_last_ret_not_past_the_table():
    eng, det = _measured(_tail_table())
    assert eng.jump_tables == {TABLE: TABLE + 8}, eng.jump_tables
    end = det._find_function_end(BASE, next_func=None, sec_end=END)
    assert end == BASE + 0x1A, f"function ends at {end:#x}"


def test_the_ret_that_is_the_last_arm_is_inside_the_function():
    # max_target lands exactly on that ret, and a target is not coverage.
    eng, det = _measured(_tail_table())
    end = det._find_function_end(BASE, next_func=None, sec_end=END)
    assert end > ARMS[-1]


def test_a_table_with_arms_after_it_is_still_stepped_over():
    # The control for the rule above: the arms sit past the table, so code DOES
    # follow it and the walk has to step over to reach it.
    #
    #   00: ff 24 85 <0x1000c>    jmp [eax*4 + 0x0c]
    #   07: 90 x5
    #   0c: 3 table dwords        arms 0x18, 0x1a, 0x1c
    #   18: 90 c3 x3
    arms = [BASE + 0x18 + 2 * n for n in range(3)]
    code = bytes.fromhex("ff2485") + struct.pack("<I", BASE + 0x0C) + b"\x90" * 5
    code += _dwords(*arms) + b"\x90\xc3" * 3
    assert len(code) == 0x1E
    eng, det = _measured(code)
    assert eng.jump_tables == {BASE + 0x0C: BASE + 0x18}, eng.jump_tables
    end = det._find_function_end(BASE, next_func=None, sec_end=BASE + 0x1E)
    assert end == BASE + 0x1E, f"function ends at {end:#x}"


def test_an_arm_exactly_at_the_end_of_the_table_still_counts_as_code_after_it():
    # Every arm is the first byte past the table, so max_target == the table
    # end. Between the jmp and the table are bytes capstone cannot decode, so
    # the walk only gets across by stepping over the table.
    #
    #   00: ff 24 85 <0x1000c>    jmp [eax*4 + 0x0c]
    #   07: ff ff ff ff ff        not instructions
    #   0c: 3 table dwords        all 0x18
    #   18: c3                    ret
    code = bytes.fromhex("ff2485") + struct.pack("<I", BASE + 0x0C) + b"\xff" * 5
    code += _dwords(BASE + 0x18, BASE + 0x18, BASE + 0x18) + b"\xc3"
    assert len(code) == 0x19
    eng, det = _measured(code)
    assert eng.jump_tables == {BASE + 0x0C: BASE + 0x18}, eng.jump_tables
    end = det._find_function_end(BASE, next_func=None, sec_end=BASE + 0x19)
    assert end == BASE + 0x19, f"function ends at {end:#x}"


# _pass_gap_prologues, with the engine reduced to what it asks.
class _Insn:
    def __init__(self, addr, size, is_ret=False):
        self.address = addr
        self.size = size
        self.end_address = addr + size
        self.is_ret = is_ret


class _Func:
    def __init__(self, start, end):
        self.start = start
        self.end = end


class _StubEngine:
    def __init__(self, insns, tables):
        self.instructions = {i.address: i for i in insns}
        self.jump_tables = dict.fromkeys(tables, 0)

    def probes_as_prologue(self, addr):
        return True

    def probes_as_constant_stub(self, addr):
        return False


class _StubImage:
    def get_section_at_va(self, addr):
        return _Section(b"\x00" * 4)


def _gap_detector(tables):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _StubEngine([_Insn(0x100F, 1, is_ret=True)], tables)
    det.image = _StubImage()
    det.functions = {0x1000: _Func(0x1000, 0x1010)}
    det._candidates = {}
    det.added = []
    det._add_candidate = lambda addr, conf, why: det.added.append(addr)
    return det


def test_alignment_padding_before_a_table_is_not_a_function():
    # ret at 0x100F, `mov edi, edi` at 0x1010, table at 0x1012.
    det = _gap_detector({0x1012})
    assert not det._pass_gap_prologues([])
    assert det.added == []


def test_a_table_directly_after_the_ret_is_not_a_function():
    assert not _gap_detector({0x1010})._pass_gap_prologues([])


def test_three_bytes_of_padding_are_still_padding():
    assert not _gap_detector({0x1013})._pass_gap_prologues([])


def test_a_table_four_bytes_on_is_not_this_pad():
    # The control: the same prologue-looking bytes with the table further away
    # are still taken for a function.
    det = _gap_detector({0x1014})
    assert det._pass_gap_prologues([])
    assert det.added == [0x1010]


def test_no_table_at_all_leaves_the_gap_pass_alone():
    det = _gap_detector(set())
    assert det._pass_gap_prologues([])
    assert det.added == [0x1010]


def _run():
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_"):
            continue
        try:
            fn()
            print(f"  ok   {name}")
        except AssertionError as exc:
            failures += 1
            print(f"  FAIL {name}: {exc}")
    print("table tail extent: " + ("OK" if not failures else f"{failures} FAILED"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(_run())
