"""Cross-body branches need executable pop/return continuations, not empty stubs."""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

BASE = 0x10000
OWNER = BASE + 9
TAIL = OWNER + 1


def entry(start, end):
    return {"start": f"0x{start:08X}", "_addr": start, "end": end,
            "size": end - start, "name": f"sub_{start:08X}"}


def translator(tail=b"\x5e\xc3", *, source=b"\x56\x8b\xf1\x85\xf6\x74\x03\xeb\x00"):
    image = source + b"\x90" + tail
    config._install([config.Section(".text", BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin="branch-epilogue-test")
    return FunctionTranslator(image, {BASE: entry(BASE, OWNER),
                                     OWNER: entry(OWNER, BASE + len(image))})


def test_only_proven_pop_return_suffixes_are_recovered():
    t = translator()
    assert t.discover_branch_epilogues() == {TAIL}
    assert t.func_db[OWNER]["end"] == TAIL + 2
    assert t.func_db[TAIL]["detection_method"] == "branch_epilogue"
    code = t.translate_function(TAIL, t.func_db[TAIL])
    assert "POP32(esp, esi);" in code and "esp += 4; return;" in code
    assert t.discover_branch_epilogues() == set()  # idempotent; do not overwrite entries
    for bad in (b"\x8b\xf0\xc3", b"\x5c\xc3", b"\x5e\xcc", b"\x5e\x74\x00\xc3"):
        assert translator(bad).discover_branch_epilogues() == set(), bad.hex()
    t = translator(b"\x5f\x5e\xc2\x04\x00")
    assert t.discover_branch_epilogues() == {TAIL}
    assert t.func_db[TAIL]["end"] == TAIL + 5


def test_mid_instruction_targets_and_calls_do_not_create_entries():
    # The branch lands inside ret's immediate, not on an instruction start.
    bad = bytearray(b"\x56\x8b\xf1\x85\xf6\x74\x05\xeb\x00")
    assert translator(b"\x5e\xc2\x04\x00", source=bytes(bad)).discover_branch_epilogues() == set()
    # A direct call carries a new return address; it is not a tail continuation.
    t = translator(source=b"\xe8\x05\x00\x00\x00\x90\x90\x90\xc3")
    assert t.discover_branch_epilogues() == set()
    # Truncating measured owner bounds before the return proves nothing.
    t = translator()
    t.func_db[OWNER]["end"] = TAIL + 1
    assert t.discover_branch_epilogues() == set()


def test_native_zero_and_nonzero_branches_restore_esi_and_esp():
    compiler = shutil.which("cc")
    if not compiler:
        return
    t = translator()
    assert t.discover_branch_epilogues() == {TAIL}
    sources = [t.translate_function(va, info) for va, info in sorted(t.func_db.items())]
    root = Path(__file__).resolve().parents[4]
    runtime = root / "tools/harness/runtime_min.c"
    header = Path(__file__).resolve().parents[2] / "templates/runtime/recomp_types.h"
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        code = path / "native.c"
        code.write_text(
            '#define RECOMP_GENERATED_CODE\n#include "' + str(header) + '"\n'
            '#include <assert.h>\n'
            + "\n".join(f"void sub_{va:08X}(void);" for va in t.func_db)
            + "\n" + "\n".join(sources)
            + '\nrecomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }\n'
            'int main(void) { uint32_t stack[64] = {0}; '
            'g_xbox_mem_offset = (intptr_t)stack - 0xE80000u; '
            'for (unsigned input = 0; input < 2; input++) { '
            'g_esp = 0xE80080u; g_esi = 0x12345678u; g_ecx = input; '
            'sub_00010000(); assert(g_esp == 0xE80084u); '
            'assert(g_esi == 0x12345678u); } return 0; }\n'
        )
        binary = path / "native"
        result = subprocess.run([compiler, "-std=gnu11", str(code), str(runtime),
                                 "-lm", "-o", str(binary)], capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        subprocess.run([str(binary)], check=True)

# Pin the actual retail split, then execute its zero branch with the external
# call modeled as a returning call. The adjacent nonzero body must stay unused.
def test_retail_split_zero_path():
    root = Path(__file__).resolve().parents[4]
    xbe = root / 'tmp/oxm-extract/retail/default.xbe'
    if not xbe.exists():
        return
    config.configure_from_xbe(str(xbe))
    image = xbe.read_bytes()
    offset = config.va_to_file_offset(0x400686)
    assert image[offset:offset + 2] == b'\x5e\xc3'
    t = FunctionTranslator(image, {0x40066C: entry(0x40066C, 0x400680),
                                   0x400680: entry(0x400680, 0x400688)})
    assert t.discover_branch_epilogues() == {0x400686}
    caller = t.translate_function(0x40066C, t.func_db[0x40066C])
    assert 'sub_00400686(); return;' in caller
    tail = t.translate_function(0x400686, t.func_db[0x400686])
    compiler = shutil.which('cc')
    if not compiler:
        return
    header = Path(__file__).resolve().parents[2] / 'templates/runtime/recomp_types.h'
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        code = path / 'retail.c'
        code.write_text('#define RECOMP_GENERATED_CODE\n#include "' + str(header) + '"\n'
            '#include <assert.h>\nvoid sub_00400686(void);\n'
            'void sub_00400261(void) { g_esp += 4; }\n'
            'void sub_00400680(void) { assert(0); }\n'
            + caller + tail +
            '\nrecomp_func_t recomp_lookup(uint32_t va) { (void)va; return NULL; }\n'
            'int main(void) { uint32_t memory[64] = {0}; '
            'g_xbox_mem_offset = (intptr_t)memory - 0xE80000u; '
            'g_esp = 0xE80080u; g_esi = 0x12345678u; g_ecx = 0xE80000u; '
            'sub_0040066C(); assert(g_esp == 0xE80084u); '
            'assert(g_esi == 0x12345678u); return 0; }\n')
        binary = path / 'retail'
        result = subprocess.run([compiler, '-std=gnu11', str(code),
            str(root / 'tools/harness/runtime_min.c'), '-lm', '-o', str(binary)],
            capture_output=True, text=True)
        assert result.returncode == 0, result.stderr
        subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    test_only_proven_pop_return_suffixes_are_recovered()
    test_mid_instruction_targets_and_calls_do_not_create_entries()
    test_native_zero_and_nonzero_branches_restore_esi_and_esp()
    test_retail_split_zero_path()
    print('branch_epilogues: ALL PASS')
