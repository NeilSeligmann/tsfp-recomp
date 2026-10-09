# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the 2D engine blit decode and replay (T578): the BLIT output group of `gpu_pgraph`, the rectangle copy of
`gpu_pgraph_replay_copy` and the surface images `d3d8_swap_replay` applies the blits over.

Grouped by what a survivor would let through:

    gate       the group gate and the two subchannels the decoder takes
    words      the measured binds, contexts and operation, each accepted at its value only
    state      the shadowed registers and the refusals of a blit with a word never written
    rect       the format table, the pitch, row width and 16 bit point refusals, the recorded fields
    lifetime   the copy list (bounded, forgotten per frame), the registers (forgotten by a reset), the counters
    copy       gpu_pgraph_replay_copy: its refusals, the inference, the flip and the rectangle arithmetic
    surfaces   the Data word names, the pass ranges, the store, the eviction and the dump of d3d8_swap_replay

The kills are by `test_gpu_pgraph_composition` (no device) for the decoder and the copy, `test_d3d8_swap_replay` (needs a
Vulkan device, `needs_device`) for the surface images.

NOT MUTATED, by design. `free_surfaces()` in `d3d8_swap_replay_disable` (the `memset` of the whole state that follows hides a skipped free
from every value a test can read, the mutant only leaks, which a sanitizer and not a ctest sees). The `later && writing` split of `resolve_blit_surface` (a SOURCE whose surface a later pass draws
into falls through to the kept image, a read of a surface no pass finished is refused by the lookup after it anyway) and the
log lines. The frame profile's `GPU_PGRAPH_OUTPUT_BLIT` in `gpu_pgraph_set_output_groups` and the host's announcement are in
code a ctest binary does not run, `tests/test_steady_state_boot.py` boots with the profile.

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

_PGRAPH = "src/gpu/gpu_pgraph.c"
_PGRAPH_H = "src/gpu/gpu_pgraph.h"
_REPLAY = "src/gpu/gpu_pgraph_replay.c"
_SWAP = "src/gpu/d3d8_swap_replay.c"
_TEST = ["test_gpu_pgraph_composition"]
_SWAP_TEST = ["test_d3d8_swap_replay"]


def _row(file: str, mutation_id: str, old: str, new: str, why: str, targets: list[str]) -> dict:
    return {
        "id": f"t578-blit-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(targets),
        "why": why,
        "needs_device": targets is _SWAP_TEST,
    }


def _pg(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_PGRAPH, mutation_id, old, new, why, _TEST)


def _hd(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_PGRAPH_H, mutation_id, old, new, why, _TEST)


def _rp(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_REPLAY, mutation_id, old, new, why, _TEST)


def _sw(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_SWAP, mutation_id, old, new, why, _SWAP_TEST)


MUTATIONS: list[dict] = [
    # ================================================================== gate
    _pg(
        "gate-group-ignored",
        "    if ((pgraph->output_groups & GPU_PGRAPH_OUTPUT_BLIT) != 0u &&\n        (subchannel == GPU_PGRAPH_SUBCHANNEL_IMAGE_BLIT",
        "    if ((pgraph->output_groups & GPU_PGRAPH_OUTPUT_BLIT) == 0u &&\n        (subchannel == GPU_PGRAPH_SUBCHANNEL_IMAGE_BLIT",
        "the blit words are decoded with the group off (and refused with it on): the default behaviour moves.",
    ),
    _pg(
        "gate-subchannel-2-refused",
        "(subchannel == GPU_PGRAPH_SUBCHANNEL_IMAGE_BLIT || subchannel == GPU_PGRAPH_SUBCHANNEL_SURFACES_2D)) {\n        return decode_blit",
        "(subchannel == 7u || subchannel == GPU_PGRAPH_SUBCHANNEL_SURFACES_2D)) {\n        return decode_blit",
        "the image blit subchannel is no longer decoded, the blit itself is refused.",
    ),
    _pg(
        "gate-subchannel-3-refused",
        "(subchannel == GPU_PGRAPH_SUBCHANNEL_IMAGE_BLIT || subchannel == GPU_PGRAPH_SUBCHANNEL_SURFACES_2D)) {\n        return decode_blit",
        "(subchannel == GPU_PGRAPH_SUBCHANNEL_IMAGE_BLIT || subchannel == 7u)) {\n        return decode_blit",
        "the surfaces 2D subchannel is no longer decoded, the state packet is refused.",
    ),
    _pg(
        "gate-subchannel-other-decoded",
        "(subchannel == GPU_PGRAPH_SUBCHANNEL_IMAGE_BLIT || subchannel == GPU_PGRAPH_SUBCHANNEL_SURFACES_2D)) {\n        return decode_blit",
        "(subchannel == GPU_PGRAPH_SUBCHANNEL_IMAGE_BLIT || subchannel != 0u)) {\n        return decode_blit",
        "every subchannel is decoded as the 2D engine, the fence's software method and subchannel 1 included.",
    ),
    _pg(
        "gate-bracket-accepted",
        '    if (pgraph->in_bracket) {\n        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, method,\n                      "a 2D engine command inside',
        '    if (pgraph->in_bracket && false) {\n        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, method,\n                      "a 2D engine command inside',
        "a blit inside a BEGIN_END bracket is decoded.",
    ),
    # ================================================================== words
    _hd(
        "words-blit-handle",
        "#define GPU_PGRAPH_BLIT_HANDLE 0x10u ",
        "#define GPU_PGRAPH_BLIT_HANDLE 0x11u ",
        "SET_OBJECT on subchannel 2 accepts the other handle.",
    ),
    _hd(
        "words-surfaces-handle",
        "#define GPU_PGRAPH_SURFACES_2D_HANDLE 0x11u ",
        "#define GPU_PGRAPH_SURFACES_2D_HANDLE 0x10u ",
        "SET_OBJECT on subchannel 3 and the blit's surfaces context accept the other handle.",
    ),
    _pg(
        "words-init-unchecked",
        "    if (data != measured) {\n        char message[200];",
        "    if (data != measured && false) {\n        char message[200];",
        "every CreateDevice init word is accepted whatever it carries.",
    ),
    _pg(
        "words-source-dma",
        'pair, method, data, 3u, "the source DMA context")',
        'pair, method, data, 4u, "the source DMA context")',
        "the source DMA context is accepted at another value.",
    ),
    _pg(
        "words-destination-dma",
        'pair, method, data, 0xBu, "the destination DMA context")',
        'pair, method, data, 0xCu, "the destination DMA context")',
        "the destination DMA context is accepted at another value.",
    ),
    _pg(
        "words-context-value",
        'pair, method, data, 0x19u, "a blit context',
        'pair, method, data, 0x18u, "a blit context',
        "the colour key, clip, pattern, rop and beta contexts are accepted at another handle.",
    ),
    _pg(
        "words-context-range-upper",
        "method < GPU_PGRAPH_BLIT_CONTEXT_SURFACES) {",
        "method <= GPU_PGRAPH_BLIT_CONTEXT_SURFACES) {",
        "the surfaces context 0x019C is read as one of the null contexts (0x19).",
    ),
    _pg(
        "words-context-range-lower",
        "method >= GPU_PGRAPH_BLIT_CONTEXT_FIRST &&",
        "method > GPU_PGRAPH_BLIT_CONTEXT_FIRST &&",
        "the first context word 0x0184 falls out of the table and is refused as unknown.",
    ),
    _pg(
        "words-operation-any",
        'pair, method, data, GPU_PGRAPH_BLIT_OPERATION_SRCCOPY,\n                                  "the blit operation',
        'pair, method, data, data,\n                                  "the blit operation',
        "any blit operation is accepted, the replay would copy a BLEND_AND as SRCCOPY.",
    ),
    _hd(
        "words-operation-value",
        "#define GPU_PGRAPH_BLIT_OPERATION_SRCCOPY 3u",
        "#define GPU_PGRAPH_BLIT_OPERATION_SRCCOPY 2u",
        "the measured operation (CreateDevice writes 3 to 0x02FC) is another number.",
    ),
    _pg(
        "words-unknown-subchannel-3",
        '        default:\n            return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, method,\n                          "an unmeasured method on subchannel 3',
        '        default:\n            return GPU_PGRAPH_OK; (void)refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, method,\n                          "an unmeasured method on subchannel 3',
        "an unknown method on the surfaces object is silently accepted.",
    ),
    _pg(
        "words-unknown-subchannel-2",
        '    } else {\n        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, method,\n                      "an unmeasured method on subchannel 2',
        '    } else if (true) {\n        return GPU_PGRAPH_OK;\n    } else {\n        return refuse(pgraph, GPU_PGRAPH_ERR_UNMEASURED, pair, method,\n                      "an unmeasured method on subchannel 2',
        "an unknown method on the image blit object is silently accepted.",
    ),
    # ================================================================== state
    _pg(
        "state-source-offset-swapped",
        "            if (method == GPU_PGRAPH_S2D_OFFSET_SOURCE) {\n                pgraph->blit.source_offset = data;",
        "            if (method != GPU_PGRAPH_S2D_OFFSET_SOURCE) {\n                pgraph->blit.source_offset = data;",
        "the source and destination offsets swap.",
    ),
    _pg(
        "state-offset-mask",
        "            if ((data & 0xF0000000u) != 0u) {",
        "            if ((data & 0xE0000000u) != 0u) {",
        "an offset with bit 28 set (past the 28 bit Data word) is accepted.",
    ),
    _pg(
        "state-offset-mask-high",
        "            if ((data & 0xF0000000u) != 0u) {",
        "            if ((data & 0x7FFFFFFFu) != 0u) {",
        "an offset with bit 31 set is accepted.",
    ),
    _pg(
        "state-format-not-shadowed",
        "            pgraph->blit.color_format = data;\n            pgraph->blit.format_written = true;",
        "            pgraph->blit.format_written = true;",
        "the colour format is never stored.",
    ),
    _pg(
        "state-pitch-not-shadowed",
        "            pgraph->blit.pitch = data;\n            pgraph->blit.pitch_written = true;",
        "            pgraph->blit.pitch_written = true;",
        "the pitches are never stored.",
    ),
    _pg(
        "state-point-in-swapped",
        "        pgraph->blit.point_in = data;\n        pgraph->blit.in_written = true;",
        "        pgraph->blit.point_out = data;\n        pgraph->blit.in_written = true;",
        "the source point lands in the destination point.",
    ),
    _pg(
        "state-point-out-swapped",
        "        pgraph->blit.point_out = data;\n        pgraph->blit.out_written = true;",
        "        pgraph->blit.point_in = data;\n        pgraph->blit.out_written = true;",
        "the destination point lands in the source point.",
    ),
    _pg(
        "state-source-unrequired",
        "if (!pgraph->blit.source_written || ",
        "if (false || ",
        "a blit with no source offset ever written runs on zero.",
    ),
    _pg(
        "state-destination-unrequired",
        "!pgraph->blit.destination_written || ",
        "false || ",
        "a blit with no destination offset ever written runs on zero.",
    ),
    _pg(
        "state-format-unrequired",
        "!pgraph->blit.format_written ||",
        "false ||",
        "a blit with no colour format ever written runs on zero (and is refused as a format, which hides it).",
    ),
    _pg(
        "state-pitch-unrequired",
        "!pgraph->blit.pitch_written || ",
        "false || ",
        "a blit with no pitches ever written runs on zero pitches.",
    ),
    _pg(
        "state-in-unrequired",
        "!pgraph->blit.in_written || ",
        "false || ",
        "a blit with no source point ever written runs from (0, 0).",
    ),
    _pg(
        "state-out-unrequired",
        "!pgraph->blit.out_written) {",
        "false) {",
        "a blit with no destination point ever written runs to (0, 0).",
    ),
    # ================================================================== rect
    _pg(
        "rect-format-refusal-dropped",
        "    if (bytes == 0u) {\n        char message[160];",
        "    if (bytes == 0u && false) {\n        char message[160];",
        "a colour format CopyRects never writes is decoded (and its row width taken as zero bytes).",
    ),
    _pg(
        "rect-format-y8-bytes",
        "    case GPU_PGRAPH_BLIT_FORMAT_Y8: return 1u;",
        "    case GPU_PGRAPH_BLIT_FORMAT_Y8: return 2u;",
        "the Y8 row width is counted at two bytes a pixel.",
    ),
    _pg(
        "rect-format-r5g6b5-bytes",
        "    case GPU_PGRAPH_BLIT_FORMAT_R5G6B5: return 2u;",
        "    case GPU_PGRAPH_BLIT_FORMAT_R5G6B5: return 4u;",
        "the R5G6B5 row width is counted at four bytes a pixel.",
    ),
    _pg(
        "rect-format-a8r8g8b8-bytes",
        "    case GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8: return 4u;",
        "    case GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8: return 2u;",
        "the A8R8G8B8 row width is counted at two bytes a pixel.",
    ),
    _pg(
        "rect-format-default-accepted",
        "    default: return 0u;\n    }\n}\n\nstatic gpu_pgraph_result accept_blit_word",
        "    default: return 4u;\n    }\n}\n\nstatic gpu_pgraph_result accept_blit_word",
        "every unknown colour format is read as 4 bytes a pixel.",
    ),
    _pg(
        "rect-zero-pitch-both",
        "    if (source_pitch == 0u || destination_pitch == 0u) {",
        "    if (source_pitch == 0u && destination_pitch == 0u) {",
        "a blit with ONE zero pitch is decoded.",
    ),
    _pg(
        "rect-point-in-x",
        "(pgraph->blit.point_in & 0xFFFFu) + width > 0xFFFFu ||",
        "(pgraph->blit.point_in & 0xFFFFu) + width > 0x1FFFFu ||",
        "a source rectangle running past x 65535 is decoded.",
    ),
    _pg(
        "rect-point-out-x",
        "(pgraph->blit.point_out & 0xFFFFu) + width > 0xFFFFu ||",
        "(pgraph->blit.point_out & 0xFFFFu) + width > 0x1FFFFu ||",
        "a destination rectangle running past x 65535 is decoded.",
    ),
    _pg(
        "rect-point-in-y",
        "(pgraph->blit.point_in >> 16) + height > 0xFFFFu ||",
        "(pgraph->blit.point_in >> 16) + height > 0x1FFFFu ||",
        "a source rectangle running past y 65535 is decoded.",
    ),
    _pg(
        "rect-point-out-y",
        "(pgraph->blit.point_out >> 16) + height > 0xFFFFu) {",
        "(pgraph->blit.point_out >> 16) + height > 0x1FFFFu) {",
        "a destination rectangle running past y 65535 is decoded.",
    ),
    _pg(
        "rect-row-wider-source",
        "    if ((uint64_t)width * bytes > source_pitch || ",
        "    if ((uint64_t)width * bytes >= source_pitch || ",
        "a row that exactly fills the source pitch is refused.",
    ),
    _pg(
        "rect-row-wider-destination",
        "(uint64_t)width * bytes > destination_pitch) {",
        "(uint64_t)width * bytes >= destination_pitch) {",
        "a row that exactly fills the destination pitch is refused.",
    ),
    _pg(
        "rect-row-source-unchecked",
        "    if ((uint64_t)width * bytes > source_pitch || ",
        "    if (false || ",
        "a row wider than the SOURCE pitch is decoded.",
    ),
    _pg(
        "rect-row-destination-unchecked",
        "(uint64_t)width * bytes > destination_pitch) {",
        "false) {",
        "a row wider than the DESTINATION pitch is decoded.",
    ),
    _pg(
        "rect-width-mask",
        "    const uint32_t width = data & 0xFFFFu;",
        "    const uint32_t width = data & 0xFFFu;",
        "the width loses its 16th to 13th bits.",
    ),
    _pg(
        "rect-height-shift",
        "    const uint32_t height = data >> 16;",
        "    const uint32_t height = data >> 15;",
        "the height reads one bit of the width.",
    ),
    _pg(
        "rect-record-destination-pitch",
        "    copy->destination_pitch = destination_pitch;",
        "    copy->destination_pitch = source_pitch;",
        "the record carries the source pitch as the destination's.",
    ),
    _pg(
        "rect-record-source-pitch",
        "    copy->source_pitch = source_pitch;",
        "    copy->source_pitch = destination_pitch;",
        "the record carries the destination pitch as the source's.",
    ),
    _pg(
        "rect-record-in-x",
        "    copy->in_x = pgraph->blit.point_in & 0xFFFFu;",
        "    copy->in_x = pgraph->blit.point_in >> 16;",
        "the record's source x reads the y field.",
    ),
    _pg(
        "rect-record-out-y",
        "    copy->out_y = pgraph->blit.point_out >> 16;",
        "    copy->out_y = pgraph->blit.point_out & 0xFFFFu;",
        "the record's destination y reads the x field.",
    ),
    _pg(
        "rect-record-operation",
        "    copy->operation = GPU_PGRAPH_BLIT_OPERATION_SRCCOPY;",
        "    copy->operation = 0u;",
        "the record names no operation.",
    ),
    _pg(
        "rect-record-before-draw",
        "    copy->before_draw = (uint32_t)pgraph->draw_count;",
        "    copy->before_draw = 0u;",
        "a blit loses its place among the draws.",
    ),
    _pg(
        "rect-record-before-clear",
        "    copy->before_clear = (uint32_t)pgraph->clear_count;",
        "    copy->before_clear = 0u;",
        "a blit loses its place among the clears.",
    ),
    _pg(
        "rect-record-command",
        "    copy->command = (uint32_t)pair;",
        "    copy->command = 0u;",
        "a blit loses its pair number.",
    ),
    # ================================================================== lifetime
    _pg(
        "lifetime-list-bound",
        "    if (pgraph->copy_count >= GPU_PGRAPH_MAX_COPIES) {",
        "    if (pgraph->copy_count >= GPU_PGRAPH_MAX_COPIES - 1u) {",
        "the blit list holds one fewer than its bound.",
    ),
    _pg(
        "lifetime-copies-not-counted",
        "    pgraph->stats.copies++;\n",
        "",
        "the blits are not counted.",
    ),
    _pg(
        "lifetime-empty-not-counted",
        "        pgraph->stats.copies_empty++;\n",
        "",
        "the empty blits are not counted.",
    ),
    _pg(
        "lifetime-empty-condition",
        "    if (width == 0u || height == 0u) {\n        pgraph->stats.copies_empty++;",
        "    if (width == 0u && height == 0u) {\n        pgraph->stats.copies_empty++;",
        "a blit with ONE zero side is not counted empty.",
    ),
    _pg(
        "lifetime-state-pairs-not-counted",
        "    pgraph->stats.blit_state_pairs++;\n",
        "",
        "the decoded 2D engine words are not counted.",
    ),
    _pg(
        "lifetime-pairs-not-advanced",
        "    pgraph->stats.blit_state_pairs++;\n    pgraph->stats.pairs++;",
        "    pgraph->stats.blit_state_pairs++;",
        "a 2D engine word takes no place in the pair count, so the pair index stops matching the stream index.",
    ),
    _pg(
        "lifetime-handled-not-counted",
        "    pgraph->stats.pairs++;\n    pgraph->stats.pairs_handled++;\n    return GPU_PGRAPH_OK;\n}\n\ngpu_pgraph_result gpu_pgraph_decode_other_subchannel",
        "    pgraph->stats.pairs++;\n    return GPU_PGRAPH_OK;\n}\n\ngpu_pgraph_result gpu_pgraph_decode_other_subchannel",
        "a decoded 2D engine word is not counted handled.",
    ),
    _pg(
        "lifetime-frame-keeps-copies",
        "    pgraph->copy_count = 0u; /* T578, before the bracket check",
        "    /* T578, before the bracket check",
        "a new frame keeps the last frame's blits.",
    ),
    _pg(
        "lifetime-reset-keeps-registers",
        "    memset(&pgraph->blit, 0, sizeof pgraph->blit);\n    pgraph->copy_count = 0u;\n",
        "    pgraph->copy_count = 0u;\n",
        "a reset keeps the 2D engine registers.",
    ),
    _pg(
        "lifetime-reset-keeps-copies",
        "    memset(&pgraph->blit, 0, sizeof pgraph->blit);\n    pgraph->copy_count = 0u;\n",
        "    memset(&pgraph->blit, 0, sizeof pgraph->blit);\n",
        "a reset keeps the blit list.",
    ),
    _pg(
        "lifetime-copy-at-bound",
        "return pgraph != NULL && index < pgraph->copy_count ? &pgraph->copies[index] : NULL;",
        "return pgraph != NULL && index <= pgraph->copy_count ? &pgraph->copies[index] : NULL;",
        "the accessor hands out the slot after the last blit.",
    ),
    # ================================================================== copy
    _rp(
        "copy-group-gate",
        '    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_BLIT) == 0u) {\n        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,\n                    "the stream blitted (2D engine',
        '    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_BLIT) == 0u && false) {\n        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,\n                    "the stream blitted (2D engine',
        "a backend that does not enable the blit group applies the blit.",
    ),
    _rp(
        "copy-empty-condition",
        "    if (copy->width == 0u || copy->height == 0u) {\n        return GPU_PGRAPH_OK;",
        "    if (copy->width == 0u && copy->height == 0u) {\n        return GPU_PGRAPH_OK;",
        "a blit with one zero side goes on to copy (or to refuse).",
    ),
    _rp(
        "copy-format-refusal",
        "    if (copy->color_format != GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8) {",
        "    if (copy->color_format != GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8 && false) {",
        "a Y8 or R5G6B5 blit is copied as A8R8G8B8 pixels.",
    ),
    _rp(
        "copy-operation-refusal",
        "    if (copy->operation != GPU_PGRAPH_BLIT_OPERATION_SRCCOPY) {",
        "    if (copy->operation != GPU_PGRAPH_BLIT_OPERATION_SRCCOPY && false) {",
        "a blit with another operation is copied as SRCCOPY.",
    ),
    _rp(
        "copy-source-pitch",
        "    if (copy->source_pitch != source->width * 4u ||",
        "    if (false ||",
        "a source pitch that does not address the image's rows is accepted.",
    ),
    _rp(
        "copy-destination-pitch",
        "copy->destination_pitch != destination->width * 4u ||",
        "false ||",
        "a destination pitch that does not address the image's rows is accepted.",
    ),
    _rp(
        "copy-source-stride",
        "        source->stride_bytes != source->width * 4u ||",
        "        false ||",
        "a source image with padded rows is read as tightly packed.",
    ),
    _rp(
        "copy-destination-stride",
        "destination->stride_bytes != destination->width * 4u) {",
        "false) {",
        "a destination image with padded rows is written as tightly packed.",
    ),
    _rp(
        "copy-rect-in-x",
        "    if ((uint64_t)copy->in_x + copy->width > source->width ||",
        "    if ((uint64_t)copy->in_x + copy->width >= source->width ||",
        "a rectangle that exactly reaches the source's right edge is refused.",
    ),
    _rp(
        "copy-rect-in-y",
        "(uint64_t)copy->in_y + copy->height > source->height ||",
        "(uint64_t)copy->in_y + copy->height >= source->height ||",
        "a rectangle that exactly reaches the source's bottom edge is refused.",
    ),
    _rp(
        "copy-rect-out-x",
        "(uint64_t)copy->out_x + copy->width > destination->width ||",
        "(uint64_t)copy->out_x + copy->width >= destination->width ||",
        "a rectangle that exactly reaches the destination's right edge is refused.",
    ),
    _rp(
        "copy-rect-out-y",
        "(uint64_t)copy->out_y + copy->height > destination->height) {",
        "(uint64_t)copy->out_y + copy->height >= destination->height) {",
        "a rectangle that exactly reaches the destination's bottom edge is refused.",
    ),
    _rp(
        "copy-rect-in-x-unchecked",
        "    if ((uint64_t)copy->in_x + copy->width > source->width ||",
        "    if (false ||",
        "a source rectangle past the source's right edge is copied (a read out of the image).",
    ),
    _rp(
        "copy-rect-in-y-unchecked",
        "(uint64_t)copy->in_y + copy->height > source->height ||",
        "false ||",
        "a source rectangle past the source's bottom edge is copied.",
    ),
    _rp(
        "copy-rect-out-x-unchecked",
        "(uint64_t)copy->out_x + copy->width > destination->width ||",
        "false ||",
        "a destination rectangle past the destination's right edge is written.",
    ),
    _rp(
        "copy-rect-out-y-unchecked",
        "(uint64_t)copy->out_y + copy->height > destination->height) {",
        "false) {",
        "a destination rectangle past the destination's bottom edge is written.",
    ),
    _rp(
        "copy-overlap-unchecked",
        "    if (source == destination && copy->in_x < copy->out_x + copy->width &&",
        "    if (source == destination && false && copy->in_x < copy->out_x + copy->width &&",
        "overlapping rectangles of one surface are copied in an unmeasured order.",
    ),
    _rp(
        "copy-overlap-other-images",
        "    if (source == destination && copy->in_x < copy->out_x + copy->width &&",
        "    if (copy->in_x < copy->out_x + copy->width &&",
        "two different images with the same rectangles are refused as an overlap.",
    ),
    _rp(
        "copy-overlap-x-edge",
        "copy->in_x < copy->out_x + copy->width && copy->out_x < copy->in_x + copy->width &&",
        "copy->in_x <= copy->out_x + copy->width && copy->out_x < copy->in_x + copy->width &&",
        "a destination rectangle right beside the source (sharing an edge, no pixel) counts as overlapping.",
    ),
    _rp(
        "copy-overlap-x-edge-mirror",
        "copy->out_x < copy->in_x + copy->width &&\n",
        "copy->out_x <= copy->in_x + copy->width &&\n",
        "a destination rectangle right before the source (sharing an edge) counts as overlapping.",
    ),
    _rp(
        "copy-overlap-y-edge",
        "copy->in_y < copy->out_y + copy->height && copy->out_y < copy->in_y + copy->height) {",
        "copy->in_y <= copy->out_y + copy->height && copy->out_y < copy->in_y + copy->height) {",
        "a destination rectangle right below the source (sharing an edge) counts as overlapping.",
    ),
    _rp(
        "copy-overlap-y-edge-mirror",
        "copy->out_y < copy->in_y + copy->height) {",
        "copy->out_y <= copy->in_y + copy->height) {",
        "a destination rectangle right above the source (sharing an edge) counts as overlapping.",
    ),
    _rp(
        "copy-inference-bit",
        "need_inference(backend, GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL, blit_model_note, used_inferences, report);",
        "need_inference(backend, GPU_PGRAPH_INFER_PROGRAM_HEADER, blit_model_note, used_inferences, report);",
        "the blit asks for the wrong inference, a replay that did not allow the blit model applies it.",
    ),
    _rp(
        "copy-flip-source-row",
        "const uint32_t source_row = backend->flip_y ? source->height - 1u - (copy->in_y + row) : copy->in_y + row;",
        "const uint32_t source_row = copy->in_y + row;",
        "the source rows are not mirrored under flip_y.",
    ),
    _rp(
        "copy-flip-destination-row",
        "            backend->flip_y ? destination->height - 1u - (copy->out_y + row) : copy->out_y + row;",
        "            copy->out_y + row;",
        "the destination rows are not mirrored under flip_y.",
    ),
    _rp(
        "copy-destination-column",
        "(size_t)copy->out_x * 4u,",
        "(size_t)copy->out_x * 2u,",
        "the destination column is counted at two bytes a pixel.",
    ),
    _rp(
        "copy-source-column",
        "(size_t)copy->in_x * 4u,\n",
        "(size_t)copy->in_x * 2u,\n",
        "the source column is counted at two bytes a pixel.",
    ),
    _rp(
        "copy-row-bytes",
        "                (size_t)copy->width * 4u);\n    }\n    return GPU_PGRAPH_OK;",
        "                (size_t)copy->width * 2u);\n    }\n    return GPU_PGRAPH_OK;",
        "only half of each row is copied.",
    ),
    _rp(
        "copy-row-count",
        "    for (uint32_t row = 0u; row < copy->height; row++) {\n        const uint32_t source_row",
        "    for (uint32_t row = 0u; row + 1u < copy->height; row++) {\n        const uint32_t source_row",
        "the last row of the rectangle is not copied.",
    ),
    # ================================================================== surfaces
    _sw(
        "surfaces-data-mask",
        "    return header == 0u ? 0u : d3d8_guest_load32(header + D3D8_SURFACE_DATA) & 0x0FFFFFFFu;",
        "    return header == 0u ? 0u : d3d8_guest_load32(header + D3D8_SURFACE_DATA) & 0x00FFFFFFu;",
        "the name a surface goes by loses its top nibble, so no blit finds it.",
    ),
    _sw(
        "surfaces-switch-names-swapped",
        "    entry->from_data = surface_data(previous);\n    entry->to_data = surface_data(target);",
        "    entry->from_data = surface_data(target);\n    entry->to_data = surface_data(previous);",
        "the passes are named after the other side of the switch.",
    ),
    _sw(
        "surfaces-pass-name-dropped",
        "            passes[pass_count].data = target_data;\n",
        "            passes[pass_count].data = target_data + 1u;\n",
        "a pass has no name, so no blit finds its image.",
    ),
    _sw(
        "surfaces-pass-end",
        "            passes[pass_count].end_draw = end;",
        "            passes[pass_count].end_draw = first;",
        "every pass counts as finished at its first draw, a blit in the middle of one reads half an image.",
    ),
    _sw(
        "surfaces-pass-first",
        "            passes[pass_count].first_draw = first;",
        "            passes[pass_count].first_draw = 0u;",
        "every pass counts as started at draw 0, a later pass reads as mid-draw.",
    ),
    _sw(
        "surfaces-apply-gate",
        "    if (gpu_pgraph_copy_count(state.pgraph) != 0u &&\n        !apply_copies(",
        "    if (gpu_pgraph_copy_count(state.pgraph) == 0u &&\n        !apply_copies(",
        "the frame's blits are never applied.",
    ),
    _sw(
        "surfaces-finished-bound",
        "        if (passes[i].end_draw <= position) {",
        "        if (passes[i].end_draw < position) {",
        "a pass that ends exactly at the blit is mid-draw.",
    ),
    _sw(
        "surfaces-straddle-bound",
        "        } else if (passes[i].first_draw < position) {",
        "        } else if (passes[i].first_draw <= position) {",
        "a pass that starts exactly at the blit is mid-draw instead of later.",
    ),
    _sw(
        "surfaces-later-refusal",
        "    if (later && writing) {",
        "    if (later && writing && false) {",
        "a blit into a surface a later pass redraws is applied to the kept image and lost.",
    ),
    _sw(
        "surfaces-store-gate",
        "    if (!surfaces_are_kept() || pass->data == 0u ||",
        "    if ((!surfaces_are_kept() && false) || pass->data == 0u ||",
        "a replay without the blit group (or any other reader of the images, T633) keeps every surface image.",
    ),
    _sw(
        "surfaces-store-unnamed",
        "!surfaces_are_kept() || pass->data == 0u ||",
        "!surfaces_are_kept() || false ||",
        "a pass with no name is kept (under the name 0).",
    ),
    _sw(
        "surfaces-store-replaces",
        "    stored_surface *slot = find_stored(data);\n",
        "    (void)find_stored(data);\n    stored_surface *slot = NULL;\n",
        "a surface drawn again takes a new slot instead of replacing its old image.",
    ),
    _sw(
        "surfaces-evict-counter",
        "        state.stats.surfaces_evicted++;\n",
        "",
        "an eviction is not counted.",
    ),
    _sw(
        "surfaces-evict-rotation",
        "        slot = &state.surfaces[state.surfaces_next++ % D3D8_SWAP_REPLAY_SURFACES];",
        "        slot = &state.surfaces[0];",
        "every eviction takes the same slot, the newest surfaces push each other out.",
    ),
    _sw(
        "surfaces-store-copy",
        "    memcpy(pixels, pass->image.pixels, bytes);\n",
        "    memset(pixels, 0, bytes);\n",
        "the kept image is blank.",
    ),
    _sw(
        "surfaces-store-name",
        "    slot->data = pass->data;\n",
        "    slot->data = pass->data + 1u;\n",
        "the kept image is filed under another name.",
    ),
    _sw(
        "surfaces-blit-order-source",
        'resolve_blit_surface("source", copy->source_offset, copy->before_draw, false,',
        'resolve_blit_surface("source", copy->destination_offset, copy->before_draw, false,',
        "the blit reads its destination surface.",
    ),
    _sw(
        "surfaces-blit-order-destination",
        'resolve_blit_surface("destination", copy->destination_offset, copy->before_draw, true,',
        'resolve_blit_surface("destination", copy->source_offset, copy->before_draw, true,',
        "the blit writes its source surface.",
    ),
    _sw(
        "surfaces-applied-counter",
        "        state.stats.copies_applied++;\n",
        "",
        "an applied blit is not counted.",
    ),
    _sw(
        "surfaces-empty-counter",
        "            state.stats.copies_empty++;\n            continue;",
        "            continue;",
        "an empty blit is not counted.",
    ),
    _sw(
        "surfaces-inference-folded",
        "        state.stats.used_inferences |= used;\n        if ((stored_source != NULL",
        "        if ((stored_source != NULL",
        "the blit model inference is not reported as used.",
    ),
    _sw(
        "surfaces-touched",
        "            stored_destination->touched = true;",
        "            stored_destination->touched = false;",
        "a blit onto a kept image is not dumped.",
    ),
    _sw(
        "surfaces-reset-keeps-images",
        "    free_surfaces(); /* CreateDevice made new surfaces: the stored images name old ones */\n",
        "",
        "a CreateDevice keeps the images of the old device's surfaces.",
    ),
    _sw(
        "surfaces-accessor-mask",
        "state.surfaces[i].data == (data & 0x0FFFFFFFu)) {",
        "state.surfaces[i].data == (data & 0x00FFFFFFu)) {",
        "the accessor names a surface by a shorter number than the store files it under.",
    ),
    _sw(
        "surfaces-pending-count",
        "        stats.copies_pending = gpu_pgraph_copy_count(state.pgraph);",
        "        stats.copies_pending = 0u;",
        "the blits decoded after the last present are not reported.",
    ),
]
