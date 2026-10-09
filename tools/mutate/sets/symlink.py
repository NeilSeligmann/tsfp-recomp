"""Mutations for the symbolic-link pair (203 NtOpenSymbolicLinkObject, 215
NtQuerySymbolicLinkObject) and for NtCreateMutant (192).

WHAT THIS SET PROTECTS. The boot reads back the `\\??\\D:` link it just created, and the guest
branches on what comes back. The ways it goes wrong are quiet. A target written one byte long,
a Length written at the wrong width, or a ReturnedLength that counts the NUL all still look like
a string to a caller that does not parse it. The one measured consumer that does parse it
(0x0037F930) reads the last eight characters as hex digits. A handle that answers by looking the
name up again changes behaviour only once a link is deleted. Entries are grouped by the property
attacked:

  1. THE ORDINALS ARE BOUND        sym-open-ordinal, sym-query-ordinal, mutant-ordinal
  2. WHAT IS WRITTEN BACK          sym-chars-off-by-one, sym-nul-appended, sym-length-plus-one,
                                   sym-length-high-byte-wrong, sym-returned-length-plus-one,
                                   sym-returned-length-skipped
  3. CAPACITY                      sym-capacity-ge, sym-capacity-slack, sym-capacity-low-byte-only,
                                   sym-too-small-length-skipped, sym-too-small-status
  4. WHICH LINK, WHICH NAME        sym-prefix-match, sym-case-sensitive, sym-miss-status
  5. THE HANDLE IS THE OBJECT      sym-slot-by-position, sym-kind-file, sym-kind-check-dropped,
                                   sym-handle-check-dropped, sym-mismatch-status,
                                   sym-invalid-handle-status
  6. SLOTS AND LIFETIME            sym-reclaim-dropped, sym-count-ignores-close,
                                   sym-reset-keeps-handles, sym-handle-max-low,
                                   sym-handle-max-high, sym-open-write-failure-ignored
  7. REFUSALS                      sym-relative-allowed, sym-relative-uncounted,
                                   sym-relative-status, sym-query-arity
  8. NtCreateMutant                mutant-owner-whole-dword, mutant-owner-ignored,
                                   mutant-name-ignored, mutant-attributes-skipped,
                                   mutant-unreadable-attributes, mutant-wrong-kind,
                                   mutant-handle-write-skipped, mutant-null-slot-allowed,
                                   mutant-arity, mutant-refusal-status

THE ARITY OF 203 HAS NO MUTATION, said here rather than left to be noticed. A frame one slot
short leaves OBJECT_ATTRIBUTES at 0, which the handler refuses with the same status it gives for
a missing argument, so no input distinguishes "read two arguments" from "read one". What 203's
arity governs is how many dwords the THUNK pops, and `src/host/kernel_thunk.c` is not a ctest
target, so no mutation can reach it. It was exercised by running the title instead: the boot
continues for 61 more calls past ordinal 203 on the oracle's 2. That shows the run continued,
not that the pop is right in isolation.

THE ENTRIES USE `&& false` rather than `false`, per `_example.py`: a mutation that fails to
compile scores NOT-A-MUTANT, which reads like evidence while meaning nothing was injected.
"""

KERNEL_FILE = "src/xbox/kernel_file.c"
KERNEL_FILE_H = "src/xbox/kernel_file.h"
KERNEL_OBJECT = "src/xbox/kernel_object.c"
FILE_SUITE = ["test_kernel_file"]
OBJECT_SUITE = ["test_kernel_object"]

QUERY_ARGS = """    uint32_t args[3];
    for (unsigned i = 0u; i < 3u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: NtQuerySymbolicLinkObject could not read argument \""""

MUTANT_ARGS = """    uint32_t args[3];
    for (unsigned i = 0u; i < 3u; i++) {
        if (!kernel_frame_arg(frame, i, &args[i])) {
            kernel_hle_log()("kernel: NtCreateMutant could not read argument %u from the \""""

MUTATIONS: list[dict] = [
    # ---- 1. the ordinals are bound --------------------------------------------------------
    {
        "id": "sym-open-ordinal",
        "file": KERNEL_FILE,
        "old": "#define ORD_NtOpenSymbolicLinkObject 203u",
        "new": "#define ORD_NtOpenSymbolicLinkObject 204u",
        "targets": FILE_SUITE,
        "why": "a wrong ordinal binds the handler to a different export. The boot then stops "
        "on 203 again, and a handler that is never called looks identical to one that works "
        "until the next ordinal.",
    },
    {
        "id": "sym-query-ordinal",
        "file": KERNEL_FILE,
        "old": "#define ORD_NtQuerySymbolicLinkObject 215u",
        "new": "#define ORD_NtQuerySymbolicLinkObject 216u",
        "targets": FILE_SUITE,
        "why": "same failure for the second half of the pair: a handle is issued and the "
        "query that reads it back stays a stub that returns 0, which the guest reads as "
        "success with an unwritten buffer.",
    },
    {
        "id": "mutant-ordinal",
        "file": KERNEL_OBJECT,
        "old": "#define ORD_NtCreateMutant 192u",
        "new": "#define ORD_NtCreateMutant 193u",
        "targets": OBJECT_SUITE,
        "why": "ordinal 193 is NtCreateSemaphore, so a slip here would answer the wrong "
        "export with a mutant handle.",
    },
    # ---- 2. what is written back ----------------------------------------------------------
    {
        "id": "sym-chars-off-by-one",
        "file": KERNEL_FILE,
        "old": "if (!kernel_guest_write_u8(kernel_guest_add(buffer, i), (uint8_t)target[i])) {",
        "new": "if (!kernel_guest_write_u8(kernel_guest_add(buffer, i), "
        "(uint8_t)(target[i] + 1))) {",
        "targets": FILE_SUITE,
        "why": "the guest copies this string out and compares it against device names, so a "
        "corrupted character is a wrong device with no diagnostic.",
    },
    {
        "id": "sym-nul-appended",
        "file": KERNEL_FILE,
        "old": "    if (!write_object_string_length(string, (uint16_t)length) ||",
        "new": "    (void)kernel_guest_write_u8(buffer + length, 0u);\n"
        "    if (!write_object_string_length(string, (uint16_t)length) ||",
        "targets": FILE_SUITE,
        "why": "a terminator no measured site needs is invented behaviour, and it writes one "
        "byte past what the guest said it owns when MaximumLength equals the target length.",
    },
    {
        "id": "sym-length-plus-one",
        "file": KERNEL_FILE,
        "old": "write_object_string_length(string, (uint16_t)length) ||",
        "new": "write_object_string_length(string, (uint16_t)(length + 1u)) ||",
        "targets": FILE_SUITE,
        "why": "two of the three measured sites read STRING.Length instead of ReturnedLength, "
        "so counting the NUL there makes the copy one byte long.",
    },
    {
        "id": "sym-length-high-byte-wrong",
        "file": KERNEL_FILE,
        "old": "kernel_guest_write_u8(kernel_guest_add(string, at + 1u), (uint8_t)(length >> 8));",
        "new": "kernel_guest_write_u8(kernel_guest_add(string, at + 1u), (uint8_t)length);",
        "targets": FILE_SUITE,
        "why": "Length is 16 bits written as two bytes. Writing the low byte twice makes every "
        "length wrong by a factor of 257 while the low half looks right.",
    },
    {
        "id": "sym-returned-length-plus-one",
        "file": KERNEL_FILE,
        "old": "(returned_length != 0u && !kernel_guest_write_u32(returned_length, length))) {",
        "new": "(returned_length != 0u &&"
        " !kernel_guest_write_u32(returned_length, length + 1u))) {",
        "targets": FILE_SUITE,
        "why": "the one site that parses the string reads Buffer[ReturnedLength-9] and the "
        "last eight characters as hex, so a NUL-inclusive count makes the last digit a NUL "
        "and the parse fails.",
    },
    {
        "id": "sym-returned-length-skipped",
        "file": KERNEL_FILE,
        "old": "(returned_length != 0u && !kernel_guest_write_u32(returned_length, length))) {",
        "new": "(returned_length != 0u && false &&"
        " !kernel_guest_write_u32(returned_length, length))) {",
        "targets": FILE_SUITE,
        "why": "that same site passes a real ReturnedLength pointer and reads the length from "
        "it, so a skipped write leaves it parsing whatever was on its stack.",
    },
    # ---- 3. capacity ----------------------------------------------------------------------
    {
        "id": "sym-capacity-ge",
        "file": KERNEL_FILE,
        "old": "    if (length > capacity) {",
        "new": "    if (length >= capacity) {",
        "targets": FILE_SUITE,
        "why": "no NUL is written, so a target of exactly MaximumLength bytes fits. Refusing "
        "it is a spurious failure at the boundary.",
    },
    {
        "id": "sym-capacity-slack",
        "file": KERNEL_FILE,
        "old": "    if (length > capacity) {",
        "new": "    if (length > capacity + 1u) {",
        "targets": FILE_SUITE,
        "why": "one byte of slack writes one byte past the buffer the guest said it owns.",
    },
    {
        "id": "sym-capacity-low-byte-only",
        "file": KERNEL_FILE,
        "old": "    const uint32_t capacity = (uint32_t)max_low | ((uint32_t)max_high << 8);",
        "new": "    const uint32_t capacity = (uint32_t)max_low;",
        "targets": FILE_SUITE,
        "why": "the guest's own MaximumLength is 0x208, whose low byte is 8. Ignoring the high "
        "byte refuses every real query with BUFFER_TOO_SMALL.",
    },
    {
        "id": "sym-too-small-length-skipped",
        "file": KERNEL_FILE,
        "old": "        if (returned_length != 0u) {\n"
        "            (void)kernel_guest_write_u32(returned_length, length);",
        "new": "        if (returned_length != 0u && false) {\n"
        "            (void)kernel_guest_write_u32(returned_length, length);",
        "targets": FILE_SUITE,
        "why": "the size needed is how a caller sizes its retry, and a missing one reads as "
        "zero bytes needed.",
    },
    {
        "id": "sym-too-small-status",
        "file": KERNEL_FILE_H,
        "old": "#define KERNEL_FILE_STATUS_BUFFER_TOO_SMALL 0xC0000023u",
        "new": "#define KERNEL_FILE_STATUS_BUFFER_TOO_SMALL 0xC0000024u",
        "targets": FILE_SUITE,
        "why": "the suite compares against the hex literal, so a macro that drifts cannot "
        "move the expectation with it.",
    },
    # ---- 4. which link, which name --------------------------------------------------------
    {
        "id": "sym-prefix-match",
        "file": KERNEL_FILE,
        "old": "        if (symlinks[i].in_use && paths_equal(symlinks[i].name, name)) {\n"
        "            return symlinks[i].target;",
        "new": "        if (symlinks[i].in_use &&"
        " prefix_match_length(name, symlinks[i].name) != 0u) {\n"
        "            return symlinks[i].target;",
        "targets": FILE_SUITE,
        "why": "opening the link object names exactly one object. A prefix match would open "
        "`\\\\??\\\\D:\\\\pak` as the link D:, and the guest would then query a target for a "
        "path that is not a link.",
    },
    {
        "id": "sym-case-sensitive",
        "file": KERNEL_FILE,
        "old": "        if (symlinks[i].in_use && paths_equal(symlinks[i].name, name)) {\n"
        "            return symlinks[i].target;",
        "new": "        if (symlinks[i].in_use && strcmp(symlinks[i].name, name) == 0) {\n"
        "            return symlinks[i].target;",
        "targets": FILE_SUITE,
        "why": "the binary spells its own device names two ways (`CdRom0` and `Cdrom0`), so a "
        "case-sensitive link match misses names this very executable uses.",
    },
    {
        "id": "sym-miss-status",
        "file": KERNEL_FILE,
        "old": "        return KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND;\n"
        "    }\n"
        "    memcpy(target, recorded, strlen(recorded) + 1u);",
        "new": "        return STATUS_UNSUCCESSFUL;\n"
        "    }\n"
        "    memcpy(target, recorded, strlen(recorded) + 1u);",
        "targets": FILE_SUITE,
        "why": "the guest probes `\\\\??\\\\W:` and takes a failure path on any negative status, "
        "but a different code is a different claim about why.",
    },
    # ---- 5. the handle is the object ------------------------------------------------------
    {
        "id": "sym-slot-by-position",
        "file": KERNEL_FILE,
        "old": "        if (symlink_handles[i].in_use && symlink_handles[i].handle == handle) {",
        "new": "        if (symlink_handles[i].in_use) {",
        "targets": FILE_SUITE,
        "why": "two live link handles would both answer with the first one's target, so a "
        "query on T: returns D:'s device.",
    },
    {
        "id": "sym-kind-file",
        "file": KERNEL_FILE,
        "old": "    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_SYMLINK,",
        "new": "    const uint32_t handle = kernel_object_create(KERNEL_OBJECT_FILE,",
        "targets": FILE_SUITE,
        "why": "a link handle recorded as a file would be accepted by NtReadFile and refused "
        "by the query that owns it.",
    },
    {
        "id": "sym-kind-check-dropped",
        "file": KERNEL_FILE,
        "old": "    if (object->kind != KERNEL_OBJECT_SYMLINK) {",
        "new": "    if (object->kind != KERNEL_OBJECT_SYMLINK && false) {",
        "targets": FILE_SUITE,
        "why": "without the kind check a thread or file handle falls through to the lookup "
        "and fails with a different status, hiding that the guest passed the wrong object.",
    },
    {
        "id": "sym-handle-check-dropped",
        "file": KERNEL_FILE,
        "old": "    const kernel_object_entry *object = kernel_object_find(handle);\n"
        "    if (!object) {\n"
        '        kernel_hle_log()("kernel: NtQuerySymbolicLinkObject(%#x) -- not a handle this "',
        "new": "    const kernel_object_entry *object = kernel_object_find(handle);\n"
        "    if (!object && false) {\n"
        '        kernel_hle_log()("kernel: NtQuerySymbolicLinkObject(%#x) -- not a handle this "',
        "targets": FILE_SUITE,
        "why": "a handle this host never issued would dereference a NULL entry.",
    },
    {
        "id": "sym-mismatch-status",
        "file": KERNEL_FILE,
        "old": "        return KERNEL_FILE_STATUS_OBJECT_TYPE_MISMATCH;",
        "new": "        return STATUS_INVALID_HANDLE;",
        "targets": FILE_SUITE,
        "why": "an unissued handle and a wrong-kind handle are different bugs with different "
        "fixes, so they must not collapse into one status.",
    },
    {
        "id": "sym-invalid-handle-status",
        "file": KERNEL_FILE,
        "old": '                         "host issued\\n",\n'
        "                         (unsigned)handle);\n"
        "        return STATUS_INVALID_HANDLE;",
        "new": '                         "host issued\\n",\n'
        "                         (unsigned)handle);\n"
        "        return STATUS_INVALID_PARAMETER;",
        "targets": FILE_SUITE,
        "why": "the same two-bugs argument from the other side.",
    },
    # ---- 6. slots and lifetime ------------------------------------------------------------
    {
        "id": "sym-reclaim-dropped",
        "file": KERNEL_FILE,
        "old": "            kernel_object_find(symlink_handles[i].handle) == NULL) {\n"
        "            memset(&symlink_handles[i], 0, sizeof(symlink_handles[i]));",
        "new": "            kernel_object_find(symlink_handles[i].handle) == NULL && false) {\n"
        "            memset(&symlink_handles[i], 0, sizeof(symlink_handles[i]));",
        "targets": FILE_SUITE,
        "why": "a closed link handle would hold its slot forever, and a title that opens "
        "`D:` once per mount would exhaust the table.",
    },
    {
        "id": "sym-count-ignores-close",
        "file": KERNEL_FILE,
        "old": "            kernel_object_find(symlink_handles[i].handle) != NULL) {\n"
        "            count++;",
        "new": "            (kernel_object_find(symlink_handles[i].handle) != NULL || true)) {\n"
        "            count++;",
        "targets": FILE_SUITE,
        "why": "the run report would count handles the guest has closed as live.",
    },
    {
        "id": "sym-reset-keeps-handles",
        "file": KERNEL_FILE,
        "old": "    memset(symlink_handles, 0, sizeof(symlink_handles));\n    /* Per slot",
        "new": "    (void)symlink_handles;\n    /* Per slot",
        "targets": FILE_SUITE,
        "why": "one test case's link handles would leak into the next and make its slot count "
        "depend on test order.",
    },
    {
        "id": "sym-handle-max-low",
        "file": KERNEL_FILE_H,
        "old": "#define KERNEL_FILE_SYMLINK_HANDLE_MAX 16u",
        "new": "#define KERNEL_FILE_SYMLINK_HANDLE_MAX 15u",
        "targets": FILE_SUITE,
        "why": "the suite opens exactly 16 and expects the 17th to be refused, so a smaller "
        "table refuses a legitimate open.",
    },
    {
        "id": "sym-handle-max-high",
        "file": KERNEL_FILE_H,
        "old": "#define KERNEL_FILE_SYMLINK_HANDLE_MAX 16u",
        "new": "#define KERNEL_FILE_SYMLINK_HANDLE_MAX 17u",
        "targets": FILE_SUITE,
        "why": "a larger table accepts the 17th, which the suite expects to be refused.",
    },
    {
        "id": "sym-open-write-failure-ignored",
        "file": KERNEL_FILE,
        "old": "    if (!kernel_guest_write_u32(handle_out, handle)) {\n"
        '        kernel_hle_log()("kernel: NtOpenSymbolicLinkObject('
        '\\"%s\\") could not write the "',
        "new": "    if (!kernel_guest_write_u32(handle_out, handle) && false) {\n"
        '        kernel_hle_log()("kernel: NtOpenSymbolicLinkObject('
        '\\"%s\\") could not write the "',
        "targets": FILE_SUITE,
        "why": "the handle slot is the guest's only way to learn the handle, so a failed "
        "write must not be reported as an opened link.",
    },
    # ---- 7. refusals ----------------------------------------------------------------------
    {
        "id": "sym-relative-allowed",
        "file": KERNEL_FILE,
        "old": '    if (!read_object_path("NtOpenSymbolicLinkObject", root_directory, '
        "name, sizeof(name))) {",
        "new": '    if (false && !read_object_path("NtOpenSymbolicLinkObject", root_directory, '
        "name, sizeof(name))) {",
        "targets": FILE_SUITE,
        "why": "bypassing the shared resolver only for OpenSymbolicLink accepts unknown roots "
        "outside the bounded 0xfffffffd plus drive-absolute policy and omits its refusal.",
    },
    {
        "id": "sym-relative-uncounted",
        "file": KERNEL_FILE,
        "old": "    if (!accepted) relative_refused_count++;",
        "new": "    if (!accepted && false) relative_refused_count++;",
        "targets": FILE_SUITE,
        "why": "suppressing the shared counter hides an OpenSymbolicLink request outside "
        "the bounded drive-root policy even though the request is refused.",
    },
    {
        "id": "sym-relative-status",
        "file": KERNEL_FILE,
        "old": "        return KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;\n"
        "    }\n"
        "\n"
        "    char target[KERNEL_FILE_PATH_MAX];",
        "new": "        return STATUS_INVALID_PARAMETER;\n"
        "    }\n"
        "\n"
        "    char target[KERNEL_FILE_PATH_MAX];",
        "targets": FILE_SUITE,
        "why": "NtOpenFile answers a refused relative open with PATH_NOT_FOUND, and the two "
        "should not disagree.",
    },
    {
        "id": "sym-query-arity",
        "file": KERNEL_FILE,
        "old": QUERY_ARGS,
        "new": QUERY_ARGS.replace("uint32_t args[3];", "uint32_t args[3] = {0u, 0u, 0u};").replace(
            "i < 3u", "i < 2u"
        ),
        "targets": FILE_SUITE,
        "why": "215 takes three arguments. A handler that read two would treat the optional "
        "ReturnedLength as absent on a frame too short to hold it.",
    },
    # ---- 8. NtCreateMutant ----------------------------------------------------------------
    {
        "id": "mutant-owner-whole-dword",
        "file": KERNEL_OBJECT,
        "old": "    const bool initial_owner = (args[2] & 0xFFu) != 0u;",
        "new": "    const bool initial_owner = args[2] != 0u;",
        "targets": OBJECT_SUITE,
        "why": "BOOLEAN is one byte and the guest pushes a dword. The wrapper forwards a Win32 "
        "BOOL, so bits above the low byte are not part of the argument.",
    },
    {
        "id": "mutant-owner-ignored",
        "file": KERNEL_OBJECT,
        "old": "    if (initial_owner) {",
        "new": "    if (initial_owner && false) {",
        "targets": OBJECT_SUITE,
        "why": "a mutant the guest asked to own would be handed over unowned, and its first "
        "wait would succeed where hardware would block.",
    },
    {
        "id": "mutant-name-ignored",
        "file": KERNEL_OBJECT,
        "old": "    if (object_name != 0u) {\n        /* The only shape that carries root",
        "new": "    if (object_name != 0u && false) {\n        /* The only shape that carries root",
        "targets": OBJECT_SUITE,
        "why": "two creators of one name would each get a private mutex and believe they "
        "shared one.",
    },
    {
        "id": "mutant-attributes-skipped",
        "file": KERNEL_OBJECT,
        "old": "    if (object_attributes != 0u &&\n"
        "        (!kernel_guest_read_u32(GUEST_FIELD(object_attributes, object_name), "
        "&object_name) ||",
        "new": "    if (object_attributes != 0u && false &&\n"
        "        (!kernel_guest_read_u32(GUEST_FIELD(object_attributes, object_name), "
        "&object_name) ||",
        "targets": OBJECT_SUITE,
        "why": "never reading OBJECT_ATTRIBUTES makes every named mutant look anonymous.",
    },
    {
        "id": "mutant-unreadable-attributes",
        "file": KERNEL_OBJECT,
        "old": "                         (unsigned)object_attributes);\n"
        "        return STATUS_INVALID_PARAMETER;\n"
        "    }\n"
        "    if (object_name != 0u) {",
        "new": "                         (unsigned)object_attributes);\n"
        "        return STATUS_SUCCESS;\n"
        "    }\n"
        "    if (object_name != 0u) {",
        "targets": OBJECT_SUITE,
        "why": "an OBJECT_ATTRIBUTES that cannot be read must be refused, not guessed to be "
        "anonymous, because the name is the one thing that decides which mutex it is.",
    },
    {
        "id": "mutant-wrong-kind",
        "file": KERNEL_OBJECT,
        "old": "kernel_object_create_nolock(KERNEL_OBJECT_MUTANT,",
        "new": "kernel_object_create_nolock(KERNEL_OBJECT_OTHER,",
        "targets": OBJECT_SUITE,
        "why": "a handle of the wrong kind is closed correctly and waited on incorrectly.",
    },
    {
        "id": "mutant-handle-write-skipped",
        "file": KERNEL_OBJECT,
        "old": "    if (!kernel_guest_write_u32(handle_out, handle)) {\n"
        "        /* The guest never learns this handle",
        "new": "    if (!kernel_guest_write_u32(handle_out, handle) && false) {\n"
        "        /* The guest never learns this handle",
        "targets": OBJECT_SUITE,
        "why": "the handle slot is the guest's only way to learn the handle, so a failed "
        "write must not be reported as a created mutant.",
    },
    {
        "id": "mutant-null-slot-allowed",
        "file": KERNEL_OBJECT,
        "old": "    if (handle_out == 0u) {\n"
        '        kernel_hle_log()("kernel: NtCreateMutant has no handle',
        "new": "    if (handle_out == 0u && false) {\n"
        '        kernel_hle_log()("kernel: NtCreateMutant has no handle',
        "targets": OBJECT_SUITE,
        "why": "a mutant would be issued and leaked for a call that had nowhere to return it.",
    },
    {
        "id": "mutant-arity",
        "file": KERNEL_OBJECT,
        "old": MUTANT_ARGS,
        "new": MUTANT_ARGS.replace("uint32_t args[3];", "uint32_t args[3] = {0u, 0u, 0u};").replace(
            "i < 3u", "i < 2u"
        ),
        "targets": OBJECT_SUITE,
        "why": "192 takes three arguments. A handler that read two would create a mutant on "
        "a frame too short to hold InitialOwner.",
    },
    {
        "id": "mutant-refusal-status",
        "file": KERNEL_OBJECT,
        "old": "        return STATUS_NOT_IMPLEMENTED;\n    }\n"
        "    if (object_attributes != 0u && (root",
        "new": "        return STATUS_UNSUCCESSFUL;\n    }\n"
        "    if (object_attributes != 0u && (root",
        "targets": OBJECT_SUITE,
        "why": "a refused named mutant must say NOT_IMPLEMENTED, not a generic failure the "
        "guest could mistake for a transient one.",
    },
]
