# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the surface model (T633, T596): what the replay takes a surface to hold when no replayed pass drew it.

T633 (1) and T596 (`--gpu-replay-surface-source`): a texture stage that samples a surface no pass of the frame drew, and a CopyRects
blit over a surface the replay holds no image of, take the kept image of an earlier frame, else the guest memory under it
(`d3d8_surface_model`, `d3d8_swap_replay`). The Y8 and R5G6B5 byte path blits move bytes over guest BYTE surfaces
(`gpu_pgraph_replay_byte_copy`), with the xemu rules MEASURED in T769 (`docs/t769-xemu-copyrects.md`): the row is clamped to the narrower pitch in
PIXELS, a row that passes its pitch runs on into the next row, rows ascend with each row read whole before it is written, formats 7 and 6
force the alpha byte, and a rectangle inside one surface or between two pitches is copied, never refused. T633 (2) (`--gpu-replay-target-persist`): a pass starts from the kept image
(`gpu_pgraph_backend.initial_pixels`). T633 (3): the overlay note and the summary. Grouped by what a survivor would let through:

    probe       `d3d8_surface_model_probe`: the 64 KiB pieces, the range, the zero test, the unreadable verdict
    argb        `d3d8_surface_model_read_argb`: the pitch guard, the row stride, the byte swap, a failed row
    acquire     the byte surface store: limits, alias, slot reuse, FULL, growth reads only the new tail, a failed grow keeps the length
    overlay     read_bytes (kept over guest), `overlaps_bytes`, the counters, the status texts, reset, the injected reader
    bytecopy    `gpu_pgraph_blit_bytes_per_pixel`, `gpu_pgraph_blit_row_pixels` (the pixel clamp), `gpu_pgraph_blit_image_rows`,
                `gpu_pgraph_blit_extent_bytes`, `gpu_pgraph_replay_byte_copy` (the extents, the ascending row buffered copy, the spill into the
                next row, the alpha byte of 7 and 6), `initial_pixels`
    enable      `d3d8_swap_replay_enable` refusals of `surface_source` and `target_persist`
    surface     `blit_written_before`, `provide_surface` (every refusal, the kept image exactness, guest or kept, the texts)
    provider    `provide_texture`'s surface branches, `texture_sampled` and its counters
    store       `claim_slot`, `store_pass_image`, `surfaces_are_kept`
    persist     `persist_initial`, the `replay_pass` wiring of the initial image and the TARGET_PERSIST and SURFACE_SOURCE bits
    blit        `resolve_blit_surface`, `guest_blit_surface`, `grow_guest_surface`, `apply_byte_copy`, `apply_copies`
    note        `note_overlay` and `d3d8_swap_replay_surface_summary`
    header      the two inference bits of `d3d8_swap_replay.h`
    hostopt     the two flags, their defaults and the requirement chain of `parse_options`

The kills are by `test_d3d8_surface_model` (no device, no guest: the model and the byte copy over a flat array), `test_gpu_pgraph_replay`
(device) and `test_d3d8_swap_replay` (the configuration, the census and the overlay scenes need no device, the replayed surface scenes
need one) and `test_host_options`. DEVICE-DEPENDENT kills (exit 77 without a Vulkan device) are reported SKIPPED by the harness, never
killed: `_NEEDS_DEVICE` at the bottom records which ones, from a run with the Vulkan loader hidden
(`VK_DRIVER_FILES=/nonexistent VK_ICD_FILENAMES=/nonexistent`).

NOT MUTATED, and why: `src/host/main.c` (the flag wiring and the announcement, `tsfp_host` is not a ctest binary), every `malloc` and
`realloc` failure branch and every `free` (an out of memory path and a leak are seen by a sanitizer, not by a ctest), the log lines of
`apply_copies` and `note_overlay` other than the once-only counters (the second frame of each kind is checked through the captured log),
the buffer sizes whose only effect is a heap overflow (`image_bytes`, `rows - old_rows` in `grow_guest_surface`, the row buffer of `read_argb`
is covered by the recorded read length instead).

EQUIVALENT, so left out (a survivor no input can kill is not a missing test):
  - `d3d8_surface_model.c`: the `(uint64_t)` casts of the overlap tests (`size_t` is 64 bit, the sum is wide anyway), the `valid` term of
    `overlaps_other` and `overlaps_bytes` (an invalid slot is zeroed: `data < 0 + 0` is never true), `held = slot->valid ? slot->length : 0`
    (an invalid slot has length 0), `if (slot->valid)` of a failed grow (an invalid slot with a kept pointer is released by the next acquire
    or by reset, no observer sees the difference), `length > PROBE_PIECE` against `>=` (the piece is the same length).
  - `gpu_pgraph_replay_byte_copy`: `memmove` for the `memcpy` of a row (the row buffer is a third array, it never overlaps the surface).
  - `provide_surface`: the `allowed_inferences` guard (enable refuses a surface source without the inference, so it cannot be reached), the
    `kept->image.pixels == NULL` and stride terms (every kept image is packed, the stride is `width * 4`), `out->width` and `out->height` of
    a dry (census) resolve (the census replays no pixel), `surface_buffer_bytes < image_bytes` for `<=` (a realloc to the same size), the
    pitch of the texture bytes and of `read_argb` and the failure of `read_argb` (`provide_texture` refuses a header whose pitch is not
    `width * 4` as "unsupported layout" before the surface is looked at, and the probe already read the same bytes), the zero pitch and
    Format guards of the kept image exactness (a kept image of a header with a Size word always has both).
  - `d3d8_swap_replay.c`: `provider.surface_sampled = false` in `replay_pass` (`used_inferences` is ORed over every pass and frame), `*stored = NULL`
    in `resolve_blit_surface` (every caller initialises it), `surfaces_are_kept`'s `surface_source` term (the option needs the BLIT group, which
    already keeps them), `copy->color_format == A8R8G8B8` of `guest` in `apply_copies` (every other format took the byte path), `source_length =
    destination_length = need` of one surface (the lengths are only compared to the extents, which are `need` at most), the `touched = false` of
    `store_pass_image` (the `memset` of `claim_slot` did it), `guest_blit_surface`'s `rows == 0u` and `pitch == 0u` terms (the decoder refuses a zero pitch and the callers pass at least one row),
    its width `pitch / 4u` against a rounded up value (a pitch that is not a multiple of 4 is refused first, so there is no remainder), `blit_written_before`'s clamped row pixels (the
    decoder refuses a row wider than a pitch until T843 lifts it, so the clamped and the requested width are the same number end to end; `gpu_pgraph_blit_row_pixels` itself is tested), `d3d8_surface_model_has_bytes(data)` (`overlaps_bytes(data, bytes)` with bytes > 0 holds for it),
    `other->data != data` (it is only reached when no kept image of that Data word exists), `slot->image.height = rows` for `rows - 1u` (the growth
    that follows reads the missing row), the pitch argument of the growth read (`surface->pitch` is `width * 4` of a guest built surface),
    `source_need == 0u || destination_need == 0u` of `apply_byte_copy` (a zero extent needs a zero pitch, which the decoder refuses, or an
    unmeasured format, which takes no byte path), the `state.pgraph != NULL` term of `blit_written_before`.
  - `persist_initial`: the width term (the stride term compares the same quantity, `stride_bytes == width * 4` of a kept image), the stride term
    (the width term), `held->image.pixels == NULL`, and `data == 0u` (a kept image under Data 0 needs a blit offset of 0, whose guest address 0 is
    never readable, and `store_pass_image` keeps no image under Data 0).

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

_MODEL = "src/gpu/d3d8_surface_model.c"
_REPLAY = "src/gpu/gpu_pgraph_replay.c"
_SWAP = "src/gpu/d3d8_swap_replay.c"
_SWAP_H = "src/gpu/d3d8_swap_replay.h"
_OPTIONS = "src/host/host_options.c"

_MODEL_TEST = ["test_d3d8_surface_model"]
_COPY_TEST = ["test_d3d8_surface_model", "test_gpu_pgraph_replay", "test_d3d8_swap_replay"]
_SWAP_TEST = ["test_d3d8_swap_replay"]
_OPTION_TEST = ["test_host_options"]


def _row(file: str, mutation_id: str, old: str, new: str, why: str, targets: list[str]) -> dict:
    return {
        "id": f"t633-surface-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(targets),
        "why": why,
    }


def _mo(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_MODEL, mutation_id, old, new, why, _MODEL_TEST)


def _rp(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_REPLAY, mutation_id, old, new, why, _COPY_TEST)


def _sw(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_SWAP, mutation_id, old, new, why, _SWAP_TEST)


def _hd(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_SWAP_H, mutation_id, old, new, why, _SWAP_TEST)


def _ho(mutation_id: str, old: str, new: str, why: str) -> dict:
    return _row(_OPTIONS, mutation_id, old, new, why, _OPTION_TEST)


MUTATIONS: list[dict] = [
    # ================================================================== probe
    _mo(
        "probe-piece-halved",
        "#define PROBE_PIECE 65536u",
        "#define PROBE_PIECE 32768u",
        "a probe reads in 32 KiB pieces: twice the reads of the documented 64 KiB pieces.",
    ),
    _mo(
        "probe-piece-doubled",
        "#define PROBE_PIECE 65536u",
        "#define PROBE_PIECE 131072u",
        "a probe asks the guest for 128 KiB at once: more than the documented piece, a reader with a bound refuses it.",
    ),
    _mo(
        "probe-bound-short",
        "for (uint64_t done = 0u; done < bytes && result == D3D8_SURFACE_ZERO;) {",
        "for (uint64_t done = 0u; done + 1u < bytes && result == D3D8_SURFACE_ZERO;) {",
        "the last byte of a range that ends a piece is never looked at.",
    ),
    _mo(
        "probe-no-early-stop",
        "for (uint64_t done = 0u; done < bytes && result == D3D8_SURFACE_ZERO;) {",
        "for (uint64_t done = 0u; done < bytes;) {",
        "a nonzero byte in the first piece does not end the probe: every later piece is read for nothing.",
    ),
    _mo(
        "probe-length-always-a-piece",
        "const size_t length = bytes - done < PROBE_PIECE ? (size_t)(bytes - done) : PROBE_PIECE;",
        "const size_t length = PROBE_PIECE;",
        "the last piece reads a whole piece, so a byte past the surface decides a surface that is all zero.",
    ),
    _mo(
        "probe-piece-offset-lost",
        "if (!guest_read(data + (uint32_t)done, piece, length)) {",
        "if (!guest_read(data, piece, length)) {",
        "every piece reads the first piece's bytes: a nonzero byte past 64 KiB is never seen.",
    ),
    _mo(
        "probe-unreadable-lost",
        "result = D3D8_SURFACE_UNREADABLE;\n            break;",
        "break;",
        "an unreadable range is reported as all zero.",
    ),
    _mo(
        "probe-last-byte-skipped",
        "for (size_t i = 0u; i < length; i++) {\n            if (piece[i] != 0u) {",
        "for (size_t i = 0u; i + 1u < length; i++) {\n            if (piece[i] != 0u) {",
        "the last byte of every piece is not looked at.",
    ),
    _mo(
        "probe-high-bit-only",
        "if (piece[i] != 0u) {",
        "if (piece[i] > 0x7Fu) {",
        "only a byte with its high bit set is nonzero: a texel written as 0x01 reads as zero.",
    ),
    # ================================================================== argb
    _mo(
        "argb-pitch-tight-refused",
        "pitch < width * 4u) {\n        return D3D8_SURFACE_MODEL_UNREADABLE;",
        "pitch <= width * 4u) {\n        return D3D8_SURFACE_MODEL_UNREADABLE;",
        "a tightly packed surface (pitch == width * 4, the back buffer) is refused.",
    ),
    _mo(
        "argb-pitch-guard-dropped",
        "rgba == NULL || width == 0u || rows == 0u || pitch < width * 4u) {",
        "rgba == NULL || width == 0u || rows == 0u) {",
        "a pitch narrower than a row is read, rows overlap.",
    ),
    _mo(
        "argb-width-zero-allowed",
        "rgba == NULL || width == 0u || rows == 0u || pitch < width * 4u) {",
        "rgba == NULL || rows == 0u || pitch < width * 4u) {",
        "a zero width surface reads as an empty image with OK.",
    ),
    _mo(
        "argb-rows-zero-allowed",
        "rgba == NULL || width == 0u || rows == 0u || pitch < width * 4u) {",
        "rgba == NULL || width == 0u || pitch < width * 4u) {",
        "a zero height surface reads as an empty image with OK.",
    ),
    _mo(
        "argb-null-image-allowed",
        "rgba == NULL || width == 0u || rows == 0u || pitch < width * 4u) {",
        "width == 0u || rows == 0u || pitch < width * 4u) {",
        "a NULL destination is written to.",
    ),
    _mo(
        "argb-red-not-swapped",
        "out[x * 4u + 0u] = row[x * 4u + 2u];",
        "out[x * 4u + 0u] = row[x * 4u + 0u];",
        "red takes the blue byte: the image is blue where the guest wrote red.",
    ),
    _mo(
        "argb-blue-not-swapped",
        "out[x * 4u + 2u] = row[x * 4u + 0u];",
        "out[x * 4u + 2u] = row[x * 4u + 2u];",
        "blue takes the red byte.",
    ),
    _mo(
        "argb-green-from-blue",
        "out[x * 4u + 1u] = row[x * 4u + 1u];",
        "out[x * 4u + 1u] = row[x * 4u + 0u];",
        "green takes the blue byte.",
    ),
    _mo(
        "argb-alpha-opaque",
        "out[x * 4u + 3u] = row[x * 4u + 3u];",
        "out[x * 4u + 3u] = 255u;",
        "every texel is opaque: the transparent black of a never drawn surface becomes black.",
    ),
    _mo(
        "argb-row-stride-tight",
        "if (!guest_read(data + y * pitch, row, (size_t)width * 4u)) {",
        "if (!guest_read(data + y * width * 4u, row, (size_t)width * 4u)) {",
        "rows are read a width apart, not a pitch: a surface with padding shears.",
    ),
    _mo(
        "argb-out-stride-pitch",
        "uint8_t *out = rgba + (size_t)y * width * 4u;",
        "uint8_t *out = rgba + (size_t)y * pitch;",
        "the image keeps the guest's padding: not the tightly packed RGBA the callers read.",
    ),
    _mo(
        "argb-row-length-pitch",
        "if (!guest_read(data + y * pitch, row, (size_t)width * 4u)) {",
        "if (!guest_read(data + y * pitch, row, (size_t)pitch)) {",
        "a row reads its padding too: a read past the end of the last row and a row buffer overflow.",
    ),
    _mo(
        "argb-failed-row-continues",
        "status = D3D8_SURFACE_MODEL_UNREADABLE;\n            break;",
        "status = D3D8_SURFACE_MODEL_UNREADABLE;",
        "the rows after an unreadable one are still read, and the image holds stale bytes of the row before.",
    ),
    # ================================================================== acquire
    _mo(
        "acquire-zero-length-allowed",
        "if (length == 0u || length > D3D8_SURFACE_MODEL_MAX_BYTES) {",
        "if (length > D3D8_SURFACE_MODEL_MAX_BYTES) {",
        "a zero length surface is kept (a NULL byte pointer and OK).",
    ),
    _mo(
        "acquire-max-inclusive",
        "if (length == 0u || length > D3D8_SURFACE_MODEL_MAX_BYTES) {",
        "if (length == 0u || length >= D3D8_SURFACE_MODEL_MAX_BYTES) {",
        "a surface of exactly the bound is refused as too large.",
    ),
    _mo(
        "acquire-alias-unchecked",
        "if (overlaps_other(data, length)) {\n        return D3D8_SURFACE_MODEL_ALIAS;\n    }\n    byte_surface *slot",
        "if (false) {\n        return D3D8_SURFACE_MODEL_ALIAS;\n    }\n    byte_surface *slot",
        "a surface overlapping another Data word's bytes is kept: two surfaces hold one byte.",
    ),
    _mo(
        "acquire-slot-reuse-lost",
        "byte_surface *slot = find_bytes(data);",
        "byte_surface *slot = NULL;",
        "a second acquire of a Data word takes a new slot: the kept bytes are shadowed and the store fills up.",
    ),
    _mo(
        "acquire-free-slot-inverted",
        "if (!model.surfaces[i].valid) {\n            slot = &model.surfaces[i];",
        "if (model.surfaces[i].valid) {\n            slot = &model.surfaces[i];",
        "a new surface takes a slot that holds another one.",
    ),
    _mo(
        "acquire-last-slot-unused",
        "for (size_t i = 0u; slot == NULL && i < D3D8_SURFACE_MODEL_BYTE_SURFACES; i++) {",
        "for (size_t i = 0u; slot == NULL && i + 1u < D3D8_SURFACE_MODEL_BYTE_SURFACES; i++) {",
        "the store holds one surface fewer than it says.",
    ),
    _mo(
        "acquire-full-status",
        "if (slot == NULL) {\n        return D3D8_SURFACE_MODEL_FULL;",
        "if (slot == NULL) {\n        return D3D8_SURFACE_MODEL_MEMORY;",
        "a full store reports out of memory.",
    ),
    _mo(
        "acquire-equal-length-regrows",
        "if (held >= length) {",
        "if (held > length) {",
        "an ask of the held length reallocates and reads again (a zero byte read of the guest).",
    ),
    _mo(
        "acquire-grow-reads-everything",
        "if (!guest_read(data + (uint32_t)held, grown + held, length - held)) {",
        "if (!guest_read(data, grown, length)) {",
        "a growth rereads the whole surface from the guest and overwrites what the blits wrote.",
    ),
    _mo(
        "acquire-grow-tail-length-whole",
        "if (!guest_read(data + (uint32_t)held, grown + held, length - held)) {",
        "if (!guest_read(data + (uint32_t)held, grown + held, length)) {",
        "a growth reads `length` bytes into the tail: past the new end, and unreadable near the end of guest memory.",
    ),
    _mo(
        "acquire-grow-tail-offset-lost",
        "if (!guest_read(data + (uint32_t)held, grown + held, length - held)) {",
        "if (!guest_read(data, grown + held, length - held)) {",
        "the new tail holds the first bytes of the surface.",
    ),
    _mo(
        "acquire-failed-grow-pointer-lost",
        "slot->bytes = grown; /* realloc moved it: keep the pointer, the length stays */",
        "/* realloc moved it */",
        "after a failed grow the slot keeps a pointer realloc freed: the kept bytes are garbage.",
    ),
    _mo(
        "acquire-failed-grow-length-bumped",
        "            free(grown);\n        }\n        return D3D8_SURFACE_MODEL_UNREADABLE;",
        "            free(grown);\n        }\n        slot->length = length;\n        return D3D8_SURFACE_MODEL_UNREADABLE;",
        "a failed grow records the longer length: bytes that were never read are kept.",
    ),
    _mo(
        "acquire-valid-not-set",
        "slot->valid = true;\n    *bytes = grown;",
        "*bytes = grown;",
        "a surface is never marked held: it is lost at the next call.",
    ),
    _mo(
        "acquire-data-not-set",
        "slot->data = data;\n    slot->valid = true;",
        "slot->valid = true;",
        "the kept surface is not found under its Data word.",
    ),
    _mo(
        "acquire-length-not-set",
        "slot->length = length;\n    slot->data = data;",
        "slot->data = data;",
        "the kept length stays 0: every read sees no kept bytes.",
    ),
    _mo(
        "acquire-held-path-bytes-not-set",
        "*bytes = slot->bytes;\n        return D3D8_SURFACE_MODEL_OK;",
        "return D3D8_SURFACE_MODEL_OK;",
        "an ask the surface already covers returns OK and leaves the caller's pointer as it was.",
    ),
    # ================================================================== overlay (the readers and the store)
    _mo(
        "overlap-other-own-data",
        "other->valid && other->data != data && data < (uint64_t)other->data + other->length &&",
        "other->valid && data < (uint64_t)other->data + other->length &&",
        "a surface is an alias of itself: a second acquire of the same Data word is refused.",
    ),
    _mo(
        "overlap-other-below-inclusive",
        "other->data != data && data < (uint64_t)other->data + other->length &&",
        "other->data != data && data <= (uint64_t)other->data + other->length &&",
        "a surface that starts exactly where another ends is an alias.",
    ),
    _mo(
        "overlap-other-above-inclusive",
        "other->data < (uint64_t)data + bytes) {\n            return true;\n        }\n    }\n    return false;\n}\n\nbool d3d8_surface_model_overlaps_bytes",
        "other->data <= (uint64_t)data + bytes) {\n            return true;\n        }\n    }\n    return false;\n}\n\nbool d3d8_surface_model_overlaps_bytes",
        "a surface that ends exactly where another starts is an alias.",
    ),
    _mo(
        "overlaps-bytes-below-inclusive",
        "if (other->valid && data < (uint64_t)other->data + other->length && other->data",
        "if (other->valid && data <= (uint64_t)other->data + other->length && other->data",
        "a range that starts exactly where a kept surface ends overlaps it: a texture right above a byte surface is refused.",
    ),
    _mo(
        "overlaps-bytes-above-inclusive",
        "other->data < (uint64_t)data + bytes) {\n            return true;\n        }\n    }\n    return false;\n}\n\nd3d8_surface_model_status d3d8_surface_model_acquire",
        "other->data <= (uint64_t)data + bytes) {\n            return true;\n        }\n    }\n    return false;\n}\n\nd3d8_surface_model_status d3d8_surface_model_acquire",
        "a range that ends exactly where a kept surface starts overlaps it.",
    ),
    _mo(
        "find-bytes-invalid-slot",
        "if (model.surfaces[i].valid && model.surfaces[i].data == data) {",
        "if (model.surfaces[i].data == data) {",
        "an empty slot (Data 0) is found as a held surface.",
    ),
    _mo(
        "has-bytes-inverted",
        "return find_bytes(data) != NULL;",
        "return find_bytes(data) == NULL;",
        "has_bytes answers the other way.",
    ),
    _mo(
        "read-bytes-alias-unchecked",
        "if (overlaps_other(data, length)) {\n        return D3D8_SURFACE_MODEL_ALIAS;\n    }\n    if (length != 0u",
        "if (false) {\n        return D3D8_SURFACE_MODEL_ALIAS;\n    }\n    if (length != 0u",
        "a read that straddles a kept surface of another Data word is served from the guest alone.",
    ),
    _mo(
        "read-bytes-zero-length-reads",
        "if (length != 0u && !guest_read(data, out, length)) {",
        "if (!guest_read(data, out, length)) {",
        "a zero length read asks the guest for it: an unreadable address refuses an empty read.",
    ),
    _mo(
        "read-bytes-unreadable-ignored",
        "!guest_read(data, out, length)) {\n        return D3D8_SURFACE_MODEL_UNREADABLE;",
        "!guest_read(data, out, length) && false) {\n        return D3D8_SURFACE_MODEL_UNREADABLE;",
        "an unreadable guest range reads as OK with stale output bytes.",
    ),
    _mo(
        "read-bytes-kept-over-lost",
        "if (kept != NULL) {\n        memcpy(out, kept->bytes,",
        "if (kept != NULL && false) {\n        memcpy(out, kept->bytes,",
        "the kept bytes are not laid over the guest's: a byte copy never shows.",
    ),
    _mo(
        "read-bytes-kept-unguarded",
        "if (kept != NULL) {\n        memcpy(out, kept->bytes,",
        "if (true) {\n        memcpy(out, kept->bytes,",
        "a range with no kept surface dereferences NULL.",
    ),
    _mo(
        "read-bytes-copies-whole-kept",
        "memcpy(out, kept->bytes, kept->length < length ? kept->length : length);",
        "memcpy(out, kept->bytes, kept->length);",
        "a short read copies the whole kept surface over the caller's buffer.",
    ),
    _mo(
        "read-bytes-copies-whole-ask",
        "memcpy(out, kept->bytes, kept->length < length ? kept->length : length);",
        "memcpy(out, kept->bytes, length);",
        "a read longer than the kept surface copies bytes past it.",
    ),
    _mo(
        "count-keeps-last",
        "count += model.surfaces[i].valid ? 1u : 0u;",
        "count = model.surfaces[i].valid ? 1u : 0u;",
        "the count is the validity of the last slot.",
    ),
    _mo(
        "count-every-slot",
        "count += model.surfaces[i].valid ? 1u : 0u;",
        "count += 1u;",
        "the count is the slot count.",
    ),
    _mo(
        "length-absent-nonzero",
        "return kept != NULL ? kept->length : 0u;",
        "return kept != NULL ? kept->length : 1u;",
        "a Data word with no byte surface has a length.",
    ),
    _mo(
        "status-alias-text-full",
        'case D3D8_SURFACE_MODEL_ALIAS: return "the range overlaps a kept byte surface of another Data word";',
        'case D3D8_SURFACE_MODEL_ALIAS: return "every byte surface slot is taken";',
        "an alias refusal names a full store.",
    ),
    _mo(
        "status-unknown-text-empty",
        'return "unknown";',
        'return "";',
        "an out of range status has an empty text.",
    ),
    _mo(
        "status-ok-text",
        'case D3D8_SURFACE_MODEL_OK: return "ok";',
        'case D3D8_SURFACE_MODEL_OK: return "";',
        "the OK status has no text.",
    ),
    _mo(
        "reset-keeps-slots",
        "memset(&model.surfaces[i], 0, sizeof model.surfaces[i]);",
        "",
        "a reset leaves the surfaces held (and their freed bytes).",
    ),
    _mo(
        "reset-last-slot-kept",
        "for (size_t i = 0u; i < D3D8_SURFACE_MODEL_BYTE_SURFACES; i++) {\n        free(model.surfaces[i].bytes);",
        "for (size_t i = 0u; i + 1u < D3D8_SURFACE_MODEL_BYTE_SURFACES; i++) {\n        free(model.surfaces[i].bytes);",
        "a reset forgets every surface but the last slot's.",
    ),
    _mo(
        "reader-context-dropped",
        "model.context = context;",
        "(void)context;\n    model.context = NULL;",
        "the injected reader is called with a NULL context.",
    ),
    _row(
        _MODEL,
        "guest-reader-default-refuses",
        "return d3d8_gpu_read_guest(NULL, address, out, bytes);",
        "return false;",
        "with no injected reader every guest read fails: the replay reads no guest memory.",
        ["test_d3d8_surface_model", "test_d3d8_swap_replay"],
    ),
    # ================================================================== bytecopy
    _rp(
        "bpp-y8",
        "case GPU_PGRAPH_BLIT_FORMAT_Y8: return 1u;",
        "case GPU_PGRAPH_BLIT_FORMAT_Y8: return 2u;",
        "a Y8 pixel is two bytes.",
    ),
    _rp(
        "bpp-r5g6b5",
        "case GPU_PGRAPH_BLIT_FORMAT_R5G6B5: return 2u;",
        "case GPU_PGRAPH_BLIT_FORMAT_R5G6B5: return 4u;",
        "an R5G6B5 pixel is four bytes.",
    ),
    _rp(
        "bpp-a8r8g8b8",
        "    case GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8:\n    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF:\n    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0: return 4u;",
        "    case GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8: return 2u;\n    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF:\n    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0: return 4u;",
        "an A8R8G8B8 pixel is two bytes.",
    ),
    _rp(
        "bpp-format-7-dropped",
        "    case GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8:\n    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF:\n    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0: return 4u;",
        "    case GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8:\n    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0: return 4u;",
        "format 7 (X8R8G8B8, alpha 0xFF) has no pixel size: every blit of it is refused as unmeasured.",
    ),
    _rp(
        "bpp-format-6-dropped",
        "    case GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8:\n    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF:\n    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0: return 4u;",
        "    case GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8:\n    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF: return 4u;",
        "format 6 (X8R8G8B8, alpha 0) has no pixel size: every blit of it is refused as unmeasured.",
    ),
    _rp(
        "bpp-format-7-two-bytes",
        "    case GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8:\n    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF:\n    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0: return 4u;",
        "    case GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8:\n    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0: return 4u;\n    case GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF: return 2u;",
        "a format 7 pixel is two bytes.",
    ),
    _rp(
        "bpp-default",
        "default: return 0u;\n    }\n}\n\nuint32_t gpu_pgraph_blit_row_pixels",
        "default: return 1u;\n    }\n}\n\nuint32_t gpu_pgraph_blit_row_pixels",
        "an unmeasured colour format is a byte a pixel.",
    ),
    _rp(
        "row-pixels-format-unchecked",
        "    if (bytes_per_pixel == 0u) {\n        return 0u;\n    }\n    uint32_t pixels = copy->width;",
        "    uint32_t pixels = copy->width;",
        "the row clamp of an unmeasured format divides by its zero pixel size.",
    ),
    _rp(
        "row-pixels-width-ignored",
        "    uint32_t pixels = copy->width;",
        "    uint32_t pixels = 0xFFFFFFFFu;",
        "the requested width is not a bound: a row is as wide as the narrower pitch.",
    ),
    _rp(
        "row-pixels-source-unclamped",
        "    if (copy->source_pitch / bytes_per_pixel < pixels) {",
        "    if (false) {",
        "the source pitch does not clamp the row (xemu: a narrow source pitch clamps every format).",
    ),
    _rp(
        "row-pixels-destination-unclamped",
        "    if (copy->destination_pitch / bytes_per_pixel < pixels) {",
        "    if (false) {",
        "the destination pitch does not clamp the row (xemu: a narrow destination pitch clamps every format).",
    ),
    _rp(
        "row-pixels-source-bytes",
        "    if (copy->source_pitch / bytes_per_pixel < pixels) {\n        pixels = copy->source_pitch / bytes_per_pixel;",
        "    if (copy->source_pitch < pixels) {\n        pixels = copy->source_pitch;",
        "the source clamp counts the pitch as pixels (a byte a pixel), so a 4 byte format is clamped too late.",
    ),
    _rp(
        "row-pixels-destination-bytes",
        "    if (copy->destination_pitch / bytes_per_pixel < pixels) {\n        pixels = copy->destination_pitch / bytes_per_pixel;",
        "    if (copy->destination_pitch < pixels) {\n        pixels = copy->destination_pitch;",
        "the destination clamp counts the pitch as pixels (a byte a pixel), so a 4 byte format is clamped too late.",
    ),
    _rp(
        "row-pixels-source-rounded-up",
        "    if (copy->source_pitch / bytes_per_pixel < pixels) {\n        pixels = copy->source_pitch / bytes_per_pixel;",
        "    if ((copy->source_pitch + bytes_per_pixel - 1u) / bytes_per_pixel < pixels) {\n        pixels = (copy->source_pitch + bytes_per_pixel - 1u) / bytes_per_pixel;",
        "the source clamp rounds the pitch UP to whole pixels (xemu: pitch 150 over 4 byte pixels is 37 pixels, not 38).",
    ),
    _rp(
        "row-pixels-destination-rounded-up",
        "    if (copy->destination_pitch / bytes_per_pixel < pixels) {\n        pixels = copy->destination_pitch / bytes_per_pixel;",
        "    if ((copy->destination_pitch + bytes_per_pixel - 1u) / bytes_per_pixel < pixels) {\n        pixels = (copy->destination_pitch + bytes_per_pixel - 1u) / bytes_per_pixel;",
        "the destination clamp rounds the pitch UP to whole pixels (xemu: pitch 151 over 2 byte pixels is 75 pixels, not 76).",
    ),
    _rp(
        "extent-format-unchecked",
        "if (bytes_per_pixel == 0u || pitch == 0u || row_pixels == 0u || height == 0u) {",
        "if (pitch == 0u || row_pixels == 0u || height == 0u) {",
        "an unmeasured format has an extent of its rows.",
    ),
    _rp(
        "extent-pitch-unchecked",
        "if (bytes_per_pixel == 0u || pitch == 0u || row_pixels == 0u || height == 0u) {",
        "if (bytes_per_pixel == 0u || row_pixels == 0u || height == 0u) {",
        "a zero pitch has an extent.",
    ),
    _rp(
        "extent-width-unchecked",
        "if (bytes_per_pixel == 0u || pitch == 0u || row_pixels == 0u || height == 0u) {",
        "if (bytes_per_pixel == 0u || pitch == 0u || height == 0u) {",
        "a zero width (an empty clamped row) has an extent.",
    ),
    _rp(
        "extent-height-unchecked",
        "if (bytes_per_pixel == 0u || pitch == 0u || row_pixels == 0u || height == 0u) {",
        "if (bytes_per_pixel == 0u || pitch == 0u || row_pixels == 0u) {",
        "a zero height has an extent (the last row wraps).",
    ),
    _rp(
        "extent-one-row-more",
        "return (size_t)(y + height - 1u) * pitch",
        "return (size_t)(y + height) * pitch",
        "the extent ends a pitch later than the last row.",
    ),
    _rp(
        "extent-y-lost",
        "return (size_t)(y + height - 1u) * pitch",
        "return (size_t)(height - 1u + 0u * y) * pitch",
        "the first row of the rectangle is not counted.",
    ),
    _rp(
        "extent-x-lost",
        "pitch + (size_t)x * bytes_per_pixel + (size_t)row_pixels * bytes_per_pixel;",
        "pitch + (size_t)row_pixels * bytes_per_pixel + 0u * x;",
        "the first column of the rectangle is not counted.",
    ),
    _rp(
        "extent-x-pixels",
        "pitch + (size_t)x * bytes_per_pixel + (size_t)row_pixels * bytes_per_pixel;",
        "pitch + (size_t)x + (size_t)row_pixels * bytes_per_pixel;",
        "the column is counted in pixels, not bytes.",
    ),
    _rp(
        "extent-width-pixels",
        "pitch + (size_t)x * bytes_per_pixel + (size_t)row_pixels * bytes_per_pixel;",
        "pitch + (size_t)x * bytes_per_pixel + (size_t)row_pixels;",
        "the row width is counted in pixels, not bytes.",
    ),
    _rp(
        "copy-empty-needs-both",
        "if (copy->height == 0u || copy->width == 0u) {\n        return GPU_PGRAPH_OK;",
        "if (copy->height == 0u && copy->width == 0u) {\n        return GPU_PGRAPH_OK;",
        "a blit with only one zero side is not empty: it goes on to be refused or copied.",
    ),
    _rp(
        "copy-group-unchecked",
        'if ((backend->output_groups & GPU_PGRAPH_OUTPUT_BLIT) == 0u) {\n        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,\n                    "the stream blitted bytes',
        'if ((backend->output_groups & GPU_PGRAPH_OUTPUT_BLIT) == 0u && false) {\n        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,\n                    "the stream blitted bytes',
        "a byte blit is applied with the BLIT group off.",
    ),
    _rp(
        "copy-used-unchecked",
        'if (copy == NULL || backend == NULL || used_inferences == NULL) {\n        return GPU_PGRAPH_ERR_ARGUMENT;\n    }\n    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_BLIT) == 0u) {\n        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,\n                    "the stream blitted bytes',
        'if (copy == NULL || backend == NULL) {\n        return GPU_PGRAPH_ERR_ARGUMENT;\n    }\n    if ((backend->output_groups & GPU_PGRAPH_OUTPUT_BLIT) == 0u) {\n        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,\n                    "the stream blitted bytes',
        "a NULL inference mask is written through.",
    ),
    _rp(
        "copy-format-unchecked",
        'if (bytes_per_pixel == 0u) {\n        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,\n                    "a byte blit of colour format',
        'if (false) {\n        return fail(report, GPU_PGRAPH_ERR_UNMEASURED,\n                    "a byte blit of colour format',
        "an unmeasured colour format is copied as zero byte pixels.",
    ),
    _rp(
        "copy-buffers-or-to-and",
        "if (source == NULL || destination == NULL) {",
        "if (source == NULL && destination == NULL) {",
        "a missing surface is dereferenced when the other one is there.",
    ),
    _rp(
        "copy-clamped-empty-unchecked",
        "    if (row_pixels == 0u) {\n        return GPU_PGRAPH_OK;\n    }\n    const size_t row_bytes",
        "    if (false) {\n        return GPU_PGRAPH_OK;\n    }\n    const size_t row_bytes",
        "a blit whose clamped row is empty (a pitch under a pixel) asks for the inference and walks its rows.",
    ),
    _rp(
        "copy-row-bytes-requested-width",
        "const size_t row_bytes = (size_t)row_pixels * bytes_per_pixel;",
        "const size_t row_bytes = (size_t)copy->width * bytes_per_pixel;",
        "a row moves the requested width, not the width clamped to the narrower pitch.",
    ),
    _rp(
        "copy-row-bytes-as-pixels",
        "const size_t row_bytes = (size_t)row_pixels * bytes_per_pixel;",
        "const size_t row_bytes = (size_t)row_pixels;",
        "a row moves a byte a pixel whatever the pixel size.",
    ),
    _rp(
        "copy-source-need-pitch",
        "const size_t source_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->source_pitch, copy->in_x, copy->in_y, row_pixels, copy->height);",
        "const size_t source_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->destination_pitch, copy->in_x, copy->in_y, row_pixels, copy->height);",
        "the source extent is sized with the destination pitch.",
    ),
    _rp(
        "copy-source-need-x",
        "const size_t source_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->source_pitch, copy->in_x, copy->in_y, row_pixels, copy->height);",
        "const size_t source_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->source_pitch, 0u, copy->in_y, row_pixels, copy->height);",
        "the source extent does not count the source column.",
    ),
    _rp(
        "copy-source-need-y",
        "const size_t source_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->source_pitch, copy->in_x, copy->in_y, row_pixels, copy->height);",
        "const size_t source_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->source_pitch, copy->in_x, 0u, row_pixels, copy->height);",
        "the source extent does not count the source row.",
    ),
    _rp(
        "copy-source-need-width",
        "const size_t source_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->source_pitch, copy->in_x, copy->in_y, row_pixels, copy->height);",
        "const size_t source_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->source_pitch, copy->in_x, copy->in_y, copy->width, copy->height);",
        "the source extent is the requested width, not the clamped row.",
    ),
    _rp(
        "copy-destination-need-pitch",
        "const size_t destination_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->destination_pitch, copy->out_x, copy->out_y, row_pixels, copy->height);",
        "const size_t destination_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->source_pitch, copy->out_x, copy->out_y, row_pixels, copy->height);",
        "the destination extent is sized with the source pitch.",
    ),
    _rp(
        "copy-destination-need-x",
        "const size_t destination_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->destination_pitch, copy->out_x, copy->out_y, row_pixels, copy->height);",
        "const size_t destination_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->destination_pitch, 0u, copy->out_y, row_pixels, copy->height);",
        "the destination extent does not count the destination column.",
    ),
    _rp(
        "copy-destination-need-y",
        "const size_t destination_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->destination_pitch, copy->out_x, copy->out_y, row_pixels, copy->height);",
        "const size_t destination_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->destination_pitch, copy->out_x, 0u, row_pixels, copy->height);",
        "the destination extent does not count the destination row.",
    ),
    _rp(
        "copy-destination-need-width",
        "const size_t destination_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->destination_pitch, copy->out_x, copy->out_y, row_pixels, copy->height);",
        "const size_t destination_need = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->destination_pitch, copy->out_x, copy->out_y, copy->width, copy->height);",
        "the destination extent is the requested width, not the clamped row.",
    ),
    _rp(
        "copy-operation-unchecked",
        "if (GPU_PGRAPH_BLIT_OPERATION_SRCCOPY != copy->operation) {",
        "if (false) {",
        "any blit operation is applied as SRCCOPY.",
    ),
    _rp(
        "copy-source-length-inclusive",
        "if (source_need > source_length || destination_need > destination_length) {",
        "if (source_need >= source_length || destination_need > destination_length) {",
        "a rectangle that ends exactly at the end of the source is refused.",
    ),
    _rp(
        "copy-destination-length-inclusive",
        "if (source_need > source_length || destination_need > destination_length) {",
        "if (source_need > source_length || destination_need >= destination_length) {",
        "a rectangle that ends exactly at the end of the destination is refused.",
    ),
    _rp(
        "copy-source-length-unchecked",
        "if (source_need > source_length || destination_need > destination_length) {",
        "if (destination_need > destination_length) {",
        "a rectangle past the source's held bytes is read.",
    ),
    _rp(
        "copy-destination-length-unchecked",
        "if (source_need > source_length || destination_need > destination_length) {",
        "if (source_need > source_length) {",
        "a rectangle past the destination's held bytes is written.",
    ),
    _rp(
        "copy-inference-skipped",
        "GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL, note, used_inferences, report);\n    if (allowed != GPU_PGRAPH_OK) {\n        return allowed;\n    }\n    uint8_t *row = malloc(row_bytes);",
        "GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL, note, used_inferences, report);\n    (void)allowed;\n    uint8_t *row = malloc(row_bytes);",
        "the byte copy is applied although the blit model inference is not allowed.",
    ),
    _rp(
        "copy-inference-bit",
        "need_inference(backend, GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL, note, used_inferences, report);",
        "need_inference(backend, GPU_PGRAPH_INFER_PROGRAM_HEADER, note, used_inferences, report);",
        "the byte copy asks for the wrong inference: a replay that did not allow the blit model applies it.",
    ),
    _rp(
        "copy-rows-descending",
        "    for (uint32_t line = 0u; line < copy->height; line++) {\n        uint8_t *to = destination",
        "    for (uint32_t line = copy->height; line-- > 0u;) {\n        uint8_t *to = destination",
        "the rows go in DESCENDING order, so a downward overlap is exact instead of smearing (xemu: ascending).",
    ),
    _rp(
        "copy-unbuffered",
        "memcpy(row, source + (size_t)(copy->in_y + line) * copy->source_pitch + (size_t)copy->in_x * bytes_per_pixel, row_bytes);\n        memcpy(to, row, row_bytes);",
        "for (size_t byte = 0u; byte < row_bytes; byte++) {\n            to[byte] = source[(size_t)(copy->in_y + line) * copy->source_pitch + (size_t)copy->in_x * bytes_per_pixel + byte];\n        }",
        "a row is not read whole before it is written (a forward byte copy): a rightward shift inside a row smears (xemu: exact).",
    ),
    _rp(
        "copy-spill-destination-lost",
        "memcpy(to, row, row_bytes);",
        "memcpy(to, row, (size_t)copy->out_x * bytes_per_pixel < copy->destination_pitch && (size_t)copy->out_x * bytes_per_pixel + row_bytes > copy->destination_pitch ? copy->destination_pitch - (size_t)copy->out_x * bytes_per_pixel : row_bytes);",
        "a destination row that runs past its pitch stops at the end of the row (xemu: it runs on into the next row).",
    ),
    _rp(
        "copy-spill-source-lost",
        "memcpy(row, source + (size_t)(copy->in_y + line) * copy->source_pitch + (size_t)copy->in_x * bytes_per_pixel, row_bytes);",
        "memset(row, 0, row_bytes);\n        memcpy(row, source + (size_t)(copy->in_y + line) * copy->source_pitch + (size_t)copy->in_x * bytes_per_pixel, (size_t)copy->in_x * bytes_per_pixel < copy->source_pitch && (size_t)copy->in_x * bytes_per_pixel + row_bytes > copy->source_pitch ? copy->source_pitch - (size_t)copy->in_x * bytes_per_pixel : row_bytes);",
        "a source row that runs past its pitch stops at the end of the row (xemu: it runs on into the next row), the rest reads as zero.",
    ),
    _rp(
        "copy-last-row-skipped",
        "    for (uint32_t line = 0u; line < copy->height; line++) {\n        uint8_t *to = destination",
        "    for (uint32_t line = 0u; line + 1u < copy->height; line++) {\n        uint8_t *to = destination",
        "the last row of the rectangle is not moved.",
    ),
    _rp(
        "copy-destination-row-lost",
        "uint8_t *to = destination + (size_t)(copy->out_y + line) * copy->destination_pitch + (size_t)copy->out_x * bytes_per_pixel;",
        "uint8_t *to = destination + (size_t)(copy->out_y) * copy->destination_pitch + (size_t)copy->out_x * bytes_per_pixel;",
        "every row lands on the first row of the destination.",
    ),
    _rp(
        "copy-source-row-lost",
        "memcpy(row, source + (size_t)(copy->in_y + line) * copy->source_pitch + (size_t)copy->in_x * bytes_per_pixel, row_bytes);",
        "memcpy(row, source + (size_t)(copy->in_y) * copy->source_pitch + (size_t)copy->in_x * bytes_per_pixel, row_bytes);",
        "every row is read from the first row of the source.",
    ),
    _rp(
        "copy-destination-pitch-source",
        "uint8_t *to = destination + (size_t)(copy->out_y + line) * copy->destination_pitch + (size_t)copy->out_x * bytes_per_pixel;",
        "uint8_t *to = destination + (size_t)(copy->out_y + line) * copy->source_pitch + (size_t)copy->out_x * bytes_per_pixel;",
        "the destination rows are a source pitch apart.",
    ),
    _rp(
        "copy-source-pitch-destination",
        "memcpy(row, source + (size_t)(copy->in_y + line) * copy->source_pitch + (size_t)copy->in_x * bytes_per_pixel, row_bytes);",
        "memcpy(row, source + (size_t)(copy->in_y + line) * copy->destination_pitch + (size_t)copy->in_x * bytes_per_pixel, row_bytes);",
        "the source rows are a destination pitch apart.",
    ),
    _rp(
        "copy-destination-x-pixels",
        "uint8_t *to = destination + (size_t)(copy->out_y + line) * copy->destination_pitch + (size_t)copy->out_x * bytes_per_pixel;",
        "uint8_t *to = destination + (size_t)(copy->out_y + line) * copy->destination_pitch + (size_t)copy->out_x;",
        "the destination column is in pixels, not bytes.",
    ),
    _rp(
        "copy-source-x-pixels",
        "+ (size_t)copy->in_x * bytes_per_pixel, row_bytes);",
        "+ (size_t)copy->in_x, row_bytes);",
        "the source column is in pixels, not bytes.",
    ),
    _rp(
        "copy-alpha-dropped",
        "if (copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF || copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0) {",
        "if (false) {",
        "formats 7 and 6 copy the source alpha like 0xA on the byte path.",
    ),
    _rp(
        "copy-alpha-for-a8r8g8b8",
        "if (copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF || copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0) {",
        "if (copy->color_format == GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8 || copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0) {",
        "format 0xA is forced to alpha 0 and format 7 copies its alpha on the byte path.",
    ),
    _rp(
        "copy-alpha-seven-lost",
        "if (copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF || copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0) {",
        "if (copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0) {",
        "format 7 copies the source alpha on the byte path.",
    ),
    _rp(
        "copy-alpha-six-lost",
        "if (copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF || copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0) {",
        "if (copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF) {",
        "format 6 copies the source alpha on the byte path.",
    ),
    _rp(
        "copy-alpha-swapped",
        "to[(size_t)pixel * 4u + 3u] = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ? 0xFFu : 0x00u;",
        "to[(size_t)pixel * 4u + 3u] = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ? 0x00u : 0xFFu;",
        "format 7 forces 0 and format 6 forces 0xFF on the byte path.",
    ),
    _rp(
        "copy-alpha-value-one",
        "to[(size_t)pixel * 4u + 3u] = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ? 0xFFu : 0x00u;",
        "to[(size_t)pixel * 4u + 3u] = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ? 0x7Fu : 0x00u;",
        "format 7 forces 0x7F instead of 0xFF on the byte path.",
    ),
    _rp(
        "copy-alpha-value-zero",
        "to[(size_t)pixel * 4u + 3u] = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ? 0xFFu : 0x00u;",
        "to[(size_t)pixel * 4u + 3u] = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ? 0xFFu : 0x01u;",
        "format 6 forces 1 instead of 0 on the byte path.",
    ),
    _rp(
        "copy-alpha-format-test",
        "to[(size_t)pixel * 4u + 3u] = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ? 0xFFu : 0x00u;",
        "to[(size_t)pixel * 4u + 3u] = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0 ? 0xFFu : 0x00u;",
        "the formats 7 and 6 force each other's alpha on the byte path.",
    ),
    _rp(
        "copy-alpha-byte",
        "to[(size_t)pixel * 4u + 3u] = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ? 0xFFu : 0x00u;",
        "to[(size_t)pixel * 4u + 2u] = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ? 0xFFu : 0x00u;",
        "the forced value lands in the red byte, not the alpha byte.",
    ),
    _rp(
        "copy-alpha-stride",
        "to[(size_t)pixel * 4u + 3u] = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ? 0xFFu : 0x00u;",
        "to[(size_t)pixel * 2u + 3u] = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF ? 0xFFu : 0x00u;",
        "the forced alphas are two bytes apart.",
    ),
    _rp(
        "copy-alpha-last-pixel",
        "for (uint32_t pixel = 0u; pixel < row_pixels; pixel++) {",
        "for (uint32_t pixel = 0u; pixel + 1u < row_pixels; pixel++) {",
        "the last pixel of each row keeps the source alpha.",
    ),
    _rp(
        "copy-alpha-first-pixel",
        "for (uint32_t pixel = 0u; pixel < row_pixels; pixel++) {",
        "for (uint32_t pixel = 1u; pixel < row_pixels; pixel++) {",
        "the first pixel of each row keeps the source alpha.",
    ),
    _rp(
        "copy-alpha-requested-width",
        "for (uint32_t pixel = 0u; pixel < row_pixels; pixel++) {",
        "for (uint32_t pixel = 0u; pixel < copy->width; pixel++) {",
        "the forced alpha runs over the requested width: past a clamped row it overwrites the destination bytes after it.",
    ),
    _rp(
        "copy-row-bytes-pixels",
        "+ (size_t)copy->in_x * bytes_per_pixel, row_bytes);",
        "+ (size_t)copy->in_x * bytes_per_pixel, row_pixels);",
        "a row is read at one byte a pixel whatever the pixel size.",
    ),
    _rp(
        "initial-flip-refusal-dropped",
        "if (backend->initial_pixels != NULL && backend->flip_y) {",
        "if (backend->initial_pixels != NULL && backend->flip_y && false) {",
        "a kept initial image is mirrored a second time with flip_y.",
    ),
    _rp(
        "initial-refuses-any-flip",
        "if (backend->initial_pixels != NULL && backend->flip_y) {",
        "if (backend->initial_pixels != NULL || backend->flip_y) {",
        "every flip_y pass is refused.",
    ),
    _rp(
        "initial-not-copied",
        "if (backend->initial_pixels != NULL) {\n        memcpy(frame.pixels, backend->initial_pixels,",
        "if (false) {\n        memcpy(frame.pixels, backend->initial_pixels,",
        "a pass starts from the clear colour whatever the kept image says.",
    ),
    _rp(
        "initial-last-row-not-copied",
        "memcpy(frame.pixels, backend->initial_pixels, (size_t)frame.stride_bytes * height);",
        "memcpy(frame.pixels, backend->initial_pixels, (size_t)frame.stride_bytes * (height - 1u));",
        "the last row of the kept image is not copied: that row is uninitialised memory.",
    ),
    # ================================================================== enable
    _sw(
        "enable-surface-without-rt-texture",
        '        if (!config->render_target_texture) {\n            reason = "the surface source feeds',
        '        if (false) {\n            reason = "the surface source feeds',
        "the surface source is accepted without the render target texture bridge.",
    ),
    _sw(
        "enable-surface-without-blit-group",
        '} else if ((config->output_groups & GPU_PGRAPH_OUTPUT_BLIT) == 0u) {\n            reason = "the surface source needs the BLIT',
        '} else if (false) {\n            reason = "the surface source needs the BLIT',
        "the surface source is accepted without the BLIT output group.",
    ),
    _sw(
        "enable-surface-without-inference",
        "} else if ((config->allowed_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) != D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) {",
        "} else if (false) {",
        "the surface source is accepted without its announced inference.",
    ),
    _sw(
        "enable-surface-wrong-inference-bit",
        "} else if ((config->allowed_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) != D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) {",
        "} else if ((config->allowed_inferences & D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST) != D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST) {",
        "the surface source checks the persistence inference's bit.",
    ),
    _sw(
        "enable-persist-with-flip",
        '        if (config->flip_y) {\n            reason = "target persistence cannot be combined',
        '        if (false) {\n            reason = "target persistence cannot be combined',
        "target persistence is accepted with flip_y: a kept image is mirrored a second time.",
    ),
    _sw(
        "enable-persist-without-inference",
        '} else if ((config->allowed_inferences & D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST) != D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST) {\n            reason = "target persistence needs',
        '} else if (false) {\n            reason = "target persistence needs',
        "target persistence is accepted without its announced inference.",
    ),
    _sw(
        "enable-persist-wrong-inference-bit",
        '} else if ((config->allowed_inferences & D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST) != D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST) {\n            reason = "target persistence needs',
        '} else if ((config->allowed_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) != D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) {\n            reason = "target persistence needs',
        "target persistence checks the surface source inference's bit.",
    ),
    # ================================================================== surface (blit_written_before, provide_surface)
    _sw(
        "blit-before-draw-strict",
        "copy->before_draw <= draw && ranges_overlap(data, bytes, copy->destination_offset, extent)",
        "copy->before_draw < draw && ranges_overlap(data, bytes, copy->destination_offset, extent)",
        "a blit recorded right before the draw (before_draw == draw) is not before it.",
    ),
    _sw(
        "blit-before-draw-after",
        "copy->before_draw <= draw && ranges_overlap(data, bytes, copy->destination_offset, extent)",
        "copy->before_draw >= draw && ranges_overlap(data, bytes, copy->destination_offset, extent)",
        "a blit after the draw is the one that counts.",
    ),
    _sw(
        "blit-before-empty-counts",
        "if (extent != 0u && copy->before_draw <= draw",
        "if (copy->before_draw <= draw",
        "an empty blit (a zero extent) inside the texture's range writes it.",
    ),
    _sw(
        "blit-before-oldest",
        "            found = copy;\n        }\n    }\n    return found;",
        "            if (found == NULL) {\n                found = copy;\n            }\n        }\n    }\n    return found;",
        "the refusal names the oldest blit that writes the surface, not the newest.",
    ),
    _sw(
        "blit-before-source-not-destination",
        "ranges_overlap(data, bytes, copy->destination_offset, extent)",
        "ranges_overlap(data, bytes, copy->source_offset, extent)",
        "a blit that reads the surface counts as one that writes it.",
    ),
    _sw(
        "blit-before-source-pitch",
        "const size_t extent = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->destination_pitch, copy->out_x, copy->out_y,",
        "const size_t extent = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->source_pitch, copy->out_x, copy->out_y,",
        "the extent a blit writes is sized with the source pitch.",
    ),
    _sw(
        "blit-before-source-corner",
        "const size_t extent = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->destination_pitch, copy->out_x, copy->out_y,",
        "const size_t extent = gpu_pgraph_blit_extent_bytes(copy->color_format, copy->destination_pitch, copy->in_x, copy->in_y,",
        "the extent a blit writes starts at the source's corner.",
    ),
    _sw(
        "blit-before-sizes-swapped",
        "ranges_overlap(data, bytes, copy->destination_offset, extent)",
        "ranges_overlap(data, extent, copy->destination_offset, bytes)",
        "the two ranges swap their sizes.",
    ),
    _sw(
        "surface-allowed-inverted",
        'if ((state.config.allowed_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) == 0u) {\n        stage_refusal(stage, out, "surface source not allowed"',
        'if ((state.config.allowed_inferences & D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE) != 0u) {\n        stage_refusal(stage, out, "surface source not allowed"',
        "an allowed surface source is refused as not allowed.",
    ),
    _sw(
        "surface-bytes-width",
        "const uint64_t bytes = (uint64_t)binding->pitch * binding->height;\n    if ((state.config.allowed_inferences",
        "const uint64_t bytes = (uint64_t)binding->pitch * binding->width;\n    if ((state.config.allowed_inferences",
        "the texture's bytes are a pitch times its width.",
    ),
    _sw(
        "surface-blit-refusal-dropped",
        'if (blit != NULL) {\n        stage_refusal(stage, out, "blit before the draw",',
        'if (blit != NULL && false) {\n        stage_refusal(stage, out, "blit before the draw",',
        "a draw samples a surface a blit of the frame wrote, without the blit.",
    ),
    _sw(
        "surface-alias-self",
        "if (other->valid && other->data != binding->data &&\n            ranges_overlap(binding->data, bytes,",
        "if (other->valid &&\n            ranges_overlap(binding->data, bytes,",
        "the kept image of the very Data word is an alias of itself: the exact kept image is never sampled.",
    ),
    _sw(
        "surface-alias-range-pixels",
        "ranges_overlap(binding->data, bytes, other->data, (uint64_t)other->image.width * 4u * other->image.height)) {",
        "ranges_overlap(binding->data, bytes, other->data, (uint64_t)other->image.width * other->image.height)) {",
        "a kept image covers a quarter of its bytes: an alias with its far end is missed.",
    ),
    _sw(
        "surface-alias-range-width-twice",
        "ranges_overlap(binding->data, bytes, other->data, (uint64_t)other->image.width * 4u * other->image.height)) {",
        "ranges_overlap(binding->data, bytes, other->data, (uint64_t)other->image.width * 4u * other->image.width)) {",
        "a kept image covers width * 4 * width bytes: a texture right above a short image is an alias.",
    ),
    _sw(
        "surface-alias-unchecked",
        "if (other->valid && other->data != binding->data &&\n            ranges_overlap(binding->data, bytes,",
        "if (other->valid && other->data != binding->data && false &&\n            ranges_overlap(binding->data, bytes,",
        "a texture that overlaps a kept image at another offset is read from the guest's bytes.",
    ),
    _sw(
        "surface-byte-surface-unchecked",
        'if (d3d8_surface_model_overlaps_bytes(binding->data, bytes)) {\n        stage_refusal(stage, out, "byte surface",',
        'if (false) {\n        stage_refusal(stage, out, "byte surface",',
        "a texture over a byte surface a CopyRects blit wrote is read from the guest's bytes alone.",
    ),
    _sw(
        "surface-byte-surface-exact-only",
        'if (d3d8_surface_model_overlaps_bytes(binding->data, bytes)) {\n        stage_refusal(stage, out, "byte surface",',
        'if (d3d8_surface_model_has_bytes(binding->data)) {\n        stage_refusal(stage, out, "byte surface",',
        "only a byte surface of the very Data word is refused: a partial overlap is read from the guest.",
    ),
    _sw(
        "surface-unreadable-ignored",
        'if (probe == D3D8_SURFACE_UNREADABLE) {\n        stage_refusal(stage, out, "unreadable bytes",',
        'if (false) {\n        stage_refusal(stage, out, "unreadable bytes",',
        "an unreadable range is not refused at the probe (a census resolves it).",
    ),
    _sw(
        "surface-kept-width-unchecked",
        "if (kept->image.width != binding->width || kept->image.height != binding->height ||",
        "if (kept->image.height != binding->height ||",
        "a kept image of another width is sampled.",
    ),
    _sw(
        "surface-kept-height-unchecked",
        "if (kept->image.width != binding->width || kept->image.height != binding->height ||",
        "if (kept->image.width != binding->width ||",
        "a kept image of another height is sampled.",
    ),
    _sw(
        "surface-kept-pitch-unchecked",
        "(kept->pitch != 0u && kept->pitch != binding->pitch) ||",
        "false ||",
        "a kept image of another pitch is sampled.",
    ),
    _sw(
        "surface-kept-format-unchecked",
        "(kept->format != 0u && kept->format != binding->format)) {",
        "false) {",
        "a kept image of another Format is sampled.",
    ),
    _sw(
        "surface-kept-ignored",
        "stored_surface *kept = find_stored(binding->data);\n    if (kept != NULL) {",
        "stored_surface *kept = find_stored(binding->data);\n    if (kept != NULL && false) {",
        "the guest memory is preferred to the kept image of an earlier frame.",
    ),
    _sw(
        "surface-cpu-written-unchecked",
        'if (probe != D3D8_SURFACE_ZERO) {\n            stage_refusal(stage, out, "cpu written surface",',
        'if (false) {\n            stage_refusal(stage, out, "cpu written surface",',
        "a kept image over guest memory the CPU wrote is sampled: the order of the write is not observed.",
    ),
    _sw(
        "surface-kept-dry-inverted",
        "out->rgba = provider.dry ? placeholder : kept->image.pixels;",
        "out->rgba = !provider.dry ? placeholder : kept->image.pixels;",
        "a replayed draw samples the one texel placeholder of a census.",
    ),
    _sw(
        "surface-kept-kind-guest",
        "provider.kind[stage] = SURFACE_KIND_KEPT;",
        "provider.kind[stage] = SURFACE_KIND_GUEST;",
        "a kept image is counted as a guest memory sample.",
    ),
    _sw(
        "surface-guest-kind-swapped",
        "provider.kind[stage] = probe == D3D8_SURFACE_ZERO ? SURFACE_KIND_GUEST_ZERO : SURFACE_KIND_GUEST;",
        "provider.kind[stage] = probe == D3D8_SURFACE_ZERO ? SURFACE_KIND_GUEST : SURFACE_KIND_GUEST_ZERO;",
        "the all zero samples are the written ones.",
    ),
    _sw(
        "surface-kept-text-size-swapped",
        "(unsigned)binding->data, (unsigned)binding->width, (unsigned)binding->height, (unsigned)binding->format);\n    } else {",
        "(unsigned)binding->data, (unsigned)binding->height, (unsigned)binding->width, (unsigned)binding->format);\n    } else {",
        "the kept image's source text swaps its width and height.",
    ),
    _sw(
        "surface-guest-text-size-swapped",
        "(unsigned)binding->data, (unsigned)binding->width, (unsigned)binding->height, (unsigned)binding->format);\n    }\n    out->linear = true;",
        "(unsigned)binding->data, (unsigned)binding->height, (unsigned)binding->width, (unsigned)binding->format);\n    }\n    out->linear = true;",
        "the guest surface's source text swaps its width and height.",
    ),
    _sw(
        "surface-kept-text-measured",
        "of an earlier frame, MEASURED in xemu (T736) the surface still holds it",
        "of an earlier frame, the surface still holds it",
        "the kept image's source text drops the MEASURED label.",
    ),
    _sw(
        "surface-guest-zero-text",
        "no replayed pass drew: all zero, transparent black (MEASURED bytes, INFERRED unchanged at the draw)",
        "no replayed pass drew: all zero, black (MEASURED bytes, INFERRED unchanged at the draw)",
        "the all zero source text says black, not transparent black.",
    ),
    _sw(
        "surface-guest-zero-text-measured",
        "all zero, transparent black (MEASURED bytes, INFERRED unchanged at the draw)",
        "all zero, transparent black (INFERRED unchanged at the draw)",
        "the all zero source text drops the MEASURED label of the bytes.",
    ),
    _sw(
        "surface-guest-text-read",
        "read at the present (INFERRED the bytes at the draw)",
        "read at the draw (INFERRED the bytes at the draw)",
        "the written guest memory text says it was read at the draw, it is read at the present.",
    ),
    _sw(
        "surface-guest-text-drew",
        "guest memory surface (Data 0x%07X, %ux%u, Format 0x%08X) no replayed pass drew: read at the present",
        "guest memory surface (Data 0x%07X, %ux%u, Format 0x%08X) no pass drew: read at the present",
        "the written guest memory text drops the replayed pass wording.",
    ),
    _sw(
        "surface-not-linear",
        "out->linear = true;\n    out->unnormalised = true;\n}\n\n/* The refusal list",
        "out->linear = false;\n    out->unnormalised = true;\n}\n\n/* The refusal list",
        "a surface texture is sampled nearest, not bilinear.",
    ),
    _sw(
        "surface-normalised",
        "out->linear = true;\n    out->unnormalised = true;\n}\n\n/* The refusal list",
        "out->linear = true;\n    out->unnormalised = false;\n}\n\n/* The refusal list",
        "a surface texture takes its coordinate normalised, the replay gives it in texels.",
    ),
    # ================================================================== provider
    _sw(
        "provider-no-pass-takes-no-surface",
        "} else if (state.config.surface_source) {\n            provide_surface(stage, binding, draw, out); /* T633 (1): the kept image",
        "} else if (false) {\n            provide_surface(stage, binding, draw, out); /* T633 (1): the kept image",
        "a surface no pass of the frame drew is refused as before even with the surface source.",
    ),
    _sw(
        "provider-later-pass-takes-no-surface",
        "if (match > provider.current && state.config.surface_source) {",
        "if (match > provider.current && false) {",
        "a surface a later pass draws is refused as out of order even with the surface source.",
    ),
    _sw(
        "provider-earlier-pass-takes-surface",
        "if (match > provider.current && state.config.surface_source) {",
        "if (match < provider.current && state.config.surface_source) {",
        "a surface an earlier pass drew is read from the kept image, not from the pass.",
    ),
    _sw(
        "provider-later-pass-without-option",
        "if (match > provider.current && state.config.surface_source) {",
        "if (match > provider.current) {",
        "a surface a later pass draws is taken from memory without the surface source.",
    ),
    _sw(
        "provider-pass-blit-without-option",
        "if (state.config.surface_source && blit_written_before(binding->data, texture_bytes, draw) != NULL) {",
        "if (blit_written_before(binding->data, texture_bytes, draw) != NULL) {",
        "a pass image sampled after a blit of the frame is refused without the surface source too.",
    ),
    _sw(
        "provider-pass-blit-first-byte",
        "blit_written_before(binding->data, texture_bytes, draw) != NULL) {",
        "blit_written_before(binding->data, 1u, draw) != NULL) {",
        "only a blit that writes the first byte of a pass image counts.",
    ),
    _sw(
        "provider-pass-blit-ignored",
        "if (state.config.surface_source && blit_written_before(binding->data, texture_bytes, draw) != NULL) {",
        "if (state.config.surface_source && blit_written_before(binding->data, texture_bytes, draw) != NULL && false) {",
        "a pass image sampled after a blit of the frame is sampled without the blit.",
    ),
    # ================================================================== sampled counters
    _sw(
        "sampled-surface-flag-lost",
        "if (provider.kind[stage] != SURFACE_KIND_PASS) {\n            provider.surface_sampled = true;",
        "if (false) {\n            provider.surface_sampled = true;",
        "the SURFACE_SOURCE inference is never reported by a draw that sampled a surface.",
    ),
    _sw(
        "sampled-kept-not-counted",
        "state.stats.surface_kept_samples++;",
        "",
        "kept samples are not counted.",
    ),
    _sw(
        "sampled-guest-not-counted",
        "state.stats.surface_guest_samples++;",
        "",
        "guest memory samples are not counted.",
    ),
    _sw(
        "sampled-zero-always",
        "state.stats.surface_guest_zero_samples += provider.kind[stage] == SURFACE_KIND_GUEST_ZERO ? 1u : 0u;",
        "state.stats.surface_guest_zero_samples += 1u;",
        "every guest sample is counted as all zero.",
    ),
    _sw(
        "sampled-pass-counted-guest",
        "} else if (provider.kind[stage] != SURFACE_KIND_PASS) {\n            state.stats.surface_guest_samples++;",
        "} else {\n            state.stats.surface_guest_samples++;",
        "a sample of a pass image is counted as a guest memory sample.",
    ),
    _sw(
        "sampled-kept-as-guest",
        "if (provider.kind[stage] == SURFACE_KIND_KEPT) {\n            state.stats.surface_kept_samples++;",
        "if (provider.kind[stage] != SURFACE_KIND_KEPT) {\n            state.stats.surface_kept_samples++;",
        "the kept and guest counters swap.",
    ),
    # ================================================================== store
    _sw(
        "claim-eviction-not-advanced",
        "slot = &state.surfaces[state.surfaces_next++ % D3D8_SWAP_REPLAY_SURFACES];",
        "slot = &state.surfaces[state.surfaces_next % D3D8_SWAP_REPLAY_SURFACES];",
        "the eviction always takes the same slot, not the oldest.",
    ),
    _sw(
        "claim-protect-ignored",
        "if (slot == protect) {",
        "if (slot == protect && false) {",
        "a claim may evict the surface the same blit reads.",
    ),
    _sw(
        "claim-protect-retakes",
        "slot = &state.surfaces[(state.surfaces_next++) % D3D8_SWAP_REPLAY_SURFACES];",
        "slot = &state.surfaces[(state.surfaces_next) % D3D8_SWAP_REPLAY_SURFACES];",
        "the protected slot is skipped to the next one, which is taken again by the next claim.",
    ),
    _sw(
        "claim-eviction-not-counted",
        "state.stats.surfaces_evicted++;\n    }\n    gpu_image_free(&slot->image);",
        "}\n    gpu_image_free(&slot->image);",
        "an eviction is not counted.",
    ),
    _sw(
        "claim-slot-not-cleared",
        "gpu_image_free(&slot->image);\n    memset(slot, 0, sizeof *slot);\n    return slot;",
        "gpu_image_free(&slot->image);\n    return slot;",
        "a claimed slot keeps the fields of the surface it held.",
    ),
    _sw(
        "claim-last-free-slot-unused",
        "for (size_t i = 0u; slot == NULL && i < D3D8_SWAP_REPLAY_SURFACES; i++) {\n        if (!state.surfaces[i].valid) {",
        "for (size_t i = 0u; slot == NULL && i + 1u < D3D8_SWAP_REPLAY_SURFACES; i++) {\n        if (!state.surfaces[i].valid) {",
        "the last slot is never free: a surface is evicted with a slot to spare.",
    ),
    _sw(
        "store-pitch-lost",
        "slot->pitch = pass->size.pitch;\n    slot->format = pass->size.format;",
        "slot->format = pass->size.format;",
        "a kept image has no pitch: a pitch that differs is not refused.",
    ),
    _sw(
        "store-format-lost",
        "slot->pitch = pass->size.pitch;\n    slot->format = pass->size.format;",
        "slot->pitch = pass->size.pitch;",
        "a kept image has no Format: a Format that differs is not refused.",
    ),
    _sw(
        "store-unnamed-kept",
        "if (!surfaces_are_kept() || pass->data == 0u || pass->image.pixels == NULL) {",
        "if (!surfaces_are_kept() || pass->image.pixels == NULL) {",
        "a target with no Data word is kept under Data 0.",
    ),
    _sw(
        "store-always-kept",
        "if (!surfaces_are_kept() || pass->data == 0u || pass->image.pixels == NULL) {",
        "if ((!surfaces_are_kept() && false) || pass->data == 0u || pass->image.pixels == NULL) {",
        "images are kept when nothing reads them.",
    ),
    _sw(
        "store-copy-short",
        "const size_t bytes = (size_t)pass->image.stride_bytes * pass->image.height;\n    uint8_t *pixels = malloc(bytes);",
        "const size_t bytes = (size_t)pass->image.stride_bytes * (pass->image.height - 1u);\n    uint8_t *pixels = malloc(bytes);",
        "the kept copy of a pass image lacks its last row.",
    ),
    _sw(
        "store-valid-lost",
        "slot->valid = true;\n    slot->touched = false;",
        "slot->touched = false;",
        "a stored image is never marked held.",
    ),
    _sw(
        "kept-persist-term-dropped",
        "state.config.surface_source || state.config.target_persist;",
        "state.config.surface_source;",
        "images are not kept for a persistent target alone.",
    ),
    _sw(
        "kept-blit-term-dropped",
        "return (state.config.output_groups & GPU_PGRAPH_OUTPUT_BLIT) != 0u || state.config.surface_source",
        "return state.config.surface_source",
        "images are not kept for blits alone.",
    ),
    # ================================================================== persist
    _sw(
        "persist-always-on",
        "if (!state.config.target_persist || data == 0u) {\n        return true;",
        "if (data == 0u) {\n        return true;",
        "every pass starts from the kept image, with the option off.",
    ),
    _sw(
        "persist-no-held-unchecked",
        "if (held == NULL) {\n        return true; /* nothing drew it before",
        "if (held == NULL && false) {\n        return true; /* nothing drew it before",
        "a surface nothing drew before dereferences NULL.",
    ),
    _sw(
        "persist-height-unchecked",
        "if (held->image.width != width || held->image.height != height || held->image.pixels == NULL ||",
        "if (held->image.width != width || held->image.pixels == NULL ||",
        "a kept image of another height is the start of the pass.",
    ),
    _sw(
        "persist-pitch-unchecked",
        "(held->pitch != 0u && size.pitch != 0u && held->pitch != size.pitch) ||",
        "false ||",
        "a kept image of another pitch is the start of the pass.",
    ),
    _sw(
        "persist-pitch-zero-allowed",
        "(held->pitch != 0u && size.pitch != 0u && held->pitch != size.pitch) ||",
        "(held->pitch != 0u && held->pitch != size.pitch) ||",
        "a target with no measured pitch (an explicit size over a header with no size word) is refused as another pitch.",
    ),
    _sw(
        "persist-format-unchecked",
        "(held->format != 0u && size.format != 0u && held->format != size.format)) {",
        "false) {",
        "a kept image of another Format is the start of the pass.",
    ),
    _sw(
        "persist-format-zero-allowed",
        "(held->format != 0u && size.format != 0u && held->format != size.format)) {",
        "(held->format != 0u && held->format != size.format)) {",
        "a target with no measured Format is refused as another Format.",
    ),
    _sw(
        "persist-probe-tight",
        "(uint64_t)(size.pitch != 0u ? size.pitch : width * 4u) * height);",
        "(uint64_t)(width * 4u) * height);",
        "the CPU written test ignores the pitch's padding: a write in the padding of the last row is not seen.",
    ),
    _sw(
        "persist-probe-width",
        "(uint64_t)(size.pitch != 0u ? size.pitch : width * 4u) * height);",
        "(uint64_t)(size.pitch != 0u ? size.pitch : width * 4u) * width);",
        "the CPU written test covers a pitch times the width, not the height.",
    ),
    _sw(
        "persist-unreadable-passes",
        'if (probe != D3D8_SURFACE_ZERO) {\n        snprintf(what, what_size,\n                 "target persistence: the guest memory under',
        'if (probe == D3D8_SURFACE_NONZERO) {\n        snprintf(what, what_size,\n                 "target persistence: the guest memory under',
        "an unreadable guest range under the kept image is taken as zero.",
    ),
    _sw(
        "persist-cpu-written-passes",
        'if (probe != D3D8_SURFACE_ZERO) {\n        snprintf(what, what_size,\n                 "target persistence: the guest memory under',
        'if (probe == D3D8_SURFACE_UNREADABLE) {\n        snprintf(what, what_size,\n                 "target persistence: the guest memory under',
        "a CPU written surface under the kept image is persisted.",
    ),
    _sw(
        "persist-text-swapped",
        'probe == D3D8_SURFACE_NONZERO ? "not zero" : "unreadable");',
        'probe == D3D8_SURFACE_NONZERO ? "unreadable" : "not zero");',
        "the refusal names a CPU write as unreadable and an unreadable range as a CPU write.",
    ),
    _sw(
        "persist-initial-not-returned",
        "    *initial = held->image.pixels;\n    return true;",
        "    return true;",
        "a persisted pass starts from a fresh image.",
    ),
    _sw(
        "wiring-initial-dropped",
        "backend.initial_pixels = initial;",
        "",
        "the replay is not handed the kept image.",
    ),
    _sw(
        "wiring-refusal-ignored",
        "if (!persist_initial(target_data, width, height, size, &initial, what, what_size)) {\n        return false;",
        "if (!persist_initial(target_data, width, height, size, &initial, what, what_size) && false) {\n        return false;",
        "a persistence refusal is not a refusal of the frame.",
    ),
    _sw(
        "wiring-persisted-not-counted",
        "if (initial != NULL) {\n        state.stats.targets_persisted++;",
        "if (false) {\n        state.stats.targets_persisted++;",
        "a persisted pass is not counted.",
    ),
    _sw(
        "wiring-persist-bit-lost",
        "(initial != NULL ? GPU_PGRAPH_INFER_OUTPUT_TARGET_PERSIST : 0u);",
        "0u;",
        "a persisted pass does not report the TARGET_PERSIST inference.",
    ),
    _sw(
        "wiring-surface-bit-lost",
        "pass->used_inferences |= (provider.surface_sampled ? GPU_PGRAPH_INFER_OUTPUT_SURFACE_SOURCE : 0u) |",
        "pass->used_inferences |= (provider.surface_sampled ? 0u : 0u) |",
        "a pass that sampled a surface does not report the SURFACE_SOURCE inference.",
    ),
    # ================================================================== blit
    _sw(
        "resolve-no-image-refused",
        "        return BLIT_SURFACE_NO_IMAGE;\n    }\n    *image = &held->image;",
        "        return BLIT_SURFACE_REFUSED;\n    }\n    *image = &held->image;",
        "a surface the replay holds no image of is always refused, the guest memory is never read.",
    ),
    _sw(
        "resolve-stored-not-returned",
        "*image = &held->image;\n    *stored = held;",
        "*image = &held->image;",
        "the kept surface of a blit is not handed back: no growth, no dump mark, no inference bit.",
    ),
    _sw(
        "guest-pitch-unaligned-allowed",
        "if (pitch == 0u || pitch % 4u != 0u || pitch > 4u * 8192u || rows == 0u || rows > 8192u) {",
        "if (pitch == 0u || pitch > 4u * 8192u || rows == 0u || rows > 8192u) {",
        "a pitch that is not a whole number of pixels is read as a surface.",
    ),
    _sw(
        "guest-pitch-bound-inclusive",
        "if (pitch == 0u || pitch % 4u != 0u || pitch > 4u * 8192u || rows == 0u || rows > 8192u) {",
        "if (pitch == 0u || pitch % 4u != 0u || pitch >= 4u * 8192u || rows == 0u || rows > 8192u) {",
        "a pitch of exactly 8192 pixels is refused.",
    ),
    _sw(
        "guest-pitch-unbounded",
        "if (pitch == 0u || pitch % 4u != 0u || pitch > 4u * 8192u || rows == 0u || rows > 8192u) {",
        "if (pitch == 0u || pitch % 4u != 0u || rows == 0u || rows > 8192u) {",
        "a pitch of more than 8192 pixels is read as a surface.",
    ),
    _sw(
        "guest-rows-bound-inclusive",
        "if (pitch == 0u || pitch % 4u != 0u || pitch > 4u * 8192u || rows == 0u || rows > 8192u) {",
        "if (pitch == 0u || pitch % 4u != 0u || pitch > 4u * 8192u || rows == 0u || rows >= 8192u) {",
        "a surface of exactly 8192 rows is refused.",
    ),
    _sw(
        "guest-rows-unbounded",
        "if (pitch == 0u || pitch % 4u != 0u || pitch > 4u * 8192u || rows == 0u || rows > 8192u) {",
        "if (pitch == 0u || pitch % 4u != 0u || pitch > 4u * 8192u || rows == 0u) {",
        "a surface of more than 8192 rows is read.",
    ),
    _sw(
        "guest-byte-surface-overlap-unchecked",
        "if (d3d8_surface_model_has_bytes(data) || d3d8_surface_model_overlaps_bytes(data, bytes)) {",
        "if (d3d8_surface_model_has_bytes(data)) {",
        "an image blit over a range that partly overlaps a byte surface is built from the guest.",
    ),
    _sw(
        "guest-alias-range-pixels",
        "if (other->valid && other->data != data && ranges_overlap(data, bytes, other->data, (uint64_t)other->image.width * 4u * other->image.height)) {",
        "if (other->valid && other->data != data && ranges_overlap(data, bytes, other->data, (uint64_t)other->image.width * other->image.height)) {",
        "a kept image covers a quarter of its bytes for an alias.",
    ),
    _sw(
        "guest-alias-unchecked",
        "if (other->valid && other->data != data && ranges_overlap(data, bytes, other->data, (uint64_t)other->image.width * 4u * other->image.height)) {",
        "if (other->valid && other->data != data && false) {",
        "a blit surface that overlaps a kept image at another offset is built from the guest.",
    ),
    _sw(
        "guest-width-pitch",
        "slot->image.width = width;",
        "slot->image.width = pitch;",
        "a guest built surface is four times as wide as its pitch says.",
    ),
    _sw(
        "guest-guest-built-lost",
        "slot->valid = true;\n    slot->guest_built = true;",
        "slot->valid = true;",
        "a guest built surface is not marked as built: no growth, no inference bit.",
    ),
    _sw(
        "guest-pitch-lost",
        "slot->guest_built = true;\n    slot->data = data;\n    slot->pitch = pitch;",
        "slot->guest_built = true;\n    slot->data = data;",
        "a guest built surface has no pitch: its growth reads the wrong rows.",
    ),
    _sw(
        "guest-not-counted",
        "state.stats.blit_guest_surfaces++;",
        "",
        "a blit surface built from guest memory is not counted.",
    ),
    _sw(
        "guest-protect-lost",
        "stored_surface *slot = claim_slot(data, protect);\n    slot->image.pixels = pixels;",
        "(void)protect;\n    stored_surface *slot = claim_slot(data, NULL);\n    slot->image.pixels = pixels;",
        "building the destination may evict the source the blit reads.",
    ),
    _sw(
        "grow-not-only-guest-built",
        "if (!surface->guest_built || surface->image.height >= rows) {",
        "if (surface->image.height >= rows) {",
        "a replayed image a blit reaches past is grown from guest memory instead of refused.",
    ),
    _sw(
        "grow-equal-regrows",
        "if (!surface->guest_built || surface->image.height >= rows) {",
        "if (!surface->guest_built || surface->image.height > rows) {",
        "a blit that reaches exactly the held rows grows the surface by zero rows, which is refused.",
    ),
    _sw(
        "grow-offset-lost",
        "surface->data + old_rows * surface->pitch, width,",
        "surface->data, width,",
        "the new rows are read from the first rows of the surface.",
    ),
    _sw(
        "grow-destination-lost",
        "pixels + (size_t)width * 4u * old_rows) != D3D8_SURFACE_MODEL_OK) {",
        "pixels) != D3D8_SURFACE_MODEL_OK) {",
        "the new rows overwrite the first rows of the image.",
    ),
    _sw(
        "grow-height-not-set",
        "surface->image.height = rows;\n    return true;",
        "return true;",
        "a grown surface keeps its old height: the blit reaches past its image.",
    ),
    _sw(
        "byte-one-side-only",
        "for (size_t side = 0u; side < 2u; side++) {",
        "for (size_t side = 0u; side < 1u; side++) {",
        "a byte path blit onto a surface held as an image is only refused for the source.",
    ),
    _sw(
        "byte-stored-image-unchecked",
        "bool image = find_stored(datas[side]) != NULL;",
        "bool image = false;",
        "a byte path blit over a kept image of an earlier frame mixes bytes and pixels.",
    ),
    _sw(
        "byte-pass-image-unchecked",
        "image = image || passes[i].data == datas[side];",
        "image = image || (passes[i].data == datas[side] && false);",
        "a byte path blit over a surface a pass of the frame drew mixes bytes and pixels.",
    ),
    _sw(
        "byte-sides-swapped",
        "const uint32_t datas[2] = {copy->source_offset, copy->destination_offset};",
        "const uint32_t datas[2] = {copy->destination_offset, copy->source_offset};",
        "the refusal names the source as the destination.",
    ),
    _sw(
        "byte-one-surface-needs-min",
        "const size_t need = source_need > destination_need ? source_need : destination_need;",
        "const size_t need = source_need > destination_need ? destination_need : source_need;",
        "a blit inside one surface holds the nearer extent: the farther rectangle is clamped.",
    ),
    _sw(
        "byte-one-surface-needs-source",
        "const size_t need = source_need > destination_need ? source_need : destination_need;",
        "const size_t need = source_need;",
        "a blit inside one surface holds the source's extent only.",
    ),
    _sw(
        "byte-one-surface-needs-destination",
        "const size_t need = source_need > destination_need ? source_need : destination_need;",
        "const size_t need = destination_need;",
        "a blit inside one surface holds the destination's extent only.",
    ),
    _sw(
        "byte-destination-acquire-unconditional",
        "if (status == D3D8_SURFACE_MODEL_OK) {\n            status = d3d8_surface_model_acquire(copy->destination_offset",
        "if (true) {\n            status = d3d8_surface_model_acquire(copy->destination_offset",
        "a refused source acquire is overwritten by the destination's.",
    ),
    _sw(
        "byte-destination-extent-source",
        "d3d8_surface_model_acquire(copy->destination_offset, destination_need, &destination)",
        "d3d8_surface_model_acquire(copy->destination_offset, source_need, &destination)",
        "the destination byte surface is sized by the source's extent.",
    ),
    _sw(
        "byte-source-extent-destination",
        "status = d3d8_surface_model_acquire(copy->source_offset, source_need, &source);\n        if (status",
        "status = d3d8_surface_model_acquire(copy->source_offset, destination_need, &source);\n        if (status",
        "the source byte surface is sized by the destination's extent.",
    ),
    _sw(
        "byte-surface-bit-lost",
        "state.stats.used_inferences |= used | GPU_PGRAPH_INFER_OUTPUT_SURFACE_SOURCE;",
        "state.stats.used_inferences |= used;",
        "a byte path blit does not report the SURFACE_SOURCE inference.",
    ),
    _sw(
        "byte-model-bit-lost",
        "state.stats.used_inferences |= used | GPU_PGRAPH_INFER_OUTPUT_SURFACE_SOURCE;",
        "state.stats.used_inferences |= GPU_PGRAPH_INFER_OUTPUT_SURFACE_SOURCE;",
        "a byte path blit does not report the BLIT_MODEL inference the copy used.",
    ),
    _sw(
        "byte-not-counted",
        "state.stats.byte_copies_applied++;",
        "",
        "a byte path blit is not counted.",
    ),
    _sw(
        "byte-refusal-text-lost",
        "report.error[0] != '\\0' ? report.error : gpu_pgraph_result_string(applied));\n        return false;\n    }\n    state.stats.used_inferences |= used |",
        "gpu_pgraph_result_string(applied));\n        return false;\n    }\n    state.stats.used_inferences |= used |",
        "a refused byte copy names the result, not the reason (clamp, overlap, two pitches).",
    ),
    _sw(
        "copies-empty-takes-byte-path",
        "const bool byte_path = copy->width != 0u && copy->height != 0u && copy->color_format",
        "const bool byte_path = copy->height != 0u && copy->color_format",
        "an empty byte format blit goes through the byte path, which is no longer a counted empty blit.",
    ),
    _sw(
        "copies-a8-takes-byte-path",
        "copy->color_format != GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8 &&\n                               state.config.surface_source;",
        "copy->color_format == GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8 &&\n                               state.config.surface_source;",
        "the formats take the other path: A8R8G8B8 is bytes and Y8 an image.",
    ),
    _sw(
        "copies-byte-path-without-option",
        "copy->color_format != GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8 &&\n                               state.config.surface_source;",
        "copy->color_format != GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8;",
        "a Y8 or R5G6B5 blit takes the byte path without the surface source.",
    ),
    _sw(
        "copies-byte-not-counted",
        "state.stats.copies_applied += 1u;\n            (void)d3d8_hle_log()",
        "(void)d3d8_hle_log()",
        "a byte path blit is not counted as an applied blit.",
    ),
    _sw(
        "copies-guest-without-option",
        "const bool guest = state.config.surface_source && copy->color_format == GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8;",
        "const bool guest = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8;",
        "a blit surface the replay holds no image of is built from guest memory without the surface source.",
    ),
    _sw(
        "copies-source-guest-lost",
        'if (found == BLIT_SURFACE_NO_IMAGE && guest) {\n                found = guest_blit_surface("source"',
        'if (false) {\n                found = guest_blit_surface("source"',
        "a source the replay holds no image of is refused even with the surface source.",
    ),
    _sw(
        "copies-destination-guest-lost",
        'if (found == BLIT_SURFACE_NO_IMAGE && guest) {\n                    found = guest_blit_surface("destination"',
        'if (false) {\n                    found = guest_blit_surface("destination"',
        "a destination the replay holds no image of is refused even with the surface source.",
    ),
    _sw(
        "copies-destination-after-failure",
        'if (found == BLIT_SURFACE_OK) {\n                found = resolve_blit_surface("destination"',
        'if (true) {\n                found = resolve_blit_surface("destination"',
        "a refused source is overwritten by the destination's lookup.",
    ),
    _sw(
        "copies-destination-protect-lost",
        "copy->out_y + copy->height,\n                                               stored_source, &destination",
        "copy->out_y + copy->height,\n                                               NULL, &destination",
        "building the destination may evict the source the blit reads.",
    ),
    _sw(
        "copies-destination-rows-short",
        'guest_blit_surface("destination", copy->destination_offset, copy->destination_pitch, copy->out_y + copy->height,',
        'guest_blit_surface("destination", copy->destination_offset, copy->destination_pitch, copy->height,',
        "a guest built destination reads only as many rows as the blit is high: the rows above the rectangle are missing.",
    ),
    _sw(
        "copies-source-rows-short",
        'guest_blit_surface("source", copy->source_offset, copy->source_pitch, copy->in_y + copy->height,',
        'guest_blit_surface("source", copy->source_offset, copy->source_pitch, copy->height,',
        "a guest built source reads only as many rows as the blit is high: the rows above the rectangle are missing.",
    ),
    _sw(
        "copies-grow-source-lost",
        "if (found == BLIT_SURFACE_OK && guest && stored_source != NULL && !grow_guest_surface(stored_source, copy->in_y + copy->height, reason, sizeof reason)) {",
        "if (found == BLIT_SURFACE_OK && guest && stored_source != NULL && false) {",
        "a later blit that reaches further down a guest built source is clamped.",
    ),
    _sw(
        "copies-grow-source-rows",
        "!grow_guest_surface(stored_source, copy->in_y + copy->height, reason, sizeof reason)",
        "!grow_guest_surface(stored_source, copy->height, reason, sizeof reason)",
        "the source grows only by the blit's height, not to the row its last row reaches.",
    ),
    _sw(
        "copies-grow-destination-lost",
        "if (found == BLIT_SURFACE_OK && guest && stored_destination != NULL &&\n                !grow_guest_surface(",
        "if (found == BLIT_SURFACE_OK && guest && stored_destination != NULL && false &&\n                !grow_guest_surface(",
        "a later blit that reaches further down a guest built destination is clamped.",
    ),
    _sw(
        "copies-grow-destination-rows",
        "!grow_guest_surface(stored_destination, copy->out_y + copy->height, reason, sizeof reason)",
        "!grow_guest_surface(stored_destination, copy->height, reason, sizeof reason)",
        "the destination grows only by the blit's height, not to the row its last row reaches.",
    ),
    _sw(
        "copies-inference-source-half",
        "if ((stored_source != NULL && stored_source->guest_built) || (stored_destination != NULL && stored_destination->guest_built)) {",
        "if (stored_destination != NULL && stored_destination->guest_built) {",
        "a blit from a guest built source does not report the SURFACE_SOURCE inference.",
    ),
    _sw(
        "copies-inference-destination-half",
        "if ((stored_source != NULL && stored_source->guest_built) || (stored_destination != NULL && stored_destination->guest_built)) {",
        "if (stored_source != NULL && stored_source->guest_built) {",
        "a blit onto a guest built destination does not report the SURFACE_SOURCE inference.",
    ),
    _sw(
        "copies-destination-not-touched",
        "if (stored_destination != NULL) {\n            stored_destination->touched = true;",
        "if (false) {\n            stored_destination->touched = true;",
        "a blit's destination is not dumped.",
    ),
    # ================================================================== note
    _sw(
        "note-no-update-counts",
        "if (overlay.updates == 0u || (overlay.enables & 1u) == 0u) {",
        "if ((overlay.enables & 1u) == 0u) {",
        "EnableOverlay(1) alone, with no UpdateOverlay, puts the frames under the overlay.",
    ),
    _sw(
        "note-enables-any",
        "if (overlay.updates == 0u || (overlay.enables & 1u) == 0u) {",
        "if (overlay.updates == 0u || overlay.enables == 0u) {",
        "an overlay turned off again by EnableOverlay(0) is still on.",
    ),
    _sw(
        "note-enables-even",
        "if (overlay.updates == 0u || (overlay.enables & 1u) == 0u) {",
        "if (overlay.updates == 0u || (overlay.enables & 1u) != 0u) {",
        "the overlay is on when the count of enables is even.",
    ),
    _sw(
        "note-not-counted",
        "state.stats.frames_under_overlay++;\n    if (overlay.color_key_enable == 0u) {",
        "if (overlay.color_key_enable == 0u) {",
        "a frame under the overlay is not counted.",
    ),
    _sw(
        "note-key-inverted",
        "if (overlay.color_key_enable == 0u) {",
        "if (overlay.color_key_enable != 0u) {",
        "a disabled colour key is the refused one.",
    ),
    _sw(
        "note-key-not-counted",
        "state.stats.frames_under_color_key++;\n    if (state.stats.frames_under_color_key == 1u) {",
        "if (state.stats.frames_under_color_key == 1u) {",
        "a frame under an enabled colour key is not counted (and never named).",
    ),
    _sw(
        "note-key-counted-without-return",
        "        }\n        return;\n    }\n    state.stats.frames_under_color_key++;",
        "        }\n    }\n    state.stats.frames_under_color_key++;",
        "a frame under the overlay with the key disabled is also counted as a refused key.",
    ),
    _sw(
        "note-disabled-named-every-frame",
        "if (state.stats.frames_under_overlay == 1u) {",
        "if (state.stats.frames_under_overlay >= 1u) {",
        "the disabled colour key is logged every frame, not once.",
    ),
    _sw(
        "note-refusal-named-every-frame",
        "if (state.stats.frames_under_color_key == 1u) {\n        (void)d3d8_hle_log()",
        "if (state.stats.frames_under_color_key >= 1u) {\n        (void)d3d8_hle_log()",
        "the enabled colour key is named every frame, not once.",
    ),
    # ================================================================== summary
    _sw(
        "summary-persist-only-silent",
        "if (!state.active || (!state.config.surface_source && !state.config.target_persist && state.stats.frames_under_overlay == 0u)) {",
        "if (!state.active || (!state.config.surface_source && state.stats.frames_under_overlay == 0u)) {",
        "target persistence alone prints no summary.",
    ),
    _sw(
        "summary-surface-only-silent",
        "if (!state.active || (!state.config.surface_source && !state.config.target_persist && state.stats.frames_under_overlay == 0u)) {",
        "if (!state.active || (!state.config.target_persist && state.stats.frames_under_overlay == 0u)) {",
        "the surface source alone prints no summary.",
    ),
    _sw(
        "summary-overlay-only-silent",
        "if (!state.active || (!state.config.surface_source && !state.config.target_persist && state.stats.frames_under_overlay == 0u)) {",
        "if (!state.active || (!state.config.surface_source && !state.config.target_persist)) {",
        "frames under the overlay print no summary with both options off.",
    ),
    _sw(
        "summary-always",
        "if (!state.active || (!state.config.surface_source && !state.config.target_persist && state.stats.frames_under_overlay == 0u)) {",
        "if (!state.active) {",
        "the summary is printed when nothing is on and nothing was under the overlay.",
    ),
    _sw(
        "summary-surface-on-inverted",
        'state.config.surface_source ? "ON" : "off",',
        'state.config.surface_source ? "off" : "ON",',
        "the summary says the opposite of the surface source.",
    ),
    _sw(
        "summary-persist-on-inverted",
        'state.config.target_persist ? "ON" : "off",',
        'state.config.target_persist ? "off" : "ON",',
        "the summary says the opposite of the target persistence.",
    ),
    _sw(
        "summary-kept-guest-swapped",
        "(unsigned long long)state.stats.surface_kept_samples,\n        (unsigned long long)state.stats.surface_guest_samples, (unsigned long long)state.stats.surface_guest_zero_samples,",
        "(unsigned long long)state.stats.surface_guest_samples,\n        (unsigned long long)state.stats.surface_kept_samples, (unsigned long long)state.stats.surface_guest_zero_samples,",
        "the kept and guest sample counts swap places.",
    ),
    _sw(
        "summary-guest-zero-swapped",
        "(unsigned long long)state.stats.surface_guest_samples, (unsigned long long)state.stats.surface_guest_zero_samples,\n        (unsigned long long)state.stats.blit_guest_surfaces",
        "(unsigned long long)state.stats.surface_guest_zero_samples, (unsigned long long)state.stats.surface_guest_samples,\n        (unsigned long long)state.stats.blit_guest_surfaces",
        "the guest and all zero sample counts swap places.",
    ),
    _sw(
        "summary-blit-bytes-swapped",
        "(unsigned long long)state.stats.blit_guest_surfaces, (unsigned long long)state.stats.byte_copies_applied,",
        "(unsigned long long)state.stats.byte_copies_applied, (unsigned long long)state.stats.blit_guest_surfaces,",
        "the blit surface and byte path counts swap places.",
    ),
    _sw(
        "summary-persisted-count",
        "(unsigned long long)state.stats.targets_persisted,",
        "(unsigned long long)state.stats.byte_copies_applied,",
        "the persisted pass count is the byte path count.",
    ),
    _sw(
        "summary-key-value",
        "(unsigned)overlay.color_key, (unsigned long long)state.stats.frames_under_overlay,",
        "(unsigned)overlay.color_key_enable, (unsigned long long)state.stats.frames_under_overlay,",
        "the last key is its enable flag.",
    ),
    _sw(
        "summary-overlay-keyed-swapped",
        "(unsigned long long)state.stats.frames_under_overlay, (unsigned long long)state.stats.frames_under_color_key);",
        "(unsigned long long)state.stats.frames_under_color_key, (unsigned long long)state.stats.frames_under_overlay);",
        "the frames under the overlay and under an enabled key swap places.",
    ),
    _sw(
        "summary-text-measured",
        "pass the key DISABLED (MEASURED)",
        "pass the key disabled",
        "the summary stops saying the disabled key is MEASURED.",
    ),
    _sw(
        "summary-text-refused",
        "an update with the key ENABLED is REFUSED by name (measured by T770, not composed here, last key",
        "an update with the key ENABLED is accepted (measured by T770, not composed here, last key",
        "the summary says an enabled key is accepted.",
    ),
    _sw(
        "summary-text-not-composed",
        "the overlay picture is NOT composed over the replayed",
        "the overlay picture is composed over the replayed",
        "the summary says the overlay picture is composed.",
    ),
    _sw(
        "summary-text-guest-measured",
        "all zero = transparent black, MEASURED bytes)",
        "all zero = transparent black)",
        "the summary stops saying the zero bytes are MEASURED.",
    ),
    _sw(
        "summary-text-refusals",
        'a size, pitch or Format that differs, a CPU written surface under a kept image, an alias "\n        "and a blit before the draw are REFUSED by name.',
        'a size, pitch or Format that differs, a CPU written surface under a kept image, an alias "\n        "and a blit before the draw are accepted.',
        "the summary stops listing what is REFUSED by name.",
    ),
    _sw(
        "summary-small-buffer-length",
        "(unsigned long long)state.stats.frames_under_color_key);\n    if (written < 0) {\n        text[0] = '\\0';\n        return 0u;\n    }\n    return strlen(text);",
        "(unsigned long long)state.stats.frames_under_color_key);\n    if (written < 0) {\n        text[0] = '\\0';\n        return 0u;\n    }\n    return (size_t)written;",
        "a truncated summary returns the length it would have had, not the length written.",
    ),
    # ================================================================== header
    _hd(
        "header-surface-bit-is-persist",
        "#define D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE GPU_PGRAPH_INFER_OUTPUT_SURFACE_SOURCE",
        "#define D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE GPU_PGRAPH_INFER_OUTPUT_TARGET_PERSIST",
        "the surface source inference is the persistence's bit.",
    ),
    _hd(
        "header-persist-bit-is-surface",
        "#define D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST GPU_PGRAPH_INFER_OUTPUT_TARGET_PERSIST",
        "#define D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST GPU_PGRAPH_INFER_OUTPUT_SURFACE_SOURCE",
        "the persistence inference is the surface source's bit.",
    ),
    _hd(
        "header-persist-bit-is-bridge",
        "#define D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST GPU_PGRAPH_INFER_OUTPUT_TARGET_PERSIST",
        "#define D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE",
        "the persistence inference is the texture bridge's bit.",
    ),
    # ================================================================== hostopt
    _ho(
        "hostopt-surface-flag-sets-persist",
        '"--gpu-replay-surface-source") == 0) {\n            out->gpu_replay_surface_source = true;',
        '"--gpu-replay-surface-source") == 0) {\n            out->gpu_replay_target_persist = true;',
        "--gpu-replay-surface-source turns on the persistence.",
    ),
    _ho(
        "hostopt-persist-flag-sets-surface",
        '"--gpu-replay-target-persist") == 0) {\n            out->gpu_replay_target_persist = true;',
        '"--gpu-replay-target-persist") == 0) {\n            out->gpu_replay_surface_source = true;',
        "--gpu-replay-target-persist turns on the surface source.",
    ),
    _ho(
        "hostopt-surface-default-kept",
        "    out->gpu_replay_surface_source = false;\n",
        "",
        "the surface source is not reset to off by a parse.",
    ),
    _ho(
        "hostopt-persist-default-kept",
        "    out->gpu_replay_target_persist = false;\n",
        "",
        "the target persistence is not reset to off by a parse.",
    ),
    _ho(
        "hostopt-surface-without-rt-texture",
        "(!out->gpu_replay_surface_source || out->gpu_replay_rt_texture) &&",
        "true &&",
        "--gpu-replay-surface-source is accepted without --gpu-replay-rt-texture.",
    ),
    _ho(
        "hostopt-surface-needs-combiner-only",
        "(!out->gpu_replay_surface_source || out->gpu_replay_rt_texture) &&",
        "(!out->gpu_replay_surface_source || out->gpu_replay_combiner) &&",
        "--gpu-replay-surface-source needs only the combiner, not the render target texture.",
    ),
    _ho(
        "hostopt-persist-without-dir",
        "(!out->gpu_replay_target_persist || (out->gpu_replay_dir != NULL && !out->gpu_replay_flip_y)) &&",
        "(!out->gpu_replay_target_persist || !out->gpu_replay_flip_y) &&",
        "--gpu-replay-target-persist is accepted without --gpu-replay.",
    ),
    _ho(
        "hostopt-persist-with-flip",
        "(!out->gpu_replay_target_persist || (out->gpu_replay_dir != NULL && !out->gpu_replay_flip_y)) &&",
        "(!out->gpu_replay_target_persist || out->gpu_replay_dir != NULL) &&",
        "--gpu-replay-target-persist is accepted with --gpu-replay-flip-y.",
    ),
    _ho(
        "hostopt-persist-needs-flip",
        "(!out->gpu_replay_target_persist || (out->gpu_replay_dir != NULL && !out->gpu_replay_flip_y)) &&",
        "(!out->gpu_replay_target_persist || (out->gpu_replay_dir != NULL && out->gpu_replay_flip_y)) &&",
        "--gpu-replay-target-persist requires --gpu-replay-flip-y.",
    ),
    _ho(
        "hostopt-persist-dir-or-flip",
        "(!out->gpu_replay_target_persist || (out->gpu_replay_dir != NULL && !out->gpu_replay_flip_y)) &&",
        "(!out->gpu_replay_target_persist || (out->gpu_replay_dir != NULL || !out->gpu_replay_flip_y)) &&",
        "--gpu-replay-target-persist needs either the directory or no flip.",
    ),
    _ho(
        "hostopt-persist-unchecked",
        "(!out->gpu_replay_target_persist || (out->gpu_replay_dir != NULL && !out->gpu_replay_flip_y)) &&",
        "true &&",
        "--gpu-replay-target-persist has no requirements.",
    ),
]

#: Mutations whose only kill is a scene that needs a Vulkan device: from a run with `VK_DRIVER_FILES=/nonexistent
#: VK_ICD_FILENAMES=/nonexistent`, in which they are reported SKIPPED (the device free checks of the swap replay test pass).
_NEEDS_DEVICE = frozenset(
    {
        "t633-surface-initial-flip-refusal-dropped",
        "t633-surface-initial-refuses-any-flip",
        "t633-surface-initial-not-copied",
        "t633-surface-initial-last-row-not-copied",
        "t633-surface-blit-before-draw-after",
        "t633-surface-blit-before-empty-counts",
        "t633-surface-blit-before-oldest",
        "t633-surface-blit-before-source-pitch",
        "t633-surface-blit-before-source-corner",
        "t633-surface-blit-before-sizes-swapped",
        "t633-surface-surface-alias-self",
        "t633-surface-surface-alias-range-pixels",
        "t633-surface-surface-alias-range-width-twice",
        "t633-surface-surface-alias-unchecked",
        "t633-surface-surface-byte-surface-unchecked",
        "t633-surface-surface-byte-surface-exact-only",
        "t633-surface-surface-kept-width-unchecked",
        "t633-surface-surface-kept-height-unchecked",
        "t633-surface-surface-kept-pitch-unchecked",
        "t633-surface-surface-kept-format-unchecked",
        "t633-surface-surface-kept-ignored",
        "t633-surface-surface-cpu-written-unchecked",
        "t633-surface-surface-kept-dry-inverted",
        "t633-surface-surface-kept-kind-guest",
        "t633-surface-surface-guest-kind-swapped",
        "t633-surface-surface-kept-text-size-swapped",
        "t633-surface-surface-kept-text-measured",
        "t633-surface-surface-guest-zero-text-measured",
        "t633-surface-surface-not-linear",
        "t633-surface-surface-normalised",
        "t633-surface-provider-later-pass-takes-no-surface",
        "t633-surface-provider-earlier-pass-takes-surface",
        "t633-surface-provider-pass-blit-without-option",
        "t633-surface-provider-pass-blit-first-byte",
        "t633-surface-provider-pass-blit-ignored",
        "t633-surface-sampled-surface-flag-lost",
        "t633-surface-sampled-kept-not-counted",
        "t633-surface-sampled-guest-not-counted",
        "t633-surface-sampled-zero-always",
        "t633-surface-sampled-pass-counted-guest",
        "t633-surface-sampled-kept-as-guest",
        "t633-surface-claim-eviction-not-advanced",
        "t633-surface-claim-protect-ignored",
        "t633-surface-claim-protect-retakes",
        "t633-surface-claim-eviction-not-counted",
        "t633-surface-claim-slot-not-cleared",
        "t633-surface-claim-last-free-slot-unused",
        "t633-surface-store-pitch-lost",
        "t633-surface-store-format-lost",
        "t633-surface-store-unnamed-kept",
        "t633-surface-store-always-kept",
        "t633-surface-store-copy-short",
        "t633-surface-store-valid-lost",
        "t633-surface-kept-persist-term-dropped",
        "t633-surface-kept-blit-term-dropped",
        "t633-surface-persist-always-on",
        "t633-surface-persist-no-held-unchecked",
        "t633-surface-persist-height-unchecked",
        "t633-surface-persist-pitch-unchecked",
        "t633-surface-persist-pitch-zero-allowed",
        "t633-surface-persist-format-unchecked",
        "t633-surface-persist-format-zero-allowed",
        "t633-surface-persist-probe-tight",
        "t633-surface-persist-probe-width",
        "t633-surface-persist-unreadable-passes",
        "t633-surface-persist-cpu-written-passes",
        "t633-surface-persist-text-swapped",
        "t633-surface-persist-initial-not-returned",
        "t633-surface-wiring-initial-dropped",
        "t633-surface-wiring-refusal-ignored",
        "t633-surface-wiring-persisted-not-counted",
        "t633-surface-wiring-persist-bit-lost",
        "t633-surface-wiring-surface-bit-lost",
        "t633-surface-resolve-no-image-refused",
        "t633-surface-resolve-stored-not-returned",
        "t633-surface-guest-pitch-unaligned-allowed",
        "t633-surface-guest-pitch-bound-inclusive",
        "t633-surface-guest-pitch-unbounded",
        "t633-surface-guest-rows-bound-inclusive",
        "t633-surface-guest-rows-unbounded",
        "t633-surface-guest-byte-surface-overlap-unchecked",
        "t633-surface-guest-alias-range-pixels",
        "t633-surface-guest-alias-unchecked",
        "t633-surface-guest-width-pitch",
        "t633-surface-guest-guest-built-lost",
        "t633-surface-guest-pitch-lost",
        "t633-surface-guest-not-counted",
        "t633-surface-guest-protect-lost",
        "t633-surface-grow-not-only-guest-built",
        "t633-surface-grow-equal-regrows",
        "t633-surface-grow-offset-lost",
        "t633-surface-grow-destination-lost",
        "t633-surface-grow-height-not-set",
        "t633-surface-byte-one-side-only",
        "t633-surface-byte-stored-image-unchecked",
        "t633-surface-byte-pass-image-unchecked",
        "t633-surface-byte-sides-swapped",
        "t633-surface-byte-one-surface-needs-min",
        "t633-surface-byte-one-surface-needs-source",
        "t633-surface-byte-one-surface-needs-destination",
        "t633-surface-byte-destination-acquire-unconditional",
        "t633-surface-byte-destination-extent-source",
        "t633-surface-byte-source-extent-destination",
        "t633-surface-byte-surface-bit-lost",
        "t633-surface-byte-model-bit-lost",
        "t633-surface-byte-not-counted",
        "t633-surface-byte-refusal-text-lost",
        "t633-surface-copies-empty-takes-byte-path",
        "t633-surface-copies-a8-takes-byte-path",
        "t633-surface-copies-byte-path-without-option",
        "t633-surface-copies-byte-not-counted",
        "t633-surface-copies-guest-without-option",
        "t633-surface-copies-source-guest-lost",
        "t633-surface-copies-destination-guest-lost",
        "t633-surface-copies-destination-after-failure",
        "t633-surface-copies-destination-protect-lost",
        "t633-surface-copies-destination-rows-short",
        "t633-surface-copies-source-rows-short",
        "t633-surface-copies-grow-source-lost",
        "t633-surface-copies-grow-source-rows",
        "t633-surface-copies-grow-destination-lost",
        "t633-surface-copies-grow-destination-rows",
        "t633-surface-copies-inference-source-half",
        "t633-surface-copies-inference-destination-half",
        "t633-surface-copies-destination-not-touched",
        "t633-surface-note-no-update-counts",
        "t633-surface-note-enables-any",
        "t633-surface-note-enables-even",
        "t633-surface-note-not-counted",
        "t633-surface-note-key-inverted",
        "t633-surface-note-key-not-counted",
        "t633-surface-note-key-counted-without-return",
        "t633-surface-note-disabled-named-every-frame",
        "t633-surface-note-refusal-named-every-frame",
        "t633-surface-summary-overlay-only-silent",
        "t633-surface-summary-kept-guest-swapped",
        "t633-surface-summary-guest-zero-swapped",
        "t633-surface-summary-blit-bytes-swapped",
        "t633-surface-summary-persisted-count",
        "t633-surface-summary-key-value",
        "t633-surface-summary-overlay-keyed-swapped",
    }
)

for _mutation in MUTATIONS:
    _mutation["needs_device"] = _mutation["id"] in _NEEDS_DEVICE
