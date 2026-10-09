"""Physical contiguous scalar tuple/AH parity admission boundaries."""
import copy
import pytest
from tools.recomp.local_sse_flags import local_sse_flag_cones
from tools.recomp.lifter import _make_condition
from tools.recomp.translator import _float_relation_snapshot_blocks
from tools.recomp.test_float_relation_provenance import cfg, translate, called_join


@pytest.mark.parametrize('opcode', ['0f2ec1', '0f2fc1'])
def test_real_cone(opcode):
    raw = called_join(bytes.fromhex(opcode + '9ff6c4447b0190c3'))
    blocks = cfg(raw)
    _, order = _float_relation_snapshot_blocks(blocks, 0x11000)
    result = local_sse_flag_cones(blocks, 0x11000, order)
    assert len(result) == 3
    assert not local_sse_flag_cones(blocks, 0x11000, order, [0x11005])
    text = translate(raw)
    assert '_local_sse_ah = ' in text
    assert 'RECOMP_FLAGS_UNRESOLVED' not in text


@pytest.mark.parametrize('middle', ['e800000000', 'f5', '90', 'b401', '89c0', 'ffc0'])
def test_interposed_instruction_refuses(middle):
    text = translate(called_join(bytes.fromhex('0f2ec1'+middle+'9ff6c4447b0190c3')))
    assert 'RECOMP_FLAGS_UNRESOLVED' in text
    assert '_local_sse_ah = ' not in text


@pytest.mark.parametrize('field,value', [('mem_scale', True), ('mem_disp', True),
                                        ('mem_size', 4), ('reg', 'eax')])
def test_corrupted_physical_operand_refuses(field, value):
    raw = called_join(bytes.fromhex('0f2ec19ff6c4447b0190c3'))
    blocks = cfg(raw)
    _, order = _float_relation_snapshot_blocks(blocks, 0x11000)
    bad = copy.deepcopy(blocks)
    compare = next(i for b in bad for i in b.instructions if i.bytes_hex.lower() == '0f2ec1')
    setattr(compare.operands[0], field, value)
    assert not local_sse_flag_cones(bad, 0x11000, order)


@pytest.mark.parametrize('tail', ['83d000', 'f583d000', '0f9ac0', '9f', 'ffc0780190', 'e8000000007b0190'])
def test_later_reader_stays_blocked(tail):
    text = translate(called_join(bytes.fromhex('0f2ec19ff6c4447b00'+tail+'c3')))
    assert 'RECOMP_FLAGS_UNRESOLVED' in text


@pytest.mark.parametrize('cc', ['je', 'jne', 'jl', 'jge', 'jp', 'jb', 'jo', 'js'])
def test_parity_marker_narrow_reader(cc):
    assert _make_condition(cc, '__float_ah_pf', []) is None
