"""Closed physical x87 cones; defaults and unsupported paths stay unchanged."""

import copy
import importlib.util

import pytest
from pathlib import Path

from tools.recomp.local_x87 import local_x87_cones, write_local_x87_header
from tools.recomp.translator import _float_relation_snapshot_blocks
from tools.recomp.test_float_relation_provenance import called_join, cfg, translate

CONE = bytes.fromhex("d900d94004dff1ddd876029090c3")


def approved(raw):
    blocks = cfg(raw)
    _, order = _float_relation_snapshot_blocks(blocks, 0x11000)
    return blocks, order, local_x87_cones(blocks, 0x11000, order)


def test_actual_closed_cone():
    raw = called_join(CONE)
    blocks, order, statements = approved(raw)
    assert statements
    text = translate(raw)
    assert "physical FCOMIP JBE" in text
    assert "recomp_x87_load32" in text
    assert "x87-control" in text
    assert "RECOMP_FLAGS_UNRESOLVED" not in text.replace(
        'RECOMP_FLAGS_UNRESOLVED(_flags, "x87-control"', "CONTROL_GATE"
    )
    assert not local_x87_cones(blocks, 0x11000, order, [0x11001])
    assert not local_x87_cones(blocks, 0x11000, order + order)
    assert not local_x87_cones(blocks, 0x11000, list(reversed(order)))


@pytest.mark.parametrize(
    "tail",
    [
        "d900dff1ddd876029090c3",  # missing local operand
        "d900d94004e800000000dff1ddd876029090c3",  # call
        "d900d94004dff1ddd872029090c3",  # CF-only consumer
        "d900d94004dff1ddd87a029090c3",  # PF-only consumer
        "d900d94004dff1ddd874029090c3",  # ZF-only consumer
        "d900d94004dff1ddd876fec3",  # entry/backedge
        "d900d94004dbf1ddd876029090c3",  # nonpop FCOMI
        "d900d94004dfe9ddd876029090c3",  # FUCOMIP
        "dd00d94004dff1ddd876029090c3",  # FLD64 unsupported
        "d900d94004dff1ddd976029090c3",  # wrong final pop
        "d900d94004dff1ddd87602",  # missing successor
    ],
)
def test_refused_physical_shapes(tail):
    _, _, statements = approved(called_join(bytes.fromhex(tail)))
    assert not statements


@pytest.mark.parametrize(
    "field,value",
    [
        ("mem_size", 8),
        ("mem_size", True),
        ("mem_base", "xmm0"),
        ("mem_index", "ax"),
        ("mem_scale", True),
        ("mem_scale", 3),
        ("mem_disp", True),
        ("mem_disp", 1 << 32),
        ("mem_seg", "fs"),
        ("imm", 0),
        ("reg", "eax"),
    ],
)
def test_malformed_memory(field, value):
    blocks, order, statements = approved(called_join(CONE))
    assert statements
    altered = copy.deepcopy(blocks)
    fld = next(i for b in altered for i in b.instructions if i.mnemonic == "fld")
    setattr(fld.operands[0], field, value)
    assert not local_x87_cones(altered, 0x11000, order)


@pytest.mark.parametrize("field,value", [("mem_base", "ebx"), ("mem_disp", 123), ("mem_index", "esp")])
def test_raw_operand_contradictions(field, value):
    blocks, order, statements = approved(called_join(CONE))
    assert statements
    altered = copy.deepcopy(blocks)
    fld = next(i for b in altered for i in b.instructions if i.mnemonic == "fld")
    setattr(fld.operands[0], field, value)
    assert not local_x87_cones(altered, 0x11000, order)


def test_raw_reader_contradiction():
    blocks, order, statements = approved(called_join(CONE))
    assert statements
    altered = copy.deepcopy(blocks)
    reader = next(i for b in altered for i in b.instructions if i.address in statements and i.mnemonic == "jbe")
    reader.bytes_hex = "72" + reader.bytes_hex[2:]
    assert not local_x87_cones(altered, 0x11000, order)


def test_required_runtime_header_package(tmp_path):
    text = translate(called_join(CONE))
    path = write_local_x87_header(tmp_path, [(0x11000, "physical", text)])
    source = Path(__file__).resolve().parents[2] / "templates/runtime/recomp_x87_local.h"
    assert Path(path).read_bytes() == source.read_bytes()
    before = Path(path).stat().st_mtime_ns
    assert write_local_x87_header(tmp_path, [(0x11000, "physical", text)]) == path
    assert Path(path).stat().st_mtime_ns == before
    Path(path).write_bytes(b"stale")
    write_local_x87_header(tmp_path, [(0x11000, "physical", text)])
    assert Path(path).read_bytes() == source.read_bytes()


def test_no_cone_adds_no_runtime_header(tmp_path):
    assert write_local_x87_header(tmp_path, [(1, "ordinary", "void ordinary(void) {}")]) is None
    assert list(tmp_path.iterdir()) == []


def test_missing_required_template_fails(tmp_path):
    # Execute the actual source from an incomplete private vendor tree;
    # no patched read function or production-file deletion is involved.
    private = tmp_path / "vendor/tools/recomp/local_x87.py"
    private.parent.mkdir(parents=True)
    private.write_bytes(Path(__file__).with_name("local_x87.py").read_bytes())
    spec = importlib.util.spec_from_file_location("tools.recomp._missing_x87_fixture", private)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    output = tmp_path / "output"
    output.mkdir()
    with pytest.raises(FileNotFoundError):
        module.write_local_x87_header(output, [(1, "real", translate(called_join(CONE)))])
    assert list(output.iterdir()) == []


@pytest.mark.parametrize(
    "tail", ["f5", "83d000", "83d800", "9f", "0f92c0", "770090", "7a0090", "e800000000770090"]
)
def test_later_reader_remains_blocked(tail):
    text = translate(called_join(CONE[:-1] + bytes.fromhex(tail) + b"\xc3"))
    assert "physical FCOMIP JBE" in text
    assert "RECOMP_FLAGS_UNRESOLVED" in text.replace(
        'RECOMP_FLAGS_UNRESOLVED(_flags, "x87-control"', "CONTROL_GATE"
    )
