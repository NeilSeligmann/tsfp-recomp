"""Only homogeneous single-precision snapshots may merge (TSFP patch22)."""
from tools.recomp.disasm import Operand
from tools.recomp.translator import _merge_flag_states


def pair(a, b):
    return [Operand(type="reg", reg=a), Operand(type="reg", reg=b)]


def test_comiss_different_operands_merge():
    a = ("comiss", pair("xmm1", "xmm0"))
    b = ("comiss", pair("xmm0", "xmm1"))
    assert _merge_flag_states([a, b]) == a


def test_ucomiss_different_operands_merge():
    a = ("ucomiss", pair("xmm1", "xmm0"))
    b = ("ucomiss", pair("xmm2", "xmm3"))
    assert _merge_flag_states([a, b]) == a


def test_missing_mixed_precision_and_operation_stay_unknown():
    a = ("comiss", pair("xmm1", "xmm0"))
    for b in [None, ("cmp", pair("eax", "edx")),
              ("ucomiss", pair("xmm0", "xmm1")),
              ("comisd", pair("xmm0", "xmm1")), ("comiss", [])]:
        assert _merge_flag_states([a, b]) is None
    assert _merge_flag_states([("comisd", pair("xmm1", "xmm0")),
                               ("comisd", pair("xmm0", "xmm1"))]) is None


def test_call_gate_preserves_identical_legacy_but_refuses_new_float_phi():
    from tools.recomp.translator import _incoming_flag_state
    for kind in ("comiss", "ucomiss"):
        a = (kind, pair("xmm0", "xmm1"))
        b = (kind, pair("xmm2", "xmm3"))
        assert _merge_flag_states([a, b], allow_zf_snapshot=False) is None
        assert _merge_flag_states([a, a], allow_zf_snapshot=False) == a
        assert _incoming_flag_state([1, 2], {1: a, 2: b}, False,
                                    allow_zf_snapshot=False) is None
        assert _incoming_flag_state([1, 2], {1: a}, False) is None
        assert _incoming_flag_state([1, 2], {1: a, 2: b}, True) is None
        assert _incoming_flag_state([1], {1: a}, True, entry_state=a) is None


def translate_float_image(image):
    from tools.recomp import config
    from tools.recomp.translator import FunctionTranslator
    base = 0x11000
    config._install([config.Section(".text", base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base, origin="float-call-guard")
    info = {"start": hex(base), "end": base + len(image), "_addr": base}
    return FunctionTranslator(image, {base: info}).translate_function(base, info)


def test_actual_float_call_cfg_refuses_new_phi_but_nocall_keeps_it():
    for opcode in ("2F", "2E"):
        image = bytes.fromhex("E30A0F" + opcode + "C1E8F60F0000EB030F" + opcode + "D37601C3C3")
        called = translate_float_image(image)
        assert "sub_00012000" in called
        assert 'RECOMP_FLAGS_UNRESOLVED(_flags, "jbe", 0x0001100Fu)' in called
        no_call = translate_float_image(image[:5] + b"\x90" * 5 + image[10:])
        assert "RECOMP_FLAGS_UNRESOLVED" not in no_call
        legacy = translate_float_image(image[:14] + b"\xc1" + image[15:])
        assert "RECOMP_FLAGS_UNRESOLVED" not in legacy
        mixed = bytearray(image); mixed[13] = 0x2e if opcode == "2F" else 0x2f
        assert "RECOMP_FLAGS_UNRESOLVED" in translate_float_image(bytes(mixed))
        missing = image[:12] + b"\x90" * 3 + image[15:]
        assert "RECOMP_FLAGS_UNRESOLVED" in translate_float_image(missing)
    assert "RECOMP_FLAGS_UNRESOLVED" in translate_float_image(bytes.fromhex("7601c3c3"))


def test_actual_called_join_executes_unresolved_trap(tmp_path):
    import shutil
    import subprocess
    from pathlib import Path

    import pytest
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("requires actual C compiler")
    root = Path(__file__).resolve().parents[4]
    vendor = root / "third_party/xboxrecomp"
    body = translate_float_image(bytes.fromhex("E30A0F2FC1E8F60F0000EB030F2FD37601C3C3"))
    source = tmp_path / "called.c"
    source.write_text('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n'
                      'void sub_00012000(void);\n' + body)
    for opt in ("-O0", "-O3"):
        exe = tmp_path / opt[1:]
        subprocess.run([compiler, opt, "-std=gnu11", "-I", str(vendor / "templates/runtime"),
                        str(source), str(root / "tests/c/float_call_guard_runner.c"),
                        "-lm", "-o", str(exe)], check=True, capture_output=True, timeout=30)
        for predecessor in ("0", "1"):
            run = subprocess.run([str(exe), predecessor], capture_output=True, timeout=5)
            assert run.returncode == 86, (opt, predecessor, run.returncode)


# T1164 patch 36: per-consumer COMISS/UCOMISS join admission in a call-containing function.
# call sub_00012000; test ecx,ecx; jz B; A: comiss xmm0,xmm1; jmp join; B: comiss xmm2,xmm3;
# join: jb taken; fall-through `mov eax,1; ret`; taken `mov eax,2; ret`. The call sits in the
# entry block, not between a producer and the consumer.
LOCAL_JOIN = "E8FB0F0000" "85C9" "7405" "0F{op}C1" "EB03" "0F{op}D3" "7206" "B801000000C3" "B802000000C3"


def local_join(opcode="2F", mutate=None):
    image = bytearray(bytes.fromhex(LOCAL_JOIN.format(op=opcode)))
    if mutate:
        mutate(image)
    return translate_float_image(bytes(image))


def test_call_elsewhere_still_resolves_the_float_join_per_consumer():
    for opcode in ("2F", "2E"):
        body = local_join(opcode)
        assert "sub_00012000" in body
        assert "RECOMP_FLAGS_UNRESOLVED" not in body
        assert "_fca < _fcb" in body


def test_call_between_producer_and_consumer_is_not_admitted():
    # path A: comiss; call; jmp join (the callee flags are unproved). The same CFG with the call
    # replaced by nops is admitted, so the call alone decides.
    template = "85C9" "740A" "0F2FC1" "{between}" "EB03" "0F2FD3" "7206" "B801000000C3" "B802000000C3"
    called = translate_float_image(bytes.fromhex(template.format(between="E8F40F0000")))
    assert "sub_00012000" in called
    assert "RECOMP_FLAGS_UNRESOLVED" in called
    free = translate_float_image(bytes.fromhex(template.format(between="9090909090")))
    assert "RECOMP_FLAGS_UNRESOLVED" not in free


def test_missing_or_mixed_producers_stay_unresolved():
    def nop_path_a(image):
        image[9:12] = b"\x90\x90\x90"

    def mixed(image):
        image[0xF] = 0x2E if image[0xF] == 0x2F else 0x2F

    for mutate in (nop_path_a, mixed):
        assert "RECOMP_FLAGS_UNRESOLVED" in local_join("2F", mutate)


def test_local_join_runtime_uses_the_executed_predecessor(tmp_path):
    import shutil
    import subprocess
    from pathlib import Path

    import pytest
    compiler = shutil.which("cc")
    if compiler is None:
        pytest.skip("requires actual C compiler")
    root = Path(__file__).resolve().parents[4]
    vendor = root / "third_party/xboxrecomp"
    for opcode in ("2F", "2E"):
        source = tmp_path / f"join{opcode}.c"
        source.write_text('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n'
                          'void sub_00012000(void);\n' + local_join(opcode))
        for opt in ("-O0", "-O3"):
            exe = tmp_path / f"run{opcode}{opt[1:]}"
            subprocess.run([compiler, opt, "-std=gnu11", "-I", str(vendor / "templates/runtime"),
                            str(source), str(root / "tests/c/float_local_join_runner.c"),
                            "-lm", "-o", str(exe)], check=True, capture_output=True, timeout=60)
            # args: ecx x0 x1 x2 x3. ecx != 0 runs path A (x0 vs x1), ecx == 0 path B (x2 vs x3).
            cases = [("1", "5", "2", "1", "9", 1), ("0", "5", "2", "1", "9", 2),
                     ("1", "1", "2", "9", "1", 2), ("0", "1", "2", "9", "1", 1),
                     ("1", "2", "2", "1", "9", 1), ("1", "nan", "2", "9", "1", 2),
                     ("0", "9", "1", "nan", "2", 2)]
            for ecx, x0, x1, x2, x3, expected in cases:
                run = subprocess.run([str(exe), ecx, x0, x1, x2, x3], capture_output=True, timeout=5)
                assert run.returncode == 0, (opcode, opt, ecx, run.returncode)
                assert int(run.stdout) == expected, (opcode, opt, ecx, x0, x1, x2, x3)


def test_stale_flags_across_a_call_in_a_predecessor_or_consumer_stay_unresolved():
    # Predecessor A: comiss; jp (splits the block); call; jmp join. The comiss is not the last flag
    # writer of the block that reaches the join, the callee flags are unproved.
    split = ("85C9" "740C" "0F2FC1" "7A05" "E8F20F0000" "EB03" "0F2FD3" "7206" "B801000000C3"
             "B802000000C3")
    assert "RECOMP_FLAGS_UNRESOLVED" in translate_float_image(bytes.fromhex(split))
    # The join block itself starts with a call, so the first flag reader is not reached through
    # flag-neutral instructions only.
    call_first = "85C9" "7405" "0F2FC1" "EB03" "0F2FD3" "E8EF0F0000" "7206" "B801000000C3" "B802000000C3"
    assert "RECOMP_FLAGS_UNRESOLVED" in translate_float_image(bytes.fromhex(call_first))


def test_back_edge_producer_is_not_admitted():
    # call; comiss; L: jb exit; comiss; jmp L (a later predecessor of the consumer).
    loop = "E8FB0F0000" "0F2FC1" "7205" "0F2FD3" "EBF9" "C3"
    assert "RECOMP_FLAGS_UNRESOLVED" in translate_float_image(bytes.fromhex(loop))


def out_of_line_join(opcode, tail_call=False):
    # Shape of a retail out-of-line epilogue (T1516): call; test; je B; A: movss/mulss/addss/movss;
    # comiss; J: jbe R; mov eax,1; ret; R: mov eax,2; ret; B (LATER address): subss; xorps; comiss;
    # jmp J. J cannot reach B again, so B is a forward out-of-line producer and not a loop.
    a = bytes.fromhex("F30F10442458" "F30F590518" "5D4700" "F30F5886D8000000"
                      "F30F100D785C4700" "0F" + opcode + "C1")
    b = bytes.fromhex("F30F104C2458" "F30F590D185D4700" "F30F1086D8000000" "F30F5CC1" "0F57C9"
                      "0F" + opcode + "C8")
    if tail_call:
        b += bytes.fromhex("E8E00F0000")  # a call after the producer: its flags are unproved
    # an indirect jump (a switch dispatch elsewhere in the function) disables the strict cone proof of patch 35
    tail = bytes.fromhex("B801000000FFE0" "B802000000C3")
    # a conditional call on a side path that rejoins before the producers, as in the retail function
    head = bytes.fromhex("85C9" "7405" "E8F80F0000" "85C9")
    je = bytes([0x74, len(a) + 2 + len(tail)])
    jbe = bytes([0x76, 7])
    back = bytes([(-(2 + len(tail) + len(b) + 2)) & 0xFF])  # to the jbe at J
    return head + je + a + jbe + tail + b + bytes([0xEB]) + back


def test_out_of_line_forward_producer_is_admitted_without_a_loop():
    for opcode in ("2F", "2E"):
        text = translate_float_image(out_of_line_join(opcode))
        assert "RECOMP_FLAGS_UNRESOLVED" not in text, opcode
        assert "_fca <= _fcb" in text or "_fca < _fcb" in text, opcode


def test_out_of_line_producer_after_a_call_stays_unresolved():
    assert "RECOMP_FLAGS_UNRESOLVED" in translate_float_image(out_of_line_join("2F", True))
