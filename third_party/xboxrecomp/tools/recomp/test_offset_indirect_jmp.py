"""
Self-check for a computed `jmp <reg>` into an unrolled instruction run (TSFP-LOCAL patch 37, T1196).

The CRT copy routine at 0x245C0 of the retail title aligns the destination, then enters a run of
eight `movsb` through
    and ecx, 7 ; sub ebx, ecx ; neg ecx ; add ecx, <end of the run> ; jmp ecx
so the register holds `end - k` for 0 <= k <= 7. Lifted as an indirect tail call it resolved to
nothing ("unresolved indirect target 0x2460C, tailjmp"). The landing addresses are the instruction
starts in [end - mask, end], so they become labels and the jump a bounded goto chain.

Properties: the gotos are emitted for the bounded shape, and NOT without the AND mask bound, with a
mask above the window, with an add target outside the function, or when the jump uses another register.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000
MOVSB = b"\xA4"


def _setup(image):
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="offset-indirect-test")
    return image


def _translate(image):
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def _build(mask=7, jump_register=b"\xFF\xE1", add_target=None, with_mask=True):
    """Return (image, run_end). The run is eight movsb followed by `mov ecx, ebx; ret`."""
    head_len = (3 if with_mask else 0) + 2 + 2 + 6 + 2
    run_start = BASE + head_len
    run_end = run_start + 8
    target = run_end if add_target is None else add_target
    head = (b"\x83\xE1" + bytes([mask]) if with_mask else b"")
    head += b"\x2B\xD9\xF7\xD9" + b"\x81\xC1" + target.to_bytes(4, "little") + jump_register
    return head + MOVSB * 8 + b"\x8B\xCB\xC3", run_end


def test_bounded_offset_jump_becomes_gotos():
    image, run_end = _build()
    c = _translate(_setup(image))
    for offset in range(8):
        assert f"if (_jt == 0x{run_end - offset:08X}u) goto loc_{run_end - offset:08X};" in c, c
    assert "intra-function indirect jmp" in c, c
    print("ok  bounded_offset_jump_becomes_gotos")


def test_without_the_mask_bound_it_stays_an_indirect_tail_call():
    image, _ = _build(with_mask=False)
    c = _translate(_setup(image))
    assert "goto loc_" not in c and "RECOMP_ITAIL" in c, c
    print("ok  without_the_mask_bound_it_stays_an_indirect_tail_call")


def test_a_mask_above_the_window_is_not_guessed():
    image, _ = _build(mask=0x7F)
    c = _translate(_setup(image))
    assert "intra-function indirect jmp" not in c and "RECOMP_ITAIL" in c, c
    print("ok  a_mask_above_the_window_is_not_guessed")


def test_an_add_target_outside_the_function_is_not_a_label():
    image, _ = _build(add_target=BASE + 0x400)
    c = _translate(_setup(image))
    assert "intra-function indirect jmp" not in c and "RECOMP_ITAIL" in c, c
    print("ok  an_add_target_outside_the_function_is_not_a_label")


def test_a_jump_through_another_register_is_not_this_pattern():
    image, _ = _build(jump_register=b"\xFF\xE2")
    c = _translate(_setup(image))
    assert "intra-function indirect jmp" not in c and "RECOMP_ITAIL" in c, c
    print("ok  a_jump_through_another_register_is_not_this_pattern")


if __name__ == "__main__":
    test_bounded_offset_jump_becomes_gotos()
    test_without_the_mask_bound_it_stays_an_indirect_tail_call()
    test_a_mask_above_the_window_is_not_guessed()
    test_an_add_target_outside_the_function_is_not_a_label()
    test_a_jump_through_another_register_is_not_this_pattern()
    print("offset_indirect_jmp: ALL PASS")
