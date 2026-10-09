"""Mutations for the D3D8 init sequence: display modes, surfaces, the pushbuffer ring, CreateDevice
and the render-state helpers (src/gpu/d3d8_{display,surface,pushbuffer,device,state,resource,
frame,guest}.c).

WHAT THIS SET IS FOR. These handlers answer the title from numbers read out of the original
machine code, and the failure mode they share is a number that is plausible and wrong: a flag
mask off by one bit, a field stored one slot over, two of four formats swapped. The boot advances
on every one of them, because the title never checks. The suites assert hardware LITERALS, and each
mutation below is a way one of those literals could have been transcribed wrong without anything
looking wrong. `why` is written before the run so a survivor cannot be rationalised afterwards.

CONVENTION. `if (cond && false)` rather than `if (false)`, because `-Wunused-parameter -Werror`
turns the latter into NOT-A-MUTANT, which reads like evidence.

NOT COVERED HERE, and why. `tests/c/test_d3d8_init_real.c` is the suite that runs over the real
executable's tables, but it exits 77 (SKIP) without the XBE and this harness reads any non-zero exit
as a kill, so it is not named as a target: a mutation scored by it would be scored by the skip. The
real-table checks are covered by the same logic in the synthetic suites. `src/host/main.c`, where
the handlers are registered and the fatal hook installed, is in `tsfp_host`, which is not a ctest
binary.

NOT MUTATED, because the mutant is EQUIVALENT: the `& 0x0FFFFFFF` on a buffer's physical address in
`d3d8_create_buffer`. The kernel module's synthetic physical addresses never reach 256 MiB, so the
mask removes nothing, and no test can make it matter without changing that module. The identical
mask in `d3d8_surface_init_header` IS mutated (`d3d-surface-header-data-mask`), because that
function is handed the original's virtual address.
"""

DISPLAY = "src/gpu/d3d8_display.c"
SURFACE = "src/gpu/d3d8_surface.c"
PUSHBUFFER = "src/gpu/d3d8_pushbuffer.c"
DEVICE = "src/gpu/d3d8_device.c"
BIND = "src/gpu/d3d8_bind.c"
STATE = "src/gpu/d3d8_state.c"
RESOURCE = "src/gpu/d3d8_resource.c"
FRAME = "src/gpu/d3d8_frame.c"
HLE = "src/gpu/d3d8_hle.c"

T_DISPLAY = ["test_d3d8_display"]
T_SURFACE = ["test_d3d8_surface"]
T_PUSH = ["test_d3d8_pushbuffer"]
T_INIT = ["test_d3d8_init"]
T_STATE = ["test_d3d8_state"]

# T536: d3d8_state.c holds the HLE handlers (d3d8_state_set_93/95) and, since T443, the library
# variants (d3d8_state_library_set_93/95) with the same statements. The d3d-state-93 and
# d3d-state-95 anchors target the HLE handlers, so each carries a line only that copy has (the
# function header, the comment above the widening, the shadow store after the flag) to match once.
HLE_95_WIDENING = (
    "     * store, which long double reproduces. */\n"
    "    long double widened = (long double)(int32_t)value;\n"
)
HLE_95_TAIL = (
    "    const float negated_float = (float)negated;\n"
    "    const float scaled_float =\n"
    "        (float)(negated * (long double)float_of_bits(d3d8_guest_load32(CONSTANT_QUARTER)));\n"
    "    const uint32_t flag = value != 0u ? 1u : 0u;\n\n"
    "    const uint32_t shadow_values"
)

MUTATIONS: list[dict] = [
    {
        "id": "d3d-display-four-formats-per-row",
        "file": DISPLAY,
        "old": "    return accepted << 2;",
        "new": "    return accepted << 1;",
        "targets": T_DISPLAY,
        "why": (
            "the title enumerates one mode per variant of each row and discards the "
            "results, so a count of two per row halves how many EnumAdapterModes "
            "calls the boot makes and changes nothing it checks."
        ),
    },
    {
        "id": "d3d-display-refresh-class-ignored",
        "file": DISPLAY,
        "old": (
            "    if ((capabilities & FLAG_REFRESH_CLASS & row_flags) == 0u) {\n"
            "        return false;"
        ),
        "new": (
            "    if ((capabilities & FLAG_REFRESH_CLASS & row_flags) == 0u && false) "
            "{\n"
            "        return false;"
        ),
        "targets": T_DISPLAY,
        "why": (
            "without the refresh class every row of a capability word with no 60 Hz "
            "bit is accepted, and the title is shown modes the console cannot "
            "display. av-policy.md records that zero modes is the MEASURED answer "
            "there."
        ),
    },
    {
        "id": "d3d-display-hd-refusal-dropped",
        "file": DISPLAY,
        "old": ("        if ((row_flags & FLAG_HD_ANY) != 0u) {\n            return false;"),
        "new": (
            "        if ((row_flags & FLAG_HD_ANY) != 0u && false) {\n            return false;"
        ),
        "targets": T_DISPLAY,
        "why": (
            "a 720p or 1080i row under an HDTV capability word that only has 480p is "
            "shown to the title, which can then request a mode the pack cannot do."
        ),
    },
    {
        "id": "d3d-display-hd-acceptance-dropped",
        "file": DISPLAY,
        "old": (
            "        if ((capabilities & FLAG_HD_ANY & row_flags) != 0u) {\n"
            "            return true;"
        ),
        "new": (
            "        if ((capabilities & FLAG_HD_ANY & row_flags) != 0u && false) {\n"
            "            return true;"
        ),
        "targets": T_DISPLAY,
        "why": (
            "the 480p row of an HDTV pack falls through to the refusal and is "
            "dropped, which leaves the pack with no progressive mode."
        ),
    },
    {
        "id": "d3d-display-widescreen-row-gate",
        "file": DISPLAY,
        "old": (
            "    if (!skip_widescreen_test && (row_flags & FLAG_WIDESCREEN) != 0u &&\n"
            "        (capabilities & FLAG_WIDESCREEN) == 0u) {"
        ),
        "new": (
            "    if (!skip_widescreen_test && (row_flags & FLAG_WIDESCREEN) != 0u &&\n"
            "        (capabilities & FLAG_WIDESCREEN) == 0u && false) {"
        ),
        "targets": T_DISPLAY,
        "why": "widescreen rows are offered to a console whose answer is not widescreen.",
    },
    {
        "id": "d3d-display-hd-rows-not-exempt",
        "file": DISPLAY,
        "old": (
            "    const bool skip_widescreen_test = pack == PACK_HDTV && (row_flags & "
            "FLAG_HD_720P_1080I) != 0u;"
        ),
        "new": (
            "    const bool skip_widescreen_test = pack == PACK_HDTV && (row_flags & "
            "FLAG_HD_720P_1080I) != 0u && false;"
        ),
        "targets": T_DISPLAY,
        "why": (
            "the 720p and 1080i widescreen rows of an HDTV pack are refused for want "
            "of a widescreen bit the original never required of them."
        ),
    },
    {
        "id": "d3d-display-variant-formats-swapped",
        "file": DISPLAY,
        "old": "static const uint32_t variant_format[4] = {0x1Eu, 0x11u, 0x1Cu, 0x12u};",
        "new": "static const uint32_t variant_format[4] = {0x1Eu, 0x1Cu, 0x11u, 0x12u};",
        "targets": T_DISPLAY,
        "why": (
            "two of the four formats a mode reports are swapped. The title discards "
            "the list, so only an assertion on each variant can see it."
        ),
    },
    {
        "id": "d3d-display-refresh-rate-inverted",
        "file": DISPLAY,
        "old": "(row_flags & FLAG_60HZ) != 0u ? 60u : 50u",
        "new": "(row_flags & FLAG_60HZ) != 0u ? 50u : 60u",
        "targets": T_DISPLAY,
        "why": "every NTSC mode reports 50 Hz and every PAL mode 60.",
    },
    {
        "id": "d3d-display-width-height-swapped",
        "file": DISPLAY,
        "old": "                d3d8_guest_store32(out_address, size_word & 0xFFFFu);",
        "new": ("                d3d8_guest_store32(out_address, (size_word >> 16) & 0xFFFFu);"),
        "targets": T_DISPLAY,
        "why": (
            "width and height of every mode are exchanged, which a square-ish test "
            "mode would not show."
        ),
    },
    {
        "id": "d3d-display-mode-ordinal-shift",
        "file": DISPLAY,
        "old": "    uint32_t remaining = mode_index >> 2;",
        "new": "    uint32_t remaining = mode_index >> 3;",
        "targets": T_DISPLAY,
        "why": (
            "mode 4 names row 0 again instead of the second accepted row, so half of "
            "the enumeration repeats."
        ),
    },
    {
        "id": "d3d-display-present-flags-progressive",
        "file": DISPLAY,
        "old": "        flags |= 0x40u;",
        "new": "        flags |= 0x20u;",
        "targets": T_DISPLAY,
        "why": "every progressive mode reports as interlaced.",
    },
    {
        "id": "d3d-display-present-flags-aspect",
        "file": DISPLAY,
        "old": "        flags |= 0x100u;",
        "new": "        flags |= 0x80u;",
        "targets": T_DISPLAY,
        "why": "the 10:11 pixel-aspect bit lands on the field bit.",
    },
    {
        "id": "d3d-display-present-flags-widescreen",
        "file": DISPLAY,
        "old": "        flags = 0x10u;",
        "new": "        flags = 0x20u;",
        "targets": T_DISPLAY,
        "why": "widescreen is reported as the interlaced bit.",
    },
    {
        "id": "d3d-display-block-needs-matching-pack",
        "file": DISPLAY,
        "old": "        if (row_pack == 0u || row_pack == pack) {",
        "new": "        if (row_pack == 0u || (row_pack == pack && false)) {",
        "targets": T_DISPLAY,
        "why": "an HDTV pack falls onto the fallback block and shows composite modes.",
    },
    {
        "id": "d3d-display-block-fallback-ignored",
        "file": DISPLAY,
        "old": "        if (row_pack == 0u || row_pack == pack) {",
        "new": "        if (row_pack == pack) {",
        "targets": T_DISPLAY,
        "why": ("a pack with no block of its own never finds the fallback and runs off the table."),
    },
    {
        "id": "d3d-display-block-first-loop-bound",
        "file": DISPLAY,
        "old": (
            "        index++;\n"
            "        if (index >= D3D8_MODE_TABLE_ROWS) {\n"
            "            return found;\n"
            "        }\n"
            "    }\n"
            "\n"
            "    /* Second loop"
        ),
        "new": (
            "        index++;\n"
            "        if (index > D3D8_MODE_TABLE_ROWS) {\n"
            "            return found;\n"
            "        }\n"
            "    }\n"
            "\n"
            "    /* Second loop"
        ),
        "targets": T_DISPLAY,
        "why": "a standard with no rows reads one row past the terminator.",
    },
    {
        "id": "d3d-display-block-standard-mask",
        "file": DISPLAY,
        "old": "    const uint32_t standard = capabilities & 0xFF00u;",
        "new": "    const uint32_t standard = capabilities & 0xFFFFu;",
        "targets": T_DISPLAY,
        "why": "the pack byte leaks into the standard and no row matches.",
    },
    {
        "id": "d3d-display-capabilities-always-asked",
        "file": DISPLAY,
        "old": "    if (d3d8_guest_load32(D3D8_GLOBAL_AV_CAPABILITIES) == 0u) {",
        "new": "    if (d3d8_guest_load32(D3D8_GLOBAL_AV_CAPABILITIES) == 0u || true) {",
        "targets": T_DISPLAY,
        "why": (
            "the kernel is asked, and a fabricated hardware claim is logged and "
            "counted, on every mode lookup instead of once."
        ),
    },
    {
        "id": "d3d-display-capabilities-wrong-option",
        "file": DISPLAY,
        "old": "#define AV_OPTION_QUERY_CAPABILITIES 6u",
        "new": "#define AV_OPTION_QUERY_CAPABILITIES 7u",
        "targets": T_DISPLAY,
        "why": (
            "the kernel refuses an option the title never sends, the cache stays "
            "zero and the title is shown no modes."
        ),
    },
    {
        "id": "d3d-display-match-widescreen-bit",
        "file": DISPLAY,
        "old": "(((row_flags >> 16) ^ (flags >> 4)) & 1u) != 0u ||",
        "new": "false ||",
        "targets": T_DISPLAY,
        "why": "a widescreen request is served by a non-widescreen row.",
    },
    {
        "id": "d3d-display-match-field-bit",
        "file": DISPLAY,
        "old": "(((row_flags >> 24) ^ (flags >> 7)) & 1u) != 0u ||",
        "new": "false ||",
        "targets": T_DISPLAY,
        "why": "a field-flagged request is served by a row with no field bit.",
    },
    {
        "id": "d3d-display-match-aspect-bit",
        "file": DISPLAY,
        "old": "(((row_flags >> 25) ^ (flags >> 8)) & 1u) != 0u)) {",
        "new": "false)) {",
        "targets": T_DISPLAY,
        "why": (
            "a request for the 10:11 pixel aspect is served by a row without it, "
            "which is the difference between the title's progressive request "
            "succeeding and failing."
        ),
    },
    {
        "id": "d3d-display-match-size",
        "file": DISPLAY,
        "old": ("                           ((size_word >> 16) & 0xFFFFu) != request->height)) {"),
        "new": "                           false)) {",
        "targets": T_DISPLAY,
        "why": "any height is served by a row of the right width.",
    },
    {
        "id": "d3d-display-match-width",
        "file": DISPLAY,
        "old": "    if (agrees && ((size_word & 0xFFFFu) != request->width ||",
        "new": "    if (agrees && (false ||",
        "targets": T_DISPLAY,
        "why": "any width is served by a row of the right height.",
    },
    {
        "id": "d3d-display-match-scan-interlaced-refusal",
        "file": DISPLAY,
        "old": "                } else if ((scan & 0x40u) == 0u || row_interlaced) {",
        "new": "                } else if ((scan & 0x40u) == 0u) {",
        "targets": T_DISPLAY,
        "why": "a progressive request is served by an interlaced row.",
    },
    {
        "id": "d3d-display-match-scan-interlaced-acceptance",
        "file": DISPLAY,
        "old": "                if ((scan & 0x20u) != 0u && row_interlaced) {",
        "new": "                if ((scan & 0x20u) != 0u && row_interlaced && false) {",
        "targets": T_DISPLAY,
        "why": "an interlaced request is refused by the interlaced row it asked for.",
    },
    {
        "id": "d3d-display-match-zero-mode-word-continues",
        "file": DISPLAY,
        "old": (
            "                if (mode_word == 0u) {\n"
            "                    return D3D8_E_FAIL;\n"
            "                }"
        ),
        "new": (
            "                if (mode_word == 0u) {\n"
            "                    row += D3D8_MODE_TABLE_ROW_BYTES;\n"
            "                    continue;\n"
            "                }"
        ),
        "targets": T_DISPLAY,
        "why": (
            "a row that agrees on everything but has no mode word is skipped and a "
            "later row serves the request, where the original fails the call."
        ),
    },
    {
        "id": "d3d-display-match-class-admission-dropped",
        "file": DISPLAY,
        "old": "                bool admitted = (capabilities & refresh_class) != 0u;",
        "new": "                bool admitted = true;",
        "targets": T_DISPLAY,
        "why": "a row of a refresh class the capability word does not carry is served.",
    },
    {
        "id": "d3d-display-match-hd-exemption-dropped",
        "file": DISPLAY,
        "old": (
            "                const bool hd_row = pack == PACK_HDTV && (row_flags & "
            "FLAG_HD_720P_1080I) != 0u;"
        ),
        "new": (
            "                const bool hd_row = pack == PACK_HDTV && (row_flags & "
            "FLAG_HD_720P_1080I) != 0u && false;"
        ),
        "targets": T_DISPLAY,
        "why": ("a widescreen 720p request on an HDTV pack without a widescreen bit fails."),
    },
    {
        "id": "d3d-display-match-widescreen-admission",
        "file": DISPLAY,
        "old": (
            "                if (!hd_row && (flags & 0x10u) != 0u && (capabilities & "
            "FLAG_WIDESCREEN) == 0u) {"
        ),
        "new": (
            "                if (!hd_row && (flags & 0x10u) != 0u && (capabilities & "
            "FLAG_WIDESCREEN) == 0u && false) {"
        ),
        "targets": T_DISPLAY,
        "why": "a widescreen request is served on a capability word that cannot show it.",
    },
    {
        "id": "d3d-display-match-mode-word-stored",
        "file": DISPLAY,
        "old": "    d3d8_guest_store32(hw_object + 0x08u, mode_word);",
        "new": "    d3d8_guest_store32(hw_object + 0x08u, 0u);",
        "targets": T_DISPLAY,
        "why": ("the mode word the title's first present hands to AvSetDisplayMode is lost."),
    },
    {
        "id": "d3d-display-match-slot-index",
        "file": DISPLAY,
        "old": "    d3d8_guest_store32(hw_object + 0x7DCu + slot * 4u, 1u);",
        "new": "    d3d8_guest_store32(hw_object + 0x7DCu + (slot & 0u) * 4u, 1u);",
        "targets": T_DISPLAY,
        "why": "the marker always lands in slot 0.",
    },
    {
        "id": "d3d-display-match-row-flags-stored",
        "file": DISPLAY,
        "old": "    d3d8_guest_store32(hw_object + 0x1B4u, d3d8_guest_load32(row));",
        "new": "    d3d8_guest_store32(hw_object + 0x1B4u, 0u);",
        "targets": T_DISPLAY,
        "why": "the matched row's flags, which the mode-set tail reads, are lost.",
    },
    {
        "id": "d3d-display-match-refresh-class-for-60",
        "file": DISPLAY,
        "old": "            refresh_class = FLAG_60HZ;",
        "new": "            refresh_class = 0x00800000u;",
        "targets": T_DISPLAY,
        "why": "a 60 Hz request is served from the 50 Hz class.",
    },
    {
        "id": "d3d-surface-format-6-maps-wrong",
        "file": SURFACE,
        "old": ("    case 0x06u:\n        return 0x12u;"),
        "new": ("    case 0x06u:\n        return 0x11u;"),
        "targets": T_SURFACE,
        "why": "the back buffer format reaches the title as 16-bit colour.",
    },
    {
        "id": "d3d-surface-format-depth-maps-wrong",
        "file": SURFACE,
        "old": ("    case 0x2Au:\n        return 0x2Eu;"),
        "new": ("    case 0x2Au:\n        return 0x2Fu;"),
        "targets": T_SURFACE,
        "why": "the depth buffer's Format word names the wrong depth format.",
    },
    {
        "id": "d3d-surface-bpp-shift",
        "file": SURFACE,
        "old": ("    return (((format_bits_per_pixel(format) * width) >> 3) + 0x3Fu) & ~0x3Fu;"),
        "new": ("    return (((format_bits_per_pixel(format) * width) >> 2) + 0x3Fu) & ~0x3Fu;"),
        "targets": T_SURFACE,
        "why": "every pitch doubles and the surface size doubles with it.",
    },
    {
        "id": "d3d-surface-pitch-round",
        "file": SURFACE,
        "old": ("    return (((format_bits_per_pixel(format) * width) >> 3) + 0x3Fu) & ~0x3Fu;"),
        "new": ("    return (((format_bits_per_pixel(format) * width) >> 3) + 0x1Fu) & ~0x1Fu;"),
        "targets": T_SURFACE,
        "why": "rows round to 32 bytes instead of 64.",
    },
    {
        "id": "d3d-surface-pitch-table-compare",
        "file": SURFACE,
        "old": "        if (row_bytes <= legal) {",
        "new": "        if (row_bytes < legal) {",
        "targets": T_SURFACE,
        "why": "a row exactly as wide as a table entry skips to the next entry.",
    },
    {
        "id": "d3d-surface-format-word-levels",
        "file": SURFACE,
        "old": "    *format_word = (1u << 16) | ((format & 0xFFu) << 8) | 0x29u;",
        "new": "    *format_word = (1u << 20) | ((format & 0xFFu) << 8) | 0x29u;",
        "targets": T_SURFACE,
        "why": ("the level count lands in the wrong nibble of the Format word the title copies."),
    },
    {
        "id": "d3d-surface-size-word-pitch",
        "file": SURFACE,
        "old": (
            "    *size_word = ((((pitch >> 6) - 1u) << 12 | (height - 1u)) << 12) | (width - 1u);"
        ),
        "new": (
            "    *size_word = ((((pitch >> 6) - 0u) << 12 | (height - 1u)) << 12) | (width - 1u);"
        ),
        "targets": T_SURFACE,
        "why": "the pitch field of the Size word is off by one.",
    },
    {
        "id": "d3d-surface-size-word-height",
        "file": SURFACE,
        "old": (
            "    *size_word = ((((pitch >> 6) - 1u) << 12 | (height - 1u)) << 12) | (width - 1u);"
        ),
        "new": (
            "    *size_word = ((((pitch >> 6) - 1u) << 12 | (height - 0u)) << 12) | (width - 1u);"
        ),
        "targets": T_SURFACE,
        "why": "the height field of the Size word is off by one.",
    },
    {
        "id": "d3d-surface-linear-bytes",
        "file": SURFACE,
        "old": "    return height * pitch;",
        "new": "    return width * pitch;",
        "targets": T_SURFACE,
        "why": "the surface is allocated for its width rather than its height.",
    },
    {
        "id": "d3d-surface-header-common",
        "file": SURFACE,
        "old": "    d3d8_guest_store32(header + D3D8_SURFACE_COMMON, 0x01050001u);",
        "new": "    d3d8_guest_store32(header + D3D8_SURFACE_COMMON, 0x01040001u);",
        "targets": T_SURFACE,
        "why": "the header's type bits name a different resource kind.",
    },
    {
        "id": "d3d-surface-header-data-mask",
        "file": SURFACE,
        "old": "    d3d8_guest_store32(header + D3D8_SURFACE_DATA, data & 0x0FFFFFFFu);",
        "new": "    d3d8_guest_store32(header + D3D8_SURFACE_DATA, data);",
        "targets": T_SURFACE,
        "why": "the physical address keeps its virtual-window bits.",
    },
    {
        "id": "d3d-surface-header-pitch-plus-one",
        "file": SURFACE,
        "old": "        return (((size_word >> 24) & 0xFFu) + 1u) << 6;",
        "new": "        return (((size_word >> 24) & 0xFFu) + 0u) << 6;",
        "targets": T_SURFACE,
        "why": "every surface pitch is 64 short.",
    },
    {
        "id": "d3d-surface-addref-parent-first",
        "file": SURFACE,
        "old": (
            "        if (parent != 0u) {\n"
            "            (void)d3d8_resource_add_ref(parent);\n"
            "        }"
        ),
        "new": (
            "        if (parent != 0u && false) {\n"
            "            (void)d3d8_resource_add_ref(parent);\n"
            "        }"
        ),
        "targets": T_SURFACE,
        "why": ("a surface child can be released while its parent texture is left unreferenced."),
    },
    {
        "id": "d3d-surface-render-target-increment",
        "file": SURFACE,
        "old": "    d3d8_guest_store32(header + D3D8_SURFACE_COMMON, common + 0x80000u);",
        "new": "    d3d8_guest_store32(header + D3D8_SURFACE_COMMON, common + 0x40000u);",
        "targets": T_SURFACE,
        "why": ("the render-target reference lands in the type bits, which the title copies."),
    },
    {
        "id": "d3d-surface-render-target-parent-once",
        "file": SURFACE,
        "old": (
            "    if ((common & 0x780000u) == 0u && (common & 0x70000u) == 0x50000u) "
            "{\n"
            "        const uint32_t parent = d3d8_guest_load32(header + "
            "D3D8_SURFACE_PARENT);\n"
            "        if (parent != 0u) {\n"
            "            d3d8_guest_store32(parent"
        ),
        "new": (
            "    if ((common & 0x70000u) == 0x50000u) {\n"
            "        const uint32_t parent = d3d8_guest_load32(header + "
            "D3D8_SURFACE_PARENT);\n"
            "        if (parent != 0u) {\n"
            "            d3d8_guest_store32(parent"
        ),
        "targets": T_SURFACE,
        "why": ("the parent gains a render-target reference on every call, not only the first."),
    },
    {
        "id": "d3d-surface-format-word-linear-bit",
        "file": SURFACE,
        "old": ("        word |= 0x100u;\n    }"),
        "new": ("        word |= 0x80u;\n    }"),
        "targets": T_SURFACE,
        "why": "the linear marker of the surface-format word moves.",
    },
    {
        "id": "d3d-surface-format-word-depth-24",
        "file": SURFACE,
        "old": ("    return word | (depth_is_16bit[depth_format - 0x2Au] != 0u ? 0x10u : 0x20u);"),
        "new": ("    return word | (depth_is_16bit[depth_format - 0x2Au] != 0u ? 0x20u : 0x10u);"),
        "targets": T_SURFACE,
        "why": "24-bit and 16-bit depth swap their surface-format bits.",
    },
    {
        "id": "d3d-surface-max-depth-24",
        "file": SURFACE,
        "old": ("    case 0x2Au:\n    case 0x2Eu:\n        return 0x4B7FFFFFu;"),
        "new": ("    case 0x2Au:\n    case 0x2Eu:\n        return 0x477FFF00u;"),
        "targets": T_SURFACE,
        "why": "a 24-bit depth buffer reports the 16-bit maximum.",
    },
    {
        "id": "d3d-surface-swizzled-fatal",
        "file": SURFACE,
        "old": "    if (format_is_swizzled_or_compressed(format) || pitch == 0u) {",
        "new": ("    if ((format_is_swizzled_or_compressed(format) || pitch == 0u) && false) {"),
        "targets": T_SURFACE,
        "why": "a swizzled or compressed format is answered with a linear guess.",
    },
    {
        "id": "d3d-pushbuffer-limit-slack",
        "file": PUSHBUFFER,
        "old": (
            "    d3d8_device_store32(D3D8_DEV_LIMIT, base + (kickoff & ~3u) - "
            "D3D8_PUSHBUFFER_SLACK_BYTES);"
        ),
        "new": "    d3d8_device_store32(D3D8_DEV_LIMIT, base + (kickoff & ~3u));",
        "targets": T_PUSH,
        "why": (
            "writers that check the limit once and then write up to 0x204 bytes run "
            "past the segment."
        ),
    },
    {
        "id": "d3d-pushbuffer-end-field",
        "file": PUSHBUFFER,
        "old": "    d3d8_device_store32(D3D8_DEV_PB_END, base + (size & ~3u));",
        "new": "    d3d8_device_store32(D3D8_DEV_PB_END, base + (kickoff & ~3u));",
        "targets": T_PUSH,
        "why": "the ring is only one kickoff long.",
    },
    {
        "id": "d3d-pushbuffer-pair-order",
        "file": PUSHBUFFER,
        "old": (
            "        d3d8_guest_store32(advanced - 8u, header);\n"
            "        d3d8_guest_store32(advanced - 4u, value);"
        ),
        "new": (
            "        d3d8_guest_store32(advanced - 8u, value);\n"
            "        d3d8_guest_store32(advanced - 4u, header);"
        ),
        "targets": T_PUSH,
        "why": ("every render-state pair the title pushes has its header and value exchanged."),
    },
    {
        "id": "d3d-pushbuffer-pair-width",
        "file": PUSHBUFFER,
        "old": ("        const uint32_t advanced = d3d8_device_load32(D3D8_DEV_CURSOR) + 8u;"),
        "new": ("        const uint32_t advanced = d3d8_device_load32(D3D8_DEV_CURSOR) + 4u;"),
        "targets": T_PUSH,
        "why": "the cursor advances half a pair.",
    },
    {
        "id": "d3d-pushbuffer-rollover-consumer",
        "file": PUSHBUFFER,
        "old": "    rollover_count++;\n    if (consumer_fn != NULL && cursor > segment_start) {",
        "new": (
            "    rollover_count++;\n"
            "    if (consumer_fn != NULL && cursor > segment_start && false) {"
        ),
        "targets": T_PUSH,
        "why": "the hand-off hook never fires, so a consumer sees no commands.",
    },
    {
        "id": "d3d-pushbuffer-segment-length",
        "file": PUSHBUFFER,
        "old": "    uint32_t candidate = cursor + kickoff;",
        "new": "    uint32_t candidate = cursor + (kickoff >> 1);",
        "targets": T_PUSH,
        "why": "each segment after a roll-over is half as long as the original's.",
    },
    {
        "id": "d3d-pushbuffer-wrap-margin",
        "file": PUSHBUFFER,
        "old": "    if (candidate + D3D8_PUSHBUFFER_WRAP_MARGIN >= end) {",
        "new": "    if (candidate >= end) {",
        "targets": T_PUSH,
        "why": "the ring is walked to its very end before it wraps.",
    },
    {
        "id": "d3d-pushbuffer-wrap-decision",
        "file": PUSHBUFFER,
        "old": "        if (cursor + half > end) {",
        "new": "        if (cursor + half > end + 0x10000u) {",
        "targets": T_PUSH,
        "why": ("the ring never wraps and clamps forever, so a long run writes past the buffer."),
    },
    {
        "id": "d3d-pushbuffer-jump-word",
        "file": PUSHBUFFER,
        "old": "        d3d8_guest_store32(cursor, (base & 0x0FFFFFFFu) + 1u);",
        "new": "        d3d8_guest_store32(cursor, (base & 0x0FFFFFFFu));",
        "targets": T_PUSH,
        "why": "the ring-jump dword lacks the jump bit.",
    },
    {
        "id": "d3d-pushbuffer-wrap-delta",
        "file": PUSHBUFFER,
        "old": "        d3d8_device_store32(D3D8_DEV_WRAP_DELTA, cursor - base);",
        "new": "        d3d8_device_store32(D3D8_DEV_WRAP_DELTA, 0u);",
        "targets": T_PUSH,
        "why": "the distance the cursor had reached at the wrap is lost.",
    },
    {
        "id": "d3d-pushbuffer-wrap-count",
        "file": PUSHBUFFER,
        "old": (
            "        d3d8_device_store32(D3D8_DEV_WRAP_COUNT, "
            "d3d8_device_load32(D3D8_DEV_WRAP_COUNT) + 1u);"
        ),
        "new": "        d3d8_device_store32(D3D8_DEV_WRAP_COUNT, 1u);",
        "targets": T_PUSH,
        "why": "the wrap counter never counts past one.",
    },
    {
        "id": "d3d-pushbuffer-begin-rolls",
        "file": PUSHBUFFER,
        "old": (
            "    if (cursor >= d3d8_device_load32(D3D8_DEV_LIMIT)) {\n"
            "        roll_over();\n"
            "        cursor = d3d8_device_load32(D3D8_DEV_CURSOR);"
        ),
        "new": (
            "    if (cursor >= d3d8_device_load32(D3D8_DEV_LIMIT) && false) {\n"
            "        roll_over();\n"
            "        cursor = d3d8_device_load32(D3D8_DEV_CURSOR);"
        ),
        "targets": T_PUSH,
        "why": "an emitter that checks the limit once writes past it.",
    },
    {
        "id": "d3d-pushbuffer-end-counts",
        "file": PUSHBUFFER,
        "old": "        dwords_written += (cursor - before) / 4u;",
        "new": "        dwords_written += (cursor - before) / 8u;",
        "targets": T_PUSH,
        "why": "the dword counter under-reports every direct writer.",
    },
    {
        "id": "d3d-pushbuffer-protect",
        "file": PUSHBUFFER,
        "old": "#define PUSHBUFFER_PROTECT 0x404u",
        "new": "#define PUSHBUFFER_PROTECT 0x4u",
        "targets": T_PUSH,
        "why": ("the ring loses its write-combine attribute, which is how the original maps it."),
    },
    {
        "id": "d3d-pushbuffer-use-before-create-fatal",
        "file": PUSHBUFFER,
        "old": (
            "    if (d3d8_device_load32(D3D8_DEV_PB_BASE) == 0u) {\n"
            "        d3d8_hle_fatal(0x003D6B20u,"
        ),
        "new": (
            "    if (d3d8_device_load32(D3D8_DEV_PB_BASE) == 0u && false) {\n"
            "        d3d8_hle_fatal(0x003D6B20u,"
        ),
        "targets": T_PUSH,
        "why": "a command written before CreateDevice goes through a null cursor.",
    },
    {
        "id": "d3d-device-handler-create-returns",
        "file": DEVICE,
        "old": ("    (void)context;\n    return 1u;"),
        "new": ("    (void)context;\n    return 0u;"),
        "targets": T_INIT,
        "why": ("Direct3DCreate8 returns 0, which the title stores as the interface pointer."),
    },
    {
        "id": "d3d-device-set-push-buffer-order",
        "file": DEVICE,
        "old": (
            "    d3d8_guest_store32(D3D8_GLOBAL_PUSHBUFFER_SIZE, size);\n"
            "    d3d8_guest_store32(D3D8_GLOBAL_KICKOFF_SIZE, kickoff);"
        ),
        "new": (
            "    d3d8_guest_store32(D3D8_GLOBAL_PUSHBUFFER_SIZE, kickoff);\n"
            "    d3d8_guest_store32(D3D8_GLOBAL_KICKOFF_SIZE, size);"
        ),
        "targets": T_INIT,
        "why": "the pushbuffer is 64 KiB with a 1 MiB kickoff.",
    },
    {
        "id": "d3d-device-set-push-buffer-return",
        "file": DEVICE,
        "old": ("    return size;\n}"),
        "new": ("    return 0u;\n}"),
        "targets": T_INIT,
        "why": "the original leaves arg0 in eax.",
    },
    {
        "id": "d3d-device-default-sizes",
        "file": DEVICE,
        "old": "        d3d8_guest_store32(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x80000u);",
        "new": "        d3d8_guest_store32(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x40000u);",
        "targets": T_INIT,
        "why": ("a title that never calls SetPushBufferSize gets half the original's ring."),
    },
    {
        "id": "d3d-device-behavior-mask",
        "file": DEVICE,
        "old": "d3d8_device_load32(D3D8_DEV_FLAGS) | (behavior_flags & 0x10u));",
        "new": "d3d8_device_load32(D3D8_DEV_FLAGS) | (behavior_flags & 0x1Fu));",
        "targets": T_INIT,
        "why": ("behaviour bits other than the pure-device bit leak into the device flags."),
    },
    {
        "id": "d3d-device-pointer-slot",
        "file": DEVICE,
        "old": (
            "    d3d8_guest_store32(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);\n"
            "    d3d8_guest_store32(GLOBAL_DEVICE_CREATED, 1u);"
        ),
        "new": (
            "    d3d8_guest_store32(D3D8_DEVICE_POINTER_SLOT, 0u);\n"
            "    d3d8_guest_store32(GLOBAL_DEVICE_CREATED, 1u);"
        ),
        "targets": T_INIT,
        "why": "every D3D8 function loads the device through this slot.",
    },
    {
        "id": "d3d-device-out-pointer",
        "file": DEVICE,
        "old": ("        d3d8_guest_store32(out_device, D3D8_DEVICE_BASE);\n    }\n    return 0u;"),
        "new": ("        d3d8_guest_store32(out_device, 0u);\n    }\n    return 0u;"),
        "targets": T_INIT,
        "why": (
            "the out-pointer is never read by the title but is part of what the original writes."
        ),
    },
    {
        "id": "d3d-device-failure-clears",
        "file": DEVICE,
        "old": (
            "        for (uint32_t offset = 0u; offset < "
            "DEVICE_BYTES_CLEARED_ON_FAILURE; offset += 4u) {\n"
            "            d3d8_device_store32(offset, 0u);\n"
            "        }"
        ),
        "new": "",
        "targets": T_INIT,
        "why": ("a failed CreateDevice leaves a half-built device a later call would trust."),
    },
    {
        "id": "d3d-device-failure-clears-slot",
        "file": DEVICE,
        "old": (
            "        d3d8_guest_store32(D3D8_DEVICE_POINTER_SLOT, 0u);\n        return result;"
        ),
        "new": "        return result;",
        "targets": T_INIT,
        "why": "the device slot still points at a cleared device after the failure.",
    },
    {
        "id": "d3d-device-failure-sign-test",
        "file": DEVICE,
        "old": "    if ((result & 0x80000000u) != 0u) {",
        "new": "    if (result != 0u && false) {",
        "targets": T_INIT,
        "why": "a failed creation is treated as success and its out-pointer is written.",
    },
    {
        "id": "d3d-device-dirty-mask",
        "file": DEVICE,
        "old": "#define DIRTY_AT_CREATE 0x00FF7F7Fu",
        "new": "#define DIRTY_AT_CREATE 0x00FF7F7Eu",
        "targets": T_INIT,
        "why": ("the game ORs into this word, so a missing bit is never noticed by the title."),
    },
    {
        "id": "d3d-device-dirty-mask-or",
        "file": DEVICE,
        "old": "d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | DIRTY_AT_CREATE);",
        "new": "DIRTY_AT_CREATE);",
        "targets": T_INIT,
        "why": "bits the guest set before CreateDevice are discarded.",
    },
    {
        "id": "d3d-device-register-base",
        "file": "src/gpu/d3d8_device.h",
        "old": "#define D3D8_NV2A_REGISTER_BASE 0xFD000000u",
        "new": "#define D3D8_NV2A_REGISTER_BASE 0xFD800000u",
        "targets": T_INIT,
        "why": ("the register window moves, and every later AvSendTVEncoderOption is handed it."),
    },
    {
        "id": "d3d-device-front-pitch-formula",
        "file": DEVICE,
        "old": (
            "    const uint32_t front_pitch = d3d8_surface_aligned_row_bytes(width, back_format);"
        ),
        "new": (
            "    const uint32_t front_pitch = d3d8_surface_pitch_for_width(width, "
            "back_format) + 0x40u;"
        ),
        "targets": T_INIT,
        "why": "the front buffer gets a different pitch from the back buffer.",
    },
    {
        "id": "d3d-device-back-allocation-store",
        "file": DEVICE,
        "old": "    d3d8_device_store32(DEV_BACK_ALLOCATION, allocation);",
        "new": "    d3d8_device_store32(DEV_BACK_ALLOCATION, 0u);",
        "targets": T_INIT,
        "why": "the library's record of the back buffer's memory is lost.",
    },
    {
        "id": "d3d-device-surface-list",
        "file": DEVICE,
        "old": "    d3d8_device_store32(DEV_SURFACE_LIST, back_header);",
        "new": ("    d3d8_device_store32(DEV_SURFACE_LIST, D3D8_DEVICE_BASE + DEV_FRONT_HEADER);"),
        "targets": T_INIT,
        "why": "GetBackBuffer2 hands out the wrong header.",
    },
    {
        "id": "d3d-device-front-list",
        "file": DEVICE,
        "old": "    d3d8_device_store32(DEV_FRONT_LIST, front_header);",
        "new": "    d3d8_device_store32(DEV_FRONT_LIST, 0u);",
        "targets": T_INIT,
        "why": "GetBackBuffer2(-1) finds no front buffer.",
    },
    {
        "id": "d3d-device-depth-optional",
        "file": DEVICE,
        "old": "    if (request->auto_depth_stencil != 0u) {",
        "new": "    if (request->auto_depth_stencil != 0u && false) {",
        "targets": T_INIT,
        "why": ("no depth buffer is created, and the title reads a null depth surface header."),
    },
    {
        "id": "d3d-device-release-references",
        "file": DEVICE,
        "old": (
            "        (void)d3d8_resource_add_ref(d3d8_device_load32(DEV_SURFACE_LIST + slot * 4u));"
        ),
        "new": "        (void)slot;",
        "targets": T_INIT,
        "why": "the surfaces keep their birth reference count, which the title copies.",
    },
    {
        "id": "d3d-device-depth-reference",
        "file": DEVICE,
        "old": (
            "    if (depth_slot != 0u) {\n        (void)d3d8_resource_add_ref(depth_slot);\n    }"
        ),
        "new": "    (void)depth_slot;",
        "targets": T_INIT,
        "why": "the depth buffer's reference count is one short.",
    },
    {
        "id": "d3d-device-bind-target-reference",
        "file": BIND,
        "old": ("    d3d8_resource_add_render_target_ref(target);"),
        "new": "    (void)target;",
        "targets": T_INIT,
        "why": (
            "the back buffer lacks its render-target reference, which the title "
            "copies in its header."
        ),
    },
    {
        "id": "d3d-device-bind-depth-reference",
        "file": BIND,
        "old": (
            "        d3d8_resource_add_render_target_ref(depth);\n"
            "        const uint32_t depth_format ="
        ),
        "new": "        const uint32_t depth_format =",
        "targets": T_INIT,
        "why": "the depth buffer lacks its render-target reference.",
    },
    {
        "id": "d3d-device-depth-max",
        "file": BIND,
        "old": (
            "        d3d8_device_store32(DEV_DEPTH_MAX, d3d8_surface_max_depth_bits(depth_format));"
        ),
        "new": (
            "        d3d8_device_store32(DEV_DEPTH_MAX, "
            "d3d8_surface_max_depth_bits(depth_format) & 0u);"
        ),
        "targets": T_INIT,
        "why": "the depth range the viewport code scales by is zero.",
    },
    {
        "id": "d3d-device-surface-format-word",
        "file": BIND,
        "old": (
            "    d3d8_device_store32(DEV_SURFACE_FORMAT, d3d8_surface_format_word(target, depth));"
        ),
        "new": "    d3d8_device_store32(DEV_SURFACE_FORMAT, 0u);",
        "targets": T_INIT,
        "why": "every later Clear and SetRenderTarget would emit surface format 0.",
    },
    {
        "id": "d3d-device-match-pitch-source",
        "file": DEVICE,
        "old": ("        .pitch = d3d8_surface_header_pitch(d3d8_device_load32(DEV_FRONT_LIST)),"),
        "new": "        .pitch = 0u,",
        "targets": T_INIT,
        "why": "the display object records no pitch.",
    },
    {
        "id": "d3d-device-filter-values",
        "file": DEVICE,
        "old": ("    d3d8_set_flicker_filter(5u);\n    d3d8_set_soft_display_filter(0u);"),
        "new": ("    d3d8_set_flicker_filter(4u);\n    d3d8_set_soft_display_filter(1u);"),
        "targets": T_INIT,
        "why": (
            "the flicker and soft-display filters are configured to values the "
            "original does not choose."
        ),
    },
    {
        "id": "d3d-device-flicker-cache",
        "file": DEVICE,
        "old": (
            "    d3d8_guest_store32(GLOBAL_FLICKER_CACHED, 1u);\n"
            "    d3d8_guest_store32(GLOBAL_FLICKER_VALUE, value);"
        ),
        "new": "    d3d8_guest_store32(GLOBAL_FLICKER_VALUE, value);",
        "targets": T_INIT,
        "why": (
            "a later SetFlickerFilter with the same value is sent again because the "
            "cache never arms."
        ),
    },
    {
        "id": "d3d-device-soft-filter-enabled-bool",
        "file": DEVICE,
        "old": "    const uint32_t enabled = value != 0u ? 1u : 0u;",
        "new": "    const uint32_t enabled = value;",
        "targets": T_INIT,
        "why": ("the soft-display cache stores the raw value and a later 0 or 2 compares wrongly."),
    },
    {
        "id": "d3d-device-get-back-buffer-front",
        "file": DEVICE,
        "old": (
            "    uint32_t slot = 1u;\n    if (index != -1) {\n        slot = index != 0 ? 2u : 0u;"
        ),
        "new": (
            "    uint32_t slot = 0u;\n    if (index != -1) {\n        slot = index != 0 ? 2u : 1u;"
        ),
        "targets": T_INIT,
        "why": "the front and back buffers are exchanged.",
    },
    {
        "id": "d3d-device-get-back-buffer-addref",
        "file": DEVICE,
        "old": ("    (void)d3d8_resource_add_ref(header);\n    return header;"),
        "new": "    return header;",
        "targets": T_INIT,
        "why": ("GetBackBuffer2 hands out a surface without the reference the title will release."),
    },
    {
        "id": "d3d-device-primitive-register-order",
        "file": DEVICE,
        "old": (
            "    const uint32_t header = frame_register(context, 0u, 0x003D6C90u);\n"
            "    const uint32_t value = frame_register(context, 1u, 0x003D6C90u);"
        ),
        "new": (
            "    const uint32_t header = frame_register(context, 1u, 0x003D6C90u);\n"
            "    const uint32_t value = frame_register(context, 0u, 0x003D6C90u);"
        ),
        "targets": T_INIT,
        "why": "ecx and edx are exchanged, so every pair is written back to front.",
    },
    {
        "id": "d3d-device-create-device-argument",
        "file": DEVICE,
        "old": ("    const uint32_t parameters = frame_argument(context, 4u, 0x003D9230u);"),
        "new": ("    const uint32_t parameters = frame_argument(context, 3u, 0x003D9230u);"),
        "targets": T_INIT,
        "why": "the presentation parameters are read from the behaviour-flags slot.",
    },
    {
        "id": "d3d-device-mode-support-fatal",
        "file": DEVICE,
        "old": "    if (request->swap_effect != SWAP_EFFECT_COPY) {",
        "new": "    if (request->swap_effect != SWAP_EFFECT_COPY && false) {",
        "targets": T_INIT,
        "why": "an unported swap effect runs the title's path anyway.",
    },
    {
        "id": "d3d-device-multisample-fatal",
        "file": DEVICE,
        "old": (
            "    if (request->multisample != 0u && request->multisample != MULTISAMPLE_NONE) {"
        ),
        "new": (
            "    if (request->multisample != 0u && request->multisample != "
            "MULTISAMPLE_NONE && false) {"
        ),
        "targets": T_INIT,
        "why": "a multisampled request is built as if it were not.",
    },
    {
        "id": "d3d-state-93-address",
        "file": STATE,
        "old": "void d3d8_state_set_93(uint32_t value)\n{\n    store_state(D3D8_STATE_93, value);",
        "new": "void d3d8_state_set_93(uint32_t value)\n{\n    store_state(D3D8_STATE_94, value);",
        "targets": T_STATE,
        "why": "two state globals swap, and neither is read by the game.",
    },
    {
        "id": "d3d-state-a3-updates-control-words",
        "file": STATE,
        "old": ("    store_state(D3D8_STATE_A3, value);\n    d3d8_state_update_control_words();"),
        "new": "    store_state(D3D8_STATE_A3, value);",
        "targets": T_STATE,
        "why": "the control words stay stale after state 0xA3 changes.",
    },
    {
        "id": "d3d-state-control-mask-a",
        "file": STATE,
        "old": ("    uint32_t word_a = d3d8_device_load32(DEV_CONTROL_WORD_A) & 0xFFFFFFF7u;"),
        "new": ("    uint32_t word_a = d3d8_device_load32(DEV_CONTROL_WORD_A) & 0xFFFFFFFFu;"),
        "targets": T_STATE,
        "why": "bit 3 is never cleared when its source goes to zero.",
    },
    {
        "id": "d3d-state-control-mask-b",
        "file": STATE,
        "old": ("    uint32_t word_b = d3d8_device_load32(DEV_CONTROL_WORD_B) & 0xE7EFFFFFu;"),
        "new": ("    uint32_t word_b = d3d8_device_load32(DEV_CONTROL_WORD_B) & 0xE7FFFFFFu;"),
        "targets": T_STATE,
        "why": "bit 20 is never cleared when its source goes to zero.",
    },
    {
        "id": "d3d-state-control-bit-a4",
        "file": STATE,
        "old": "        word_b |= 0x08000000u;",
        "new": "        word_b |= 0x04000000u;",
        "targets": T_STATE,
        "why": "state 0xA4 sets the wrong bit of the control word.",
    },
    # d3d-state-9a-condition is gone (T598): the title handler no longer announces. The render
    # target test it mutated is d3d8_state_library_set_9a's, mutated by t533-state-9a-condition
    # and run through the title handler by test_d3d8_state and the t598 oracle cases.
    {
        "id": "d3d-state-8f-leave-announced",
        "file": STATE,
        "old": "    if (previous == 2u || value == 2u) {",
        "new": "    if (value == 2u || (previous == 2u && false)) {",
        "targets": T_STATE,
        "why": "leaving value 2 is not announced.",
    },
    {
        "id": "d3d-state-95-negate",
        "file": STATE,
        "old": "    const long double negated = -widened;\n" + HLE_95_TAIL,
        "new": "    const long double negated = widened;\n" + HLE_95_TAIL,
        "targets": T_STATE,
        "why": "the depth bias keeps its sign.",
    },
    {
        "id": "d3d-state-95-unsigned-widening",
        "file": STATE,
        "old": HLE_95_WIDENING + "    if ((int32_t)value < 0) {\n        widened +=",
        "new": HLE_95_WIDENING + "    if ((int32_t)value < 0 && false) {\n        widened +=",
        "targets": T_STATE,
        "why": "a value with the sign bit set is read as signed.",
    },
    {
        "id": "d3d-state-95-flags",
        "file": STATE,
        "old": (
            "    const uint32_t flag = value != 0u ? 1u : 0u;\n\n    const uint32_t shadow_values"
        ),
        "new": "    const uint32_t flag = value;\n\n    const uint32_t shadow_values",
        "targets": T_STATE,
        "why": "the three flag states take the raw value.",
    },
    {
        "id": "d3d-state-95-shadow-order",
        "file": STATE,
        "old": (
            "    const uint32_t shadow_values[5] = {bits_of_float(scaled_float), "
            "bits_of_float(negated_float),"
        ),
        "new": (
            "    const uint32_t shadow_values[5] = {bits_of_float(negated_float), "
            "bits_of_float(scaled_float),"
        ),
        "targets": T_STATE,
        "why": "the scaled and unscaled floats land in each other's render states.",
    },
    {
        "id": "d3d-state-95-scale",
        "file": STATE,
        "old": "#define CONSTANT_QUARTER 0x00475D4Cu",
        "new": "#define CONSTANT_QUARTER 0x00475CD4u",
        "targets": T_STATE,
        "why": "the scale constant is read from the neighbouring float (0.5).",
    },
    {
        "id": "d3d-state-constant-mode-bit",
        "file": STATE,
        "old": "    const uint32_t flags = (old_flags & 0xFFFFFDFFu) | ((mode & 0x10u) << 5u);",
        "new": "    const uint32_t flags = (old_flags & 0xFFFFFDFFu) | ((mode & 0x10u) << 4u);",
        "targets": T_STATE,
        "why": "the shader-constant mode sets the wrong device flag.",
    },
    {
        "id": "d3d-state-constant-mode-clear",
        "file": STATE,
        "old": "    const uint32_t flags = (old_flags & 0xFFFFFDFFu) | ((mode & 0x10u) << 5u);",
        "new": "    const uint32_t flags = (old_flags & 0xFFFFFFFFu) | ((mode & 0x10u) << 5u);",
        "targets": T_STATE,
        "why": "mode 0 never clears the flag.",
    },
    {
        "id": "d3d-state-constant-mode-stored",
        "file": STATE,
        "old": "    d3d8_guest_store32(device + DEV_CONSTANT_MODE, masked);",
        "new": "    d3d8_guest_store32(device + DEV_CONSTANT_MODE, mode);",
        "targets": T_STATE,
        "why": "the stored mode keeps bit 4.",
    },
    {
        "id": "d3d-state-constant-mode-early-return",
        "file": STATE,
        "old": ("    if (masked != 0u) {\n        return masked;\n    }"),
        "new": "",
        "targets": T_STATE,
        "why": "the title's mode 0x11 also marks the constant groups dirty.",
    },
    {
        "id": "d3d-state-constant-mode-dirty",
        "file": STATE,
        "old": ("                       d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | 0x1600u);"),
        "new": ("                       d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | 0x1200u);"),
        "targets": T_STATE,
        "why": "the dirty bits for the constant groups are wrong.",
    },
    {
        "id": "d3d-state-constant-shift",
        "file": STATE,
        "old": "    const uint32_t shadow = D3D8_CONSTANT_SHADOW + (index << 4);",
        "new": "    const uint32_t shadow = D3D8_CONSTANT_SHADOW + (index << 3);",
        "targets": T_STATE,
        "why": "constants overlap each other in the shadow.",
    },
    {
        "id": "d3d-state-constant-bound",
        "file": STATE,
        "old": "    if (index >= D3D8_CONSTANT_REGISTERS) {",
        "new": "    if (index > D3D8_CONSTANT_REGISTERS) {",
        "targets": T_STATE,
        "why": "register 192 is written one slot past the shadow.",
    },
    {
        "id": "d3d-state-constant-register-args",
        "file": STATE,
        "old": (
            "    if (!kernel_frame_reg_arg((const kernel_call_frame *)context, 0u, "
            "&index) ||\n"
            "        !kernel_frame_reg_arg((const kernel_call_frame *)context, 1u, "
            "&source)) {"
        ),
        "new": (
            "    if (!kernel_frame_reg_arg((const kernel_call_frame *)context, 1u, "
            "&index) ||\n"
            "        !kernel_frame_reg_arg((const kernel_call_frame *)context, 0u, "
            "&source)) {"
        ),
        "targets": T_STATE,
        "why": "ecx and edx are exchanged: the register number is read as a pointer.",
    },
    {
        "id": "d3d-resource-common",
        "file": RESOURCE,
        "old": "    d3d8_guest_store32(header, D3D8_BUFFER_COMMON);",
        "new": "    d3d8_guest_store32(header, 0u);",
        "targets": ["test_d3d8_resource"],
        "why": "the buffer header carries no reference count and no type.",
    },
    {
        "id": "d3d-resource-registry-lookup",
        "file": RESOURCE,
        "old": "        if (registry[index].physical == physical &&",
        "new": "        if (registry[index].physical != physical &&",
        "targets": ["test_d3d8_resource"],
        "why": "Lock finds the wrong buffer or none.",
    },
    {
        "id": "d3d-resource-failed-data-releases-header",
        "file": RESOURCE,
        "old": ("        (void)guest_heap_free(header_heap, header);\n        return 0u;"),
        "new": "        return header;",
        "targets": ["test_d3d8_resource"],
        "why": "a buffer whose data allocation failed is handed to the title anyway.",
    },
    {
        "id": "d3d-frame-nonblack-flags",
        "file": FRAME,
        "old": "    if ((call->flags & 0xF0u) != 0u && call->color != 0u) {",
        "new": "    if (call->color != 0u) {",
        "targets": ["test_d3d8_frame"],
        "why": ("a depth-only clear with a stale colour word is counted as a colour clear."),
    },
    {
        "id": "d3d-frame-clear-argument-order",
        "file": FRAME,
        "old": (
            "    const d3d8_clear_call call = {words[0], words[1], words[2], "
            "words[3], words[4], words[5]};"
        ),
        "new": (
            "    const d3d8_clear_call call = {words[0], words[1], words[3], "
            "words[2], words[4], words[5]};"
        ),
        "targets": ["test_d3d8_frame"],
        "why": "Flags and Color are read from each other's slot.",
    },
    {
        "id": "d3d-hle-note-dedupe",
        "file": HLE,
        "old": (
            "        if (unmodelled_seen[index].address == guest_address &&\n"
            "            unmodelled_seen[index].what == what) {"
        ),
        "new": "        if (unmodelled_seen[index].address == guest_address) {",
        "targets": ["test_d3d8_hle"],
        "why": "a second omission at the same address is never announced.",
    },
    {
        "id": "d3d-hle-note-count-every-time",
        "file": HLE,
        "old": (
            "    unmodelled_total++;\n"
            "    for (size_t index = 0; index < unmodelled_seen_count; index++) {"
        ),
        "new": "    for (size_t index = 0; index < unmodelled_seen_count; index++) {",
        "targets": ["test_d3d8_hle"],
        "why": "the count only counts the first announcement of each pair.",
    },
    {
        "id": "d3d-hle-fatal-calls-hook",
        "file": HLE,
        "old": ("    if (fatal_handler) {\n        fatal_handler(guest_address, message);\n    }"),
        "new": "",
        "targets": ["test_d3d8_hle"],
        "why": (
            "the host's stop hook is never called and the handler aborts the process "
            "instead of ending the run at the address."
        ),
    },
]
