"""Closed local binary80 cones inside the existing opt-in float-taint DAG.

No incoming x87 operands, calls, memory stores, or status-word readers are
admitted. The native model still does not represent guest tags/exceptions.
"""

from pathlib import Path

from .lifter import _fmt_mem
from .disasm import Disassembler

_GPRS = frozenset("eax ebx ecx edx esi edi ebp esp".split())


def _plain_register(op, names):
    return (
        op.type == "reg" and op.reg in names and op.imm is None
        and op.mem_base is None and op.mem_index is None and op.mem_seg is None
        and type(op.mem_size) is int and op.mem_size == 0
        and type(op.mem_disp) is int and op.mem_disp == 0
        and type(op.mem_scale) is int and op.mem_scale == 1
    )


def _memory32(op):
    return (
        op.type == "mem" and op.reg is None and op.imm is None
        and op.mem_base in _GPRS | {None} and op.mem_index in _GPRS | {None}
        and op.mem_seg is None and type(op.mem_size) is int and op.mem_size == 4
        and type(op.mem_disp) is int and -(1 << 31) <= op.mem_disp < (1 << 32)
        and type(op.mem_scale) is int and op.mem_scale in (1, 2, 4, 8)
    )


def _memory_opcode(insn, opcode, group):
    try:
        raw = bytes.fromhex(insn.bytes_hex)
    except (ValueError, TypeError):
        return False
    return (type(insn.size) is int and len(raw) == insn.size and len(raw) >= 2
            and raw[0] == opcode and raw[1] >> 6 != 3
            and (raw[1] >> 3) & 7 == group)


def _neutral(insn):
    ops = list(insn.operands)
    if insn.mnemonic == "nop":
        return not ops
    if insn.mnemonic == "pop":
        return len(ops) == 1 and _plain_register(ops[0], _GPRS - {"esp"})
    if insn.mnemonic == "movss":
        xmm = {f"xmm{i}" for i in range(8)}
        return (len(ops) == 2 and
                ((_plain_register(ops[0], xmm) and
                  (_plain_register(ops[1], xmm) or _memory32(ops[1]))) or
                 (_memory32(ops[0]) and _plain_register(ops[1], xmm))))
    if insn.mnemonic == "mov":
        def source(op):
            return (_plain_register(op, _GPRS) or _memory32(op) or
                    (op.type == "imm" and type(op.imm) is int and
                     -(1 << 31) <= op.imm < (1 << 32) and op.reg is None and
                     op.mem_base is None and op.mem_index is None and
                     op.mem_seg is None and type(op.mem_size) is int and
                     op.mem_size == 0 and type(op.mem_disp) is int and
                     op.mem_disp == 0 and type(op.mem_scale) is int and
                     op.mem_scale == 1))
        return (len(ops) == 2 and
                ((_plain_register(ops[0], _GPRS) and source(ops[1])) or
                 (_memory32(ops[0]) and _plain_register(ops[1], _GPRS))))
    return False


def _stack_index(op):
    names = {f"st({i})" for i in range(8)}
    return int(op.reg[3]) if _plain_register(op, names) else None


def local_x87_cones(blocks, entry, float_order, alternative_entries=()):
    """Return exact statement replacements for single-block, balanced cones.

    The caller supplies the existing verified float-taint DAG. Independently
    reject missing graph edges, cyclic/domain duplication and alternate entry.
    A complete locally built stack is required before a physical DF F1 reader;
    the only admitted consumer is its immediately following DD D8 + JBE.
    """
    graph = {b.start: b for b in blocks}
    domain = set(float_order)
    if (entry not in graph or len(graph) != len(blocks) or
            len(domain) != len(float_order) or not domain <= set(graph) or
            any(a != entry for a in alternative_entries)):
        return {}
    predecessors = {a: [] for a in graph}
    for b in blocks:
        for dst in b.successors:
            if dst not in graph:
                return {}
            predecessors[dst].append(b.start)
    if predecessors[entry]:
        return {}
    reachable, todo = set(), [entry]
    while todo:
        a = todo.pop()
        if a not in reachable:
            reachable.add(a)
            todo.extend(graph[a].successors)
    position = {a: i for i, a in enumerate(float_order)}
    if any(position[p] >= position[a] for a in domain
           for p in predecessors[a] if p in domain):
        return {}
    approved = {}
    for block in blocks:
        if block.start not in domain or block.start not in reachable:
            continue
        candidate = _block_cone(block)
        if candidate:
            approved.update(candidate)
    return approved


def _physical_consistent(insn):
    """Only NEW admission: bind metadata to its actual encoded instruction."""
    if (type(insn.address) is not int or not 0 <= insn.address < (1 << 32)
            or type(insn.size) is not int or not 0 < insn.size <= 15):
        return False
    try:
        raw = bytes.fromhex(insn.bytes_hex)
    except (ValueError, TypeError):
        return False
    if len(raw) != insn.size:
        return False
    decoded = Disassembler().disassemble_function(raw, insn.address, insn.end_address)
    if len(decoded) != 1:
        return False
    physical = decoded[0]
    return (physical.address == insn.address and physical.size == insn.size and
            physical.mnemonic == insn.mnemonic and physical.op_str == insn.op_str and
            physical.jump_target == insn.jump_target and
            physical.call_target == insn.call_target and
            [vars(op) for op in physical.operands] == [vars(op) for op in insn.operands])


def _block_cone(block):
    insns = block.instructions
    if any(not _physical_consistent(insn) for insn in insns):
        return {}
    if (not insns or insns[0].address != block.start or
            any(type(i.size) is not int or i.size <= 0 for i in insns) or
            any(left.end_address != right.address for left, right in zip(insns, insns[1:])) or
            insns[-1].mnemonic not in ("jbe", "jna")):
        return {}
    starts = [i for i, insn in enumerate(insns) if insn.mnemonic.startswith("f")]
    if not starts:
        return {}
    first = starts[0]
    if any(not _neutral(i) for i in insns[:first]):
        return {}
    suffix = insns[first:]
    if len(suffix) < 4:
        return {}
    compare, pop, reader = suffix[-3:]
    if (compare.mnemonic not in ("fcompi", "fcomip") or
            compare.bytes_hex.lower() != "dff1" or len(compare.operands) != 1 or
            _stack_index(compare.operands[0]) != 1 or
            pop.mnemonic != "fstp" or pop.bytes_hex.lower() != "ddd8" or
            len(pop.operands) != 1 or _stack_index(pop.operands[0]) != 0 or
            type(reader.jump_target) is not int or len(reader.operands) != 1 or
            reader.operands[0].type != "imm" or type(reader.operands[0].imm) is not int or
            reader.operands[0].imm != reader.jump_target or
            set(block.successors) != {reader.jump_target, reader.end_address}):
        return {}
    name = f"_x87_{suffix[0].address:08X}"
    values, cw, flags = name + "_values", name + "_cw", name + "_flags"
    depth = 0
    result = {}
    for insn in suffix[:-3]:
        ops, mnemonic = list(insn.operands), insn.mnemonic
        statements = None
        top = f"{values}[{depth - 1}]"
        if mnemonic == "fld" and len(ops) == 1:
            if _memory32(ops[0]) and _memory_opcode(insn, 0xd9, 0):
                statements = [f"{values}[{depth}] = recomp_x87_load32(MEM32({_fmt_mem(ops[0])}), {cw});"]
            else:
                index = _stack_index(ops[0])
                if index is not None and index < depth and insn.bytes_hex.lower() == f"d9{0xc0 + index:02x}":
                    statements = [f"{values}[{depth}] = {values}[{depth - 1 - index}];"]
            depth += 1
            if depth > 4:
                return {}
        elif mnemonic in ("fsub", "fsubr") and len(ops) == 1 and depth:
            if _memory32(ops[0]) and _memory_opcode(insn, 0xd8, 4 if mnemonic == "fsub" else 5):
                helper = "sub32" if mnemonic == "fsub" else "subr32"
                statements = [f"{top} = recomp_x87_{helper}({top}, MEM32({_fmt_mem(ops[0])}), {cw});"]
        elif mnemonic == "fmul" and len(ops) == 1 and depth:
            index = _stack_index(ops[0])
            if index is not None and index < depth and insn.bytes_hex.lower() == f"d8{0xc8 + index:02x}":
                statements = [f"{top} = recomp_x87_mul({top}, {values}[{depth - 1 - index}], {cw});"]
        elif mnemonic == "faddp" and len(ops) == 1 and depth >= 2:
            if _stack_index(ops[0]) == 1 and insn.bytes_hex.lower() == "dec1":
                statements = [f"{values}[{depth - 2}] = recomp_x87_addp({top}, {values}[{depth - 2}], {cw});"]
                depth -= 1
        elif mnemonic == "fsqrt" and not ops and depth and insn.bytes_hex.lower() == "d9fa":
            statements = [f"{top} = recomp_x87_sqrt({top}, {cw});"]
        elif mnemonic == "fstp" and len(ops) == 1 and depth:
            index = _stack_index(ops[0])
            if index is not None and index < depth and insn.bytes_hex.lower() == f"dd{0xd8 + index:02x}":
                statements = [f"{values}[{depth - 1 - index}] = {top};"]
                depth -= 1
        elif mnemonic == "fxch" and depth >= 2:
            indices = [_stack_index(op) for op in ops]
            if indices in ([1], [0, 1]) and insn.bytes_hex.lower() == "d9c9":
                statements = [f"{{ RecompX87Value temporary = {top}; {top} = {values}[{depth - 2}]; {values}[{depth - 2}] = temporary; }}"]
        elif mnemonic in ("mov", "movss", "pop", "nop"):
            # Keep these original statements in original order, including
            # POPs that change subsequent ESP-relative memory addresses.
            if not _neutral(insn):
                return {}
            continue
        if statements is None:
            return {}
        result[insn.address] = statements
    if depth != 2 or suffix[0].address not in result:
        return {}
    result[suffix[0].address] = [
        f"RecompX87Value {values}[4];",
        f"uint16_t {cw} = g_fp_control_word;",
        f"unsigned {flags} = 0;",
        f'if (!recomp_x87_local_available({cw})) (void)RECOMP_FLAGS_UNRESOLVED(_flags, "x87-control", 0x{suffix[0].address:08X}u);',
    ] + result[suffix[0].address]
    result[compare.address] = [f"{flags} = recomp_x87_comip({values}[1], {values}[0], {cw}); /* physical DF F1; one compare, then pop */"]
    result[pop.address] = ["/* local FSTP ST0: final depth zero; no incoming operand used */"]
    result[reader.address] = [f"if ({flags} & 0x41u) goto loc_{reader.jump_target:08X}; /* physical FCOMIP JBE */"]
    return result


def write_local_x87_header(output_dir, translations):
    """Bundle the source-bound helper only when a real emitted body uses it.

    A missing required template is an error, never a successful pipeline with
    an unresolved external include. Default/no-cone generation adds no file.
    """
    if not any('#include "recomp_x87_local.h"' in body for _, _, body in translations):
        return None
    source = Path(__file__).resolve().parents[2] / "templates/runtime/recomp_x87_local.h"
    wanted = source.read_bytes()
    destination = Path(output_dir) / "recomp_x87_local.h"
    if not destination.exists() or destination.read_bytes() != wanted:
        destination.write_bytes(wanted)
    return str(destination)
