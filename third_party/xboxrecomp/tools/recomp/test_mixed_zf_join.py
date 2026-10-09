"""Only computed ZF may cross compatible mixed CMP/TEST joins (patch23)."""
import pytest
from tools.recomp.disasm import Operand
from tools.recomp.lifter import _make_condition
from tools.recomp.translator import _merge_flag_states, _incoming_flag_state


def pair(reg, value):
    return [Operand(type="reg", reg=reg), Operand(type="imm", imm=value)]


def test_mixed_width_and_operation_merge_only_zf():
    a = ("cmp", pair("eax", 0))
    b = ("test", pair("cl", 4))
    state = _merge_flag_states([a, b])
    assert state == ("__zf_snapshot", [])
    assert _make_condition("je", *state)[0] == "_zf_snapshot"
    assert _make_condition("jne", *state)[0] == "!_zf_snapshot"
    for cc in ("jb", "ja", "jl", "jge", "jp", "jo", "js"):
        assert _make_condition(cc, *state) is None


def test_snapshot_propagates_without_claiming_other_flags():
    marker = ("__zf_snapshot", [])
    assert _merge_flag_states([marker, ("cmp", pair("edx", 8))]) == marker
    assert _merge_flag_states([marker, marker]) == marker


@pytest.mark.parametrize("bad", [None, (None, []), ("sub", pair("eax", 1)),
    ("dec", [Operand(type="reg", reg="eax")]), ("cmp", []),
    ("test", [Operand(type="imm", imm=4), Operand(type="imm", imm=8)]),
    ("comiss", pair("xmm0", 0))])
def test_missing_or_unsupported_producer_stays_unknown(bad):
    assert _merge_flag_states([("cmp", pair("eax", 0)), bad]) is None


def test_entry_and_missing_backedge_stay_unknown():
    a = ("cmp", pair("eax", 0))
    b = ("test", pair("cl", 4))
    assert _incoming_flag_state([1, 2], {1: a}, False) is None
    assert _incoming_flag_state([1, 2], {1: a, 2: b}, True) is None


def synthetic_join(consumer):
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator
    base = 0x11000
    # TEST ecx,ecx; JZ alternate; CMP eax,edx; JMP join;
    # alternate: TEST bl,4; join: chosen consumer.
    image = bytes.fromhex("85c9740439d0eb03f6c304") + consumer
    config._install([config.Section(".text", base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base, origin="mixed-zf-test")
    info = {"start": hex(base), "end": base + len(image), "_addr": base}
    return FunctionTranslator(image, {base: info}).translate_function(base, info)


@pytest.mark.parametrize("consumer", ["7201c3c3", "7001c3c3", "7a01c3c3",
                                      "9fc3", "9cc3", "11d0c3", "19d0c3",
                                      "d1d0c3", "d1d8c3"])
def test_non_zf_readers_remain_actual_traps(consumer):
    code = synthetic_join(bytes.fromhex(consumer))
    assert "RECOMP_FLAGS_UNRESOLVED" in code, code
    assert "int _zf_snapshot = 0;" in code


def test_setne_and_cmovne_consume_only_saved_bit():
    for consumer in ("0f95c0c3", "0f45c7c3"):
        code = synthetic_join(bytes.fromhex(consumer))
        assert "!_zf_snapshot" in code
        assert "RECOMP_FLAGS_UNRESOLVED" not in code
        assert "_zf_snapshot = (_fa == _fb);" in code
        assert "_zf_snapshot = ((_fa & _fb) == 0);" in code


def test_no_mixed_marker_body_is_byte_identical():
    import hashlib
    from tools.recomp.test_flag_join import translate_join
    code = translate_join()
    assert "_zf_snapshot" not in code
    assert hashlib.sha256(code.encode()).hexdigest() == (
        "edabbf14aa8b466d9d635a42d2de924eb129a1047edbf2013057dcf52d866556")


@pytest.mark.parametrize("bad", [
    pair("xmm1", 4), pair("unknownreg", 4),
    [Operand(type="reg", reg="al"), Operand(type="reg", reg="ebx")],
    [Operand(type="mem", mem_size=0), Operand(type="imm", imm=4)],
    [Operand(type="mem", mem_size=1), Operand(type="reg", reg="eax")],
    [Operand(type="mem", mem_size=4, mem_base="xmm0"), Operand(type="imm", imm=4)],
    [Operand(type="mem", mem_size=4), Operand(type="mem", mem_size=4)],
    [Operand(type="imm", imm=4), Operand(type="reg", reg="eax")],
    [Operand(type="reg", reg="eax"), Operand(type="imm", imm=0x100000000)],
])
def test_new_marker_rejects_malformed_integer_shapes(bad):
    assert _merge_flag_states([("cmp", pair("eax", 0)), ("test", bad)]) is None


def test_malformed_snapshot_marker_cannot_answer_condition():
    assert _make_condition("je", "__zf_snapshot", pair("eax", 0)) is None


def test_actual_call_path_refuses_new_snapshot_admission():
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator
    base = 0x11000
    # JECXZ alternate; CMP EAX,0; CALL12000; JMP join;
    # alternate TEST BL,4; joined JNE. The callee may change flags.
    image = bytes.fromhex("E30A83F800E8F60F0000EB03F6C3047501C3C3")
    config._install([config.Section(".text", base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base, origin="mixed-zf-call-test")
    info = {"start": hex(base), "end": base + len(image), "_addr": base}
    code = FunctionTranslator(image, {base: info}).translate_function(base, info)
    assert "sub_00012000" in code
    assert "RECOMP_FLAGS_UNRESOLVED" in code
    assert "_zf_snapshot" not in code


# TSFP-LOCAL (patch28): per-consumer admission inside call-containing functions.
def call_function_join(consumer, alternate="f6c304", skip="03"):
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator
    base = 0x11000
    # CALL 12000 (unrelated, before any producer); TEST ecx,ecx; JZ alternate;
    # CMP eax,edx; JMP join; alternate; join: chosen consumer.
    image = (bytes.fromhex("e8fb0f0000" "85c9" "7404" "39d0" "eb") + bytes.fromhex(skip)
             + bytes.fromhex(alternate) + consumer)
    config._install([config.Section(".text", base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base, origin="mixed-zf-local-test")
    info = {"start": hex(base), "end": base + len(image), "_addr": base}
    return FunctionTranslator(image, {base: info}).translate_function(base, info)


def test_call_function_admits_join_with_call_free_producers():
    code = call_function_join(bytes.fromhex("7501c3c3"))
    assert "sub_00012000" in code
    assert "_zf_snapshot = (_fa == _fb);" in code
    assert "_zf_snapshot = ((_fa & _fb) == 0);" in code
    assert "RECOMP_FLAGS_UNRESOLVED" not in code, code


@pytest.mark.parametrize("consumer", ["7201c3c3", "7001c3c3", "7a01c3c3", "9fc3",
                                      "9cc3", "11d0c3", "d1d0c3"])
def test_call_function_non_zf_readers_get_no_new_admission(consumer):
    # Only a first JE/JNE reader is admitted: any other reader keeps the
    # pre-patch28 (legacy) body, with no computed-ZF marker at all.
    code = call_function_join(bytes.fromhex(consumer))
    assert "_zf_snapshot" not in code, code


def test_call_after_producer_in_a_predecessor_still_refuses():
    # alternate: TEST bl,4; CALL 12000 (callee flags unproved); join: JNE
    code = call_function_join(bytes.fromhex("7501c3c3"),
                              alternate="f6c304" "e8f10f0000", skip="08")
    assert "RECOMP_FLAGS_UNRESOLVED" in code, code
    assert "_zf_snapshot" not in code


def test_merge_local_flag_is_the_only_new_switch():
    a, b = ("cmp", pair("eax", 0)), ("test", pair("cl", 4))
    assert _merge_flag_states([a, b], False, False) is None
    assert _merge_flag_states([a, b], False, False, False, True) == ("__zf_snapshot", [])
