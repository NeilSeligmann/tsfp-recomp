"""Physical CMP32 restoration within the opt-in float-taint cone."""

import copy
import pytest
from tools.recomp.disasm import Operand
from tools.recomp.lifter import _float_cmp_zf_producer, _make_condition
from tools.recomp.translator import (
    _float_cmp_zf_cones,
    _float_relation_snapshot_blocks,
    _incoming_flag_state,
)
from tools.recomp.test_float_relation_provenance import cfg, translate, called_join

PAIR = [Operand(type="mem", mem_base="esi", mem_disp=0x2C, mem_size=4), Operand(type="imm", imm=5)]


@pytest.mark.parametrize(
    "field,value",
    [
        ("mem_size", 1),
        ("mem_size", 2),
        ("mem_size", 8),
        ("mem_size", True),
        ("mem_scale", True),
        ("mem_scale", 3),
        ("mem_disp", True),
        ("mem_disp", 0x100000000),
        ("mem_base", "si"),
        ("mem_index", "xmm0"),
        ("mem_seg", "fs"),
        ("reg", "eax"),
        ("imm", 0),
    ],
)
def test_strict_memory_shape(field, value):
    pair = copy.deepcopy(PAIR)
    assert _float_cmp_zf_producer("cmp", pair)
    setattr(pair[0], field, value)
    assert not _float_cmp_zf_producer("cmp", pair)


@pytest.mark.parametrize("value", [True, False, None, 1.0, "5", 0x100000000, -0x80000001])
def test_strict_immediate(value):
    pair = copy.deepcopy(PAIR)
    pair[1].imm = value
    assert not _float_cmp_zf_producer("cmp", pair)


@pytest.mark.parametrize("kind", ["test", "sub", "add", "dec", "cmp-pseudo", "COMISS"])
def test_only_actual_cmp(kind):
    assert not _float_cmp_zf_producer(kind, PAIR)


@pytest.mark.parametrize(
    "suffix,expected",
    [
        ("837e2c05750190c3", True),
        ("83f805750190c3", True),
        ("66837e2c05750190c3", False),
        ("807e2c05750190c3", False),
        ("f7462c05000000750190c3", False),
        ("837e2c05f5750190c3", False),
        ("837e2c05e800000000750190c3", False),
        ("837e2c05770190c3", False),
        ("837e2c050f95c0c3", False),
    ],
)
def test_real_cfg_cones(suffix, expected):
    raw = called_join(bytes.fromhex(suffix))
    blocks = cfg(raw)
    _, order = _float_relation_snapshot_blocks(blocks, 0x11000)
    approved = _float_cmp_zf_cones(blocks, 0x11000, order)
    assert bool(approved) is expected
    text = translate(raw)
    if expected:
        assert "(_fa != _fb)" in text
    else:
        assert "RECOMP_FLAGS_UNRESOLVED" in text
    assert not _float_cmp_zf_cones(blocks, 0x11000, order, [0x11005])


@pytest.mark.parametrize("reader", ["83d000", "83d800", "f5", "9f", "0f9ac0", "0f92c0", "770190"])
def test_late_nonzero_readers(reader):
    text = translate(called_join(bytes.fromhex("837e2c057500" + reader + "c3")))
    assert "RECOMP_FLAGS_UNRESOLVED" in text


def test_incoming_domain_and_default_api():
    marker = ("__float_cmp_zf", [])
    blocked = ("__float_relation_blocked", [])
    assert _incoming_flag_state([1, 2], {1: marker, 2: marker}, False) is None
    assert (
        _incoming_flag_state([1, 2], {1: marker, 2: marker}, False, preserve_float_taint=True)
        == marker
    )
    for other in [
        None,
        ("__zf_snapshot", []),
        ("__float_relation_comiss", []),
        ("cmp", PAIR),
        blocked,
    ]:
        assert (
            _incoming_flag_state([1, 2], {1: marker, 2: other}, False, preserve_float_taint=True)
            == blocked
        )
    assert _make_condition("jne", marker[0], []) == ("(_fa != _fb)", "not equal / not zero")
    for cc in ["jb", "jbe", "jp", "jo", "js", "jg"]:
        assert _make_condition(cc, marker[0], []) is None


@pytest.mark.parametrize(
    "tail", ["6685c0750190c3", "837e2c05ffc0750190c3", "837e2c05f8750190c3", "837e2c05f9750190c3"]
)
def test_normalized_test_partial_and_carry_writers_refuse(tail):
    text = translate(called_join(bytes.fromhex(tail)))
    assert "RECOMP_FLAGS_UNRESOLVED" in text


def test_memory_change_after_cmp_uses_single_snapshot():
    text = translate(called_join(bytes.fromhex("837e2c05c7462c05000000750190c3")))
    assert "(_fa != _fb)" in text
    assert text.count("_fa = (uint32_t)(MEM32(esi + 0x2C))") == 1


def test_domain_cycle_and_missing_entry_refuse():
    raw = called_join(bytes.fromhex("837e2c05750190c3"))
    blocks = cfg(raw)
    _, order = _float_relation_snapshot_blocks(blocks, 0x11000)
    assert _float_cmp_zf_cones(blocks, 0x11000, order)
    assert not _float_cmp_zf_cones(blocks, 0x99999, order)
    altered = copy.deepcopy(blocks)
    by_start = {b.start: b for b in altered}
    by_start[order[-1]].successors.append(order[0])
    assert not _float_cmp_zf_cones(altered, 0x11000, order)
