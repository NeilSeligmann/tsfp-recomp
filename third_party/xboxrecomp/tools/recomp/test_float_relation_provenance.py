"""Private local-phi relation policy, distinct from legacy SSE states."""

import copy
import pytest
from tools.recomp.disasm import Operand, Disassembler
from tools.recomp.lifter import _float_relation_producer, _make_condition
from tools.recomp.translator import (
    _incoming_flag_state,
    _merge_flag_states,
    _float_relation_snapshot_blocks,
    FunctionTranslator,
)
from tools.recomp import config

PAIR = [Operand(type="reg", reg="xmm0"), Operand(type="reg", reg="xmm1")]
MARK = ("__float_relation_comiss", [])
BLOCKED = ("__float_relation_blocked", [])


def translate(raw):
    a = 0x11000
    config._install(
        [config.Section(".text", a, len(raw), 0, len(raw), True)],
        entry_point=a,
        kernel_thunk_addr=a,
        origin="private-float-relation",
    )
    info = {"start": hex(a), "end": a + len(raw), "_addr": a}
    return FunctionTranslator(raw, {a: info}, local_float_relations=True).translate_function(
        a, info
    )


def cfg(raw):
    d = Disassembler()
    return d.build_basic_blocks(
        d.disassemble_function(raw, 0x11000, 0x11000 + len(raw)), 0x11000, 0x11000 + len(raw)
    )


def called_join(suffix=b"\xc3"):
    return bytes.fromhex("E8FB0F0000E3050F2FC1EB030F2FD376029090") + suffix


def test_explicit_taint_api_no_legacy_alias():
    assert _merge_flag_states([MARK, MARK]) is None
    assert _incoming_flag_state([1, 2], {1: MARK, 2: MARK}, False) is None
    assert (
        _incoming_flag_state([1, 2], {1: MARK, 2: MARK}, False, preserve_float_taint=True) == MARK
    )
    for other in [
        None,
        ("__zf_snapshot", []),
        ("__cmp_sub_zf", []),
        ("__dec_test_zf", []),
        ("comiss", PAIR),
        BLOCKED,
        ("__float_relation_ucomiss", []),
    ]:
        assert (
            _incoming_flag_state([1, 2], {1: MARK, 2: other}, False, preserve_float_taint=True)
            == BLOCKED
        )
    assert _incoming_flag_state([1, 2], {1: MARK}, False, preserve_float_taint=True) == BLOCKED
    assert _incoming_flag_state([], {}, True, MARK, preserve_float_taint=True) == BLOCKED
    assert _make_condition("jbe", "__float_relation_comiss", [])
    assert _make_condition("ja", "__float_relation_ucomiss", [])
    for cc in ("je", "jb", "jp", "jo", "js", "jbe"):
        assert _make_condition(cc, "__float_relation_blocked", []) is None
    assert _make_condition("jbe", "__float_relation_comiss", PAIR) is None


@pytest.mark.parametrize(
    "field,value",
    [
        ("mem_size", 2),
        ("mem_size", True),
        ("mem_size", 8),
        ("mem_base", "ax"),
        ("mem_index", "xmm0"),
        ("mem_scale", True),
        ("mem_scale", 3),
        ("mem_disp", True),
        ("mem_seg", "fs"),
        ("mem_disp", 0x100000000),
    ],
)
def test_strict_memory_shape(field, value):
    pair = [copy.deepcopy(PAIR[0]), Operand(type="mem", mem_base="eax", mem_size=4)]
    assert _float_relation_producer("comiss", pair)
    setattr(pair[1], field, value)
    assert not _float_relation_producer("comiss", pair)


@pytest.mark.parametrize(
    "suffix,reader",
    [
        ("83D000", "adc"),
        ("83D800", "sbb"),
        ("F5", "cmc"),
        ("9F", "lahf"),
        ("9C", "pushfd"),
        ("0F9AC0", "setp"),
        ("0F42C1", "cmovb"),
        ("7200", "jb"),
        ("7A00", "jp"),
        ("7500", "jne"),
        ("437600", "jbe"),
        ("E8E80F00007600", "jbe"),
        ("83F8007600", "jbe"),
    ],
)
def test_actual_later_readers_never_regain_fallback(suffix, reader):
    code = translate(called_join(bytes.fromhex(suffix) + b"\xc3"))
    assert f'"{reader}"' in code and "RECOMP_FLAGS_UNRESOLVED" in code
    assert 'RECOMP_FLAGS_UNRESOLVED(_flags, "jbe", 0x0001100Fu)' not in code


def test_actual_fresh_scalar_writer_restores_bounded_relation():
    code = translate(called_join(bytes.fromhex("E8E80F00000F2FC17600C3")))
    assert "RECOMP_FLAGS_UNRESOLVED" not in code


def test_actual_entry_interior_and_missing_endpoint_refuse():
    blocks = cfg(called_join())
    consumers, order = _float_relation_snapshot_blocks(blocks, 0x11000)
    assert consumers and order
    assert _float_relation_snapshot_blocks(blocks, 0x11000, [0x11007]) == ({}, [])
    assert _float_relation_snapshot_blocks(blocks, 0x11001) == ({}, [])
    assert _float_relation_snapshot_blocks(blocks[:-1], 0x11000) == ({}, [])


def test_actual_cycle_downstream_refuses_initial_admission():
    raw = called_join(bytes.fromhex("EBFE"))
    assert _float_relation_snapshot_blocks(cfg(raw), 0x11000) == ({}, [])
    # T1164 patch 36: the private relation domain still refuses the cycle, but the jbe join is
    # now resolved by the per-consumer COMISS admission (the call sits in the entry block, not
    # between a producer and the consumer), so it no longer falls to the unresolved trap.
    assert "RECOMP_FLAGS_UNRESOLVED" not in translate(raw)
