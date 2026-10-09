"""
Self-check for jump tables below the minimum entry count, and for the count cap
on the displacement-plus-four shape.

Run: py -3 tools/disasm/test_jump_table_bytemap.py

TSFP-LOCAL (patch 15). Two fixes to what patch 14 left behind.

1. MSVC's two-level switch keeps arm addresses in a dword table and the
   case-to-arm mapping in a byte table directly after it:

       cmp   eax, 2
       ja    default
       movzx eax, byte ptr [eax + MAP]        ; MAP == table + 4 * arms
       jmp   [eax*4 + table]

   Two arms is under the minimum of three, so the table stayed decoded as
   instructions. On the Xbox build that cut sub_001CE650 one byte before its own
   `ret`, which is the second arm. The byte table is the evidence: it starts
   exactly where the pointer run ends, the range check says how long it is, and
   its largest value is the last arm's index.

2. `and eax, 3 / jmp [eax*4 + START - 4]` had no terminator but the first dword
   that is not a pointer. The mask is the compiler's own statement of the count.

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
    """Engine after a sweep and a table resync, plus a detector over it."""
    eng = DisasmEngine(_Image(data))
    eng.linear_sweep(eng.image.get_section_at_va(BASE))
    eng.resync_jump_tables()
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = eng
    return eng, det


# The sub_001CE650 shape: the last arm is the function's own `ret`, and the
# table sits more than 16 bytes past the dispatch (see FunctionDetector._table_after).
#
#   00: 83 f8 02              cmp   eax, 2
#   03: 77 0e                 ja    0x13            (the default)
#   05: 0f b6 80 <MAP>        movzx eax, byte ptr [eax + 0x38]
#   0c: ff 24 85 <TABLE>      jmp   [eax*4 + 0x30]
#   13: 33 c0 c3              xor eax, eax / ret      default
#   16: b8 01 00 00 00 c3     mov eax, 1 / ret        arm 0
#   1c: c3                    ret                     arm 1
#   1d: 90 x 19
#   30: 2 table dwords        arm 0, arm 1
#   38: 3 map bytes           cmp eax, 2 -> indices 0, 1, 2
TABLE = BASE + 0x30
MAP = BASE + 0x38
ARMS = [BASE + 0x16, BASE + 0x1C]
END = BASE + 0x3B


def _two_level(
    cmp=b"\x83\xf8\x02",
    ja=b"\x77\x0e",
    load=b"\x0f\xb6\x80",
    jmp=b"\xff\x24\x85",
    map_at=MAP,
    table=ARMS,
    mapping=b"\x00\x01\x00",
):
    code = cmp + ja + load + struct.pack("<I", map_at)
    code += jmp + struct.pack("<I", TABLE)
    code += bytes.fromhex("33c0c3" "b801000000c3" "c3") + b"\x90" * 19
    code += _dwords(*table) + mapping
    assert len(code) == 0x3B, hex(len(code))
    return code


def test_two_entry_table_with_a_byte_map_is_measured():
    eng, _ = _measured(_two_level())
    assert eng.jump_tables == {TABLE: TABLE + 8}, eng.jump_tables
    assert eng.jump_table_entries(TABLE) == ARMS


def test_function_reaches_its_own_ret_through_the_second_arm():
    # The sub_001CE650 consequence: arm 1 IS the last `ret`.
    eng, det = _measured(_two_level())
    end = det._find_function_end(BASE, next_func=None, sec_end=END)
    assert end == BASE + 0x1D, f"function cut at {end:#x}"


def test_without_the_rule_the_function_ends_before_that_ret():
    # The control that makes the test above mean something: the walk stops
    # at the default's ret, 7 bytes short.
    saved = DisasmEngine._byte_map_table
    DisasmEngine._byte_map_table = lambda self, *a, **k: False
    try:
        eng, det = _measured(_two_level())
        end = det._find_function_end(BASE, next_func=None, sec_end=END)
    finally:
        DisasmEngine._byte_map_table = saved
    assert eng.jump_tables == {}
    assert end == BASE + 0x16, f"expected the old cut at 0x16, got {end:#x}"


def test_the_table_is_data_so_no_instruction_is_decoded_over_it():
    eng, _ = _measured(_two_level())
    inside = [a for a in eng.instructions if TABLE <= a < TABLE + 8]
    assert inside == [], [hex(a) for a in inside]


def test_a_map_cut_off_by_the_end_of_the_section_is_refused():
    # The range check promises 3 map bytes and only one exists, whose value
    # happens to be the last arm's index.
    eng, _ = _measured(_two_level(mapping=b"\x01\x00\x00")[:0x39])
    assert eng.jump_tables == {}, eng.jump_tables


def test_a_single_pointer_is_not_a_two_level_table():
    # One pointer then the map, every map byte 0: a switch has two arms at least.
    code = _two_level(map_at=BASE + 0x34)[:0x30] + _dwords(ARMS[0]) + b"\x00\x00\x00"
    eng, _ = _measured(code)
    assert eng.jump_tables == {}, eng.jump_tables


def test_a_register_other_than_the_index_may_be_the_map_base():
    # movzx ecx, byte ptr [eax + MAP] / jmp [ecx*4 + TABLE], as sub_0015A2E0 does.
    eng, _ = _measured(_two_level(load=b"\x0f\xb6\x88", jmp=b"\xff\x24\x8d"))
    assert eng.jump_tables == {TABLE: TABLE + 8}, eng.jump_tables


def test_two_pointers_without_a_byte_load_are_not_a_table():
    # The 0x1CE944 neighbourhood: two valid pointers and nothing that says
    # they are a table. movzx of a word is not a byte map.
    eng, _ = _measured(_two_level(load=b"\x0f\xb7\x80"))   # a WORD load
    assert eng.jump_tables == {}, eng.jump_tables


def test_a_map_that_does_not_start_where_the_pointers_end_is_refused():
    eng, _ = _measured(_two_level(map_at=MAP + 4))
    assert eng.jump_tables == {}, eng.jump_tables


def test_a_map_naming_a_third_arm_is_refused():
    # Index 2 means a third slot exists, so two pointers are not the table.
    eng, _ = _measured(_two_level(mapping=b"\x00\x01\x02"))
    assert eng.jump_tables == {}, eng.jump_tables


def test_a_map_that_never_reaches_the_last_arm_is_refused():
    eng, _ = _measured(_two_level(mapping=b"\x00\x00\x00"))
    assert eng.jump_tables == {}, eng.jump_tables


def test_a_range_check_on_another_register_is_refused():
    eng, _ = _measured(_two_level(cmp=b"\x83\xfa\x02"))          # cmp edx, 2
    assert eng.jump_tables == {}, eng.jump_tables


def test_no_above_branch_after_the_compare_is_refused():
    eng, _ = _measured(_two_level(ja=b"\x90\x90"))
    assert eng.jump_tables == {}, eng.jump_tables


def test_a_map_loaded_into_a_different_register_than_the_index_is_refused():
    # movzx ecx, ... / jmp [eax*4 + TABLE]: the loaded value is not the index.
    eng, _ = _measured(_two_level(load=b"\x0f\xb6\x88"))
    assert eng.jump_tables == {}, eng.jump_tables


def test_the_movzx_must_end_at_the_dispatch():
    # A nop between them: nothing says the index is still the map's value.
    # Everything after the jmp moves up one byte, the pointers do not matter.
    code = (
        b"\x83\xf8\x02\x77\x0f"
        + b"\x0f\xb6\x80" + struct.pack("<I", MAP)
        + b"\x90"
        + b"\xff\x24\x85" + struct.pack("<I", TABLE)
    )
    code += bytes.fromhex("33c0c3" "b801000000c3" "c3") + b"\x90" * 18
    code += _dwords(*ARMS) + b"\x00\x01\x00"
    assert len(code) == 0x3B, hex(len(code))
    eng, _ = _measured(code)
    assert eng.jump_tables == {}, eng.jump_tables


def test_a_run_of_three_is_not_touched_by_the_byte_map_rule():
    # Three pointers are accepted by the forward rule on their own.
    arms = [BASE + 0x16, BASE + 0x1C, BASE + 0x1C]
    code = _two_level(table=arms[:2])
    code = code[:0x30] + _dwords(*arms) + b"\x00\x01\x02"
    eng, _ = _measured(code)
    assert eng.jump_tables == {TABLE: TABLE + 12}, eng.jump_tables


# `and eax, 3` leaves 1..3, so the table is disp + 4 and holds three arms. Two
# more valid pointers follow it, a sibling table the scan would swallow.
#
#   00: 83 e0 03              and eax, 3
#   03: ff 24 85 <disp>       jmp [eax*4 + 0x10]
#   0a: 90 x6
#   10: 90 x4                 the dword AT disp, not a pointer
#   14: 3 table dwords, then 2 sibling dwords
#   28: 90 x4
#   2c: 5 arms                90 c3
PLUS4_ARMS = [BASE + 0x2C + 2 * n for n in range(5)]
PLUS4_START = BASE + 0x14


def _plus4(prefix=b"\x83\xe0\x03", between=b""):
    code = prefix + between + bytes.fromhex("ff2485") + struct.pack("<I", BASE + 0x10)
    code += b"\x90" * (0x10 - len(code))
    code += struct.pack("<I", 0x90909090)
    code += _dwords(*PLUS4_ARMS)
    code += b"\x90" * 4
    code += b"\x90\xc3" * 5
    assert len(code) == 0x36, hex(len(code))
    return code


def test_and_mask_caps_the_displacement_plus_four_run():
    eng, _ = _measured(_plus4())
    assert eng.jump_tables == {PLUS4_START: PLUS4_START + 12}, eng.jump_tables
    assert eng.jump_table_entries(BASE + 0x10) == PLUS4_ARMS[:3]


def test_without_the_mask_the_dispatch_is_refused():
    # The control for the cap: with the mask unreadable the run is not trusted.
    saved = DisasmEngine._mask_bound_before
    DisasmEngine._mask_bound_before = lambda self, *a, **k: None
    try:
        eng, _ = _measured(_plus4())
    finally:
        DisasmEngine._mask_bound_before = saved
    assert eng.jump_tables == {}, eng.jump_tables


def test_a_pointer_run_at_an_unmasked_dispatch_is_not_a_plus_four_table():
    # mov eax, [esp + 4] in place of the and: nothing bounds the run, and a
    # run of pointers four bytes past a displacement is what near-miss
    # displacements beside any table produce.
    eng, _ = _measured(_plus4(prefix=bytes.fromhex("8b442404")))
    assert eng.jump_tables == {}, eng.jump_tables


def test_a_mask_wider_than_the_run_does_not_lengthen_it():
    # `and eax, 7` allows 7 slots and the run holds 5: the cap only shortens.
    eng, _ = _measured(_plus4(prefix=b"\x83\xe0\x07"))
    assert eng.jump_tables == {PLUS4_START: PLUS4_START + 20}, eng.jump_tables


def test_a_mask_that_is_not_low_bits_is_not_a_bound():
    # `and eax, 5` leaves 0, 1, 4, 5: it names no count.
    eng, _ = _measured(_plus4(prefix=b"\x83\xe0\x05"))
    assert eng.jump_tables == {}, eng.jump_tables


def test_only_an_and_bounds_the_index():
    # add eax, 3 leaves a value that happens to print like a low-bit mask.
    eng, _ = _measured(_plus4(prefix=bytes.fromhex("83c003")))
    assert eng.jump_tables == {}, eng.jump_tables


def test_a_later_write_to_the_index_refuses_the_mask():
    # add eax, 1 after the and: the mask no longer describes the index.
    eng, _ = _measured(_plus4(between=b"\x83\xc0\x01"))
    assert eng.jump_tables == {}, eng.jump_tables


def test_a_call_between_the_mask_and_the_dispatch_refuses_the_mask():
    # call rel32 may rewrite eax; the 5 bytes fit in the fixture's lead.
    eng, _ = _measured(_plus4(between=bytes.fromhex("e800000000")))
    assert eng.jump_tables == {}, eng.jump_tables


def test_a_write_to_another_register_leaves_the_cap_in_force():
    # add ecx, eax between the and and the jmp, as memmove does.
    eng, _ = _measured(_plus4(between=b"\x01\xc1"))
    assert eng.jump_tables == {PLUS4_START: PLUS4_START + 12}, eng.jump_tables


def test_two_dispatches_with_different_masks_take_the_wider():
    # `and eax, 3` and `and ecx, 7` both land on one 5-slot run: the table is
    # the union of what either can reach, so the cap is the larger mask.
    #
    #   00: and eax, 3 / jmp [eax*4 + 0x18]      0a: and ecx, 7 / jmp [ecx*4 + 0x18]
    #   14: 90 x4    18: the dword at disp    1c: 5 slots    30: 90 x4    34: 5 arms
    arms = [BASE + 0x34 + 2 * n for n in range(5)]
    disp = struct.pack("<I", BASE + 0x18)
    code = bytes.fromhex("83e003" "ff2485") + disp + bytes.fromhex("83e107" "ff248d") + disp
    code += b"\x90" * (0x18 - len(code)) + struct.pack("<I", 0x90909090)
    code += _dwords(*arms) + b"\x90" * 4 + b"\x90\xc3" * 5
    eng, _ = _measured(code)
    assert eng.jump_tables == {BASE + 0x1C: BASE + 0x30}, eng.jump_tables


def test_jt_bound_still_reads_a_plain_range_check():
    # The extraction of _range_check_before must not move patch 08's result.
    #   cmp eax, 2 / ja / jmp [eax*4 + T]: three arms
    arms = [BASE + 0x20, BASE + 0x21, BASE + 0x22]
    code = bytes.fromhex("83f802" "7701" "c3") + bytes.fromhex("ff2485") + struct.pack("<I", BASE + 0x10)
    code += b"\x90" * (0x10 - len(code)) + _dwords(*arms, BASE + 0x20, BASE + 0x20)
    code += b"\x90" * (0x20 - len(code)) + b"\xc3\xc3\xc3"
    eng, _ = _measured(code)
    assert eng.jump_tables == {BASE + 0x10: BASE + 0x1C}, eng.jump_tables
    assert eng.jump_table_trims == {BASE + 0x10: (5, 3, "bound")}, eng.jump_table_trims


def test_jt_bound_refuses_a_redefined_index():
    # movzx rewrites eax between the compare and the dispatch: the 2 bounds
    # the OUTER index, not the table.
    arms = [BASE + 0x20, BASE + 0x21, BASE + 0x22]
    code = bytes.fromhex("83f802" "7701" "c3") + bytes.fromhex("0fb680") + struct.pack("<I", BASE + 0x18)
    code += bytes.fromhex("ff2485") + struct.pack("<I", BASE + 0x10)
    code += b"\x90" * (0x10 - len(code)) + _dwords(*arms, BASE + 0x20, BASE + 0x20)
    eng = DisasmEngine(_Image(code + b"\x90" * (0x30 - len(code))))
    eng.linear_sweep(eng.image.get_section_at_va(BASE))
    assert eng._jt_bound(BASE + 0x10) is None


# TSFP-LOCAL (T89). A RECOGNISED run that swallowed its own byte map's first
# dword. Three real arms, and the map's first four bytes happen to read as an
# in-section address, so the greedy run is four dwords. The map load's
# displacement (table + 12) is where the table really ends.
#
#   00: 83 f8 07              cmp   eax, 7           (the map is 8 bytes)
#   03: 77 0e                 ja    0x13
#   05: 0f b6 80 <MAP3>       movzx eax, byte ptr [eax + 0x3c]
#   0c: ff 24 85 <TABLE3>     jmp   [eax*4 + 0x30]
#   13: 33 c0 c3              default
#   16: b8 01 00 00 00 c3     arm 0
#   1c: c3                    arm 1
#   1d: 90 x 19
#   30: 3 table dwords        arm 0, arm 1, default
#   3c: 8 map bytes           first four read as dword 0x00010002, in-section
TABLE3 = BASE + 0x30
MAP3 = BASE + 0x3C
ARMS3 = [BASE + 0x16, BASE + 0x1C, BASE + 0x13]


def _overlong(
    cmp=b"\x83\xf8\x07",
    load=b"\x0f\xb6\x80",
    map_disp=MAP3,
    mapping=b"\x02\x00\x01\x00\x01\x02\x00\x00",
):
    code = cmp + b"\x77\x0e" + load + struct.pack("<I", map_disp)
    code += b"\xff\x24\x85" + struct.pack("<I", TABLE3)
    code += bytes.fromhex("33c0c3" "b801000000c3" "c3") + b"\x90" * 19
    code += _dwords(*ARMS3) + mapping
    assert len(code) == 0x44, hex(len(code))
    # The trap the fixture exists for: the map's first dword IS in-section.
    assert BASE <= struct.unpack("<I", mapping[:4])[0] < BASE + len(code)
    return code


def test_a_run_one_dword_past_its_own_map_is_trimmed_to_the_map():
    eng, _ = _measured(_overlong())
    assert eng.jump_tables == {TABLE3: TABLE3 + 12}, eng.jump_tables
    assert eng.jump_table_entries(TABLE3) == ARMS3
    assert eng.jump_table_trims == {TABLE3: (4, 3, "bytemap")}, \
        eng.jump_table_trims


def test_without_the_cap_the_map_dword_counts_as_an_arm():
    # The control: this is exactly the six-table defect on the Xbox build.
    saved = DisasmEngine._map_cap
    DisasmEngine._map_cap = lambda self, *a, **k: None
    try:
        eng, _ = _measured(_overlong())
    finally:
        DisasmEngine._map_cap = saved
    assert eng.jump_tables == {TABLE3: TABLE3 + 16}, eng.jump_tables


def test_a_map_naming_an_arm_at_or_past_the_cap_refuses_it():
    # max(map) == 3 says a fourth arm exists, so table + 12 cannot be the end.
    eng, _ = _measured(_overlong(
        mapping=b"\x03\x00\x01\x00\x01\x02\x00\x00"))
    assert eng.jump_tables == {TABLE3: TABLE3 + 16}, eng.jump_tables


def test_a_map_off_the_run_dword_grid_is_not_a_cap():
    # The load's displacement lands mid-dword (table + 10): whatever it reads,
    # it does not name a table end.
    eng, _ = _measured(_overlong(map_disp=TABLE3 + 10))
    assert eng.jump_tables == {TABLE3: TABLE3 + 16}, eng.jump_tables


def test_the_cap_holds_without_a_readable_map_length():
    # cmp edx, 7 guards another register, so the map's extent is unreadable
    # (0x244AE4's shape). The load's displacement alone places the end.
    eng, _ = _measured(_overlong(cmp=b"\x83\xfa\x07"))
    assert eng.jump_tables == {TABLE3: TABLE3 + 12}, eng.jump_tables


def test_a_cap_under_the_minimum_must_pass_the_byte_map_proof():
    # Two real arms, a 4-byte map whose first dword is in-section: greedy 3,
    # capped 2, and 2 is under the minimum, so the patch 15 proof decides.
    code = b"\x83\xf8\x03\x77\x0e" + b"\x0f\xb6\x80" + struct.pack("<I", MAP)
    code += b"\xff\x24\x85" + struct.pack("<I", TABLE)
    code += bytes.fromhex("33c0c3" "b801000000c3" "c3") + b"\x90" * 19
    code += _dwords(*ARMS) + b"\x01\x00\x01\x00"
    assert len(code) == 0x3C, hex(len(code))
    eng, _ = _measured(code)
    assert eng.jump_tables == {TABLE: TABLE + 8}, eng.jump_tables
    assert eng.jump_table_trims == {TABLE: (3, 2, "bytemap")}, \
        eng.jump_table_trims


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
    print("jump table bytemap: " + ("OK" if not failures else f"{failures} FAILED"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(_run())
