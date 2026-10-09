"""Patch27 local acyclic DEC32/byte TEST ZF provenance and persistent taint."""
import pytest

from tools.recomp.disasm import Operand
from tools.recomp.lifter import _dec_test_zf_producer, _make_condition
from tools.recomp.translator import _incoming_flag_state
from tools.recomp.test_cmp_sub_zf_join import translate


def dec(reg="eax", **kwargs):
    return [Operand(type="reg", reg=reg, **kwargs)]


def mem_test_ops(**kwargs):
    return [Operand(type="mem", mem_base="ebp", mem_size=1, **kwargs),
            Operand(type="imm", imm=128)]


@pytest.mark.parametrize("kind,ops", [
    ("dec", dec()), ("dec", dec("ebx")), ("test", mem_test_ops()),
    ("test", [Operand(type="mem", mem_disp=0x771688, mem_size=1), Operand(type="imm", imm=64)]),
])
def test_strict_supported(kind, ops):
    assert _dec_test_zf_producer(kind, ops)


@pytest.mark.parametrize("kind,ops", [
    ("dec", dec("al")), ("dec", dec("ax")), ("dec", dec("unknown")),
    ("dec", dec(mem_disp=1)), ("dec", dec(mem_scale=True)),
    ("dec", dec(imm=0)), ("dec", dec(mem_base="eax")),
    ("dec", dec(mem_size=4)), ("dec", dec(mem_size=True)), ("dec", []), ("dec", None),
    ("dec", False), ("dec", [False]), ("inc", dec()),
    ("test", [Operand(type="reg", reg="eax"), Operand(type="reg", reg="eax")]),
    ("test", [Operand(type="mem", mem_size=4), Operand(type="imm", imm=128)]),
    ("test", [Operand(type="mem", mem_size=2), Operand(type="imm", imm=128)]),
    ("test", [Operand(type="mem", mem_size=True), Operand(type="imm", imm=128)]),
    ("test", [Operand(type="mem", mem_size=1), Operand(type="imm", imm=True)]),
    ("test", [Operand(type="mem", mem_size=1), Operand(type="imm", imm=-1)]),
    ("test", [Operand(type="mem", mem_size=1), Operand(type="imm", imm=256)]),
    ("test", mem_test_ops(mem_seg="fs")), ("test", mem_test_ops(mem_index="ax")),
    ("test", mem_test_ops(mem_index="ecx", mem_scale=3)),
    ("test", mem_test_ops(mem_disp=True)), ("test", None),
])
def test_malformed_or_other_shapes_refuse(kind, ops):
    assert not _dec_test_zf_producer(kind, ops)


# JECXZ alternate; DEC EAX; JMP join; TEST byte[EBP+1C],80; join.
PREFIX = "E30348EB04F6451C80"


def test_actual_cfg_zero_join_overwritten_register_and_memory():
    s = translate(bytes.fromhex(PREFIX + "7501C3C3"))
    assert "int _dec_test_zf = 0;" in s
    assert "DEC32 post-write ZF" in s and "byte TEST executed ZF" in s
    assert "RECOMP_FLAGS_UNRESOLVED" not in s
    # The physical producer snapshots survive later writes to their input.
    s = translate(bytes.fromhex("E30848B807000000EB08F6451C80C6451C007501C3C3"))
    assert "int _dec_test_zf = 0;" in s
    assert "RECOMP_FLAGS_UNRESOLVED" not in s


@pytest.mark.parametrize("suffix", ["7201C3C3", "7601C3C3", "7E01C3C3", "0F95C0C3", "9FC3", "9CC3", "F5C3"])
def test_new_admission_only_first_je_jne(suffix):
    s = translate(bytes.fromhex(PREFIX + suffix))
    assert "int _dec_test_zf" not in s


@pytest.mark.parametrize("hexcode", [
    "E30848E8F70F0000EB04F6451C807501C3C3",  # CALL after DEC
    "E30390EB04F6451C807501C3C3",  # missing producer
    "E30566FFC8EB04F6451C807501C3C3",  # narrow DEC
    "E30348EB0285C07501C3C3",  # normalized TEST eax,eax is not physical byte TEST
    "E30348EB04F6451C8075F5C3",  # real cycle back toward producer
    "7501C3C3",  # unproved entry flags
    "E30348EB04F6451C80750190FFE0",  # unknown indirect edge
])
def test_actual_interposed_call_missing_cycle_or_shape_refuses(hexcode):
    s = translate(bytes.fromhex(hexcode))
    assert "int _dec_test_zf" not in s


@pytest.mark.parametrize("reader", ["F511C0", "19C0", "0F92C0", "0F94C0", "9F", "E8F00F00007501C3", "83C8017501C3"])
def test_taint_does_not_recover_generic_flags_after_loss(reader):
    s = translate(bytes.fromhex(PREFIX + "750190EB0090" + reader + "C3"))
    assert "int _dec_test_zf = 0;" in s
    assert "RECOMP_FLAGS_UNRESOLVED" in s


def test_real_complete_cmp_can_establish_new_owner():
    s = translate(bytes.fromhex(PREFIX + "75019083F8017201C3C3"))
    assert "int _dec_test_zf = 0;" in s
    assert "RECOMP_FLAGS_UNRESOLVED" not in s


def test_unknown_or_entry_merge_preserves_blocked_taint():
    m = ("__dec_test_zf", [])
    b = ("__dec_test_zf_blocked", [])
    assert _incoming_flag_state([1, 2], {1:m}, False) == b
    assert _incoming_flag_state([1], {1:m}, True) == b
    assert _incoming_flag_state([], {}, True, entry_state=m) == b
    assert _incoming_flag_state([1, 2], {1:m, 2:None}, False) == b
    for cc in ("je", "jz", "jne", "jnz"):
        assert _make_condition(cc, *m)
    for cc in ("jle", "jb", "ja", "jo", "jp", "js"):
        assert _make_condition(cc, *m) is None


def test_missing_cfg_node_or_unreachable_entry_is_not_evidence():
    from tools.recomp.disasm import BasicBlock
    from tools.recomp.translator import _dec_test_snapshot_blocks
    assert _dec_test_snapshot_blocks([], 1) == set()
    assert _dec_test_snapshot_blocks([BasicBlock(1, [], [2])], 1) == set()
    assert _dec_test_snapshot_blocks([BasicBlock(1), BasicBlock(2)], 1) == set()
    assert _dec_test_snapshot_blocks([BasicBlock(1, [], [1])], 1) == set()


def test_snapshot_name_is_not_a_guest_callable_collision():
    from tools.recomp.lifter import _func_ident
    assert _func_ident(0x11000, "_dec_test_zf") == "_dec_test_zf_00011000"


def test_malformed_marker_cannot_supply_initial_zero():
    from tools.recomp.translator import _merge_flag_states
    bad = ("__dec_test_zf", dec())
    assert _merge_flag_states([bad, bad]) is None
    assert _make_condition("je", *bad) is None


def test_reverse_address_neutral_chain_retains_zero_only_taint():
    # Real acyclic reviewer witness: initial CF=1 makes ADC fallback wrong.
    code = bytes.fromhex("EB0E83D000C3EBFAEBFCEBFCEBFCEBFCE30348EB04F6451C807500EBF1")
    body = translate(code)
    assert "int _dec_test_zf = 0;" in body
    assert "RECOMP_FLAGS_UNRESOLVED" in body
    assert "0x00011002" in body
