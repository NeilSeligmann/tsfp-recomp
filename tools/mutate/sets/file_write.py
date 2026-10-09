"""Mutations for the FILE WRITE PATH: ordinal 236 NtWriteFile and what it may touch.

WHY THIS SET IS THE ONE THAT MATTERS MOST. Every other mutation set in this directory
protects a wrong ANSWER. This one protects the user's files. The write path is the only
code in the tree that puts bytes on the operator's filesystem on the guest's behalf, and
the two ways it can be wrong are not symmetric: writing where it must not is unrecoverable,
while refusing where it should have written merely stops a run.

So the entries below are grouped by the property each one attacks:

  1. THE DISC IS NEVER WRITTEN           hdd-image-opened-writable, write-disc-guard-removed,
                                         write-overwrite-on-disc-allowed
  2. NOTHING IS WRITABLE WITHOUT --hdd   write-empty-backing-absorbs-it,
                                         write-refusal-reported-as-success
  3. THE COUNT REPORTED IS THE COUNT     write-reports-requested-not-transferred,
     THAT LANDED                         write-failure-reports-requested-count
  4. THE POSITION IS HONOURED            write-ignores-the-handle-position,
                                         write-advances-on-explicit-offset
  5. ACCESS IS NOT GRANTED UNASKED       write-access-gate-removed, write-always-opens-rdwr
  6. NO PATH AROUND THE GATE             write-zero-length-skips-the-gate
  7. THE RECORDED SIZE IS THE REAL ONE   write-does-not-grow-the-size
  8. AN OVERWRITE REALLY OVERWRITES      create-overwrite-does-not-truncate,
                                         create-overwrite-reports-opened
  9. A LARGE WRITE CROSSES THE STAGING   write-chunk-source-does-not-advance,
     BUFFER INTACT                       write-chunk-target-does-not-advance

ONE BRANCH IS DELIBERATELY ABSENT FROM THIS SET, said here rather than left to be noticed:
the arm in `kernel_file_write_backing` that handles `pwrite` returning 0 for a non-zero
request. No suite can reach it -- `pwrite` does not return 0 on a regular local file -- so a
mutation of it would survive on every future run and would then be rationalised away, which
is worse than an acknowledged gap. It is defended by construction rather than by test: it
reports and fails instead of looping, which is the only behaviour that cannot spin forever.

ONE ENTRY TOUCHES A FILE THIS SET'S AUTHOR DOES NOT OWN, `src/xbox/xdvdfs.c`, and that is
deliberate. The claim "no code path may open the image for writing" is a claim about THAT
open, and a mutation set that could not reach it would be testing the claim's restatement
rather than the claim. The edit is temporary and the harness restores it in a `finally`.
"""

MUTATIONS: list[dict] = [
    # ---- 1. the disc is never written -------------------------------------------
    {
        "id": "hdd-image-opened-writable",
        "file": "src/xbox/xdvdfs.c",
        "old": "    int fd = open(path, O_RDONLY | O_CLOEXEC);",
        "new": "    int fd = open(path, O_RDWR | O_CLOEXEC);",
        "targets": ["test_hdd_backing", "test_disc_seam", "test_xdvdfs_c"],
        "why": "THE SHARPEST MUTATION IN THIS SET. The user's disc image is their "
        "property and this is the one line that decides whether the process can write "
        "to it at all. Every fingerprint-based test in the tree passes against this "
        "mutant, because an O_RDWR descriptor that nothing happens to write through "
        "leaves the bytes alone -- so the only thing that can catch it is a test that "
        "asks the kernel for each descriptor's access mode. If this survives, the "
        "read-only guarantee rests on nobody having written a pwrite yet.",
    },
    {
        "id": "write-disc-guard-removed",
        "file": "src/xbox/kernel_file.c",
        "old": "    if (entry->state.backing != KERNEL_FILE_BACKING_HOST_DIR) {\n"
        "        write_refused_count++;\n"
        "        *out_status = KERNEL_FILE_STATUS_ACCESS_DENIED;",
        "new": "    if (entry->state.backing == KERNEL_FILE_BACKING_EMPTY) {\n"
        "        write_refused_count++;\n"
        "        *out_status = KERNEL_FILE_STATUS_ACCESS_DENIED;",
        "targets": ["test_hdd_backing", "test_disc_seam"],
        "why": "lets a DISC-backed handle past the one check that exists to stop it. "
        "The claim being tested is that the disc is unwritable STRUCTURALLY rather than "
        "by this single line, so the interesting outcome is HOW it dies: a disc entry "
        "carries no host descriptor, so it should fall to the next refusal rather than "
        "reaching a pwrite. A survivor means the next refusal does not exist either.",
    },
    {
        "id": "write-overwrite-on-disc-allowed",
        "file": "src/xbox/kernel_file.c",
        "old": "        if (resolved_to.backing != KERNEL_FILE_BACKING_HOST_DIR ||\n"
        "            resolved_to.is_directory || resolved_to.host_fd_plus_one == 0 ||\n"
        "            !resolved_to.writable || resolved_to.device) {",
        "new": "        if (resolved_to.is_directory && false) {",
        "targets": ["test_hdd_backing", "test_disc_seam"],
        "why": "an overwriting CreateDisposition is a SECOND route to a truncation, and "
        "it lives in a different function from the write path -- so the write path being "
        "safe says nothing about it. Reporting success here would tell the title it had "
        "blanked a file on the user's disc, and the title would then read back content "
        "that disagrees with what it believes it stored.",
    },
    # ---- 2. nothing is writable without --hdd -----------------------------------
    {
        "id": "write-empty-backing-absorbs-it",
        "file": "src/xbox/kernel_file.c",
        "old": "            entry->state.backing == KERNEL_FILE_BACKING_DISC\n"
        '                ? "the user\'s READ-ONLY disc image"\n'
        '                : "a FABRICATED empty file with no host object behind it");\n'
        "        return NULL;",
        "new": "            entry->state.backing == KERNEL_FILE_BACKING_DISC\n"
        '                ? "the user\'s READ-ONLY disc image"\n'
        '                : "a FABRICATED empty file with no host object behind it");\n'
        "        *out_status = STATUS_SUCCESS;\n"
        "        return entry;",
        "targets": ["test_hdd_backing", "test_kernel_io"],
        "why": "THE DEFAULT IS A SAFETY ARGUMENT, NOT A CONVENIENCE. With no --hdd a "
        "handle is a fabricated empty file with no host object, and the two possible "
        "answers are a refusal and a success that transferred nothing. The second is "
        "indistinguishable, from the title's side, from a hard disk that silently "
        "discards save data -- and a zero-byte file is precisely what it leaves behind.",
    },
    {
        "id": "write-refusal-reported-as-success",
        "file": "src/xbox/kernel_io.c",
        "old": "        total += put;\n        if (!complete) {\n"
        "            status = backing_status;\n            break;\n        }",
        "new": "        total += put;\n        if (!complete) {\n            break;\n        }",
        "targets": ["test_hdd_backing", "test_kernel_io", "test_disc_seam"],
        "why": "makes the handler DISCARD the backing layer's failure status and fall "
        "through to its success report. This is the exact failure mode the brief names: "
        "a refused write that returns success having written nothing. It is also the "
        "failure mode a zero-byte TitleMeta.xbx already looked like, so a survivor here "
        "means the suite cannot tell the two apart.",
    },
    # ---- 3. the count reported is the count that landed -------------------------
    {
        "id": "write-reports-requested-not-transferred",
        "file": "src/xbox/kernel_io.c",
        "old": "        total += put;\n        if (!complete) {\n"
        "            status = backing_status;",
        "new": "        total += want;\n        if (!complete) {\n"
        "            status = backing_status;",
        "targets": ["test_hdd_backing", "test_kernel_io", "test_disc_seam"],
        "why": "reports the REQUESTED byte count where the TRANSFERRED one belongs. A "
        "short write then reads as a complete one: the status is still a failure, so a "
        "test that only checked the status would pass, while the guest reads "
        "IO_STATUS_BLOCK.information and would conclude its whole buffer reached the "
        "disk. Every refusal in this tree transfers zero and requests more, so this is "
        "reachable without needing a filesystem that short-writes.",
    },
    {
        "id": "write-failure-reports-requested-count",
        "file": "src/xbox/kernel_io.c",
        "old": '        (void)write_io_status(io_status, status, total, "NtWriteFile");\n'
        '        kernel_hle_log()("kernel: NtWriteFile(\\"%s\\") asked %u byte(s) at %llu and "',
        "new": '        (void)write_io_status(io_status, status, length, "NtWriteFile");\n'
        '        kernel_hle_log()("kernel: NtWriteFile(\\"%s\\") asked %u byte(s) at %llu and "',
        "targets": ["test_hdd_backing", "test_kernel_io", "test_disc_seam"],
        "why": "the same lie one layer out: the loop counts honestly and the "
        "IO_STATUS_BLOCK is filled with the request anyway. Separate from the entry above "
        "because they are different lines and a test could easily cover one and not the "
        "other -- the failing path is the one a run in trouble actually takes.",
    },
    # ---- 4. the position is honoured --------------------------------------------
    {
        "id": "write-ignores-the-handle-position",
        "file": "src/xbox/kernel_io.c",
        "old": "    uint64_t offset = file.offset;\n"
        "    const bool explicit_offset = byte_offset_ptr != 0u;\n"
        "    if (explicit_offset) {\n        uint32_t low = 0u;\n"
        "        uint32_t high = 0u;\n"
        "        if (!kernel_guest_read_u32(byte_offset_ptr, &low) ||\n"
        "            !kernel_guest_read_u32(byte_offset_ptr + 4u, &high)) {\n"
        '            kernel_hle_log()("kernel: NtWriteFile could not read the 64-bit ByteOffset "',
        "new": "    uint64_t offset = 0u;\n"
        "    const bool explicit_offset = byte_offset_ptr != 0u;\n"
        "    if (explicit_offset) {\n        uint32_t low = 0u;\n"
        "        uint32_t high = 0u;\n"
        "        if (!kernel_guest_read_u32(byte_offset_ptr, &low) ||\n"
        "            !kernel_guest_read_u32(byte_offset_ptr + 4u, &high)) {\n"
        '            kernel_hle_log()("kernel: NtWriteFile could not read the 64-bit ByteOffset "',
        "why": "THE SITE THIS BOOT WILL REACH PASSES ByteOffset = NULL (measured at "
        "0x003810BF, the push ebx at 0x003810AA surviving an intervening ret 4), so the "
        "write goes to the handle's own cursor. On a freshly created file that cursor is "
        "0, which means this mutation is INVISIBLE on the one call the boot makes -- and "
        "that is exactly why it is dangerous rather than harmless. The second write "
        "through the same handle lands back on top of the first.",
        "targets": ["test_hdd_backing"],
    },
    {
        "id": "write-advances-on-explicit-offset",
        "file": "src/xbox/kernel_io.c",
        "old": "    if (!explicit_offset) {\n"
        "        (void)kernel_file_open_set_offset(file_handle, offset + total);\n"
        "    }\n\n    lock();\n    write_count++;",
        "new": "    {\n"
        "        (void)kernel_file_open_set_offset(file_handle, offset + total);\n"
        "    }\n\n    lock();\n    write_count++;",
        "targets": ["test_hdd_backing"],
        "why": "a positioned write is not a seek. Advancing the cursor after an explicit "
        "ByteOffset makes the next implicit write land somewhere the title never asked "
        "for, and the mirror of the append bug: the same handle is used for both forms in "
        "this image, so one form corrupting the other's position is reachable.",
    },
    # ---- 5. access is not granted unasked ---------------------------------------
    {
        "id": "write-access-gate-removed",
        "file": "src/xbox/kernel_file.c",
        "old": "    if (!entry->state.writable) {",
        "new": "    if (!entry->state.writable && false) {",
        "targets": ["test_hdd_backing"],
        "why": "NT refuses a write through a handle opened without write access, and so "
        "does this. Removing the check does not produce a successful write -- the "
        "descriptor really is O_RDONLY -- but it turns a named refusal into an EBADF from "
        "pwrite, which is a diagnosis that blames the wrong layer and would send an "
        "investigation into the host filesystem instead of the ACCESS_MASK.",
    },
    {
        "id": "write-always-opens-rdwr",
        "file": "src/xbox/kernel_file.c",
        "old": "    int file_fd = openat(dir_fd, real,\n"
        "                         (wants_write ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC);\n"
        "    bool writable = wants_write;",
        "new": "    int file_fd = openat(dir_fd, real, O_RDWR | O_NOFOLLOW | O_CLOEXEC);\n"
        "    bool writable = true;",
        "targets": ["test_hdd_backing"],
        "why": "the design decision this set exists to pin. Making every file on a "
        "writable volume O_RDWR passes every end-to-end write test, because the writes "
        "all still work -- it only hands the guest a capability it never asked for, on "
        "the volume that holds save games. A survivor means nothing in the suite "
        "distinguishes 'writable because requested' from 'writable always'.",
    },
    # ---- 6. no path around the gate ---------------------------------------------
    {
        "id": "write-zero-length-skips-the-gate",
        "file": "src/xbox/kernel_io.c",
        # `(!gate_consulted && false)` rather than deleting the term: dropping it outright
        # leaves `gate_consulted` set and never read, which is -Wunused-but-set-variable
        # under -Werror and scores NOT-A-MUTANT -- a result that READS like evidence while
        # meaning the mutation was never injected. Measured: the first version of this entry
        # did exactly that.
        "old": "    while (!gate_consulted || total < length) {",
        "new": "    while ((!gate_consulted && false) || total < length) {",
        "targets": ["test_hdd_backing"],
        "why": "the natural loop shape, and the trap in it. A zero-length request never "
        "enters the body, so the handler falls through to its success report having "
        "consulted NEITHER the backing nor the access mask -- a path on which a write "
        "aimed at the user's disc returns STATUS_SUCCESS. The transferred count is zero "
        "either way, so no count-based assertion can see this.",
    },
    # ---- 7. the recorded size is the real one -----------------------------------
    {
        "id": "write-does-not-grow-the-size",
        "file": "src/xbox/kernel_file.c",
        "old": "    const uint64_t end = offset + (uint64_t)done;\n"
        "    if (end > entry->state.size) {\n        entry->state.size = end;\n    }",
        "new": "    const uint64_t end = offset + (uint64_t)done;\n"
        "    if (end > entry->state.size && false) {\n        entry->state.size = end;\n    }",
        "targets": ["test_hdd_backing"],
        "why": "the recorded size is what NtQueryInformationFile class 0x22 reports at "
        "+0x28, and the guest TESTS THAT FIELD AGAINST ZERO -- measured at 0x00381084 and "
        "0x0038108D -- to decide whether the file it just opened is empty. A stale zero "
        "sends a second pass down the nothing-saved-yet arm over content that is on the "
        "disk, which is a wrong answer that looks exactly like a right one.",
    },
    # ---- 8. an overwrite really overwrites --------------------------------------
    {
        "id": "create-overwrite-does-not-truncate",
        "file": "src/xbox/kernel_file.c",
        "old": "        if (ftruncate(resolved_to.host_fd_plus_one - 1, 0) != 0) {",
        "new": "        if (resolved_to.host_fd_plus_one < 0) {",
        "targets": ["test_hdd_backing"],
        "why": "restores exactly the behaviour this handler had before the write path "
        "existed: FILE_OVERWRITE_IF opens the file untouched and reports a disposition "
        "result as though it had truncated. A title then writing shorter content than "
        "last time leaves the old tail in place, indistinguishable from data it wrote "
        "itself. That is a silent wrong answer rather than a refusal.",
    },
    {
        "id": "create-overwrite-reports-opened",
        "file": "src/xbox/kernel_file.c",
        "old": "        information = (disposition == FILE_SUPERSEDE) ? FILE_INFORMATION_SUPERSEDED\n"  # noqa: E501 -- must match the C source exactly
        "                                                      : FILE_INFORMATION_OVERWRITTEN;",
        "new": "        information = FILE_INFORMATION_OPENED;",
        "targets": ["test_hdd_backing"],
        "why": "IO_STATUS_BLOCK.information is READ by the guest -- 0x0037D394 compares "
        "it and turns the result into ERROR_ALREADY_EXISTS, which is Win32 CreateFile's "
        "documented behaviour for CREATE_ALWAYS. Reporting FILE_OPENED after a "
        "truncation tells the title its data was already there when it has just been "
        "destroyed, which is the one lie that matters on a volume holding a save game.",
    },
    # ---- 9. a large write crosses the staging buffer intact ---------------------
    {
        "id": "write-chunk-source-does-not-advance",
        "file": "src/xbox/kernel_io.c",
        "old": "                (const uint8_t *)kernel_guest_at(buffer + total, (size_t)want);\n"
        "            if (!source) {\n"
        '                kernel_hle_log()("kernel: NtWriteFile(\\"%s\\") cannot read',
        "new": "                (const uint8_t *)kernel_guest_at(buffer, (size_t)want);\n"
        "            if (!source) {\n"
        '                kernel_hle_log()("kernel: NtWriteFile(\\"%s\\") cannot read',
        "targets": ["test_hdd_backing"],
        "why": "the REAL boot's largest write is 10240 bytes, under the 16 KiB staging buffer, "
        "so no boot run ever crosses a chunk boundary. Re-reading the first chunk of the "
        "guest buffer for every chunk reports the right byte count and writes the wrong "
        "bytes, which is a corrupt save file with a clean status.",
    },
    {
        "id": "write-chunk-target-does-not-advance",
        "file": "src/xbox/kernel_io.c",
        "old": "kernel_file_write_backing(file_handle, offset + total,",
        "new": "kernel_file_write_backing(file_handle, offset,",
        "targets": ["test_hdd_backing"],
        "why": "the mirror of the entry above: every chunk lands at the starting offset, so "
        "the file is the right length only if the last chunk happens to be the longest. "
        "Unreachable from the boot for the same reason.",
    },
]
