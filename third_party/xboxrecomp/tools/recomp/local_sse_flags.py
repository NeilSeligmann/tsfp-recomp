"""Closed physical scalar-compare/LAHF/TEST-AH parity cones.

Only contiguous same-block cones inside the existing opt-in float DAG are
admitted. This does not recover flags across calls or merge physical writers.
"""
from .lifter import _fmt_mem
from .local_x87 import _memory32, _physical_consistent, _plain_register


def _sse_read(op):
    return f"{op.reg}.f[0]" if op.type == "reg" else f"MEMF({_fmt_mem(op)})"

def local_sse_flag_cones(blocks, entry, float_order, alternative_entries=()):
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
        insns = block.instructions
        if (block.start not in domain or block.start not in reachable or
                len(insns) < 4 or insns[0].address != block.start or
                any(not _physical_consistent(i) for i in insns) or
                any(a.end_address != b.address for a, b in zip(insns, insns[1:]))):
            continue
        compare, lahf, test, reader = insns[-4:]
        ops = compare.operands
        xmm = {f"xmm{i}" for i in range(8)}
        if (compare.mnemonic not in ("comiss", "ucomiss") or len(ops) != 2 or
                not _plain_register(ops[0], xmm) or
                not (_plain_register(ops[1], xmm) or _memory32(ops[1])) or
                lahf.mnemonic != "lahf" or lahf.bytes_hex.lower() != "9f" or
                lahf.operands or test.mnemonic != "test" or
                len(test.operands) != 2 or
                not _plain_register(test.operands[0], {"ah"}) or
                test.operands[1].type != "imm" or
                type(test.operands[1].imm) is not int or
                not 0 <= test.operands[1].imm <= 255 or
                bytes.fromhex(test.bytes_hex) != bytes((0xf6, 0xc4, test.operands[1].imm)) or
                reader.mnemonic not in ("jnp", "jpo") or
                type(reader.jump_target) is not int or
                set(block.successors) != {reader.jump_target, reader.end_address}):
            continue
        approved[compare.address] = ([
            f"_fca = {_sse_read(ops[0])}; _fcb = {_sse_read(ops[1])};",
            "_local_sse_ah = (_fca != _fca || _fcb != _fcb) ? 0x47u : "
            "(_fca < _fcb ? 0x03u : (_fca == _fcb ? 0x42u : 0x02u));",
        ], "__float_lahf_tuple")
        approved[lahf.address] = ([
            "eax = (eax & 0xFFFF00FFu) | (_local_sse_ah << 8);",
        ], "__float_lahf_tuple")
        approved[test.address] = ([
            f"_local_sse_test = ((eax >> 8) & 0xFFu) & {test.operands[1].imm}u;",
            "_local_sse_pf = (__builtin_parity(_local_sse_test) == 0);",
        ], "__float_ah_pf")
    return approved
