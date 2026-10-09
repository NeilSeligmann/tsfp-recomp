# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for `src/loader/`, the XBE parser, mapper and dump oracle (T383).

T351's inventory measured src/loader (448 lines, `xbe.c` + `xbe_dump.c`) as a
zero-set module. `xbe.c` is the trust boundary for the user's executable: every
guest address the whole recompilation believes in comes out of this parser, and
`xbe_map` is what turns untrusted file offsets into writes at fixed host addresses.
`xbe_dump.c` is the C half of the loader's cross-validation oracle; a dump that
prints the wrong field would make tests/test_integration.py compare a wrong pair
and bless it. xbe_dump.c had NO ctest reaching it before T383 (only the XBE-gated
integration pytest), so `tests/c/test_xbe_dump.c` was built first, compiling the
tool's own main into a ctest binary.

What these mutations are chosen to catch, by family:

    parse      the single little-endian reader every field flows through, the
               magic gate, the measured header offsets (certificate, thunk,
               title id), the section-header field order, and the section cap
               boundary.
    decode     the XOR key scheme that identifies the build: the retail flag,
               the debug fallback, and the deterministic answer for an entry
               that decodes under neither key.
    imports    the kernel thunk table walk: section location (including a thunk
               at a section's first byte and one in the BSS tail past raw
               data), the ordinal mask, the zero terminator and the stride.
    map        the identity mapping's refusals: double-map, EEXIST diagnosed as
               non-PIE, section raw bytes past EOF, section VA outside the
               image, header and payload placement, and the recorded length
               that unmap depends on.
    at         the guest-to-host translation guards: mapped-ness, range, the
               half-open end (a read ending exactly at the image end fits) and
               64-bit end arithmetic that a 32-bit wrap must not satisfy.
    dump       the oracle tool: exit codes that CI would key on, the field
               printed per line, the --map flag parse and the live-mapping
               probe.

Kill suites are the plain-build ctest binaries `test_xbe_loader` and
`test_xbe_dump`: no XBE, no disc. The integration pytest would also kill several
of these but only on a machine with the user's default.xbe, so it is
deliberately NOT a target (the xinput.py precedent).

EQUIVALENT MUTANTS CONSIDERED AND LEFT OUT, so nobody re-adds them:
  - `len < HEADER_MIN_SIZE` and the section-table bounds pre-check in xbe_parse:
    every field read goes through the bounds-checked `read_u32`, so dropping
    either pre-check yields the identical XBE_ERR_TRUNCATED from the first
    failing read. Defence in depth with no distinct observable.
  - `copy_cstring` without the `!= 0` NUL stop: it would copy the terminator and
    the next pool entry's bytes into the buffer, but the embedded NUL makes
    `strcmp` (the only consumer) see the same string.
  - `decode_xored` boundary arms (`>=` vs `>`, `<` vs `<=`): when the retail arm
    misses, the fallback returns the same retail decode with the same flag, so
    the boundary is observable only if the OTHER key happens to decode in-range,
    which no fixture can force honestly.
  - `xbe_map` dropping the explicit round-up of `end`: mmap and munmap both round
    a partial page up themselves, so the mapping and the release are identical.
  - `xbe_check_pie` with MAP_FIXED instead of MAP_FIXED_NOREPLACE: observable
    only in a non-PIE host, and the build enforces PIE, so no test in this tree
    can reach the difference.
  - the XBE_MAX_KERNEL_IMPORTS cap off-by-one: observable only with a 512-entry
    thunk table whose overflow writes one slot past the array, which is UB that
    may well not fault; a fixture that large pins nothing else.
  - `read_file` in xbe_dump.c accepting a short fread: a regular file only reads
    short on an I/O error, which no portable fixture can inject.

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

XBE = "src/loader/xbe.c"
DUMP = "src/loader/xbe_dump.c"
XBE_SUITE = ["test_xbe_loader"]
DUMP_SUITE = ["test_xbe_dump"]


def _xbe(mutation_id: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"xbe-{mutation_id}",
        "file": XBE,
        "old": old,
        "new": new,
        "targets": list(XBE_SUITE),
        "why": why,
    }


def _dump(mutation_id: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"xbed-{mutation_id}",
        "file": DUMP,
        "old": old,
        "new": new,
        "targets": list(DUMP_SUITE),
        "why": why,
    }


MUTATIONS: list[dict] = [
    # ------------------------------------------------------------------ parse
    _xbe(
        "read-u32-big-endian",
        "    *out = (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) |\n"
        "           ((uint32_t)data[offset + 2] << 16) | ((uint32_t)data[offset + 3] << 24);",
        "    *out = ((uint32_t)data[offset] << 24) | ((uint32_t)data[offset + 1] << 16) |\n"
        "           ((uint32_t)data[offset + 2] << 8) | (uint32_t)data[offset + 3];",
        "every header field flows through this one reader; byte-swapped, the magic "
        "'XBEH' no longer matches and a valid image is refused as not an XBE.",
    ),
    _xbe(
        "magic-unchecked",
        "    if (!read_u32(data, len, 0, &magic) || magic != XBE_MAGIC) {",
        "    if (!read_u32(data, len, 0, &magic)) {",
        "the magic is the only thing standing between this parser and an arbitrary "
        "file; unchecked, any 0x17C bytes parse as an XBE and the error the "
        "operator sees moves arbitrarily far downstream.",
    ),
    _xbe(
        "cert-field-at-wrong-offset",
        "#define OFF_CERTIFICATE_ADDR 0x118",
        "#define OFF_CERTIFICATE_ADDR 0x114",
        "0x118 is measured from the retail TSFP XBE; four bytes off reads a "
        "different field as the certificate address and the title id comes from "
        "wherever that points.",
    ),
    _xbe(
        "thunk-field-at-wrong-offset",
        "#define OFF_KERNEL_THUNK_ADDR 0x158",
        "#define OFF_KERNEL_THUNK_ADDR 0x15C",
        "the thunk address is how every kernel import is found; read from the "
        "wrong header dword it decodes to garbage under both keys and the "
        "deterministic fallback hands back a plausible-looking wrong address.",
    ),
    _xbe(
        "section-cap-off-by-one",
        "    if (count > XBE_MAX_SECTIONS) {",
        "    if (count >= XBE_MAX_SECTIONS) {",
        "the fixed table holds exactly XBE_MAX_SECTIONS entries, so a count equal "
        "to the cap is valid; refusing it rejects a well-formed image at the "
        "boundary the cap exists to accept.",
    ),
    _xbe(
        "cert-below-base-accepted",
        "    if (sec_va < base || cert_va < base) {",
        "    if (sec_va < base) {",
        "a certificate address below the base makes cert_off wrap to a huge "
        "offset; the original refuses the image as truncated, the mutant reports "
        "XBE_OK with a silently absent title id, and the two must not look alike.",
    ),
    _xbe(
        "section-raw-addr-reads-raw-size",
        "            !read_u32(data, len, off + 0x0C, &raw) ||",
        "            !read_u32(data, len, off + 0x10, &raw) ||",
        "the section header field order is the measurement; raw_addr read from "
        "the raw_size slot makes every section copy from the wrong file offset "
        "and the mapped image is plausible-looking zeros.",
    ),
    _xbe(
        "section-name-reads-raw-size",
        "            !read_u32(data, len, off + 0x14, &name_va)) {",
        "            !read_u32(data, len, off + 0x10, &name_va)) {",
        "the name pointer read from the wrong slot leaves every section nameless "
        "(the raw size is below base), so xbe_find_section stops finding .text "
        "and D3D while the parse still reports XBE_OK.",
    ),
    _xbe(
        "title-id-at-wrong-cert-offset",
        "#define CERT_OFF_TITLE_ID 0x08",
        "#define CERT_OFF_TITLE_ID 0x04",
        "the title id is the one certificate field anything reads; four bytes off "
        "it reports a different dword as the title and the image identifies as a "
        "different game.",
    ),
    # ------------------------------------------------------------------ decode
    _xbe(
        "decode-reports-retail-as-debug",
        "    uint32_t retail = raw ^ retail_key;\n"
        "    if (retail >= base && retail < base + size) {\n"
        "        if (is_retail) {\n"
        "            *is_retail = true;\n"
        "        }",
        "    uint32_t retail = raw ^ retail_key;\n"
        "    if (retail >= base && retail < base + size) {\n"
        "        if (is_retail) {\n"
        "            *is_retail = false;\n"
        "        }",
        "which key decodes identifies the build, and the retail/debug call is "
        "load-bearing for every downstream measurement that assumes the retail "
        "image; the address being right makes the wrong flag extra plausible.",
    ),
    _xbe(
        "decode-debug-key-never-tried",
        "    uint32_t debug = raw ^ debug_key;\n    if (debug >= base && debug < base + size) {",
        "    uint32_t debug = raw ^ debug_key;\n"
        "    if (debug >= base && debug < base + size && false) {",
        "a debug image's entry point then falls through to the deterministic "
        "retail fallback: a garbage address presented with the same confidence as "
        "a real one, and is_retail says true for a debug build.",
    ),
    _xbe(
        "decode-fallback-prefers-debug",
        "    if (is_retail) {\n        *is_retail = true;\n    }\n    return retail;\n}",
        "    if (is_retail) {\n        *is_retail = true;\n    }\n    return debug;\n}",
        "the fallback's contract is a DETERMINISTIC answer for a malformed image: "
        "the retail decode, flagged retail. Returning the debug decode while "
        "flagging retail makes the flag and the address disagree about which key "
        "was applied.",
    ),
    # ------------------------------------------------------------------ imports
    _xbe(
        "imports-section-start-excluded",
        "        if (va >= section->virtual_addr &&\n"
        "            va < section->virtual_addr + section->virtual_size) {",
        "        if (va > section->virtual_addr &&\n"
        "            va < section->virtual_addr + section->virtual_size) {",
        "a thunk table at a section's first byte is a legal layout; excluded, the "
        "table is never located and the image reports zero kernel imports, which "
        "reads as a statically-linked title rather than as a loader bug.",
    ),
    _xbe(
        "imports-located-in-the-bss-tail",
        "            if (delta < section->raw_size) {",
        "            if (delta < section->virtual_size) {",
        "a thunk VA inside virtual_size but past raw_size points at zero-filled "
        "BSS with no file bytes behind it; locating it anyway reads whatever the "
        "file holds at that offset and presents it as the import table.",
    ),
    _xbe(
        "imports-take-the-high-half",
        "        out->kernel_imports[out->kernel_import_count++] = (uint16_t)(value & 0xFFFFu);",
        "        out->kernel_imports[out->kernel_import_count++] = (uint16_t)(value >> 16);",
        "each entry is ordinal | 0x80000000, so the ordinal is the LOW half; the "
        "high half is 0x8000 for every entry and the import list collapses to one "
        "repeated nonsense ordinal.",
    ),
    _xbe(
        "imports-ignore-the-terminator",
        "        if (!read_u32(data, len, offset, &value) || value == 0) {",
        "        if (!read_u32(data, len, offset, &value)) {",
        "the table is NUL-terminated, not length-prefixed; without the stop the "
        "walk continues into whatever follows the table and the import count "
        "inflates with ordinals nothing imports.",
    ),
    _xbe(
        "imports-stride-eight",
        "        offset += 4;",
        "        offset += 8;",
        "entries are four bytes; striding eight reports every other import, and "
        "a half-length import list under-reports the kernel surface the backlog "
        "is measured against.",
    ),
    # ------------------------------------------------------------------ map
    _xbe(
        "map-remap-allowed",
        "    if (!image || !data || image->mapped) {",
        "    if (!image || !data) {",
        "mapping an already-mapped image must be refused as the caller bug it is; "
        "attempted anyway it fails EEXIST against its own mapping and is "
        "misreported as a non-PIE host.",
    ),
    _xbe(
        "map-eexist-reported-generic",
        "        return (errno == EEXIST) ? XBE_ERR_NOT_PIE : XBE_ERR_MAP_FAILED;",
        "        return XBE_ERR_MAP_FAILED;",
        "EEXIST at 0x10000 almost always means a non-PIE host sitting inside the "
        "guest range; the specific diagnosis is what tells an operator to fix the "
        "build rather than hunt a phantom mmap failure.",
    ),
    _xbe(
        "map-section-tail-unchecked",
        "        if ((size_t)section->raw_addr + section->raw_size > len) {",
        "        if (false && (size_t)section->raw_addr + section->raw_size > len) {",
        "section raw offsets are untrusted input; unchecked, a section whose bytes "
        "lie past EOF memcpys from beyond the caller's buffer into guest memory "
        "and reports XBE_OK.",
    ),
    _xbe(
        "map-section-va-unchecked",
        "        if (!xbe_contains(image, section->virtual_addr)) {",
        "        if (false && !xbe_contains(image, section->virtual_addr)) {",
        "a section VA outside the image writes outside the mapping this function "
        "just created, at whatever host address the guest pointer zero-extends "
        "to; the refusal is the only thing making that impossible.",
    ),
    _xbe(
        "map-headers-not-copied",
        "    memcpy((void *)(uintptr_t)image->base_address, data, header_bytes);",
        "    (void)header_bytes;",
        "the guest reads its own certificate and section table from the mapped "
        "headers; without them the mapping starts with a zero page where the "
        "magic should be.",
    ),
    _xbe(
        "map-sections-land-at-base",
        "        memcpy((void *)(uintptr_t)section->virtual_addr, data + section->raw_addr,\n"
        "               section->raw_size);",
        "        memcpy((void *)(uintptr_t)image->base_address, data + section->raw_addr,\n"
        "               section->raw_size);",
        "every section copied to the base overwrites the headers with whichever "
        "section comes last and leaves the real section addresses zero-filled; "
        "the map still returns XBE_OK.",
    ),
    _xbe(
        "map-length-not-recorded",
        "    image->mapped = true;\n    image->mapped_length = length;",
        "    image->mapped = true;\n    image->mapped_length = 0u;",
        "unmap releases exactly mapped_length bytes; recorded as zero, the unmap "
        "is a no-op and the guest range stays occupied, so the next load of an "
        "image at the same base fails as if the host were non-PIE.",
    ),
    _xbe(
        "unmap-keeps-the-flag",
        "    munmap((void *)start, image->mapped_length);\n    image->mapped = false;",
        "    munmap((void *)start, image->mapped_length);",
        "an image that stays flagged mapped lets xbe_at keep handing out pointers "
        "into a range munmap just released, which faults arbitrarily later in "
        "whoever trusted the translation.",
    ),
    # ------------------------------------------------------------------ lookup
    _xbe(
        "find-section-first-mismatch",
        "        if (strcmp(image->sections[i].name, name) == 0) {",
        "        if (strcmp(image->sections[i].name, name) != 0) {",
        "an inverted match returns the first section whose name DIFFERS, so "
        "lookups for .text hand back D3D with full confidence and a lookup for a "
        "absent name finds something.",
    ),
    _xbe(
        "contains-excludes-the-base",
        "    return addr >= image->base_address &&\n"
        "           addr < image->base_address + image->size_of_image;",
        "    return addr > image->base_address &&\n"
        "           addr < image->base_address + image->size_of_image;",
        "the base address is the first valid guest byte (the XBE magic lives "
        "there); excluding it refuses the one address every header read starts "
        "from.",
    ),
    _xbe(
        "contains-includes-the-end",
        "    return addr >= image->base_address &&\n"
        "           addr < image->base_address + image->size_of_image;",
        "    return addr >= image->base_address &&\n"
        "           addr <= image->base_address + image->size_of_image;",
        "the range is half-open; including base+size blesses the first byte past "
        "the image, and xbe_map's section-VA refusal quietly weakens with it "
        "because it asks this function.",
    ),
    # ------------------------------------------------------------------ at
    _xbe(
        "at-ignores-mapped",
        "    if (!image || !image->mapped) {\n"
        "        return NULL;\n"
        "    }\n"
        "    if (!xbe_contains(image, addr)) {",
        "    if (!image) {\n        return NULL;\n    }\n    if (!xbe_contains(image, addr)) {",
        "before xbe_map the guest range is unmapped host memory; a translation "
        "that answers anyway returns a pointer whose first dereference faults, "
        "from a function whose contract is exactly to prevent that.",
    ),
    _xbe(
        "at-end-exact-fit-refused",
        "    if ((uint64_t)length > limit - (uint64_t)addr) {",
        "    if ((uint64_t)length >= limit - (uint64_t)addr) {",
        "the bound is half-open: a read ending exactly at the image end fits. "
        "Off by one here refuses the last dword of the image, which is precisely "
        "where a trailing structure ends.",
    ),
    _xbe(
        "at-end-wraps-32bit",
        "    if ((uint64_t)length > limit - (uint64_t)addr) {",
        "    if ((uint64_t)(uint32_t)length > limit - (uint64_t)addr) {",
        "the length is compared in full width; truncated to 32 bits, a length of "
        "2^32 + 4 reads as 4 and the guard blesses a read of the whole address "
        "space.",
    ),
    _xbe(
        "at-end-sum-overflows",
        "    if ((uint64_t)length > limit - (uint64_t)addr) {",
        "    if ((uint64_t)addr + length > limit) {",
        "the overflow-safe guard subtracts on the known-larger side. Adding addr "
        "and length wraps for a length near SIZE_MAX (SIZE_MAX itself lands "
        "just below the limit), so xbe_at(base, SIZE_MAX) returns a pointer.",
    ),
    # ------------------------------------------------------------------ dump
    _dump(
        "usage-exits-clean",
        '        fprintf(stderr, "usage: xbe_dump [--map] <file.xbe>\\n");\n        return 2;',
        '        fprintf(stderr, "usage: xbe_dump [--map] <file.xbe>\\n");\n        return 0;',
        "exit 2 is how a script telling this tool apart from a parse failure "
        "knows it passed no path; exiting clean makes a misspelled invocation "
        "read as a successful dump of nothing.",
    ),
    _dump(
        "parse-failure-exits-clean",
        '        fprintf(stderr, "parse failed: %s\\n", xbe_status_str(status));\n'
        "        free(data);\n"
        "        return 1;",
        '        fprintf(stderr, "parse failed: %s\\n", xbe_status_str(status));\n'
        "        free(data);\n"
        "        return 0;",
        "the integration oracle shells this tool and trusts its exit code; a "
        "clean exit on a failed parse makes an empty dump diff as 'both parsers "
        "agree' instead of as a refusal.",
    ),
    _dump(
        "map-failure-exits-clean",
        '            fprintf(stderr, "map failed: %s\\n", xbe_status_str(status));\n'
        "            free(data);\n"
        "            return 1;",
        '            fprintf(stderr, "map failed: %s\\n", xbe_status_str(status));\n'
        "            free(data);\n"
        "            return 0;",
        "--map exists to prove the mapping is live; a clean exit on a failed map "
        "reports the identity-mapping property as holding on a host where it "
        "just failed.",
    ),
    _dump(
        "entry-line-prints-the-thunk",
        '    printf("entry_point %#x\\n", image.entry_point);',
        '    printf("entry_point %#x\\n", image.kernel_thunk_addr);',
        "each line is one field by name and the cross-validation diffs them "
        "field-for-field; the wrong field under the right label is exactly the "
        "defect a dump oracle must not have.",
    ),
    _dump(
        "section-line-swaps-virtual-and-raw",
        '        printf("section %s %#x %#x %#x %#x %#x\\n", s->name, s->flags, s->virtual_addr,\n'
        "               s->virtual_size, s->raw_addr, s->raw_size);",
        '        printf("section %s %#x %#x %#x %#x %#x\\n", s->name, s->flags, s->raw_addr,\n'
        "               s->virtual_size, s->virtual_addr, s->raw_size);",
        "virtual and raw addresses are both plausible hex in each other's "
        "columns; swapped, the Python comparison would have to disagree, and a "
        "test that only counted lines would not.",
    ),
    _dump(
        "map-flag-inverted",
        '        if (strcmp(argv[i], "--map") == 0) {',
        '        if (strcmp(argv[i], "--map") != 0) {',
        "inverted, the path is taken as the flag and the flag as the path: a "
        "plain dump invocation maps nothing it was asked to and dies on usage "
        "with the file right there.",
    ),
    _dump(
        "mapped-magic-reads-the-entry",
        "        const uint32_t *magic = xbe_at(&image, image.base_address, 4);",
        "        const uint32_t *magic = xbe_at(&image, image.entry_point, 4);",
        "the mapped_magic probe proves the HEADERS landed at the base; probing "
        "the entry point instead proves a different section and the headers "
        "could be absent with the line still printed.",
    ),
]
