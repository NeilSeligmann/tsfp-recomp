# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the decode of `0x1E98` (T521), `NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN`.

`0x00081E94, 6, 0` (header count 2) is the CreateDevice execution-mode packet, so its second dword lands on 0x1E98.
The decoder accepts the measured value 0 only, keeps it, counts it and takes no snapshot for it.

    method number   the register moved: the real second dword is unhandled again (strict refuses every boot)
    value rule      any nonzero value accepted, or refused with the wrong class
    model           the kept word, its written flag, the counter
    snapshots      a pixel-free word forcing a new snapshot per write
    bracket         the in-bracket refusal shared with the other vertex-stage state

Kills come from `test_gpu_pgraph` (`test_cxt_write_en`, `test_refusals`) and `test_d3d8_gpu_pgraph` (the packet
`d3d8_shader_create` writes, recorded and decoded strict).

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

_PGRAPH = "src/gpu/gpu_pgraph.c"
_HEADER = "src/gpu/gpu_pgraph.h"
_TARGETS = ["test_gpu_pgraph", "test_d3d8_gpu_pgraph"]


def _row(file: str, mutation_id: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"gpu-1e98-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(_TARGETS),
        "why": why,
    }


MUTATIONS: list[dict] = [
    _row(
        _HEADER,
        "method-number",
        "#define GPU_PGRAPH_CXT_WRITE_EN 0x1E98u",
        "#define GPU_PGRAPH_CXT_WRITE_EN 0x1E9Au",
        "the decoder listens on 0x1E9A: the real second dword of every CreateDevice packet is unhandled again and a strict replay stops at pair 1.",
    ),
    _row(
        _PGRAPH,
        "nonzero-accepted",
        '        if (data != 0u) {\n            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,\n                          "program context write enable is not 0,',
        '        if (data > 1u) {\n            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,\n                          "program context write enable is not 0,',
        "a context write enable of 1 (vertex programs writing constants, which the replay does not model) is accepted and replayed as if nothing changed.",
    ),
    _row(
        _PGRAPH,
        "wrong-refusal-class",
        '        if (data != 0u) {\n            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, index, method,\n                          "program context write enable is not 0,',
        '        if (data != 0u) {\n            return refuse(pgraph, GPU_PGRAPH_ERR_MALFORMED, index, method,\n                          "program context write enable is not 0,',
        "the nonzero value is refused as a malformed measured method instead of an unmeasured value.",
    ),
    _row(
        _PGRAPH,
        "value-not-kept",
        "        state->cxt_write_en = data;\n        state->cxt_write_en_set = true;\n",
        "        state->cxt_write_en = data;\n",
        "the word is decoded but never recorded as written.",
    ),
    _row(
        _PGRAPH,
        "counter-dropped",
        "        pgraph->stats.cxt_write_en_pairs++;\n",
        "        (void)pgraph->stats.cxt_write_en_pairs;\n",
        "the documented no-op is no longer counted: a run cannot show how many 0x1E98 pairs it consumed.",
    ),
    _row(
        _PGRAPH,
        "forces-snapshot",
        "        pgraph->stats.cxt_write_en_pairs++;\n        return GPU_PGRAPH_OK;\n",
        "        pgraph->stats.cxt_write_en_pairs++;\n        pgraph->snapshot_dirty = true;\n        return GPU_PGRAPH_OK;\n",
        "a pixel-free word forces a new snapshot, splitting draws that share every drawn state.",
    ),
    _row(
        _PGRAPH,
        "bracket-not-refused",
        "           method == GPU_PGRAPH_CXT_WRITE_EN ||\n",
        "",
        "0x1E98 inside a BEGIN_END bracket is accepted although it is vertex-stage state no measured emitter changes there.",
    ),
]
