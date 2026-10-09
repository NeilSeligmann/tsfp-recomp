"""
Self-check for jump tables whose first slot is not at the dispatch's displacement.

Run: py -3 tools/disasm/test_jump_table_offset.py

TSFP-LOCAL (patch 14). MSVC's CRT memcpy/memmove dispatch their tail and lead
vectors with

    neg  ecx / jmp [ecx*4 + LAST_SLOT]       -- table at and BELOW the base
    and  eax, 3 / jmp [eax*4 + START - 4]    -- slot 0 is the jmp's own bytes

The forward greedy scan finds fewer than three entries, refuses, and the bytes
stay decoded as instructions. The function walk then runs through them until it
hits something capstone cannot decode: on the Xbox build memmove ended at
0x3C9AA2, 160 bytes before the function that follows it, and everything after
(the lead arms, a second table, the epilogue) had no generated body.

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


# Eight slots, indexed 0, -1 .. -7 from the LAST one, behind a range check that
# says so: `cmp ecx, 8 / jb` sends only ecx < 8 to the neg. Three sibling
# dwords sit directly below the table and are valid pointers too, which is
# exactly what a greedy backward scan would swallow.
#
#   00: 83 f9 08            cmp ecx, 8
#   03: 72 01               jb   0x06
#   05: c3                  ret
#   06: f7 d9               neg  ecx
#   08: ff 24 8d <disp>     jmp  [ecx*4 + 0x1000_0038]   (the LAST slot)
#   0f: 90                  nop
#   10: 3 sibling dwords    (arms 0, 1, 2)
#   1c: 8 table dwords      (slot for index -k is at 0x38 - 4k)
#   3c: 8 arms, each        90 c3   nop / ret
ARM = [BASE + 0x3C + 2 * n for n in range(8)]
LAST_SLOT = BASE + 0x38


_HEADS = {
    "jb": bytes.fromhex("83f908" "7201") + b"\xc3",        # cmp ecx, 8 / jb
    "jbe": bytes.fromhex("83f907" "7601") + b"\xc3",       # cmp ecx, 7 / jbe
    "other-register": bytes.fromhex("83fa08" "7201") + b"\xc3",  # cmp edx, 8
    "none": b"\x90" * 5 + b"\xc3",
}


def _neg_fixture(guard="jb", between=False):
    code = _HEADS[guard]
    code += bytes.fromhex("f7d9")                                  # neg ecx
    code += b"\x90" if between else b""                            # nop
    code += bytes.fromhex("ff248d") + struct.pack("<I", LAST_SLOT)
    code += b"" if between else b"\x90"
    code += _dwords(ARM[0], ARM[1], ARM[2])
    # index 0 is the slot at the highest address, so arm 0 comes last
    code += _dwords(*[ARM[7 - n] for n in range(8)])
    code += b"\x90\xc3" * 8
    assert len(code) == 0x4C
    return code


def test_negated_index_table_is_measured_below_its_base():
    eng, _ = _measured(_neg_fixture())
    assert eng.jump_tables == {BASE + 0x1C: BASE + 0x3C}, eng.jump_tables
    assert eng.jump_table_entries(LAST_SLOT) == [ARM[7 - n] for n in range(8)]


def test_range_check_stops_the_scan_at_the_siblings():
    # Without the guard's 8 the scan would read 11 and start at 0x10.
    eng, _ = _measured(_neg_fixture())
    start = next(iter(eng.jump_tables))
    assert start == BASE + 0x1C, f"swallowed a sibling table: {start:#x}"


def test_no_hallucinated_instruction_survives_over_the_table():
    eng, _ = _measured(_neg_fixture())
    inside = [a for a in eng.instructions if BASE + 0x1C <= a < BASE + 0x3C]
    assert inside == [], [hex(a) for a in inside]
    # And the real arms are decoded as themselves.
    assert all(a in eng.instructions for a in ARM)


def test_function_extends_over_the_arms_through_the_table():
    eng, det = _measured(_neg_fixture())
    end = det._find_function_end(BASE, next_func=None, sec_end=BASE + 0x4C)
    assert end == BASE + 0x4C, f"function cut at {end:#x}"


def test_without_the_fix_the_function_ends_at_its_own_dispatch():
    # The control that makes the test above mean something: with the offset
    # rule disabled the dispatch has no known arms, so the walk stops on the
    # `jmp` itself, 0x3D bytes short.
    saved = DisasmEngine._offset_table
    DisasmEngine._offset_table = lambda self, *a, **k: None
    try:
        eng, det = _measured(_neg_fixture())
        end = det._find_function_end(BASE, next_func=None, sec_end=BASE + 0x4C)
    finally:
        DisasmEngine._offset_table = saved
    assert eng.jump_tables == {}
    assert end == BASE + 0x0F, f"expected the old cut at 0x0F, got {end:#x}"


def test_negated_index_without_a_guard_falls_back_to_the_pointer_run():
    # No `cmp / jb` to read: the run of valid pointers is all there is, and the
    # siblings are swallowed. Documented, not endorsed: this is the case the
    # guard exists to make exact.
    eng, _ = _measured(_neg_fixture("none"))
    assert next(iter(eng.jump_tables)) == BASE + 0x10


def test_a_jbe_guard_counts_one_more_than_its_limit():
    eng, _ = _measured(_neg_fixture("jbe"))
    assert eng.jump_tables == {BASE + 0x1C: BASE + 0x3C}, eng.jump_tables


def test_a_guard_on_another_register_is_not_a_guard():
    eng, _ = _measured(_neg_fixture("other-register"))
    assert next(iter(eng.jump_tables)) == BASE + 0x10


def test_the_neg_must_end_at_the_dispatch():
    # Something between the neg and the jmp may have rewritten the index, so
    # nothing says it is still negative.
    eng, _ = _measured(_neg_fixture("jb", between=True))
    assert eng.jump_tables == {}, eng.jump_tables


# `and eax, 3` leaves 1..3, so the displacement is the table start minus 4 and
# the dword AT it is the jmp's own bytes.
#
#   00: 83 e0 03            and eax, 3
#   03: ff 24 85 <disp>     jmp [eax*4 + 0x1000_000c]
#   0a: 90 90               nop x2
#   0c: 90 90 90 90         the dword AT disp, 0x90909090: not a pointer
#   10: 3 table dwords
#   1c: 90 x 4              filler, so the sweep's straddler ends before the arms
#   20: 3 arms              90 c3
PLUS4_ARMS = [BASE + 0x20 + 2 * n for n in range(3)]


def _plus4_fixture():
    code = bytes.fromhex("83e003") + bytes.fromhex("ff2485") + struct.pack("<I", BASE + 0x0C)
    code += b"\x90" * 2
    code += struct.pack("<I", 0x90909090)
    code += _dwords(*PLUS4_ARMS)
    code += b"\x90" * 4
    code += b"\x90\xc3" * 3
    assert len(code) == 0x26
    return code


def test_plus_four_table_starts_after_the_displacement_slot():
    eng, det = _measured(_plus4_fixture())
    assert eng.jump_tables == {BASE + 0x10: BASE + 0x1C}, eng.jump_tables
    assert eng.jump_table_entries(BASE + 0x0C) == PLUS4_ARMS
    end = det._find_function_end(BASE, next_func=None, sec_end=BASE + 0x26)
    assert end == BASE + 0x26, f"function cut at {end:#x}"


def test_plus_four_needs_three_pointers():
    # Two pointers at disp + 4 are a coincidence, not a table.
    code = bytearray(_plus4_fixture())
    code[0x18:0x1C] = struct.pack("<I", 0x90909090)
    eng, _ = _measured(bytes(code))
    assert eng.jump_tables == {}, eng.jump_tables


# A run of valid pointers BELOW a base is not evidence of a table without the
# negated index: MSVC parks sibling tables back to back, and 0x1CE944 in the
# Xbox build is a two-entry table with eight valid pointers under it.
#
#   00: ff 24 8d <disp>     jmp [ecx*4 + 0x1000_0020]    (no neg before it)
#   07: 90 x 9
#   10: 4 dwords            valid pointers, a sibling table
#   20: slot(s) at disp
#   2c: 4 arms              90 c3
def _below_fixture(slots):
    arm = BASE + 0x2C
    code = bytes.fromhex("ff248d") + struct.pack("<I", BASE + 0x20)
    code += b"\x90" * 9
    code += _dwords(arm, arm, arm, arm)
    code += _dwords(*slots)
    code += _dwords(0x90909090) * (3 - len(slots))
    code += b"\x90\xc3" * 4
    assert len(code) == 0x34
    return code


def test_a_pointer_run_below_a_two_entry_table_is_not_a_table():
    # The 0x1CE944 shape: two real entries at disp, the sibling's eight below.
    arm = BASE + 0x2C
    eng, _ = _measured(_below_fixture([arm, arm]))
    assert eng.jump_tables == {}, eng.jump_tables


def test_a_pointer_run_below_an_empty_slot_is_not_a_table_either():
    # The 0x3C995C shape. No neg, so nothing says the index is negative.
    eng, _ = _measured(_below_fixture([]))
    assert eng.jump_tables == {}, eng.jump_tables


# Two dispatches, one table: a lead dispatch `and eax, 3 / jmp [eax*4 + D]`
# and a tail dispatch `neg ecx / jmp [ecx*4 + LAST]` both land on the same three
# slots. The table is measured and resynced once. TSFP-LOCAL (patch 15): the
# fixture had four slots, which the lead dispatch's own mask (index 1..3) can
# not address, so the two dispatches disagreed about the extent. Three agree.
#
#   00: 83 e0 03 / ff 24 85 <0x1001c>      and eax, 3 / jmp [eax*4 + D]
#   0a: 83 f9 03 / 72 01 / c3              cmp ecx, 3 / jb neg / ret
#   10: f7 d9 / ff 24 8d <0x10028>         neg ecx / jmp [ecx*4 + LAST]
#   19: 90 x3
#   1c: 90 x4                              the dword at D, not a pointer
#   20: 3 slots, 2c: filler, 30: 3 arms
SHARED_ARMS = [BASE + 0x30 + 2 * n for n in range(3)]


def test_two_dispatches_on_one_table_resync_it_once():
    code = bytes.fromhex("83e003") + bytes.fromhex("ff2485") + struct.pack("<I", BASE + 0x1C)
    code += bytes.fromhex("83f903" "7201") + b"\xc3"
    code += bytes.fromhex("f7d9") + bytes.fromhex("ff248d") + struct.pack("<I", BASE + 0x28)
    code += b"\x90" * 3 + b"\x90" * 4
    code += _dwords(*SHARED_ARMS)
    code += b"\x90" * 4
    code += b"\x90\xc3" * 3
    assert len(code) == 0x36
    eng = DisasmEngine(_Image(code))
    eng.linear_sweep(eng.image.get_section_at_va(BASE))
    assert eng.resync_jump_tables() == 1
    assert eng.jump_tables == {BASE + 0x20: BASE + 0x2C}, eng.jump_tables
    assert eng._jt_alias == {BASE + 0x1C: BASE + 0x20, BASE + 0x28: BASE + 0x20}
    assert eng.jump_table_entries(BASE + 0x28) == SHARED_ARMS


def test_a_table_the_forward_rule_accepts_is_untouched():
    #   00: ff 24 85 <disp>     jmp [eax*4 + 0x1000_0010]
    #   10: 3 dwords, then arms
    arms = [BASE + 0x1C + 2 * n for n in range(3)]
    code = bytes.fromhex("ff2485") + struct.pack("<I", BASE + 0x10)
    code += b"\x90" * 9
    code += _dwords(*arms)
    code += b"\x90\xc3" * 3
    eng, _ = _measured(code)
    assert eng.jump_tables == {BASE + 0x10: BASE + 0x1C}
    assert eng._jt_alias == {}, "the forward rule must not consult the offset rule"


# TSFP-LOCAL (T47). The entry-value range check: a dword only extends a table
# while its VALUE lands inside the dispatch's own section. The dwords below are
# all READABLE (they sit inside the section), so only the value check can
# refuse them -- values just past the section's end are what a table crossing
# a section boundary hands the scan, and 0x52D520/0x52DE2C in the Xbox build
# are .data pointer arrays (XBE marks .data executable) that only the
# same-section rule keeps out. Measured: dropping the check blows 401 of 706
# table extents to the 512-entry cap and admits 9 false tables.
OUTSIDE = [BASE - 0x1000, BASE + 0x10000, BASE + 0x10040]


def test_forward_entries_pointing_outside_the_section_are_not_a_table():
    #   00: ff 24 85 <disp>     jmp [eax*4 + 0x1000_0010]
    #   10: 3 readable dwords whose values are OUTSIDE the section
    code = bytes.fromhex("ff2485") + struct.pack("<I", BASE + 0x10)
    code += b"\x90" * 9
    code += _dwords(*OUTSIDE)
    code += b"\x90\xc3" * 3
    eng, _ = _measured(code)
    assert eng.jump_tables == {}, eng.jump_tables


def test_plus_four_entries_pointing_outside_the_section_are_rejected():
    # The _plus4_fixture shape, mask and all, but the three table dwords
    # hold addresses past the section's end -- the cross-boundary case.
    code = bytearray(_plus4_fixture())
    code[0x10:0x1C] = _dwords(*OUTSIDE)
    eng, _ = _measured(bytes(code))
    assert eng.jump_tables == {}, eng.jump_tables
    assert eng._jt_alias == {}, eng._jt_alias


def test_negated_run_entries_pointing_outside_the_section_are_rejected():
    # The _neg_fixture shape with its guard intact, but every slot's value
    # lands outside the section, so the backward run must count zero.
    code = bytearray(_neg_fixture())
    code[0x1C:0x3C] = _dwords(*([BASE + 0x10000 + 4 * n for n in range(8)]))
    eng, _ = _measured(bytes(code))
    assert eng.jump_tables == {}, eng.jump_tables


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
    print("jump table offset: " + ("OK" if not failures else f"{failures} FAILED"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(_run())
