"""Mutations for NtDeleteFile (195, T8f-1) and NtQueryFullAttributesFile (210, T8d-1).

OWNED BY THE T258 KERNEL-ORDINAL PORT. One file per owner, see `tools/mutate/sets/_example.py`
for the rule and the traps. These replace the hand sweeps recorded only as "29/33" and
"31/31" in commits 1e033c3 and c672df6, re-derived from the code rather than recovered
(the original mutants were never saved). The commit-recorded EQUIVALENT mutants are left out
on purpose and are listed here so nobody re-adds them as "missing coverage":

  - the st_dev half of the held-file identity compare in `open_slot_holds_locked` (a device
    and inode pair is unique, the inode half already separates every file the test can make),
  - the ENOENT arm after `unlinkat` (a race between the lstat and the unlink, unreachable
    single-threaded),
  - the NULL-frame guard in `hle_nt_delete_file` and in `hle_nt_query_full_attributes_file`
    (`kernel_frame_arg` refuses a NULL frame with the same status),
  - 210's creation = last-write policy (the test host cannot tell the two apart),
  - `open_slot_holds_locked` skipping the `in_use` test (a released slot is memset to zero, so
    its descriptor field is already 0 and the next test rejects it),
  - the attributes dword written as one byte (the only values are 0x10 and 0x20, so bytes 1-3
    are zero either way).

`t8-delete-non-file-object-not-refused` (drop `|| !S_ISREG` so a FIFO is unlinked) survived
`test_kernel_delete` as first written, because nothing planted a non-regular host object. T258 added
`test_a_non_regular_host_object_is_refused_not_removed` (a FIFO) and the mutant is now in the set.

CLOSED BY T268: `out->backing = resolved_to.backing;` in `kernel_file_query_attributes` had no
consumer
and no check, so dropping it survived. `test_the_library_call_reports_the_backing_it_resolved` in
`tests/c/test_kernel_attributes.c` now asserts HOST_DIR for a file and a directory, and the mutant
(`t8-attributes-library-call-drops-the-backing`) is in the set.

WHAT THESE ARE CHOSEN TO CATCH. Neither ordinal fails loudly when wrong. A delete that
removes the wrong file, a directory, a held file or something a symbolic link points at
reports success and leaves the guest running; an attribute query that writes a plausible
structure with one field moved is read by the XONLINE size sum as a different size.

Both ordinals live in `kernel_file.c` (resolution, the delete body) and `kernel_io.c` (the 0x38
byte structure, shared with class 0x22 of ordinal 211, so the structure mutations are run
against `test_kernel_io` as well).
"""

DELETE = "src/xbox/kernel_file.c"
IO = "src/xbox/kernel_io.c"

MUTATIONS: list[dict] = [
    # ---------------------------------------------------------------- ordinal 195
    {
        "id": "t8-delete-bound-to-the-wrong-ordinal",
        "file": DELETE,
        "old": "#define ORD_NtDeleteFile 195u",
        "new": "#define ORD_NtDeleteFile 194u",
        "targets": ["test_kernel_delete"],
        "why": "a correct handler on the wrong number leaves 195 unregistered, so the XONLINE "
        "cache closer's delete never runs and nothing in the boot says so.",
    },
    {
        "id": "t8-delete-raw-device-name-allowed-through",
        "file": DELETE,
        "old": "    if (names_raw_view(volume, rest)) {\n"
        '        kernel_hle_log()("kernel: NtDeleteFile(',
        "new": "    if (names_raw_view(volume, rest) && false) {\n"
        '        kernel_hle_log()("kernel: NtDeleteFile(',
        "targets": ["test_kernel_delete"],
        "why": "the virtual raw devices (Partition0, the cache images) are not files. Without "
        "the refusal the name falls through to a host lookup and answers an honest-looking "
        "NAME_NOT_FOUND for an object that does exist.",
    },
    {
        "id": "t8-delete-cache-view-gate-skipped",
        "file": DELETE,
        "old": "        if (!cache_view_prepare_locked(volume, guest_path, &gate)) {\n"
        "            return gate;\n"
        "        }\n"
        "    }\n"
        "    char leaf[KERNEL_FILE_PATH_MAX];\n"
        "    uint32_t status = STATUS_SUCCESS;\n"
        "    const int dir_fd = hostdir_walk_locked(volume, guest_path, rest, leaf,",
        "new": "        if (!cache_view_prepare_locked(volume, guest_path, &gate) && false) {\n"
        "            return gate;\n"
        "        }\n"
        "    }\n"
        "    char leaf[KERNEL_FILE_PATH_MAX];\n"
        "    uint32_t status = STATUS_SUCCESS;\n"
        "    const int dir_fd = hostdir_walk_locked(volume, guest_path, rest, leaf,",
        "targets": ["test_kernel_delete"],
        "why": "a cache partition's directory view is only valid once its FATX image has been "
        "validated. Ignoring a failed gate deletes from a view the open path would refuse.",
    },
    {
        "id": "t8-delete-cache-view-gate-status-dropped",
        "file": DELETE,
        "old": "            return gate;\n"
        "        }\n"
        "    }\n"
        "    char leaf[KERNEL_FILE_PATH_MAX];\n"
        "    uint32_t status = STATUS_SUCCESS;\n"
        "    const int dir_fd = hostdir_walk_locked(volume, guest_path, rest, leaf,",
        "new": "            return STATUS_UNSUCCESSFUL;\n"
        "        }\n"
        "    }\n"
        "    char leaf[KERNEL_FILE_PATH_MAX];\n"
        "    uint32_t status = STATUS_SUCCESS;\n"
        "    const int dir_fd = hostdir_walk_locked(volume, guest_path, rest, leaf,",
        "targets": ["test_kernel_delete"],
        "why": "the gate's own status is the open path's answer for this name. Replacing it "
        "with a generic failure makes delete disagree with open about the same name.",
    },
    {
        "id": "t8-delete-volume-root-not-refused",
        "file": DELETE,
        "old": "    if (leaf[0] == '\\0') {\n"
        "        (void)close(dir_fd);\n"
        '        kernel_hle_log()("kernel: NtDeleteFile(\\"%s\\") names the volume root',
        "new": "    if (leaf[0] == '\\0' && false) {\n"
        "        (void)close(dir_fd);\n"
        '        kernel_hle_log()("kernel: NtDeleteFile(\\"%s\\") names the volume root',
        "targets": ["test_kernel_delete"],
        "why": "the volume root is a directory the host mounted, not a file. The refusal is "
        "ACCESS_DENIED and counted; without it the walk's empty leaf is looked up as a name "
        "and the answer degrades to NAME_NOT_FOUND, an absence the root is not.",
    },
    {
        "id": "t8-delete-missing-leaf-answers-path-not-found",
        "file": DELETE,
        "old": "        return KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND;\n"
        "    }\n"
        "    if (S_ISDIR(info.st_mode)) {",
        "new": "        return KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;\n"
        "    }\n"
        "    if (S_ISDIR(info.st_mode)) {",
        "targets": ["test_kernel_delete"],
        "why": "an absent file in an existing directory is NAME not found and an absent "
        "directory is PATH not found. Collapsing them changes what the XONLINE closer's error "
        "mapper does with the result.",
    },
    {
        "id": "t8-delete-directory-not-refused",
        "file": DELETE,
        "old": "    if (S_ISDIR(info.st_mode)) {\n"
        "        (void)close(dir_fd);\n"
        '        kernel_hle_log()("kernel: NtDeleteFile(\\"%s\\") names a directory',
        "new": "    if (S_ISDIR(info.st_mode) && false) {\n"
        "        (void)close(dir_fd);\n"
        '        kernel_hle_log()("kernel: NtDeleteFile(\\"%s\\") names a directory',
        "targets": ["test_kernel_delete"],
        "why": "removing a directory is not a measured mode. The fall-through arm still "
        "refuses it, but as ACCESS_DENIED with the wrong text, so only the status can tell.",
    },
    {
        "id": "t8-delete-directory-status-is-access-denied",
        "file": DELETE,
        "old": "                         guest_path);\n"
        "        return STATUS_NOT_IMPLEMENTED;\n"
        "    }\n"
        "    if (S_ISLNK(info.st_mode) || !S_ISREG(info.st_mode)) {",
        "new": "                         guest_path);\n"
        "        return KERNEL_FILE_STATUS_ACCESS_DENIED;\n"
        "    }\n"
        "    if (S_ISLNK(info.st_mode) || !S_ISREG(info.st_mode)) {",
        "targets": ["test_kernel_delete"],
        "why": "NOT_IMPLEMENTED says the host did not guess an unmeasured mode, ACCESS_DENIED "
        "says the guest may not. A title reacting to the second would be reacting to a "
        "refusal the host never made.",
    },
    {
        "id": "t8-delete-symlink-not-counted-as-escape",
        "file": DELETE,
        "old": "        if (S_ISLNK(info.st_mode)) {\n"
        "            escape_refused_count++;\n"
        "        }\n"
        '        kernel_hle_log()("kernel: NtDeleteFile(',
        "new": "        if (S_ISLNK(info.st_mode) && false) {\n"
        "            escape_refused_count++;\n"
        "        }\n"
        '        kernel_hle_log()("kernel: NtDeleteFile(',
        "targets": ["test_kernel_delete"],
        "why": "the refusal is still ACCESS_DENIED and nothing is removed, so only the escape "
        "COUNTER can tell. A refusal nobody counts is one nobody notices stopping.",
    },
    {
        "id": "t8-delete-live-handle-not-checked",
        "file": DELETE,
        "old": "    if (open_slot_holds_locked(&info)) {",
        "new": "    if (open_slot_holds_locked(&info) && false) {",
        "targets": ["test_kernel_delete"],
        "why": "unlinking a file the guest still holds open leaves a handle to a vanished "
        "file. INFERRED sharing violation, but the INFERENCE is the contract the test pins.",
    },
    {
        "id": "t8-delete-closed-handle-still-counts-as-live",
        "file": DELETE,
        "old": "            !kernel_object_file_identity_live(slot->state.handle)) {\n"
        "            continue;",
        "new": "            false) {\n            continue;",
        "targets": ["test_kernel_delete"],
        "why": "the measured caller CLOSES its handle and then deletes. A closed handle's slot "
        "is reclaimed lazily, so without the liveness test the close-then-delete the title "
        "actually does is refused with a sharing violation.",
    },
    {
        "id": "t8-delete-identity-compare-ignores-the-inode",
        "file": DELETE,
        "old": "        if (fstat(slot->host_fd_plus_one - 1, &held) == 0 && held.st_dev == "
        "target->st_dev &&\n"
        "            held.st_ino == target->st_ino) {",
        "new": "        if (fstat(slot->host_fd_plus_one - 1, &held) == 0 && held.st_dev == "
        "target->st_dev) {",
        "targets": ["test_kernel_delete"],
        "why": "device alone is the same for every file on the backing directory, so ANY open "
        "handle would block deleting ANY file. The inode half is what makes the check per-file.",
    },
    {
        "id": "t8-delete-unlinks-the-guest-spelling-not-the-host-name",
        "file": DELETE,
        "old": "    const int result = unlinkat(dir_fd, real, 0);",
        "new": "    const int result = unlinkat(dir_fd, leaf, 0);",
        "targets": ["test_kernel_delete"],
        "why": "NT names are case-insensitive and the walk matched the host's real spelling. "
        "Unlinking the guest's spelling fails ENOENT on a case-sensitive host for a file the "
        "lookup just found.",
    },
    {
        "id": "t8-delete-unlink-removes-directories-flag",
        "file": DELETE,
        "old": "    const int result = unlinkat(dir_fd, real, 0);",
        "new": "    const int result = unlinkat(dir_fd, real, AT_REMOVEDIR);",
        "targets": ["test_kernel_delete"],
        "why": "the wrong flag turns every file delete into a failed rmdir. The log still "
        "reads as a host failure, so only a status and a file that is still there say so.",
    },
    {
        "id": "t8-delete-read-only-failure-reported-unsuccessful",
        "file": DELETE,
        "old": "        return (failure == EACCES || failure == EPERM || failure == EROFS)",
        "new": "        return (failure == EPERM || failure == EROFS)",
        "targets": ["test_kernel_delete"],
        "why": "EACCES is the usual answer for an unwritable directory. Falling to "
        "UNSUCCESSFUL hides that the guest was refused rather than that something broke.",
    },
    {
        "id": "t8-delete-success-not-counted",
        "file": DELETE,
        "old": "    deleted_count++;\n",
        "new": "",
        "targets": ["test_kernel_delete"],
        "why": "the run report's only proof that a real host file was removed. A silent "
        "delete is invisible in the report.",
    },
    {
        "id": "t8-delete-success-reports-failure",
        "file": DELETE,
        "old": "                     guest_path, real, volume->host_root);\n    return "
        "STATUS_SUCCESS;",
        "new": "                     guest_path, real, volume->host_root);\n    return "
        "STATUS_UNSUCCESSFUL;",
        "targets": ["test_kernel_delete"],
        "why": "the file is gone and the guest is told it is not. The closer reads only the "
        "sign, so it would retry a delete that already happened.",
    },
    {
        "id": "t8-delete-unreadable-frame-answers-success",
        "file": DELETE,
        "old": '                         "stack (or has no argument frame)\\n");\n'
        "        return STATUS_INVALID_PARAMETER;",
        "new": '                         "stack (or has no argument frame)\\n");\n'
        "        return STATUS_SUCCESS;",
        "targets": ["test_kernel_delete"],
        "why": "success for a call whose argument could not even be read tells the guest a "
        "delete happened that was never attempted.",
    },
    {
        "id": "t8-delete-unmodelled-attributes-not-reported",
        "file": DELETE,
        "old": '    report_unmodelled_attributes("NtDeleteFile", path, attributes);',
        "new": "    (void)attributes;",
        "targets": ["test_kernel_delete"],
        "why": "the measured site passes 0x40 and the open path reports attribute bits the "
        "port does not model. Delete shares the reader, so it must share the report.",
    },
    {
        "id": "t8-delete-bad-path-answers-name-not-found",
        "file": DELETE,
        "old": '    if (!read_object_path("NtDeleteFile", root_directory, path, sizeof(path))) {\n'
        "        return KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;",
        "new": '    if (!read_object_path("NtDeleteFile", root_directory, path, sizeof(path))) {\n'
        "        return KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND;",
        "targets": ["test_kernel_delete"],
        "why": "an unresolvable root handle or over-long path is a PATH problem, the status "
        "NtOpenFile gives. Delete answering differently for the same name is the drift.",
    },
    {
        "id": "t8-delete-root-directory-handle-ignored",
        "file": DELETE,
        "old": '    if (!read_object_path("NtDeleteFile", root_directory, path, sizeof(path))) {',
        "new": '    if (!read_object_path("NtDeleteFile", 0u, path, sizeof(path))) {',
        "targets": ["test_kernel_delete"],
        "why": "a name relative to a directory handle resolves under that directory. "
        "Ignoring the handle resolves it from nowhere, so a relative delete misses or, worse, "
        "hits a same-named file elsewhere.",
    },
    {
        "id": "t8-delete-undeclared-name-under-fail-policy-not-absent",
        "file": DELETE,
        "old": "        if (!is_openable_locked(path) && missing_policy == "
        "KERNEL_FILE_MISSING_FAIL) {",
        "new": "        if (!is_openable_locked(path) && missing_policy == "
        "KERNEL_FILE_MISSING_FAIL &&\n"
        "            false) {",
        "targets": ["test_kernel_delete"],
        "why": "with no volume behind the name and the FAIL policy the open path says the "
        "name does not exist. Delete must say the same, not a refusal that is counted.",
    },
    {
        "id": "t8-delete-declared-name-under-fail-policy-absent",
        "file": DELETE,
        "old": "        if (!is_openable_locked(path) && missing_policy == "
        "KERNEL_FILE_MISSING_FAIL) {",
        "new": "        if (!is_openable_locked(path) || missing_policy == "
        "KERNEL_FILE_MISSING_FAIL) {",
        "targets": ["test_kernel_delete"],
        "why": "a DECLARED openable name has no host file but the open path hands it back as "
        "a file, so the honest answer is the NOT_IMPLEMENTED refusal. `||` makes every name "
        "under the FAIL policy an absence, hiding the declared case.",
    },
    {
        "id": "t8-delete-no-volume-refusal-is-access-denied",
        "file": DELETE,
        "old": "            status = STATUS_NOT_IMPLEMENTED;\n"
        "        }\n"
        "    } else if (volume->backing != KERNEL_FILE_BACKING_HOST_DIR) {",
        "new": "            status = KERNEL_FILE_STATUS_ACCESS_DENIED;\n"
        "        }\n"
        "    } else if (volume->backing != KERNEL_FILE_BACKING_HOST_DIR) {",
        "targets": ["test_kernel_delete"],
        "why": "a name no volume backs is a mode the host cannot honour, not a permission "
        "problem. The two statuses steer the guest differently.",
    },
    {
        "id": "t8-delete-disc-refusal-is-not-implemented",
        "file": DELETE,
        "old": "        status = KERNEL_FILE_STATUS_ACCESS_DENIED;\n"
        "    } else {\n"
        "        status = hostdir_delete_locked(volume, path, rest);",
        "new": "        status = STATUS_NOT_IMPLEMENTED;\n"
        "    } else {\n"
        "        status = hostdir_delete_locked(volume, path, rest);",
        "targets": ["test_kernel_delete"],
        "why": "a disc image is never written: ACCESS_DENIED, as for a read-only volume. A "
        "NOT_IMPLEMENTED would claim the host might support it one day.",
    },
    {
        "id": "t8-delete-refusal-not-counted",
        "file": DELETE,
        "old": "        delete_refused_count++;",
        "new": "        (void)0;",
        "targets": ["test_kernel_delete"],
        "why": "the refused counter is how the run report says a delete was stopped. Without "
        "it every refusal is silent in the report even though the log line is printed.",
    },
    {
        "id": "t8-delete-honest-path-absence-counted-as-refusal",
        "file": DELETE,
        "old": "        status != KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND) {\n"
        "        delete_refused_count++;",
        "new": "        true) {\n        delete_refused_count++;",
        "targets": ["test_kernel_delete"],
        "why": "the header says an absent name or parent is not a refusal. Counting it "
        "inflates the refused total with calls that behaved exactly as NT would.",
    },
    {
        "id": "t8-delete-counters-survive-reset",
        "file": DELETE,
        "old": "    deleted_count = 0u;\n",
        "new": "",
        "targets": ["test_kernel_delete"],
        "why": "a reset that leaves the removed-file count behind reports files deleted by "
        "an earlier run as this run's.",
    },
    {
        "id": "t8-delete-refused-counter-survives-reset",
        "file": DELETE,
        "old": "    delete_refused_count = 0u;\n",
        "new": "",
        "targets": ["test_kernel_delete"],
        "why": "the same for the refusal count, which is what a later run's report would inherit.",
    },
    {
        "id": "t8-delete-non-file-object-not-refused",
        "file": DELETE,
        "old": "    if (S_ISLNK(info.st_mode) || !S_ISREG(info.st_mode)) {",
        "new": "    if (S_ISLNK(info.st_mode)) {",
        "targets": ["test_kernel_delete"],
        "why": "only a regular file is removable. A FIFO or device node planted in the "
        "backing directory is neither a link nor a directory, so dropping the regular-file "
        "test unlinks it.",
    },
    # ---------------------------------------------------------------- ordinal 210
    {
        "id": "t8-fullattr-bound-to-the-wrong-ordinal",
        "file": IO,
        "old": "#define ORD_NtQueryFullAttributesFile 210u",
        "new": "#define ORD_NtQueryFullAttributesFile 209u",
        "targets": ["test_kernel_attributes"],
        "why": "the XAPI GetFileAttributesExA body calls through slot 210. A correct handler on "
        "another number leaves the XONLINE package-size sum answering 0.",
    },
    {
        "id": "t8-fullattr-query-not-counted",
        "file": DELETE,
        "old": "    attribute_query_count++;\n",
        "new": "",
        "targets": ["test_kernel_attributes"],
        "why": "the count is the report's proof the title asked, whatever the outcome.",
    },
    {
        "id": "t8-fullattr-count-survives-reset",
        "file": DELETE,
        "old": "    attribute_query_count = 0u;\n",
        "new": "",
        "targets": ["test_kernel_attributes"],
        "why": "an earlier run's queries would be reported as this run's.",
    },
    {
        "id": "t8-fullattr-refusal-leaves-the-result-unzeroed",
        "file": DELETE,
        "old": "    memset(out, 0, sizeof(*out));\n    uint32_t root_directory = 0u;",
        "new": "    memset(out, 0xA5, sizeof(*out));\n    uint32_t root_directory = 0u;",
        "targets": ["test_kernel_attributes"],
        "why": "the header promises `*out` zeroed on refusal. A caller that ignores the "
        "status would read stale bytes as a size and a time.",
    },
    {
        "id": "t8-fullattr-unmodelled-attributes-not-reported",
        "file": DELETE,
        "old": '    report_unmodelled_attributes("NtQueryFullAttributesFile", path, attributes);',
        "new": "    (void)attributes;",
        "targets": ["test_kernel_attributes"],
        "why": "the measured site passes attributes 0x40, which the port does not model. "
        "Unreported, the attribute bits the title sets on a query go unnoticed.",
    },
    {
        "id": "t8-fullattr-bad-path-answers-name-not-found",
        "file": DELETE,
        "old": '    if (!read_object_path("NtQueryFullAttributesFile", root_directory, path, '
        "sizeof(path))) {\n"
        "        return KERNEL_FILE_STATUS_OBJECT_PATH_NOT_FOUND;",
        "new": '    if (!read_object_path("NtQueryFullAttributesFile", root_directory, path, '
        "sizeof(path))) {\n"
        "        return KERNEL_FILE_STATUS_OBJECT_NAME_NOT_FOUND;",
        "targets": ["test_kernel_attributes"],
        "why": "the same PATH versus NAME distinction NtOpenFile draws for an unusable root "
        "or name.",
    },
    {
        "id": "t8-fullattr-root-directory-handle-ignored",
        "file": DELETE,
        "old": '    if (!read_object_path("NtQueryFullAttributesFile", root_directory, path, '
        "sizeof(path))) {",
        "new": '    if (!read_object_path("NtQueryFullAttributesFile", 0u, path, sizeof(path))) {',
        "targets": ["test_kernel_attributes"],
        "why": "a root handle names the directory a relative name resolves under.",
    },
    {
        "id": "t8-fullattr-directory-flag-not-reported",
        "file": DELETE,
        "old": "    out->is_directory = resolved_to.is_directory;\n",
        "new": "",
        "targets": ["test_kernel_attributes"],
        "why": "a directory reported as a file gets attribute 0x20 instead of 0x10, so a "
        "title testing the directory bit walks into a directory as if it were a file.",
    },
    {
        "id": "t8-fullattr-size-not-reported",
        "file": DELETE,
        "old": "    out->size = resolved_to.size;\n",
        "new": "",
        "targets": ["test_kernel_attributes"],
        "why": "the one field the XONLINE caller reads (nFileSizeLow). Left at zero the "
        "package-size sum is silently short.",
    },
    {
        "id": "t8-fullattr-host-times-never-read",
        "file": DELETE,
        "old": "    if (resolved_to.backing == KERNEL_FILE_BACKING_HOST_DIR) {\n"
        "        out->times_known = host_object_times_locked(&resolved_to, out);",
        "new": "    if (resolved_to.backing == KERNEL_FILE_BACKING_HOST_DIR && false) {\n"
        "        out->times_known = host_object_times_locked(&resolved_to, out);",
        "targets": ["test_kernel_attributes"],
        "why": "a host file would report four zero FILETIMEs and be announced as fabricated. "
        "Zero times parse as 1601, which a title comparing timestamps reads as older than "
        "anything.",
    },
    {
        "id": "t8-fullattr-resolution-descriptor-leaked",
        "file": DELETE,
        "old": "    release_resolution_locked(&resolved_to);\n"
        "    unlock();\n"
        "    if (fabricating) {",
        "new": "    unlock();\n    if (fabricating) {",
        "targets": ["test_kernel_attributes"],
        "why": "the header promises nothing outlives the call. A query that keeps its read-only "
        "descriptor leaks one per call, and a title polling file sizes exhausts them.",
    },
    {
        "id": "t8-fullattr-access-time-read-from-the-write-time",
        "file": DELETE,
        "old": "    out->last_access_time = dir_filetime(info.st_atim.tv_sec, "
        "info.st_atim.tv_nsec);\n"
        "    out->last_write_time = dir_filetime(info.st_mtim.tv_sec, info.st_mtim.tv_nsec);\n"
        "    /* Linux keeps no birth time",
        "new": "    out->last_access_time = dir_filetime(info.st_mtim.tv_sec, "
        "info.st_mtim.tv_nsec);\n"
        "    out->last_write_time = dir_filetime(info.st_mtim.tv_sec, info.st_mtim.tv_nsec);\n"
        "    /* Linux keeps no birth time",
        "targets": ["test_kernel_attributes"],
        "why": "access and write time are different host fields. Reading both from mtime "
        "reports a file as last accessed when it was last written.",
    },
    {
        "id": "t8-fullattr-write-time-read-from-the-access-time",
        "file": DELETE,
        "old": "    out->last_write_time = dir_filetime(info.st_mtim.tv_sec, "
        "info.st_mtim.tv_nsec);\n"
        "    /* Linux keeps no birth time",
        "new": "    out->last_write_time = dir_filetime(info.st_atim.tv_sec, "
        "info.st_atim.tv_nsec);\n"
        "    /* Linux keeps no birth time",
        "targets": ["test_kernel_attributes"],
        "why": "the swapped half. Creation and change time are INFERRED from the write "
        "time, so this one moves three of the four times.",
    },
    {
        "id": "t8-fullattr-write-time-loses-its-nanoseconds",
        "file": DELETE,
        "old": "    out->last_write_time = dir_filetime(info.st_mtim.tv_sec, "
        "info.st_mtim.tv_nsec);\n"
        "    /* Linux keeps no birth time",
        "new": "    out->last_write_time = dir_filetime(info.st_mtim.tv_sec, 0);\n"
        "    /* Linux keeps no birth time",
        "targets": ["test_kernel_attributes"],
        "why": "truncation to whole seconds is invisible unless the test file has a "
        "sub-second mtime, and then it shows as two writes in one second comparing equal.",
    },
    {
        "id": "t8-fullattr-creation-time-follows-the-access-time",
        "file": DELETE,
        "old": "    out->creation_time = out->last_write_time;\n    return true;",
        "new": "    out->creation_time = out->last_access_time;\n    return true;",
        "targets": ["test_kernel_attributes"],
        "why": "the stated INFERRED policy is creation = last write. Following the access "
        "time makes creation move whenever the file is merely read.",
    },
    {
        "id": "t8-fullattr-times-not-announced-as-known",
        "file": DELETE,
        "old": "        out->times_known = host_object_times_locked(&resolved_to, out);",
        "new": "        out->times_known = host_object_times_locked(&resolved_to, out) && false;",
        "targets": ["test_kernel_attributes"],
        "why": "real host times announced as FABRICATED zeros make the run report claim "
        "invented data it did not invent, and inflate the fabricated-timestamp count.",
    },
    {
        "id": "t8-fullattr-frame-info-pointer-read-from-slot-zero",
        "file": IO,
        "old": "        !kernel_frame_arg(frame, 1u, &information)) {\n"
        '        kernel_hle_log()("kernel: NtQueryFullAttributesFile could not read its two',
        "new": "        !kernel_frame_arg(frame, 0u, &information)) {\n"
        '        kernel_hle_log()("kernel: NtQueryFullAttributesFile could not read its two',
        "targets": ["test_kernel_attributes"],
        "why": "the order is pinned by the one measured site: OBJECT_ATTRIBUTES first, the "
        "0x38-byte buffer second. Reading both from slot 0 overwrites the OBJECT_ATTRIBUTES.",
    },
    {
        "id": "t8-fullattr-nonresolving-name-still-writes-the-structure",
        "file": IO,
        "old": "    const uint32_t status = kernel_file_query_attributes(object_attributes, "
        "&found);\n"
        "    if (status != STATUS_SUCCESS) {\n"
        "        return status;\n"
        "    }\n"
        "\n"
        "    const uint32_t attributes =\n"
        "        found.is_directory",
        "new": "    const uint32_t status = kernel_file_query_attributes(object_attributes, "
        "&found);\n"
        "    if (status != STATUS_SUCCESS && false) {\n"
        "        return status;\n"
        "    }\n"
        "\n"
        "    const uint32_t attributes =\n"
        "        found.is_directory",
        "targets": ["test_kernel_attributes"],
        "why": "as in NT nothing is written unless the name resolves. Continuing writes a "
        "zeroed structure with attribute 0x20 into the guest and reports success for a "
        "file that does not exist.",
    },
    {
        "id": "t8-fullattr-file-attribute-is-not-archive",
        "file": IO,
        "old": "        found.is_directory ? FILE_ATTRIBUTE_DIRECTORY_BIT : "
        "FILE_ATTRIBUTE_ARCHIVE_BIT;\n"
        "    const network_open_information answer = {\n"
        "        .creation_time = found.creation_time,",
        "new": "        found.is_directory ? FILE_ATTRIBUTE_DIRECTORY_BIT : 0u;\n"
        "    const network_open_information answer = {\n"
        "        .creation_time = found.creation_time,",
        "targets": ["test_kernel_attributes"],
        "why": "a plain file reports 0x20, the same rule as class 0x22. Zero is a legal-looking "
        "attribute word that GetFileAttributesEx callers treat as 'normal' rather than archive.",
    },
    {
        "id": "t8-fullattr-directory-attribute-swapped-for-archive",
        "file": IO,
        "old": "        found.is_directory ? FILE_ATTRIBUTE_DIRECTORY_BIT : "
        "FILE_ATTRIBUTE_ARCHIVE_BIT;\n"
        "    const network_open_information answer = {\n"
        "        .creation_time = found.creation_time,",
        "new": "        found.is_directory ? FILE_ATTRIBUTE_ARCHIVE_BIT : "
        "FILE_ATTRIBUTE_DIRECTORY_BIT;\n"
        "    const network_open_information answer = {\n"
        "        .creation_time = found.creation_time,",
        "targets": ["test_kernel_attributes"],
        "why": "the inverted branch: every file reads as a directory and every directory "
        "as a file.",
    },
    {
        "id": "t8-fullattr-change-time-follows-the-access-time",
        "file": IO,
        "old": "        .change_time = found.last_write_time,",
        "new": "        .change_time = found.last_access_time,",
        "targets": ["test_kernel_attributes"],
        "why": "INFERRED: never read by the one consumer, so only a test pinning the "
        "inference notices the field drifting.",
    },
    {
        "id": "t8-fullattr-allocation-is-the-unrounded-size",
        "file": IO,
        "old": "        .allocation_size = found.backing == KERNEL_FILE_BACKING_HOST_DIR "
        "? found.size : network_open_allocation(found.size),",
        "new": "        .allocation_size = found.size,",
        "targets": ["test_kernel_attributes"],
        "why": "allocation is the end of file rounded up to the 2048-byte sector, the one "
        "answer for DISC backing and both ordinals share. Equal to the size, it differs only "
        "for sizes that are not a sector multiple.",
    },
    {
        "id": "t8-fullattr-allocation-rounds-down",
        "file": IO,
        "old": "    return (size + XDVDFS_SECTOR_SIZE - 1u) / XDVDFS_SECTOR_SIZE * "
        "XDVDFS_SECTOR_SIZE;",
        "new": "    return (size + XDVDFS_SECTOR_SIZE) / XDVDFS_SECTOR_SIZE * XDVDFS_SECTOR_SIZE;",
        "targets": ["test_kernel_attributes", "test_kernel_io"],
        "why": "dropping the -1 allocates one extra sector for every exact multiple, wrong "
        "only at the boundary a test must probe on purpose.",
    },
    {
        "id": "t8-fullattr-structure-written-without-its-tail",
        "file": IO,
        "old": "    return kernel_guest_write_bytes(address, bytes, sizeof(bytes));",
        "new": "    return kernel_guest_write_bytes(address, bytes, sizeof(bytes) - 4u);",
        "targets": ["test_kernel_attributes", "test_kernel_io"],
        "why": "the 4 padding bytes are written on purpose, so the title never reads its own "
        "stack. Dropping them is deterministic in the port and not in the title.",
    },
    {
        "id": "t8-fullattr-structure-scratch-is-not-zero",
        "file": IO,
        "old": "    uint8_t bytes[KERNEL_IO_FILE_NETWORK_OPEN_BYTES];\n"
        "    memset(bytes, 0, sizeof(bytes));",
        "new": "    uint8_t bytes[KERNEL_IO_FILE_NETWORK_OPEN_BYTES];\n"
        "    memset(bytes, 0xA5, sizeof(bytes));",
        "targets": ["test_kernel_attributes", "test_kernel_io"],
        "why": "the tail is zero only because of this memset. A different fill is the same "
        "bug as an uninitialised buffer, made deterministic so a test can see it.",
    },
    {
        "id": "t8-fullattr-access-and-write-offsets-swapped",
        "file": IO,
        "old": "    store_u64_le(&bytes[NETWORK_OPEN_LAST_ACCESS_TIME_OFFSET], "
        "information->last_access_time);",
        "new": "    store_u64_le(&bytes[NETWORK_OPEN_LAST_ACCESS_TIME_OFFSET], "
        "information->last_write_time);",
        "targets": ["test_kernel_attributes", "test_kernel_io"],
        "why": "a field written to the neighbouring slot's value. The layout is MEASURED, so "
        "a value in the wrong slot is a plausible structure with a wrong time.",
    },
    {
        "id": "t8-fullattr-allocation-and-end-of-file-offsets-swapped",
        "file": IO,
        "old": "    store_u64_le(&bytes[NETWORK_OPEN_END_OF_FILE_OFFSET], "
        "information->end_of_file);",
        "new": "    store_u64_le(&bytes[NETWORK_OPEN_END_OF_FILE_OFFSET], "
        "information->allocation_size);",
        "targets": ["test_kernel_attributes", "test_kernel_io"],
        "why": "nFileSizeLow is end of file. Writing the rounded allocation there makes "
        "every package size a multiple of 2048.",
    },
    {
        "id": "t8-fullattr-u64-store-truncates-to-the-low-half",
        "file": IO,
        "old": "        out[i] = (uint8_t)(value >> (8u * i));",
        "new": "        out[i] = (uint8_t)((value & 0xFFFFFFFFu) >> (8u * i));",
        "targets": ["test_kernel_attributes", "test_kernel_io"],
        "why": "a FILETIME needs all 64 bits (a current time has a nonzero high dword). "
        "Truncating passes every test that uses small values.",
    },
    {
        "id": "t8-fullattr-unwritable-result-still-succeeds",
        "file": IO,
        "old": "                         (unsigned)KERNEL_IO_FILE_NETWORK_OPEN_BYTES, "
        "(unsigned)information);\n"
        "        return STATUS_INVALID_PARAMETER;",
        "new": "                         (unsigned)KERNEL_IO_FILE_NETWORK_OPEN_BYTES, "
        "(unsigned)information);\n"
        "        return STATUS_SUCCESS;",
        "targets": ["test_kernel_attributes"],
        "why": "success with nothing written leaves the title reading its own stack as a "
        "file size.",
    },
    {
        "id": "t8-fullattr-fabricated-times-not-counted",
        "file": IO,
        "old": "    if (!found.times_known) {\n"
        "        lock();\n"
        "        fabricated_timestamp_count++;",
        "new": "    if (found.times_known && false) {\n"
        "        lock();\n"
        "        fabricated_timestamp_count++;",
        "targets": ["test_kernel_attributes"],
        "why": "the count is how the run report says four zero timestamps were invented. "
        "Uncounted, a disc file's zeros read as real data.",
    },
    {
        "id": "t8-fullattr-host-times-counted-as-fabricated",
        "file": IO,
        "old": "    if (!found.times_known) {\n"
        "        lock();\n"
        "        fabricated_timestamp_count++;",
        "new": "    if (found.times_known || !found.times_known) {\n"
        "        lock();\n"
        "        fabricated_timestamp_count++;",
        "targets": ["test_kernel_attributes"],
        "why": "counting real host times as invented makes the report accuse the port of "
        "fabricating data it read from disk.",
    },
    # ---------------------------------------------------------------- T268 gap closure
    {
        "id": "t8-attributes-library-call-drops-the-backing",
        "file": DELETE,
        "old": "    out->backing = resolved_to.backing;\n    out->is_directory = resolved_to.is_dir"
        "ectory;",
        "new": "    out->is_directory = resolved_to.is_directory;",
        "targets": ["test_kernel_attributes"],
        "why": "the library result"
        "'s `backing` says "
        "which kind of volum"
        "e answered (host di"
        "rectory, disc or fabricated empty). Dropping the copy leaves every answer EMPTY, whi"
        "ch a caller that branches on host-backed objects reads as a fabricated file.",
    },
]
