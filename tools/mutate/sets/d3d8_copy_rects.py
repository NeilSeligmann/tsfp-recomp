# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for CopyRects 0x003D3AD0 (T555, src/gpu/d3d8_copy.c): the two copy paths, the pitch and byte
size helpers 0x003D3640 and 0x003D36B0, the state and blit packets, the fence tags and the refusals that
precede every write.

Every mutation is covered by `tests/test_d3d8_copy_rects_oracle.py`, which replays the ORIGINAL bytes
under the oracle over the same guest windows (ring cursor, limit, every command, the caller's rectangles,
the arena) and the named refusals. The ctest binary beside it is the compile guard (a mutation that does
not compile makes the pytest runner build fail, which reads like a kill).

NOT MUTATED, by design. The `cursor != planned[...]` assertions after each preamble are unreachable
(the plan and the writer run the same arithmetic), so their mutants would be equivalent. The writability
probe of the caller's rectangle list on the byte path is unreachable in the harness: every mapping the
list can be read from is writable, so the probe adds nothing a test can show.

CONVENTION. `&& false` rather than a bare `false`, because `-Wunused-parameter -Werror` turns the latter
into NOT-A-MUTANT, which reads like evidence.
"""

COPY = "src/gpu/d3d8_copy.c"
GUARD = "test_d3d8_state"
ORACLE = "pytest:tests/test_d3d8_copy_rects_oracle.py"


def mutation(identifier: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"t555-copy-rects-{identifier}",
        "file": COPY,
        "old": old,
        "new": new,
        "targets": [GUARD, ORACLE],
        "why": why,
    }


MUTATIONS: list[dict] = [
    mutation(
        "pitch-linear-shift",
        "if (size != 0u) return ((size >> 24) + 1u) << 6;",
        "if (size != 0u) return ((size >> 24) + 1u) << 5;",
        "a linear surface keeps pitch / 64 - 1 in the top byte of its size word, a wrong scale puts every blit's pitch word at half the row.",
    ),
    mutation(
        "pitch-dxt1",
        "if (format == 0xCu) return width * 2u;",
        "if (format == 0xCu) return width * 4u;",
        "a swizzled DXT1 surface is 2 bytes of pitch per texel column in the original, the pitch word is what the blit strides by.",
    ),
    mutation(
        "pitch-dxt35",
        "if (format > 0xDu && format <= 0xFu) return width * 4u;",
        "if (format > 0xDu && format <= 0xFu) return width * 2u;",
        "DXT3 and DXT5 take 4 bytes per texel column, DXT1 2: the two arms must not be swapped.",
    ),
    mutation(
        "pitch-generic",
        "return ((format_flags(format) & 0x3Cu) * width) >> 3;",
        "return ((format_flags(format) & 0x3Cu) * width) >> 2;",
        "every other swizzled pitch is bits per pixel times width over 8.",
    ),
    mutation(
        "bytes-compressed-minimum",
        "const uint32_t minimum = format_compressed(format) ? 2u : 0u;",
        "const uint32_t minimum = format_compressed(format) ? 1u : 0u;",
        "a compressed swizzled surface is at least 4x4 texels, which raises both log sizes to 2 before the byte size is taken.",
    ),
    mutation(
        "bytes-swizzled-size",
        "return ((1u << ((wide + tall) & 31u)) * bits) >> 3;",
        "return ((1u << ((wide | tall) & 31u)) * bits) >> 3;",
        "the swizzled byte size is 2^(log u + log v) texels, the byte path's row split depends on it.",
    ),
    mutation(
        "bytes-linear-size",
        "return (((size >> 12) & 0xFFFu) + 1u) * ((size >> 24) + 1u) << 6;",
        "return (((size >> 12) & 0xFFFu) + 1u) * ((size >> 24)) << 6;",
        "a linear surface is height rows of pitch bytes, both fields stored minus one.",
    ),
    mutation(
        "compressed-formats",
        "return format == 0xCu || format == 0xEu || format == 0xFu;",
        "return format == 0xCu || format == 0xEu;",
        "DXT5 (0xF) takes the byte path like the other two compressed formats.",
    ),
    mutation(
        "byte-path-selection-bit",
        "const bool byte_path = (flags & 1u) != 0u || format_compressed(format);",
        "const bool byte_path = format_compressed(format);",
        "formats whose table bit 0 is set copy as bytes even when they are not compressed.",
    ),
    mutation(
        "byte-path-selection-compressed",
        "const bool byte_path = (flags & 1u) != 0u || format_compressed(format);",
        "const bool byte_path = (flags & 1u) != 0u;",
        "a compressed source takes the byte path whatever its table bit says.",
    ),
    mutation(
        "byte-path-limit",
        "#define BYTE_PATH_LIMIT 0x1FC0u",
        "#define BYTE_PATH_LIMIT 0x1FBFu",
        "127 rows of 64 bytes are exactly 0x1FC0 and still one row, one byte less and the whole surface changes shape.",
    ),
    mutation(
        "byte-path-row",
        "#define BYTE_PATH_ROW 0x1000u",
        "#define BYTE_PATH_ROW 0x2000u",
        "a large byte surface is copied as rows of 0x1000 bytes, the pitch word and the rectangle width both carry it.",
    ),
    mutation(
        "byte-path-height",
        "byte_height = bytes >> 12;",
        "byte_height = bytes >> 11;",
        "the row count of a large byte copy is its size over 0x1000.",
    ),
    mutation(
        "byte-path-pitch-round",
        "source_pitch = (bytes + 0x3Fu) & 0xFFFFFFC0u;",
        "source_pitch = bytes & 0xFFFFFFC0u;",
        "a one row byte copy rounds its pitch UP to 64, a tiny surface would otherwise get pitch 0.",
    ),
    mutation(
        "byte-path-destination-pitch",
        "destination_pitch = source_pitch;",
        "destination_pitch = destination_pitch + 0u;",
        "the byte path forces both pitches to the source's, the destination's own pitch must not reach the packet.",
    ),
    mutation(
        "override-absent-byte-format",
        "            blit_format = 1u;\n        } else {\n            if (bytes_per_pixel == 0u)",
        "            blit_format = 4u;\n        } else {\n            if (bytes_per_pixel == 0u)",
        "without a format override the byte path blits as format 1 (one byte a pixel).",
    ),
    mutation(
        "override-zero-bytes-refusal",
        '            if (bytes_per_pixel == 0u)\n                d3d8_hle_fatal(RECTS_ENTRY, "CopyRects byte path with a format override divides by zero bytes per pixel");',
        '            if (bytes_per_pixel == 0u && false)\n                d3d8_hle_fatal(RECTS_ENTRY, "CopyRects byte path with a format override divides by zero bytes per pixel");',
        "the original divides by the source's bytes per pixel here and faults on zero: a named refusal must replace it, a port that divides would crash the host.",
    ),
    mutation(
        "override-width-divide",
        "byte_width /= bytes_per_pixel;",
        "byte_width *= bytes_per_pixel;",
        "with a format override the byte path counts pixels, not bytes.",
    ),
    mutation(
        "format-one-byte",
        "blit_format = bytes_per_pixel == 1u ? 1u : (bytes_per_pixel == 2u ? 4u : 0xAu);",
        "blit_format = bytes_per_pixel == 1u ? 4u : (bytes_per_pixel == 2u ? 4u : 0xAu);",
        "an 8 bit source blits as colour format 1.",
    ),
    mutation(
        "format-two-bytes",
        "blit_format = bytes_per_pixel == 1u ? 1u : (bytes_per_pixel == 2u ? 4u : 0xAu);",
        "blit_format = bytes_per_pixel == 1u ? 1u : (bytes_per_pixel == 2u ? 1u : 0xAu);",
        "a 16 bit source blits as colour format 4.",
    ),
    mutation(
        "format-four-bytes",
        "blit_format = bytes_per_pixel == 1u ? 1u : (bytes_per_pixel == 2u ? 4u : 0xAu);",
        "blit_format = bytes_per_pixel == 1u ? 1u : (bytes_per_pixel == 2u ? 4u : 0x12u);",
        "every other source, the title's A8R8G8B8 included, blits as colour format 0xA.",
    ),
    mutation(
        "state-header",
        "d3d8_guest_store32(cursor, 0x00086308u);",
        "d3d8_guest_store32(cursor, 0x00086300u);",
        "the first state packet header names the two surface offsets.",
    ),
    mutation(
        "state-source-offset",
        "d3d8_guest_store32(cursor + 4u, from[SURFACE_DATA]);",
        "d3d8_guest_store32(cursor + 4u, to[SURFACE_DATA]);",
        "the source offset comes from the source header's data word.",
    ),
    mutation(
        "state-pitch-word",
        "d3d8_guest_store32(cursor + 20u, (destination_pitch << 16) | (source_pitch & 0xFFFFu));",
        "d3d8_guest_store32(cursor + 20u, (source_pitch << 16) | (destination_pitch & 0xFFFFu));",
        "the destination pitch is the high half, the source pitch the low.",
    ),
    mutation(
        "blit-header",
        "d3d8_guest_store32(cursor, 0x000C4300u);",
        "d3d8_guest_store32(cursor, 0x00084300u);",
        "the blit packet writes three words: source point, destination point, size.",
    ),
    mutation(
        "blit-source-point-mask",
        "d3d8_guest_store32(cursor + 4u, ((uint32_t)rectangle.top << 16) | ((uint32_t)rectangle.left & 0xFFFFu));",
        "d3d8_guest_store32(cursor + 4u, ((uint32_t)rectangle.top << 16) | ((uint32_t)rectangle.left));",
        "a negative left must not smear its sign into the top half.",
    ),
    mutation(
        "blit-destination-point-mask",
        "d3d8_guest_store32(cursor + 8u, ((uint32_t)point_y << 16) | ((uint32_t)point_x & 0xFFFFu));",
        "d3d8_guest_store32(cursor + 8u, ((uint32_t)point_y << 16) | ((uint32_t)point_x));",
        "a negative destination x must not smear its sign into the y half.",
    ),
    mutation(
        "blit-size-mask",
        "(((uint32_t)rectangle.right - (uint32_t)rectangle.left) & 0xFFFFu));",
        "(((uint32_t)rectangle.right - (uint32_t)rectangle.left)));",
        "the original XORs the masked width into the height half: an inverted rectangle flips the high bits without the mask.",
    ),
    mutation(
        "blit-size-height",
        "(((uint32_t)rectangle.bottom - (uint32_t)rectangle.top) << 16) ^",
        "(((uint32_t)rectangle.bottom) << 16) ^",
        "the height is bottom minus top.",
    ),
    mutation(
        "whole-surface-swizzled-width",
        "rectangle.right = (int32_t)(1u << ((from[SURFACE_FORMAT] >> 20) & 0xFu));",
        "rectangle.right = (int32_t)(1u << ((from[SURFACE_FORMAT] >> 24) & 0xFu));",
        "a swizzled surface's width is 2^(format bits 20 to 23).",
    ),
    mutation(
        "whole-surface-swizzled-height",
        "rectangle.bottom = (int32_t)(1u << ((from[SURFACE_FORMAT] >> 24) & 0xFu));",
        "rectangle.bottom = (int32_t)(1u << ((from[SURFACE_FORMAT] >> 20) & 0xFu));",
        "and its height 2^(format bits 24 to 27).",
    ),
    mutation(
        "whole-surface-linear-width",
        "rectangle.right = (int32_t)((from[SURFACE_SIZE] & 0xFFFu) + 1u);",
        "rectangle.right = (int32_t)((from[SURFACE_SIZE] & 0xFFFu));",
        "a linear surface's width is its size word's low 12 bits plus one.",
    ),
    mutation(
        "whole-surface-linear-height",
        "rectangle.bottom = (int32_t)(((from[SURFACE_SIZE] >> 12) & 0xFFFu) + 1u);",
        "rectangle.bottom = (int32_t)(((from[SURFACE_SIZE] >> 12) & 0xFFu) + 1u);",
        "and its height the next 12 bits plus one.",
    ),
    mutation(
        "missing-points-x",
        "const int32_t point_x = points != 0u ? where[index][0] : rectangle.left;",
        "const int32_t point_x = points != 0u ? where[index][0] : 0;",
        "without destination points each rectangle lands where it came from.",
    ),
    mutation(
        "missing-points-y",
        "const int32_t point_y = points != 0u ? where[index][1] : rectangle.top;",
        "const int32_t point_y = points != 0u ? where[index][1] : 0;",
        "the same on the vertical axis.",
    ),
    mutation(
        "byte-path-rewrites-callers-rectangles",
        "            if (rects != 0u) {\n                const uint32_t written[4]",
        "            if (rects != 0u && false) {\n                const uint32_t written[4]",
        "the original overwrites the caller's own rectangles with (0, 0, width, height) on the byte path.",
    ),
    mutation(
        "byte-path-rewrite-order",
        "const uint32_t written[4] = {0u, 0u, byte_width, byte_height};",
        "const uint32_t written[4] = {0u, 0u, byte_height, byte_width};",
        "right is the width and bottom the height.",
    ),
    mutation(
        "source-fence-tag",
        "d3d8_guest_store32(source_tag, fence);",
        "d3d8_guest_store32(source_tag, fence + 2u);",
        "the source (its parent) is tagged with the device's next fence value.",
    ),
    mutation(
        "destination-fence-tag",
        "d3d8_guest_store32(destination_tag, fence);",
        "d3d8_guest_store32(destination_tag, fence + 2u);",
        "and the destination.",
    ),
    mutation(
        "source-parent-tag",
        "const uint32_t source_tag = (from[SURFACE_PARENT] != 0u ? from[SURFACE_PARENT] : source) + SURFACE_LOCK_BYTES;",
        "const uint32_t source_tag = source + SURFACE_LOCK_BYTES;",
        "a source with a parent tags the parent, not itself.",
    ),
    mutation(
        "return-value",
        "return to[SURFACE_PARENT] != 0u ? to[SURFACE_PARENT] : fence;",
        "return fence;",
        "eax at the ret is the destination's parent when it has one, else the fence.",
    ),
    mutation(
        "count-zero-is-one",
        "if (count == 0u) count = 1u;",
        "if (count == 0u && false) count = 1u;",
        "a zero count still copies one rectangle.",
    ),
    mutation(
        "null-destination",
        "if (source == 0u || destination == 0u)",
        "if (source == 0u)",
        "the original dereferences a null destination: a named refusal precedes any write.",
    ),
    mutation(
        "null-source",
        "if (source == 0u || destination == 0u)",
        "if (destination == 0u)",
        "and a null source.",
    ),
    mutation(
        "rectangle-bound",
        "#define MAX_RECTS 4096u",
        "#define MAX_RECTS 4097u",
        "the port bounds the rectangle count and refuses above it by name.",
    ),
    mutation(
        "state-packet-bytes",
        "#define BLIT_STATE_BYTES 24u",
        "#define BLIT_STATE_BYTES 28u",
        "the state packet is six dwords, the cursor advances by exactly that.",
    ),
    mutation(
        "blit-packet-bytes",
        "#define BLIT_RECT_BYTES 16u",
        "#define BLIT_RECT_BYTES 20u",
        "each blit is four dwords, planned and written.",
    ),
    mutation(
        "copy-format-offset",
        "#define DEV_COPY_FORMAT 0x192Cu",
        "#define DEV_COPY_FORMAT 0x1928u",
        "the override word is device+0x192C.",
    ),
    mutation(
        "alias-array-ring",
        "if (touches_library_state(array_spans[index][0], array_spans[index][1]))",
        "if (touches_library_state(array_spans[index][0], array_spans[index][1]) && false)",
        "a rectangle or point array in the ring would be read after the blits have overwritten it.",
    ),
    mutation(
        "alias-header-ring",
        "if (touches_library_state(input_spans[index][0], input_spans[index][1]))",
        "if (touches_library_state(input_spans[index][0], input_spans[index][1]) && false)",
        "a surface header in the library's own state is refused.",
    ),
    mutation(
        "alias-lock-word-ring",
        "if (touches_library_state(source_tag, 4u) || touches_library_state(destination_tag, 4u))",
        "if (touches_library_state(source_tag, 4u) && false)",
        "the destination's Lock word is checked against the library's state too.",
    ),
    mutation(
        "alias-lock-word-source",
        "if (touches_library_state(source_tag, 4u) || touches_library_state(destination_tag, 4u))",
        "if (touches_library_state(destination_tag, 4u))",
        "and the source's.",
    ),
    mutation(
        "alias-lock-word-header-word",
        "if (spans_overlap(source_tag, 4u, read_words[word], 4u) ||\n            spans_overlap(destination_tag, 4u, read_words[word], 4u))",
        "if (spans_overlap(source_tag, 4u, read_words[word], 4u))",
        "a Lock word store over a header word the copy still reads changes the original's later reads.",
    ),
    mutation(
        "alias-arrays-with-each-other",
        "if (first != second && spans_overlap(array_spans[first][0], array_spans[first][1], array_spans[second][0],",
        "if (first != second && false && spans_overlap(array_spans[first][0], array_spans[first][1], array_spans[second][0],",
        "the byte path rewrites the rectangle list while the points are still to be read.",
    ),
    mutation(
        "alias-array-header",
        "if (spans_overlap(array_spans[first][0], array_spans[first][1], input_spans[header][0],",
        "if (false && spans_overlap(array_spans[first][0], array_spans[first][1], input_spans[header][0],",
        "an array over a surface header changes what the copy reads.",
    ),
    mutation(
        "alias-array-lock-word",
        "if (spans_overlap(array_spans[first][0], array_spans[first][1], source_tag, 4u) ||\n            spans_overlap(array_spans[first][0], array_spans[first][1], destination_tag, 4u))",
        "if (spans_overlap(array_spans[first][0], array_spans[first][1], source_tag, 4u))",
        "an array over the destination's Lock word.",
    ),
    mutation(
        "library-state-ring",
        "return spans_overlap(address, bytes, ring, ring_end - ring) ||",
        "return (spans_overlap(address, bytes, ring, ring_end - ring) && false) ||",
        "the ring is where the copy writes its packets.",
    ),
    mutation(
        "library-state-cursor",
        "spans_overlap(address, bytes, D3D8_DEVICE_BASE, DEV_REFILL_WORDS) ||",
        "false ||",
        "the cursor, limit, fence and refill words are the first 0x64 bytes of the device.",
    ),
    mutation(
        "library-state-copy-format",
        "spans_overlap(address, bytes, D3D8_DEVICE_BASE + DEV_COPY_FORMAT, 4u) ||",
        "false ||",
        "the format override word is read once, before any write.",
    ),
    mutation(
        "library-state-device-slot",
        "spans_overlap(address, bytes, D3D8_DEVICE_POINTER_SLOT, 4u) ||",
        "false ||",
        "the device pointer slot every emitter reloads.",
    ),
    mutation(
        "library-state-control-page",
        "spans_overlap(address, bytes, d3d8_device_load32(0x30u) & ~0xFFFu, 0x1000u) ||",
        "false ||",
        "the control page a refill reads and writes.",
    ),
    mutation(
        "library-state-history",
        "history_bytes > UINT32_MAX || spans_overlap(address, bytes, history, (uint32_t)history_bytes);",
        "history_bytes > UINT32_MAX || (spans_overlap(address, bytes, history, (uint32_t)history_bytes) && false);",
        "the kick history ring a refill appends to.",
    ),
    mutation(
        "probe-source-tag",
        "    probe_writable(source_tag, 4u);\n",
        "",
        "an unwritable Lock word is refused before the first packet instead of after the blits.",
    ),
    mutation(
        "probe-destination-tag",
        "    probe_writable(destination_tag, 4u);\n",
        "",
        "and the destination's.",
    ),
    mutation(
        "plan-sites",
        "        planned[1u + index] = d3d8_pushbuffer_sim_site(&sim, RECTS_ENTRY, BLIT_RECT_BYTES);",
        "        planned[1u + index] = 0u;",
        "a roll-over the model refuses at a later blit must be found before the first write.",
    ),
]
