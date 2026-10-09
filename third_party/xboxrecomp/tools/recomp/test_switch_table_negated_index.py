"""
Self-check for a negated-index switch table: the base slot is index 0, an arm.

Run: py -3 tools/recomp/test_switch_table_negated_index.py

TSFP-LOCAL (patch 14). MSVC's CRT memmove runs its backward tail copy through

    neg  ecx                  ; ecx = 0, -1, -2, ...
    jmp  [ecx*4 + LAST]       ; LAST is the slot for index 0

so the table lies at and BELOW the displacement. The below-the-base rule found
the slots under LAST and dropped LAST itself, which is the value a zero count
loads: the commonest tail copy there is fell to the unresolved tail jump and
the run stopped on it.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000
TABLE = BASE + 12
LAST = TABLE + 12
# Slot k lives at TABLE + 4k and holds the arm for index -(3 - k).
ARMS = [BASE + 28 + 6 * n for n in range(4)]


def _translate(image):
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="negated-index-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def _image(disp=LAST):
    code = b"\xF7\xD9"                                  # neg ecx
    code += b"\xFF\x24\x8D" + disp.to_bytes(4, "little")  # jmp [ecx*4 + disp]
    code += b"\x90" * (TABLE - BASE - len(code))
    code += b"".join(ARMS[3 - k].to_bytes(4, "little") for k in range(4))
    for n in range(4):
        code += b"\xB8" + n.to_bytes(4, "little") + b"\xC3"   # mov eax, n; ret
    return code


def test_the_base_slot_is_an_arm():
    code = _translate(_image())
    assert "switch: 4 entries" in code, f"base slot dropped\n{code}"
    for arm in ARMS:
        assert f"goto loc_{arm:08X};" in code, f"arm {arm:#x} missing"


def test_an_unrelated_dword_at_the_base_is_not_invented():
    # Base one past the last slot (the `sub ecx, 4` form): the dword there is a
    # mov opcode and an immediate, not a code address, so the four real slots
    # are the arms and nothing is added.
    code = _translate(_image(disp=LAST + 4))
    assert "switch: 4 entries" in code, code
    assert code.count("goto loc_") == 4, code


if __name__ == "__main__":
    test_the_base_slot_is_an_arm()
    test_an_unrelated_dword_at_the_base_is_not_invented()
    print("ok")
