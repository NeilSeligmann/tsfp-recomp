"""Narrow mixed32-bit ADD/SUB snapshot admission and actual CFG refusals."""

import hashlib

import pytest

from tools.recomp.disasm import Operand
from tools.recomp.lifter import _make_condition
from tools.recomp.translator import _incoming_flag_state, _merge_flag_states


def pair(reg: str = "eax", source: int = 4) -> list[Operand]:
    return [Operand(type="reg", reg=reg), Operand(type="imm", imm=source)]


def test_measured_jle_only_and_marker_propagation() -> None:
    states = [("add", pair()), ("sub", pair())]
    marker = _merge_flag_states(states)
    assert marker == ("__signed_snapshot", [])
    for cc in ("jle", "jng"):
        assert _make_condition(cc, *marker)[0] == "(_signed_zf || _signed_sf != _signed_of)"
    for cc in ("je", "jne", "jl", "jg", "jge", "js", "jns", "jo", "jno", "jb", "jbe", "ja", "jp"):
        assert _make_condition(cc, *marker) is None
    assert _merge_flag_states([marker, states[0]]) == marker
    assert _merge_flag_states(states, False) != marker
    assert _make_condition("jle", "__signed_snapshot", pair()) is None


@pytest.mark.parametrize(
    "bad",
    [
        None,
        (None, []),
        ("cmp", pair()),
        ("dec", [Operand(type="reg", reg="eax")]),
        ("sub", pair("ax")),
        ("sub", pair("unknown")),
        ("sub", pair("xmm0")),
        ("sub", pair(source=0x100000000)),
        ("sub", pair(source=-0x80000001)),
        ("sub", [Operand(type="reg", reg="eax"), Operand(type="reg", reg="al")]),
        ("sub", [Operand(type="mem", mem_size=4), Operand(type="imm", imm=4)]),
        ("sub", [Operand(type="reg", reg="eax"), Operand(type="mem", mem_size=4)]),
        ("__signed_snapshot", pair()),
    ],
)
def test_missing_and_malformed_producers_refuse(bad: object) -> None:
    state = _merge_flag_states([("add", pair()), bad])
    assert state != ("__signed_snapshot", [])
    if state is not None:
        assert _make_condition("jle", *state) is None


def test_entry_and_unknown_backedge_refuse() -> None:
    a = ("add", pair())
    b = ("sub", pair())
    assert _incoming_flag_state([1, 2], {1: a}, False) is None
    assert _incoming_flag_state([1, 2], {1: a, 2: b}, True) is None


def translate(image: bytes) -> str:
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator

    base = 0x11000
    config._install(
        [config.Section(".text", base, len(image), 0, len(image), True)],
        entry_point=base,
        kernel_thunk_addr=base,
        origin="signed-join-fixture",
    )
    info = {"start": hex(base), "end": base + len(image), "_addr": base}
    return FunctionTranslator(image, {base: info}).translate_function(base, info)


def join(consumer: bytes) -> str:
    # TESTecx; fork ADD/SUBeax4; MOVedi,eax neutral; consumer.
    return translate(bytes.fromhex("85c9740583c004eb0383e80489c7") + consumer)


def test_actual_neutral_block_and_destination_overwrite() -> None:
    code = join(bytes.fromhex("b8777777777e01c3c3"))
    assert "int _signed_zf" in code
    assert "RECOMP_FLAGS_UNRESOLVED" not in code
    assert "RECOMP_ADD_OF(_fa, _fb, 31)" in code
    assert "RECOMP_SUB_OF(_fa, _fb, 31)" in code


@pytest.mark.parametrize(
    "consumer",
    [
        "7401c3c3",
        "7201c3c3",
        "7a01c3c3",
        "7c01c3c3",
        "7f01c3c3",
        "9fc3",
        "9cc3",
        "11d0c3",
        "19d0c3",
        "d1d0c3",
        "d1d8c3",
    ],
)
def test_other_consumers_are_actual_traps(consumer: str) -> None:
    assert "RECOMP_FLAGS_UNRESOLVED" in join(bytes.fromhex("7e00" + consumer))


def test_call_path_refuses_new_marker() -> None:
    # A call anywhere keeps the conservative NEW-marker boundary intact.
    code = join(bytes.fromhex("e8000000007e01c3c3"))
    assert "int _signed_zf" not in code
    assert "RECOMP_FLAGS_UNRESOLVED" in code


def test_actual_missing_and_entry_backedge_paths_refuse() -> None:
    for image in ["e30583c004eb039090907e01c3c3", "7e0383c004ebf983e804c3"]:
        code = translate(bytes.fromhex(image))
        assert "int _signed_zf" not in code
        assert "RECOMP_FLAGS_UNRESOLVED" in code


def test_unrelated_no_marker_body_identity() -> None:
    from tools.recomp.test_flag_join import translate_join

    code = translate_join()
    assert "_signed_" not in code
    assert (
        hashlib.sha256(code.encode()).hexdigest()
        == "edabbf14aa8b466d9d635a42d2de924eb129a1047edbf2013057dcf52d866556"
    )


def test_non_signed_join_keeps_legacy_zf_behavior() -> None:
    code = join(bytes.fromhex("7401c3c3"))
    assert "int _signed_zf" not in code
    assert "RECOMP_FLAGS_UNRESOLVED" not in code


def test_neutral_separate_block_reaches_signed_consumer() -> None:
    code = join(bytes.fromhex("eb00907e01c3c3"))
    assert "int _signed_zf" in code
    assert "RECOMP_FLAGS_UNRESOLVED" not in code


@pytest.mark.parametrize(
    "bad",
    [
        pair(source=True),
        pair(source=False),
        [Operand(type="reg", reg="eax", mem_size=1), Operand(type="imm", imm=4)],
        [Operand(type="reg", reg="eax", mem_size=4), Operand(type="imm", imm=4)],
        [Operand(type="reg", reg="eax"), Operand(type="reg", reg="ebx", mem_size=1)],
    ],
)
def test_contradictory_width_and_bool_do_not_admit(bad: object) -> None:
    state = _merge_flag_states([("add", pair()), ("sub", bad)])
    assert state != ("__signed_snapshot", [])
    if state is not None:
        assert _make_condition("jle", *state) is None
