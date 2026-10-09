"""Patch26 strict CMP/SUB computed-ZF admission and real CFG boundaries."""
import pytest

from tools.recomp.disasm import Operand
from tools.recomp.lifter import _cmp_sub_zf_producer, _make_condition
from tools.recomp.translator import _incoming_flag_state, _merge_flag_states


def pair(reg="eax", value=3):
    return [Operand(type="reg", reg=reg), Operand(type="imm", imm=value)]


def test_zero_only_and_marker_gate():
    a, b = ("cmp", pair(value=0x37b)), ("sub", pair())
    marker = ("__cmp_sub_zf", [])
    assert _merge_flag_states([a, b], allow_cmp_sub_snapshot=True) == marker
    assert _merge_flag_states([a, b], allow_cmp_sub_snapshot=False) is None
    assert _merge_flag_states([a, b], allow_zf_snapshot=False) is None
    for cc in ("je", "jz", "jne", "jnz"):
        assert _make_condition(cc, *marker) is not None
    for cc in ("jle", "jg", "jge", "jl", "js", "jns", "jb", "jbe", "ja", "jo", "jp"):
        assert _make_condition(cc, *marker) is None
    assert _make_condition("je", marker[0], pair()) is None
    assert _merge_flag_states([marker, marker], allow_cmp_sub_snapshot=False) is None
    assert _merge_flag_states([marker, a], allow_zf_snapshot=False) is None
    assert _merge_flag_states([(marker[0], pair()), a]) is None
    assert _incoming_flag_state([1, 2], {1: a}, False) is None
    assert _incoming_flag_state([1, 2], {1: a, 2: b}, True) is None
    assert _incoming_flag_state([1], {1: a}, True, entry_state=a) is None


@pytest.mark.parametrize("kind", ("cmp", "sub"))
@pytest.mark.parametrize("bad", [
    pair("al"), pair("ax"), pair("unknown"), pair("xmm0"),
    pair(value=True), pair(value=False), pair(value=0x100000000), pair(value=-0x80000001),
    [Operand(type="reg", reg="eax", mem_size=4), Operand(type="imm", imm=3)],
    [Operand(type="reg", reg="eax", mem_size=True), Operand(type="imm", imm=3)],
    [Operand(type="reg", reg="eax"), Operand(type="imm", imm=3, mem_size=4)],
    [Operand(type="reg", reg="eax"), Operand(type="reg", reg="al")],
    [Operand(type="mem", mem_size=4), Operand(type="imm", imm=3)],
    [Operand(type="reg", reg="eax"), Operand(type="mem", mem_size=4)], [],
    None, False, 0, "not-operands",
])
def test_strict_shapes_do_not_widen_legacy_cmp_test(kind, bad):
    assert not _cmp_sub_zf_producer(kind, bad)
    other = ("sub" if kind == "cmp" else "cmp", pair())
    assert _merge_flag_states([(kind, bad), other]) != ("__cmp_sub_zf", [])


def translate(data):
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator
    base = 0x11000
    config._install([config.Section(".text", base, len(data), 0, len(data), True)],
                    entry_point=base, kernel_thunk_addr=base, origin="cmp-sub-guard")
    info = {"start": hex(base), "end": base + len(data), "_addr": base}
    return FunctionTranslator(data, {base: info}).translate_function(base, info)


def test_real_zero_join_and_overwritten_destination_snapshot():
    # JECXZ alternate; CMP eax,3; JMP join; SUB ebx,4; MOV ebx,9; JE join.
    code = translate(bytes.fromhex("E30583F803EB0883EB04BB090000007401C3C3"))
    assert "int _cmp_sub_zf = 0;" in code
    assert "_cmp_sub_zf = (_fa == 0); /* SUB post-write ZF */" in code
    assert "RECOMP_FLAGS_UNRESOLVED" not in code
    for kind in ("test", "add", "dec", "comiss"):
        assert not _cmp_sub_zf_producer(kind, pair())


@pytest.mark.parametrize("consumer", ["7201c3c3", "7601c3c3", "7e01c3c3", "7a01c3c3",
                                     "9fc3", "9cc3", "11d0c3", "19d0c3",
                                     "0f95c0c3", "0f45c7c3"])
def test_actual_nonzero_readers_refuse(consumer):
    code = translate(bytes.fromhex("E30583F803EB0383EB04" + consumer))
    # Existing CF/virtual-FLAGS paths are outside NEW marker admission.
    assert "int _cmp_sub_zf" not in code


@pytest.mark.parametrize("image", [
    "E30A83F803E8F60F0000EB0383EB047401C3C3",  # any CALL
    "E30685C09090EB0383EB047401C3C3",  # normalized TEST is not actual CMP
    "E305909090EB0383EB047401C3C3",  # missing producer
    "83F803740583EB04EBF9C3",  # SUB backedge directly into CMP/SUB join
    "7401C3C3",  # entry with no producer
])
def test_actual_cfg_missing_normalized_test_call_backedge_entry_refuse(image):
    code = translate(bytes.fromhex(image))
    assert "RECOMP_FLAGS_UNRESOLVED" in code
    assert "int _cmp_sub_zf" not in code


@pytest.mark.parametrize("reader", ["11C0", "19C0", "0F92C0", "9F", "7401C3"])
def test_later_block_reader_cannot_recover_generic_flags(reader):
    # Both JE successors reach a neutral JMP block before a later flag reader.
    body = translate(bytes.fromhex("E30583F803EB0383EB04740190EB0090" + reader + "C3"))
    assert "int _cmp_sub_zf = 0;" in body
    assert "RECOMP_FLAGS_UNRESOLVED" in body


def test_missing_predecessor_preserves_zero_only_taint():
    marker = ("__cmp_sub_zf", [])
    assert _incoming_flag_state([1, 2], {1: marker}, False,
                                preserve_cmp_sub_taint=True) == ("__cmp_sub_zf_blocked", [])


def test_zero_only_taint_reentering_entry_is_not_generic_flags():
    assert _incoming_flag_state([1], {1: ("__cmp_sub_zf", [])}, True,
                                preserve_cmp_sub_taint=True) == ("__cmp_sub_zf_blocked", [])


def test_cmc_cannot_discard_zero_only_provenance_before_adc():
    body = translate(bytes.fromhex("E30583F800EB0383EB047404F583D000C3"))
    assert "int _cmp_sub_zf = 0;" in body
    assert '"cmc", 0x0001100Cu' in body
    assert "RECOMP_FLAGS_UNRESOLVED" in body
