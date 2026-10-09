"""Mutations for the RTL and object groups: ordinals 279 and 246.

OWNED BY THE RTL/OBJECT TASK. One file per owner, so concurrent tasks cannot clobber each
other's mutation bytes -- see `tools/mutate/sets/_example.py` for the rule and for the two
traps it records.

WHAT THESE MUTATIONS ARE CHOSEN TO CATCH. Neither of these ordinals can fail loudly. A
wrong `RtlEqualString` makes `_XGetSectionHandleA@4` return the WRONG XBE section or
report a section that exists as missing, and both look like working code -- the boot would
still reach 124 kernel calls and still stop in the same place. A wrong
`ObReferenceObjectByHandle` hands the guest a status it was not given or leaves a
reference nobody can release. So every entry below is a rule whose violation produces a
PLAUSIBLE answer, not a crash.

ONE MUTATION REMAINS ABSENT: taking the reference before the out-parameter write
succeeds. The mapping probe now makes non-zero unmapped output addresses testable;
this catalog does not yet include that mutation. Its absence no longer reflects an
inability of kernel_guest_write_u32 to reject such an address.
"""

_ENTRY_LOOKUP = "handle_is_own_thread ? mutable_find_any(handle) : mutable_find(handle);"

MUTATIONS: list[dict] = [
    # ---------------------------------------------------------------- ordinal 279
    {
        "id": "rtl-equal-length-mismatch-becomes-one-sided",
        "file": "src/xbox/kernel_rtl.c",
        "old": "    if (length1 != length2) {",
        "new": "    if (length1 > length2) {",
        "targets": ["test_kernel_rtl"],
        "why": "this is the most dangerous wrong rule available and it is not obviously "
        "wrong: `>` still rejects half the mismatches, so a suite that only ever "
        "compared a long string against a short one would pass. With it, "
        "'$$XTINFO' compares EQUAL to '$$XTINFOEXTRA' and _XGetSectionHandleA@4 "
        "hands the title the wrong section handle. The boot would still reach 124 "
        "calls and still stop on XeLoadSection, so the live run cannot catch it.",
    },
    {
        "id": "rtl-equal-zero-length-still-maps-the-buffers",
        "file": "src/xbox/kernel_rtl.c",
        "old": """    if (length1 == 0u) {
        *equal = true;
        return true;
    }
""",
        "new": "",
        "targets": ["test_kernel_rtl"],
        "why": "ordinal 289 writes Buffer = NULL for a NULL source, so an empty "
        "descriptor legitimately has no buffer. Without the short circuit, "
        "kernel_guest_at(0, 0) refuses and two genuinely equal empty strings are "
        "REFUSED and answered FALSE -- a wrong answer arriving through the error "
        "path, which is the hardest kind to notice because the log line looks like "
        "a real diagnostic.",
    },
    {
        "id": "rtl-equal-fold-range-excludes-its-upper-endpoint",
        "file": "src/xbox/kernel_rtl.c",
        "old": "    if (byte >= (unsigned char)'a' && byte <= (unsigned char)'z') {",
        "new": "    if (byte >= (unsigned char)'a' && byte < (unsigned char)'z') {",
        "targets": ["test_kernel_rtl"],
        "why": "an off-by-one at the end of the fold range is invisible for 25 of the 26 "
        "letters, so a test that folded 'abc' would pass. Only a comparison "
        "involving 'z' notices, and nothing about the three section names the boot "
        "looks up would reveal it -- the defect would sit waiting for the first "
        "name containing a z.",
    },
    {
        "id": "rtl-equal-fold-moves-the-wrong-direction",
        "file": "src/xbox/kernel_rtl.c",
        "old": "        return (unsigned char)(byte - 0x20u);",
        "new": "        return (unsigned char)(byte + 0x20u);",
        "targets": ["test_kernel_rtl"],
        "why": "0x20 is the right magnitude in the wrong direction, which is the classic "
        "form of this bug. Lowercase maps to 0x81..0x9A instead of uppercase, so "
        "case-insensitive comparison of mixed case never matches -- and because "
        "equal-case strings still compare equal, a suite that only compared "
        "identical strings would see nothing wrong.",
    },
    {
        "id": "rtl-equal-ignores-the-case-insensitive-flag",
        "file": "src/xbox/kernel_rtl.c",
        "old": "        if (case_insensitive) {",
        "new": "        if (case_insensitive || true) {",
        "targets": ["test_kernel_rtl"],
        "why": "folds unconditionally, so a caller that explicitly asked for a "
        "case-SENSITIVE comparison silently gets a case-insensitive one. The live "
        "boot cannot catch this at all: its single call site passes "
        "CaseInSensitive = 1 as a literal, so the sensitive path is never "
        "exercised by the running title. Written as `|| true` rather than `true` "
        "so the parameter stays used and the mutation actually compiles -- an "
        "uncompilable mutation scores NOT-A-MUTANT, which reads like evidence.",
    },
    {
        "id": "rtl-equal-stops-scanning-at-the-first-difference",
        "file": "src/xbox/kernel_rtl.c",
        "old": """        if (left != right) {
            match = false;""",
        "new": """        if (left != right) {
            match = false;
            break;""",
        "targets": ["test_kernel_rtl"],
        "why": "the ANSWER stays correct, which is exactly why this needs a mutation "
        "rather than trust. What breaks is the non-ASCII counter: a byte above "
        "0x7F later in the span is never reached, so whether the unmeasured "
        "code-page path is reported depends on where in the string the first "
        "difference happened to fall. The counter would then under-report "
        "precisely for the mostly-different strings it most needs to flag, and "
        "`kernel_rtl_nonascii_fold_count()` staying at zero would be read as "
        "'the ASCII assumption holds' when it means 'we stopped looking'.",
    },
    {
        "id": "rtl-equal-nonascii-boundary-off-by-one",
        "file": "src/xbox/kernel_rtl.c",
        "old": "    if (byte >= 0x80u) {",
        "new": "    if (byte > 0x80u) {",
        "targets": ["test_kernel_rtl"],
        "why": "byte 0x80 is the FIRST byte outside ASCII and the one most likely to turn "
        "up first in a code-page-dependent string. With `>` it is folded as though "
        "it were ASCII -- it is outside 'a'..'z' so it passes through unchanged, "
        "the answer happens to come out the same, and nothing is counted or "
        "logged. That is a silent loss of the only signal saying the OEM table "
        "needs measuring, with no visible symptom whatsoever.",
    },
    {
        "id": "rtl-equal-always-reports-equal",
        "file": "src/xbox/kernel_rtl.c",
        "old": "    *equal = match;",
        "new": "    *equal = true;",
        "targets": ["test_kernel_rtl"],
        "why": "the crudest defect in the set, included because it is the one a reader "
        "would assume could not survive. _XGetSectionHandleA@4 would return the "
        "FIRST section's header for every lookup, and the boot would still reach "
        "124 kernel calls and still stop on XeLoadSection -- just with three wrong "
        "section handles. If this survives, the suite is asserting nothing about "
        "the comparison at all.",
    },
    {
        "id": "rtl-equal-scan-bounded-by-maximum-length",
        "file": "src/xbox/kernel_rtl.c",
        "old": "    for (uint32_t i = 0u; i < length1; i++) {",
        "new": "    for (uint32_t i = 0u; i < first->maximum_length; i++) {",
        "targets": ["test_kernel_rtl"],
        "why": "MaximumLength is a CAPACITY, not a length, and the guest sets it to "
        "Length + 1 -- so bounding the scan by it reads one byte past every "
        "comparison, which for descriptors built by ordinal 289 is the NUL and "
        "usually agrees. The defect only shows when MaximumLength does not happen "
        "to be Length + 1, which is why the suite sets it deliberately wrong on "
        "both operands instead of mirroring what 289 produces.",
    },
    # ---------------------------------------------------------------- ordinal 246
    {
        "id": "object-246-zeroes-the-out-parameter-on-failure",
        "file": "src/xbox/kernel_object.c",
        "old": """left untouched\\n",
                         (unsigned)handle);
        return STATUS_INVALID_HANDLE;""",
        "new": """left untouched\\n",
                         (unsigned)handle);
        (void)kernel_guest_write_u32((kernel_guest_ptr)returned_object, 0u);
        return STATUS_INVALID_HANDLE;""",
        "targets": ["test_kernel_object"],
        "why": "zeroing looks like tidy defensive hygiene and is the opposite. The status "
        "is already a failure, so the write adds nothing -- but it destroys "
        "whatever sentinel the caller put in its own slot, and a zeroed slot "
        "beside a failure status is indistinguishable from a kernel that succeeded "
        "and returned a NULL object. The real caller aliases that slot with the "
        "handle it passed in (0x0037FC27), so the wipe also loses the handle.",
    },
    {
        "id": "object-246-judges-the-handle-before-the-out-parameter",
        "file": "src/xbox/kernel_object.c",
        "old": """    if (returned_object == 0u) {
        kernel_hle_log()("kernel: ObReferenceObjectByHandle(%#x) with a NULL "
                         "ReturnedObject -- REFUSED, there is nowhere to put the "
                         "answer\\n",
                         (unsigned)handle);
        return STATUS_INVALID_PARAMETER;
    }

    kernel_object_entry *entry = """
        + _ENTRY_LOOKUP
        + """
    if (entry == NULL) {""",
        "new": """    kernel_object_entry *entry = """
        + _ENTRY_LOOKUP
        + """
    if (returned_object == 0u && entry != NULL) {
        kernel_hle_log()("kernel: ObReferenceObjectByHandle(%#x) with a NULL "
                         "ReturnedObject -- REFUSED, there is nowhere to put the "
                         "answer\\n",
                         (unsigned)handle);
        return STATUS_INVALID_PARAMETER;
    }

    if (entry == NULL) {""",
        "targets": ["test_kernel_object"],
        "why": "reordering two checks changes nothing when only one fault is present, "
        "which is why it needs a test with BOTH. A caller passing a NULL "
        "out-parameter AND a stale handle is told STATUS_INVALID_HANDLE and goes "
        "looking at its handle bookkeeping, when the bug is the pointer it passed. "
        "Two different bugs with two different fixes, reported as one. NOTE: the "
        "FIRST version of this mutation moved only the lookup and not the "
        "invalid-handle RETURN, so it changed no observable behaviour at all and "
        "SURVIVED -- a defect in the mutation, not a gap in the suite. It is "
        "recorded here because a no-op mutation scoring SURVIVED reads exactly like "
        "an untested code path, and the two have to be told apart by reading the "
        "diff rather than the verdict.",
    },
    {
        "id": "object-246-object-type-silently-ignored",
        "file": "src/xbox/kernel_object.c",
        "old": "        unchecked_type_count++;",
        "new": "",
        "targets": ["test_kernel_object"],
        "why": "the handler still works and still logs, so nothing visibly breaks -- the "
        "counter is the only machine-readable record that a type we cannot check "
        "was passed. With it gone, `kernel_object_unchecked_type_count()` reads "
        "zero forever and the day the guest starts relying on the kernel to reject "
        "a mismatched handle looks exactly like the day before it.",
    },
    {
        "id": "object-246-returns-a-pointer-that-does-not-round-trip",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (!kernel_guest_write_u32((kernel_guest_ptr)returned_object, published)) {",
        "new": "    if (!kernel_guest_write_u32((kernel_guest_ptr)returned_object,"
        " published + 4u)) {",
        "targets": ["test_kernel_object", "test_kernel_file_object"],
        "why": "offsetting the published THREAD body, FILE body or opaque handle by +4 breaks its "
        "identity. For an opaque handle, +4 is the handle STRIDE and a plausible "
        "handle -- just the next one. STATUS_SUCCESS is still returned and the "
        "reference is still taken, but the guest's matching ObfDereferenceObject "
        "(0x0037FC5C passes the value in ECX) then decrements a DIFFERENT object "
        "or none at all, so the reference this call took is never released and the "
        "neighbouring object's count is corrupted. Nothing reports an error at any "
        "point, which is precisely the failure the round-trip assertion exists for.",
    },
]
