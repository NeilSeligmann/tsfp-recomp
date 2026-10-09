"""Strict physical TEST32 restoration in the opt-in float-taint domain."""

import copy
import pytest
from tools.recomp.disasm import Operand
from tools.recomp.lifter import _float_test32_producer, _make_condition
from tools.recomp.translator import _float_test32_cones, _float_relation_snapshot_blocks
from tools.recomp.test_float_relation_provenance import cfg, translate, called_join

PAIR = [Operand(type="reg", reg="eax"), Operand(type="reg", reg="eax")]


@pytest.mark.parametrize(
    "field,value",
    [
        ("reg", "ax"),
        ("reg", "xmm0"),
        ("mem_size", True),
        ("mem_size", 4),
        ("mem_size", 2),
        ("mem_base", "eax"),
        ("mem_seg", "fs"),
        ("mem_disp", True),
        ("mem_disp", 1),
        ("mem_scale", True),
        ("mem_scale", 2),
        ("imm", 0),
        ("type", "mem"),
    ],
)
def test_strict_shape(field, value):
    assert _float_test32_producer("test", PAIR)
    for i in [0, 1]:
        pair = copy.deepcopy(PAIR)
        setattr(pair[i], field, value)
        assert not _float_test32_producer("test", pair)


@pytest.mark.parametrize("kind", ["cmp", "TEST", "test-pseudo", "and", "dec"])
def test_actual_mnemonic(kind):
    assert not _float_test32_producer(kind, PAIR)


@pytest.mark.parametrize(
    "tail,admitted",
    [
        ("85c0740190c3", True),
        ("85c0750190c3", True),
        ("85c07c0190c3", True),
        ("85c07d0190c3", True),
        ("6685c07c0190c3", False),
        ("84c07c0190c3", False),
        ("f7c0010000007c0190c3", False),
        ("85007c0190c3", False),
        ("85c0e8000000007c0190c3", False),
        ("85c0ffc07c0190c3", False),
        ("85c0f57c0190c3", False),
        ("85c07e0190c3", False),
        ("85c07f0190c3", False),
    ],
)
def test_actual_cones(tail, admitted):
    raw = called_join(bytes.fromhex(tail))
    blocks = cfg(raw)
    _, order = _float_relation_snapshot_blocks(blocks, 0x11000)
    assert bool(_float_test32_cones(blocks, 0x11000, order)) is admitted
    assert not _float_test32_cones(blocks, 0x11000, order, [0x11005])
    text = translate(raw)
    assert ("physical TEST32 snapshot" in text) is admitted
    if admitted:
        assert "RECOMP_FLAGS_UNRESOLVED" not in text
    else:
        assert "RECOMP_FLAGS_UNRESOLVED" in text


@pytest.mark.parametrize(
    "tail",
    [
        "83d000",
        "83d800",
        "f583d000",
        "9f",
        "0f9ac0",
        "770190",
        "7a0190",
        "ffc07d0190",
        "83f8007d0190",
        "e8000000007d0190",
    ],
)
def test_later_reader_kills(tail):
    assert "RECOMP_FLAGS_UNRESOLVED" in translate(
        called_join(bytes.fromhex("85c07d00" + tail + "c3"))
    )


@pytest.mark.parametrize("cc", ["jle", "jg", "jb", "ja", "jp", "jo", "js", "jbe"])
def test_unsupported_flags(cc):
    assert _make_condition(cc, "__float_test32", []) is None


def test_snapshot_survives_neutral_register_write():
    text = translate(called_join(bytes.fromhex("85c0b8000000007d00c3")))
    assert "_fa = (uint32_t)(eax) & (uint32_t)(eax)" in text
    assert "((int32_t)_fa >= 0)" in text
