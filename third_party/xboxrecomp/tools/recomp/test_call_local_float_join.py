"""Default call-local scalar joins retain strict original producer proof."""
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator


def translate(raw):
    base = 0x11000
    config._install([config.Section(".text", base, len(raw), 0, len(raw), True)],
                    entry_point=base, kernel_thunk_addr=base, origin="t1167")
    info = {"start": hex(base), "end": base + len(raw), "_addr": base}
    return FunctionTranslator(raw, {base: info}).translate_function(base, info)


def test_call_outside_scalar_join_admitted():
    raw = bytes.fromhex("E8FB0F0000E3050F2FC1EB030F2FD376029090C3")
    code = translate(raw)
    assert "RECOMP_FLAGS_UNRESOLVED" not in code
    assert "_fca" in code


def test_call_after_scalar_producer_refused():
    raw = bytes.fromhex("E8FB0F0000E3050F2FC1EB080F2FD3E8E80F000076029090C3")
    assert "RECOMP_FLAGS_UNRESOLVED" in translate(raw)
