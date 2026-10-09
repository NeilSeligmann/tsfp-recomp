"""Mutations for ordinals 198 NtFlushBuffersFile and 207 NtQueryDirectoryFile (T94, T258).

OWNED BY THE T258 KERNEL PORT. One file per owner, see `tools/mutate/sets/_example.py`.
Ported from the hand sweeps of T94 commits 1958c95 (198) and bdaf60a (207), whose messages
record only that the sweeps were run. Every entry is a one-line (or one-statement) defect
with an anchor that appears EXACTLY ONCE in its file; `tests/test_mutation_anchors.py`
checks that without a build.

WHAT THESE CATCH. Neither ordinal can fail loudly. A flush that returns success for the
wrong handle, or an enumeration that lists a symlink, skips an entry after a delete or
reports a size at the wrong offset, still lets the title run: it sees plausible
FILE_DIRECTORY_INFORMATION and moves on. Each entry is a rule whose violation produces a
PLAUSIBLE answer.

PORTED EFFECTIVE MUTANTS ONLY. Equivalent mutants considered and NOT included:
  - `mask_length > KERNEL_FILE_DIR_NAME_MAX` in `kernel_file_dir_next`: unreachable, the
    only caller (`read_directory_mask`) already refuses a mask longer than 255.
  - `strlen(name) > KERNEL_FILE_DIR_NAME_MAX` in the directory scan: a Linux d_name is at
    most 255 bytes, so the guard cannot fire on a real host directory.
  - `found && dir_name_compare(name, best) >= 0` as `> 0`: two entries of one directory
    never compare equal.
  - `mask_string != 0u ? mask : NULL`: a zero-length mask and no mask both mean "all".
  - `memset(out, 0, ...)` / `memset(&best_info, 0, ...)`: every field is assigned after.

CLOSED BY T268 (were 17 survivors, each is now a `t8-flush-io-status-*` / `t8-querydir-*` mutant
below,
killed by `test_kernel_io` for 198 and `test_hdd_backing` for 207): an unwritable IoStatusBlock on
198;
207 ApcRoutine and ApcContext each ignored on their own; a null FileInformation; an unreadable
FileName
mask; RestartScan read as a whole dword; a nonzero FileIndex; the change time taken from the access
time; the allocation rounding (down, and by a whole extra page); restart keeping the cursor flag
(NO_MORE_FILES instead of NO_SUCH_FILE); a name that exactly fills the buffer being refused; the
FileTime sub-second divisor; the DOS "*.*" mask treated as a plain wildcard; a trailing `*` not
skipped; the fold range excluding 'Z'; and case-only-different names comparing equal.

HOST DIRECTORY st_size (`t8-querydir-directory-reports-the-host-inode-size`): the scratch directory
of
the suite lives beside the binary, where overlayfs reports 0 for a directory inode, so a
shared-scratch
assertion cannot kill it. `test_hdd_backing` therefore lists a directory created on /dev/shm (tmpfs,
st_size 40) and SKIPS, with a printed note, when the host has no tmpfs there. On such a host the
mutant
survives, which is a property of the host and not a test gap.
"""

FLUSH_IO = "src/xbox/kernel_io.c"
FILE = "src/xbox/kernel_file.c"

MUTATIONS: list[dict] = [
    # ---------------------------------------------------------------- ordinal 198
    {
        "id": "t94-flush-argument-zero-is-not-the-handle",
        "file": FLUSH_IO,
        "old": "!kernel_frame_arg(frame, 0u, &file_handle) ||",
        "new": "!kernel_frame_arg(frame, 1u, &file_handle) ||",
        "targets": ["test_kernel_io"],
        "why": "reading the IoStatusBlock slot as the handle turns every flush into a "
        "refusal. The stub-free alternative is a flush that always claims success, "
        "which is what a swapped argument pair looks like from the title's side.",
    },
    {
        "id": "t94-flush-argument-one-is-not-the-status-block",
        "file": FLUSH_IO,
        "old": "!kernel_frame_arg(frame, 1u, &io_status)) {\n"
        '        kernel_hle_log()("kernel: NtFlushBuffersFile could not read',
        "new": "!kernel_frame_arg(frame, 0u, &io_status)) {\n"
        '        kernel_hle_log()("kernel: NtFlushBuffersFile could not read',
        "targets": ["test_kernel_io"],
        "why": "the status block would be written at the handle value, a wild guest "
        "address, and the flush reported as a bad parameter. A title that only "
        "tests the sign of the result would see a failed commit.",
    },
    {
        "id": "t94-flush-accepts-a-handle-that-is-not-an-open-file",
        "file": FLUSH_IO,
        "old": "    if (!kernel_file_open_info(file_handle, &file)) {\n"
        '        kernel_hle_log()("kernel: NtFlushBuffersFile on handle',
        "new": "    if (!kernel_file_open_info(file_handle, &file) && (memset(&file, 0, "
        "sizeof(file)), false)) {\n"
        '        kernel_hle_log()("kernel: NtFlushBuffersFile on handle',
        "targets": ["test_kernel_io"],
        "why": "a flush that succeeds on a thread handle or a dead handle tells the "
        "title its data is committed when the call touched nothing. The memset keeps "
        "the path logged below well defined under the mutation.",
    },
    {
        "id": "t94-flush-refusal-leaves-the-status-block-untouched",
        "file": FLUSH_IO,
        "old": "        (void)write_io_status((kernel_guest_ptr)io_status, STATUS_INVALID_HANDLE, "
        "0u,\n"
        '                              "NtFlushBuffersFile");',
        "new": "        (void)io_status;",
        "targets": ["test_kernel_io"],
        "why": "the guest wrapper at 0x0037D0F6 reads the status block, not only EAX, "
        "so a refusal that leaves stale bytes there is read as whatever the stack "
        "held.",
    },
    {
        "id": "t94-flush-refusal-reports-success",
        "file": FLUSH_IO,
        "old": '                              "NtFlushBuffersFile");\n'
        "        return STATUS_INVALID_HANDLE;",
        "new": '                              "NtFlushBuffersFile");\n'
        "        return STATUS_SUCCESS;",
        "targets": ["test_kernel_io"],
        "why": "the status block says refused while EAX says committed. The wrapper "
        "tests EAX's sign (jl at 0x0037D0FE), so this is the worse of the two halves.",
    },
    {
        "id": "t94-flush-success-skips-the-status-block",
        "file": FLUSH_IO,
        "old": "    if (!write_io_status((kernel_guest_ptr)io_status, STATUS_SUCCESS, 0u,\n"
        '                         "NtFlushBuffersFile")) {',
        "new": "    if (false && !write_io_status((kernel_guest_ptr)io_status, STATUS_SUCCESS, "
        "0u,\n"
        '                         "NtFlushBuffersFile")) {',
        "targets": ["test_kernel_io"],
        "why": "EAX still says success so the common path works, and the block keeps "
        "whatever the guest stack held.",
    },
    {
        "id": "t94-flush-success-reports-a-nonzero-information",
        "file": FLUSH_IO,
        "old": "    if (!write_io_status((kernel_guest_ptr)io_status, STATUS_SUCCESS, 0u,\n"
        '                         "NtFlushBuffersFile")) {',
        "new": "    if (!write_io_status((kernel_guest_ptr)io_status, STATUS_SUCCESS, 1u,\n"
        '                         "NtFlushBuffersFile")) {',
        "targets": ["test_kernel_io"],
        "why": "a flush transfers no bytes. A nonzero Information reads as a byte "
        "count to any caller that sums it.",
    },
    {
        "id": "t94-flush-success-reports-the-wrong-status-in-the-block",
        "file": FLUSH_IO,
        "old": "    if (!write_io_status((kernel_guest_ptr)io_status, STATUS_SUCCESS, 0u,\n"
        '                         "NtFlushBuffersFile")) {',
        "new": "    if (!write_io_status((kernel_guest_ptr)io_status, STATUS_UNSUCCESSFUL, 0u,\n"
        '                         "NtFlushBuffersFile")) {',
        "targets": ["test_kernel_io"],
        "why": "block and EAX disagree on a successful flush.",
    },
    {
        "id": "t94-flush-count-never-recorded",
        "file": FLUSH_IO,
        "old": "    flush_count++;",
        "new": "    (void)flush_count;",
        "targets": ["test_kernel_io"],
        "why": "the flush counter is the only evidence a title ever reached 198.",
    },
    {
        "id": "t94-flush-refusal-counted-as-a-flush",
        "file": FLUSH_IO,
        "old": "        (void)write_io_status((kernel_guest_ptr)io_status, STATUS_INVALID_HANDLE, "
        "0u,\n"
        '                              "NtFlushBuffersFile");',
        "new": "        (void)write_io_status((kernel_guest_ptr)io_status, STATUS_INVALID_HANDLE, "
        "0u,\n"
        '                              "NtFlushBuffersFile");\n'
        "        flush_count++;",
        "targets": ["test_kernel_io"],
        "why": "a refused flush that still counts overstates how often the title committed data.",
    },
    # ---------------------------------------------------------------- ordinal 207
    {
        "id": "t94-querydir-event-argument-ignored",
        "file": FLUSH_IO,
        "old": "    if (args[1] != 0u || args[2] != 0u || args[3] != 0u) {",
        "new": "    if (args[2] != 0u || args[3] != 0u) {",
        "targets": ["test_hdd_backing"],
        "why": "an Event the title expects signalled would never be, and the call "
        "would still answer as if synchronous. Every measured site passes zero.",
    },
    {
        "id": "t94-querydir-unmeasured-async-returns-the-wrong-status",
        "file": FLUSH_IO,
        "old": "        (void)write_io_status(io_status, STATUS_NOT_IMPLEMENTED, 0u, who);\n"
        "        return STATUS_NOT_IMPLEMENTED;",
        "new": "        (void)write_io_status(io_status, STATUS_NOT_IMPLEMENTED, 0u, who);\n"
        "        return STATUS_INVALID_PARAMETER;",
        "targets": ["test_hdd_backing"],
        "why": "block and EAX disagree, and NOT_IMPLEMENTED is the status that tells "
        "a reader the shape is unmeasured rather than malformed.",
    },
    {
        "id": "t94-querydir-accepts-a-second-information-class",
        "file": FLUSH_IO,
        "old": "    if (info_class != KERNEL_IO_FILE_CLASS_DIRECTORY) {",
        "new": "    if (info_class != KERNEL_IO_FILE_CLASS_DIRECTORY && info_class != 2u) {",
        "targets": ["test_hdd_backing"],
        "why": "class 2 (FILE_FULL_DIRECTORY_INFORMATION) has a different layout, so "
        "filling the class-1 layout under it puts the name at the wrong offset.",
    },
    {
        "id": "t94-querydir-wrong-class-returns-the-wrong-status",
        "file": FLUSH_IO,
        "old": "        (void)write_io_status(io_status, KERNEL_IO_STATUS_INVALID_INFO_CLASS, 0u, "
        "who);\n"
        "        return KERNEL_IO_STATUS_INVALID_INFO_CLASS;",
        "new": "        (void)write_io_status(io_status, KERNEL_IO_STATUS_INVALID_INFO_CLASS, 0u, "
        "who);\n"
        "        return STATUS_INVALID_PARAMETER;",
        "targets": ["test_hdd_backing"],
        "why": "block and EAX disagree on the class refusal.",
    },
    {
        "id": "t94-querydir-length-below-the-header-accepted",
        "file": FLUSH_IO,
        "old": "    if (information == 0u || length < DIRECTORY_INFO_HEADER_BYTES) {",
        "new": "    if (information == 0u || length < DIRECTORY_INFO_HEADER_BYTES / 2u) {",
        "targets": ["test_hdd_backing"],
        "why": "a buffer shorter than the fixed header would be written past its end.",
    },
    {
        "id": "t94-querydir-restart-flag-never-honoured",
        "file": FLUSH_IO,
        "old": "    const bool restart = (args[9] & 0xFFu) != 0u;",
        "new": "    const bool restart = false && (args[9] & 0xFFu) != 0u;",
        "targets": ["test_hdd_backing"],
        "why": "the first call on a handle starts at the beginning anyway, so the "
        "defect shows only when a listing is replayed from the top.",
    },
    {
        "id": "t94-querydir-next-entry-offset-nonzero",
        "file": FLUSH_IO,
        "old": "    if (!kernel_guest_write_u32(information, 0u) ||",
        "new": "    if (!kernel_guest_write_u32(information, 4u) ||",
        "targets": ["test_hdd_backing"],
        "why": "the consumers read one record per call and never follow NextEntryOffset, "
        "so a nonzero value is the only thing that would make a follower read garbage.",
    },
    {
        "id": "t94-querydir-end-of-file-at-the-wrong-offset",
        "file": FLUSH_IO,
        "old": "#define DIRECTORY_INFO_END_OF_FILE_OFFSET 0x28u",
        "new": "#define DIRECTORY_INFO_END_OF_FILE_OFFSET 0x20u",
        "targets": ["test_hdd_backing"],
        "why": "the guest reads the size at +0x28. Elsewhere the same value lands in "
        "the change time, so every file lists as empty and nothing errors.",
    },
    {
        "id": "t94-querydir-allocation-size-at-the-wrong-offset",
        "file": FLUSH_IO,
        "old": "#define DIRECTORY_INFO_ALLOCATION_OFFSET 0x30u",
        "new": "#define DIRECTORY_INFO_ALLOCATION_OFFSET 0x28u",
        "targets": ["test_hdd_backing"],
        "why": "the allocation size overwrites the end of file at +0x28, so a 3 byte "
        "file lists as 4096 bytes.",
    },
    {
        "id": "t94-querydir-attributes-at-the-wrong-offset",
        "file": FLUSH_IO,
        "old": "#define DIRECTORY_INFO_ATTRIBUTES_OFFSET 0x38u",
        "new": "#define DIRECTORY_INFO_ATTRIBUTES_OFFSET 0x30u",
        "targets": ["test_hdd_backing"],
        "why": "attributes are what decide whether the guest recurses into an entry.",
    },
    {
        "id": "t94-querydir-name-length-at-the-wrong-offset",
        "file": FLUSH_IO,
        "old": "#define DIRECTORY_INFO_NAME_LENGTH_OFFSET 0x3Cu",
        "new": "#define DIRECTORY_INFO_NAME_LENGTH_OFFSET 0x34u",
        "targets": ["test_hdd_backing"],
        "why": "the guest reads the length at +0x3C. Elsewhere it reads the zero or "
        "the sentinel the buffer held and copies a name of that length.",
    },
    {
        "id": "t94-querydir-name-at-the-wrong-offset",
        "file": FLUSH_IO,
        "old": "#define DIRECTORY_INFO_NAME_OFFSET 0x40u",
        "new": "#define DIRECTORY_INFO_NAME_OFFSET 0x44u",
        "targets": ["test_hdd_backing"],
        "why": "the name is read at +0x40, so every entry lists with its first four "
        "characters missing.",
    },
    {
        "id": "t94-querydir-access-time-offset-collides-with-write",
        "file": FLUSH_IO,
        "old": "#define DIRECTORY_INFO_ACCESS_OFFSET 0x10u",
        "new": "#define DIRECTORY_INFO_ACCESS_OFFSET 0x18u",
        "targets": ["test_hdd_backing"],
        "why": "access time lands on the write time, which the title shows as the file's date.",
    },
    {
        "id": "t94-querydir-change-time-offset-collides-with-write",
        "file": FLUSH_IO,
        "old": "#define DIRECTORY_INFO_CHANGE_OFFSET 0x20u",
        "new": "#define DIRECTORY_INFO_CHANGE_OFFSET 0x10u",
        "targets": ["test_hdd_backing"],
        "why": "the change time overwrites the access time field.",
    },
    {
        "id": "t94-querydir-creation-time-takes-the-access-time",
        "file": FLUSH_IO,
        "old": "!write_u64(information + DIRECTORY_INFO_CREATION_OFFSET, entry.creation_time) ||",
        "new": "!write_u64(information + DIRECTORY_INFO_CREATION_OFFSET, entry.last_access_time) "
        "||",
        "targets": ["test_hdd_backing"],
        "why": "creation time is the stored last-write time (INFERRED). The access "
        "time differs from it in the fixture, so the swap is visible.",
    },
    {
        "id": "t94-querydir-write-time-takes-the-access-time",
        "file": FLUSH_IO,
        "old": "!write_u64(information + DIRECTORY_INFO_WRITE_OFFSET, entry.last_write_time) ||",
        "new": "!write_u64(information + DIRECTORY_INFO_WRITE_OFFSET, entry.last_access_time) ||",
        "targets": ["test_hdd_backing"],
        "why": "the last-write time is what the title shows as a save's date.",
    },
    {
        "id": "t94-querydir-size-reported-as-the-allocation",
        "file": FLUSH_IO,
        "old": "!write_u64(information + DIRECTORY_INFO_END_OF_FILE_OFFSET, entry.size) ||",
        "new": "!write_u64(information + DIRECTORY_INFO_END_OF_FILE_OFFSET, allocation) ||",
        "targets": ["test_hdd_backing"],
        "why": "a 3 byte file lists as 4096 bytes, the cluster-rounded size the guest "
        "must not see in the end-of-file field.",
    },
    {
        "id": "t94-querydir-attributes-swapped",
        "file": FLUSH_IO,
        "old": "                                entry.is_directory ? FILE_ATTRIBUTE_DIRECTORY_BIT\n"
        "                                                   : FILE_ATTRIBUTE_ARCHIVE_BIT) ||",
        "new": "                                entry.is_directory ? FILE_ATTRIBUTE_ARCHIVE_BIT\n"
        "                                                   : FILE_ATTRIBUTE_DIRECTORY_BIT) ||",
        "targets": ["test_hdd_backing"],
        "why": "files list as directories and directories as files, so the delete loop "
        "recurses into a file or unlinks a directory.",
    },
    {
        "id": "t94-querydir-name-terminated-by-us",
        "file": FLUSH_IO,
        "old": "        !kernel_guest_write_bytes(information + DIRECTORY_INFO_NAME_OFFSET, "
        "entry.name,\n"
        "                                  name_length)) {",
        "new": "        !kernel_guest_write_bytes(information + DIRECTORY_INFO_NAME_OFFSET, "
        "entry.name,\n"
        "                                  name_length + 1u)) {",
        "targets": ["test_hdd_backing"],
        "why": "the guest writes its own NUL at +0x40+len (MEASURED). A terminator "
        "written by us is one byte past the length it was given and overruns a "
        "buffer sized exactly to the name.",
    },
    {
        "id": "t94-querydir-information-omits-the-header",
        "file": FLUSH_IO,
        "old": "    (void)write_io_status(io_status, STATUS_SUCCESS, DIRECTORY_INFO_HEADER_BYTES "
        "+ name_length,",
        "new": "    (void)write_io_status(io_status, STATUS_SUCCESS, name_length,",
        "targets": ["test_hdd_backing"],
        "why": "Information is the number of bytes written, header included.",
    },
    {
        "id": "t94-querydir-information-counts-a-terminator",
        "file": FLUSH_IO,
        "old": "    (void)write_io_status(io_status, STATUS_SUCCESS, DIRECTORY_INFO_HEADER_BYTES "
        "+ name_length,",
        "new": "    (void)write_io_status(io_status, STATUS_SUCCESS, DIRECTORY_INFO_HEADER_BYTES "
        "+ name_length + 1u,",
        "targets": ["test_hdd_backing"],
        "why": "one byte too many, claiming the NUL this module never writes.",
    },
    {
        "id": "t94-querydir-end-of-listing-leaves-the-status-block-stale",
        "file": FLUSH_IO,
        "old": "        (void)write_io_status(io_status, status, 0u, who);\n        return "
        "status;\n    }\n\n"
        "    const uint32_t name_length",
        "new": "        return status;\n    }\n\n    const uint32_t name_length",
        "targets": ["test_hdd_backing"],
        "why": "site 1 compares both NO_MORE_FILES and NO_SUCH_FILE, from EAX, but a "
        "caller reading the block would see the previous entry's SUCCESS.",
    },
    {
        "id": "t94-querydir-end-of-listing-counted-as-an-entry",
        "file": FLUSH_IO,
        "old": "        (void)write_io_status(io_status, status, 0u, who);\n        return "
        "status;\n    }\n\n"
        "    const uint32_t name_length",
        "new": "        (void)write_io_status(io_status, status, 0u, who);\n        "
        "directory_entry_count++;\n"
        "        return status;\n    }\n\n    const uint32_t name_length",
        "targets": ["test_hdd_backing"],
        "why": "the entry counter is the evidence a listing returned entries. Counting "
        "the end marker overstates it by one per listing.",
    },
    {
        "id": "t94-querydir-entry-never-counted",
        "file": FLUSH_IO,
        "old": "    directory_entry_count++;",
        "new": "    (void)directory_entry_count;",
        "targets": ["test_hdd_backing"],
        "why": "a listing that returns entries and reports none.",
    },
    {
        "id": "t94-querydir-buffer-room-ignores-the-header",
        "file": FLUSH_IO,
        "old": "                              length - DIRECTORY_INFO_HEADER_BYTES, &entry, "
        "&status)) {",
        "new": "                              length, &entry, &status)) {",
        "targets": ["test_hdd_backing"],
        "why": "the name room is Length minus the 0x40 header. Passing Length lets a "
        "name that overflows the buffer by up to 0x40 bytes through.",
    },
    # ------------------------------------------------- the enumeration in kernel_file.c
    {
        "id": "t94-querydir-file-handle-listed-as-a-directory",
        "file": FILE,
        "old": "    if (!entry->state.is_directory) {\n"
        "        *out_status = STATUS_INVALID_PARAMETER;\n"
        '        kernel_hle_log()("kernel: directory query on',
        "new": "    if (!entry->state.is_directory && false) {\n"
        "        *out_status = STATUS_INVALID_PARAMETER;\n"
        '        kernel_hle_log()("kernel: directory query on',
        "targets": ["test_hdd_backing"],
        "why": "a regular file handle must be refused with INVALID_PARAMETER, not "
        "listed as if its host descriptor were a directory.",
    },
    {
        "id": "t94-querydir-disc-directory-listed-empty",
        "file": FILE,
        "old": "    if (entry->state.backing != KERNEL_FILE_BACKING_HOST_DIR ||\n"
        "        entry->dir_fd_plus_one == 0) {",
        "new": "    if ((entry->state.backing != KERNEL_FILE_BACKING_HOST_DIR ||\n"
        "        entry->dir_fd_plus_one == 0) && false) {",
        "targets": ["test_hdd_backing"],
        "why": "only an --hdd host directory is measured. Any other backing must be "
        "NOT_IMPLEMENTED, never an empty listing a title reads as 'no files'.",
    },
    {
        "id": "t94-querydir-unlistable-backing-wrong-status",
        "file": FILE,
        "old": "        *out_status = STATUS_NOT_IMPLEMENTED;\n"
        '        kernel_hle_log()("kernel: listing',
        "new": "        *out_status = STATUS_UNSUCCESSFUL;\n"
        '        kernel_hle_log()("kernel: listing',
        "targets": ["test_hdd_backing"],
        "why": "NOT_IMPLEMENTED is how an unmeasured backing is told apart from an I/O failure.",
    },
    {
        "id": "t94-querydir-restart-does-not-rewind",
        "file": FILE,
        "old": "    if (restart) {\n        entry->dir_started = false;",
        "new": "    if (restart && false) {\n        entry->dir_started = false;",
        "targets": ["test_hdd_backing"],
        "why": "the first call starts at the top either way, so the defect shows only "
        "when a listing is replayed after it ran out.",
    },
    {
        "id": "t94-querydir-later-mask-restarts-the-listing",
        "file": FILE,
        "old": "    if (!entry->dir_started) {\n        entry->dir_started = true;",
        "new": "    if (!entry->dir_started || mask) {\n        entry->dir_started = true;",
        "targets": ["test_hdd_backing"],
        "why": "the mask is fixed by the first call (MEASURED: only the first call at "
        "0x00381AC8 carries one). Honouring a later mask also resets the cursor, so a "
        "caller listing with a pattern loops on the first match.",
    },
    {
        "id": "t94-querydir-rewinddir-skipped",
        "file": FILE,
        "old": "    rewinddir(dir);",
        "new": "    (void)dir;",
        "targets": ["test_hdd_backing"],
        "why": "the duplicated descriptor shares its directory offset with the stored "
        "one, so without the rewind a second call starts where the first stopped "
        "and finds nothing.",
    },
    {
        "id": "t94-querydir-dot-entry-listed",
        "file": FILE,
        "old": '        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {',
        "new": '        if (strcmp(name, "..") == 0) {',
        "targets": ["test_hdd_backing"],
        "why": "FATX has no dot entries (INFERRED) and the delete loop would recurse "
        "into the directory itself forever.",
    },
    {
        "id": "t94-querydir-dotdot-entry-listed",
        "file": FILE,
        "old": '        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {',
        "new": '        if (strcmp(name, ".") == 0) {',
        "targets": ["test_hdd_backing"],
        "why": "listing the parent lets a recursive delete walk out of the volume.",
    },
    {
        "id": "t94-querydir-mask-ignored",
        "file": FILE,
        "old": "            !dir_mask_matches(entry->dir_mask, name)) {",
        "new": '            !dir_mask_matches("", name)) {',
        "targets": ["test_hdd_backing"],
        "why": "every pattern lists everything, so a title searching for one save "
        "slot would treat the first file of any name as the slot.",
    },
    {
        "id": "t94-querydir-cursor-allows-the-same-name-again",
        "file": FILE,
        "old": "dir_name_compare(name, entry->dir_last) <= 0) {",
        "new": "dir_name_compare(name, entry->dir_last) < 0) {",
        "targets": ["test_hdd_backing"],
        "why": "the cursor is the last name returned and the next entry is strictly "
        "greater. `<` returns the same entry forever.",
    },
    {
        "id": "t94-querydir-cursor-ignored-after-the-first-call",
        "file": FILE,
        "old": "        if (entry->dir_has_last && dir_name_compare(name, entry->dir_last) <= 0) {",
        "new": "        if (false && entry->dir_has_last && dir_name_compare(name, "
        "entry->dir_last) <= 0) {",
        "targets": ["test_hdd_backing"],
        "why": "without the cursor every call answers the first entry.",
    },
    {
        "id": "t94-querydir-picks-the-largest-name-not-the-smallest",
        "file": FILE,
        "old": "        if (found && dir_name_compare(name, best) >= 0) {",
        "new": "        if (found && dir_name_compare(name, best) <= 0) {",
        "targets": ["test_hdd_backing"],
        "why": "the listing would run in descending order, which is a plausible order "
        "to the guest and wrong against the on-disk FATX order the title saw.",
    },
    {
        "id": "t94-querydir-host-link-or-device-node-listed",
        "file": FILE,
        "old": "(!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))) {",
        "new": "(!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode) && false)) {",
        "targets": ["test_hdd_backing"],
        "why": "the open path refuses a planted link, so listing one hands the title "
        "a name it can never open.",
    },
    {
        "id": "t94-querydir-stat-follows-host-symlinks",
        "file": FILE,
        "old": "        if (fstatat(dir_fd, name, &info, AT_SYMLINK_NOFOLLOW) != 0 ||",
        "new": "        if (fstatat(dir_fd, name, &info, 0) != 0 ||",
        "targets": ["test_hdd_backing"],
        "why": "following the link turns a link to a directory into a listed "
        "directory, and its size and times come from outside the volume.",
    },
    {
        "id": "t94-querydir-first-empty-match-reports-no-more-files",
        "file": FILE,
        "old": "        *out_status = entry->dir_has_last ? KERNEL_FILE_STATUS_NO_MORE_FILES\n"
        "                                          : KERNEL_FILE_STATUS_NO_SUCH_FILE;",
        "new": "        *out_status = KERNEL_FILE_STATUS_NO_MORE_FILES;",
        "targets": ["test_hdd_backing"],
        "why": "NT answers NO_SUCH_FILE when the first call matches nothing. Site 1 "
        "compares both, but other consumers do not.",
    },
    {
        "id": "t94-querydir-exhausted-listing-reports-no-such-file",
        "file": FILE,
        "old": "        *out_status = entry->dir_has_last ? KERNEL_FILE_STATUS_NO_MORE_FILES\n"
        "                                          : KERNEL_FILE_STATUS_NO_SUCH_FILE;",
        "new": "        *out_status = KERNEL_FILE_STATUS_NO_SUCH_FILE;",
        "targets": ["test_hdd_backing"],
        "why": "the end of a non-empty listing is NO_MORE_FILES.",
    },
    {
        "id": "t94-querydir-oversized-name-fits-anyway",
        "file": FILE,
        "old": "    if (strlen(best) > max_name_bytes) {",
        "new": "    if (strlen(best) > max_name_bytes && false) {",
        "targets": ["test_hdd_backing"],
        "why": "a name longer than the caller's buffer would be written past it.",
    },
    {
        "id": "t94-querydir-buffer-too-small-advances-the-cursor",
        "file": FILE,
        "old": "    if (strlen(best) > max_name_bytes) {\n"
        "        *out_status = KERNEL_FILE_STATUS_INFO_LENGTH_MISMATCH;",
        "new": "    if (memcpy(entry->dir_last, best, strlen(best) + 1u), entry->dir_has_last = "
        "true,\n"
        "        strlen(best) > max_name_bytes) {\n"
        "        *out_status = KERNEL_FILE_STATUS_INFO_LENGTH_MISMATCH;",
        "targets": ["test_hdd_backing"],
        "why": "a refused entry must stay the next one, or a retry with a larger buffer "
        "silently skips it.",
    },
    {
        "id": "t94-querydir-too-small-reports-the-wrong-status",
        "file": FILE,
        "old": "        *out_status = KERNEL_FILE_STATUS_INFO_LENGTH_MISMATCH;\n"
        '        kernel_hle_log()("kernel: listing',
        "new": "        *out_status = KERNEL_FILE_STATUS_NO_MORE_FILES;\n"
        '        kernel_hle_log()("kernel: listing',
        "targets": ["test_hdd_backing"],
        "why": "reporting the end of the listing for a buffer that is merely too small "
        "makes the title stop with entries unread.",
    },
    {
        "id": "t94-querydir-directory-flag-inverted",
        "file": FILE,
        "old": "    out->is_directory = S_ISDIR(best_info.st_mode);",
        "new": "    out->is_directory = !S_ISDIR(best_info.st_mode);",
        "targets": ["test_hdd_backing"],
        "why": "files list as directories and the other way round.",
    },
    {
        "id": "t94-querydir-access-time-from-the-modification-time",
        "file": FILE,
        "old": "dir_filetime(best_info.st_atim.tv_sec, best_info.st_atim.tv_nsec);",
        "new": "dir_filetime(best_info.st_mtim.tv_sec, best_info.st_mtim.tv_nsec);",
        "targets": ["test_hdd_backing"],
        "why": "the fixture stamps access and write apart, so the two are distinguishable.",
    },
    {
        "id": "t94-querydir-write-time-from-the-access-time",
        "file": FILE,
        "old": "dir_filetime(best_info.st_mtim.tv_sec, best_info.st_mtim.tv_nsec);\n"
        "    /* Linux keeps no birth time",
        "new": "dir_filetime(best_info.st_atim.tv_sec, best_info.st_atim.tv_nsec);\n"
        "    /* Linux keeps no birth time",
        "targets": ["test_hdd_backing"],
        "why": "the last-write time is the date a title shows for a save.",
    },
    {
        "id": "t94-querydir-filetime-epoch-offset-off-by-one-second",
        "file": FILE,
        "old": "    return ((uint64_t)seconds + 11644473600ull) * 10000000ull +",
        "new": "    return ((uint64_t)seconds + 11644473601ull) * 10000000ull +",
        "targets": ["test_hdd_backing"],
        "why": "the 1601-to-1970 offset, off by one second, is a plausible date.",
    },
    {
        "id": "t94-querydir-filetime-scale-wrong",
        "file": FILE,
        "old": "    return ((uint64_t)seconds + 11644473600ull) * 10000000ull +",
        "new": "    return ((uint64_t)seconds + 11644473600ull) * 1000000ull +",
        "targets": ["test_hdd_backing"],
        "why": "FILETIME counts 100 ns ticks, so the scale is 10^7 per second.",
    },
    {
        "id": "t94-querydir-cursor-name-not-recorded",
        "file": FILE,
        "old": "    memcpy(entry->dir_last, best, strlen(best) + 1u);\n    entry->dir_has_last = "
        "true;",
        "new": "    entry->dir_has_last = true;",
        "targets": ["test_hdd_backing"],
        "why": "the cursor is the last name returned. Without recording it the next "
        "call resumes after an empty name and repeats the first entry.",
    },
    {
        "id": "t94-querydir-cursor-flag-not-set",
        "file": FILE,
        "old": "    memcpy(entry->dir_last, best, strlen(best) + 1u);\n    entry->dir_has_last = "
        "true;",
        "new": "    memcpy(entry->dir_last, best, strlen(best) + 1u);",
        "targets": ["test_hdd_backing"],
        "why": "recording the name without arming the cursor repeats the first entry.",
    },
    # ------------------------------------------------------------ matching and ordering
    {
        "id": "t94-querydir-empty-mask-matches-nothing",
        "file": FILE,
        "old": "    if (mask[0] == '\\0' || strcmp(mask, \"*.*\") == 0) {",
        "new": '    if (strcmp(mask, "*.*") == 0) {',
        "targets": ["test_hdd_backing"],
        "why": "the guest turns '*.*' into a zero-length mask (0x00381A6B..0x00381A82), "
        "so zero length must mean everything.",
    },
    {
        "id": "t94-querydir-mask-is-case-sensitive",
        "file": FILE,
        "old": "(*mask != '\\0' && dir_fold(*mask) == dir_fold(*name))) {",
        "new": "(*mask != '\\0' && *mask == *name)) {",
        "targets": ["test_hdd_backing"],
        "why": "FATX names compare case-insensitively.",
    },
    {
        "id": "t94-querydir-question-mark-is-literal",
        "file": FILE,
        "old": "        } else if (*mask == '?' || (*mask != '\\0'",
        "new": "        } else if (false || (*mask != '\\0'",
        "targets": ["test_hdd_backing"],
        "why": "'?' matches exactly one character.",
    },
    {
        "id": "t94-querydir-star-does-not-backtrack",
        "file": FILE,
        "old": "        } else if (star) {\n            mask = star + 1;",
        "new": "        } else if (star && false) {\n            mask = star + 1;",
        "targets": ["test_hdd_backing"],
        "why": "'*.xbx' must retry the star after a failed literal.",
    },
    {
        "id": "t94-querydir-mask-accepts-a-leftover-pattern",
        "file": FILE,
        "old": "    while (*mask == '*') {\n        mask++;\n    }\n    return *mask == '\\0';",
        "new": "    while (*mask == '*') {\n        mask++;\n    }\n    return true;",
        "targets": ["test_hdd_backing"],
        "why": "a name consumed before the mask is must not match: 'ab' against 'abc'.",
    },
    {
        "id": "t94-querydir-order-is-descending",
        "file": FILE,
        "old": "            return fa < fb ? -1 : 1;",
        "new": "            return fa < fb ? 1 : -1;",
        "targets": ["test_hdd_backing"],
        "why": "the cursor order is ascending.",
    },
    {
        "id": "t94-querydir-order-is-byte-order",
        "file": FILE,
        "old": "        const unsigned char fa = dir_fold(a[i]);",
        "new": "        const unsigned char fa = (unsigned char)a[i];",
        "targets": ["test_hdd_backing"],
        "why": "ASCII byte order puts every capital before every lowercase name, "
        "which is not the case-folded order FATX lists in.",
    },
    {
        "id": "t94-querydir-fold-offset-wrong",
        "file": FILE,
        "old": "(unsigned char)(value + 32u) : value;",
        "new": "(unsigned char)(value + 33u) : value;",
        "targets": ["test_hdd_backing"],
        "why": "the fold distance is 32. An off-by-one folds 'A' onto 'b'.",
    },
    # ---------------------------------------------------------------- T268 gap closures
    {
        "id": "t8-flush-io-status-write-failure-reported-as-success",
        "file": FLUSH_IO,
        "old": "                         (unsigned)io_status);\n        return STATUS_INVALID_PARAM"
        "ETER;\n    }\n    lock();\n    flush_count++;",
        "new": "                         (unsigned)io_status);\n        return STATUS_SUCCESS;\n   "
        " }\n    lock();\n    flush_count++;",
        "targets": ["test_kernel_io"],
        "why": "a flush whose IO_S"
        "TATUS_BLOCK cannot be written must say so. Success leaves the guest's status block "
        "holding stale stack bytes that its wrapper then reads as the result.",
    },
    {
        "id": "t8-querydir-apc-routine-ignored",
        "file": FLUSH_IO,
        "old": "    if (args[1] != 0u || args[2] != 0u || args[3] != 0u) {",
        "new": "    if (args[1] != 0u || args[3] != 0u) {",
        "targets": ["test_hdd_backing"],
        "why": "ApcRoutine is asyn"
        "chronous completio"
        "n, which this host "
        "never delivers. Ignoring it while the other two are checked would complete a request"
        " the guest expects to be signalled later, and the suite only probed Event.",
    },
    {
        "id": "t8-querydir-apc-context-ignored",
        "file": FLUSH_IO,
        "old": "    if (args[1] != 0u || args[2] != 0u || args[3] != 0u) {",
        "new": "    if (args[1] != 0u || args[2] != 0u) {",
        "targets": ["test_hdd_backing"],
        "why": "the ApcContext partner of the previous entry: each of the three unmeasured argument"
        "s must refuse on its own, not only when Event is set.",
    },
    {
        "id": "t8-querydir-null-buffer-not-refused",
        "file": FLUSH_IO,
        "old": "    if (information == 0u || length < DIRECTORY_INFO_HEADER_BYTES) {",
        "new": "    if (length < DIRECTORY_INFO_HEADER_BYTES) {",
        "targets": ["test_hdd_backing"],
        "why": "a null FileInforma"
        "tion is a length m"
        "ismatch decided BEF"
        "ORE the scan. Witho"
        "ut the test the entry is consumed from the cursor and the write to address 0 then fa"
        "ils as INVALID_PARAMETER, so a retry with a real buffer silently skips a file.",
    },
    {
        "id": "t8-querydir-unreadable-mask-ignored",
        "file": FLUSH_IO,
        "old": "    if (mask_string != 0u && !read_directory_mask(mask_string, mask, sizeof(mask))"
        ") {",
        "new": "    if (mask_string != 0u && !read_directory_mask(mask_string, mask, sizeof(mask)) "
        "&& false) {",
        "targets": ["test_hdd_backing"],
        "why": "a FileName the gue"
        "st cannot read must be refused. Ignoring the failed read lists the WHOLE directory "
        "for a title that asked for one name, which XapiNukeDirectory-style loops then delete.",
    },
    {
        "id": "t8-querydir-restart-reads-the-whole-dword",
        "file": FLUSH_IO,
        "old": "    const bool restart = (args[9] & 0xFFu) != 0u;",
        "new": "    const bool restart = args[9] != 0u;",
        "targets": ["test_hdd_backing"],
        "why": "RestartScan is a B"
        "OOLEAN pushed as a"
        " byte: stale upper bits of the stack dword mean nothing. Reading the dword rewinds a"
        " scan the title meant to continue and an enumeration loop never finishes.",
    },
    {
        "id": "t8-querydir-file-index-nonzero",
        "file": FLUSH_IO,
        "old": "        !kernel_guest_write_u32(information + 4u, 0u) ||",
        "new": "        !kernel_guest_write_u32(information + 4u, 1u) ||",
        "targets": ["test_hdd_backing"],
        "why": "FileIndex is docum"
        "ented as undefined for FAT-like volumes and the structure's measured shape writes 0"
        ". A nonzero value is a plausible-looking resume key a title could feed back.",
    },
    {
        "id": "t8-querydir-change-time-is-the-access-time",
        "file": FLUSH_IO,
        "old": "        !write_u64(information + DIRECTORY_INFO_CHANGE_OFFSET, entry.last_write_tim"
        "e) ||",
        "new": "        !write_u64(information + DIRECTORY_INFO_CHANGE_OFFSET, entry.last_access_ti"
        "me) ||",
        "targets": ["test_hdd_backing"],
        "why": "change time follows last-write (INFERRED). Taking the access time instead moves eve"
        "ry listed file's change stamp whenever it is merely read.",
    },
    {
        "id": "t8-querydir-allocation-rounds-down",
        "file": FLUSH_IO,
        "old": "    const uint64_t allocation = (entry.size + 4095u) / 4096u * 4096u;",
        "new": "    const uint64_t allocation = entry.size / 4096u * 4096u;",
        "targets": ["test_hdd_backing"],
        "why": "allocation must cover the size. Rounding down reports a 1-byte file as occupying 0 "
        "bytes.",
    },
    {
        "id": "t8-querydir-allocation-adds-a-whole-page",
        "file": FLUSH_IO,
        "old": "    const uint64_t allocation = (entry.size + 4095u) / 4096u * 4096u;",
        "new": "    const uint64_t allocation = (entry.size + 4096u) / 4096u * 4096u;",
        "targets": ["test_hdd_backing"],
        "why": "an off-by-one in the round-up reports an empty file as one page and an exact multip"
        "le as one page too many.",
    },
    {
        "id": "t8-querydir-restart-keeps-the-has-last-flag",
        "file": FILE,
        "old": "        entry->dir_started = true;\n        entry->dir_has_last = false;\n",
        "new": "        entry->dir_started = true;\n",
        "targets": ["test_hdd_backing"],
        "why": "a restart begins a"
        " fresh scan, so an"
        " empty answer is NO_SUCH_FILE. The kept flag answers NO_MORE_FILES instead, which th"
        "e title's cleanup loop treats as the end of a listing it had already begun.",
    },
    {
        "id": "t8-querydir-exact-fit-name-refused",
        "file": FILE,
        "old": "    if (strlen(best) > max_name_bytes) {",
        "new": "    if (strlen(best) >= max_name_bytes) {",
        "targets": ["test_hdd_backing"],
        "why": "a name that exactl"
        "y fills the room is legal. `>=` refuses it with INFO_LENGTH_MISMATCH and the title "
        "that sized its buffer from the name length can never read that entry.",
    },
    {
        "id": "t8-querydir-filetime-subsecond-divisor-wrong",
        "file": FILE,
        "old": "           (uint64_t)(nanoseconds / 100);",
        "new": "           (uint64_t)(nanoseconds / 1000);",
        "targets": ["test_hdd_backing"],
        "why": "a FILETIME tick is 100 ns. Dividing by 1000 loses an order of magnitude of every li"
        "sted timestamp's sub-second part, invisible to a test whose stamps are whole seconds.",
    },
    {
        "id": "t8-querydir-dos-star-dot-star-is-a-plain-wildcard",
        "file": FILE,
        "old": "    if (mask[0] == '\\0' || strcmp(mask, \"*.*\") == 0) {",
        "new": "    if (mask[0] == '\\0') {",
        "targets": ["test_hdd_backing"],
        "why": '"*.*" is the DOS spelling of everything. As a plain wildcard it demands a dot, so a'
        " name without an extension disappears from the listing.",
    },
    {
        "id": "t8-querydir-trailing-star-not-skipped",
        "file": FILE,
        "old": "    while (*mask == '*') {\n        mask++;\n    }\n    return *mask == '\\0';",
        "new": "    return *mask == '\\0';",
        "targets": ["test_hdd_backing"],
        "why": 'a trailing `*` matches the empty remainder: "abc*" must find "abc". Without the ski'
        "p the name is exhausted with the mask unfinished and the match fails.",
    },
    {
        "id": "t8-querydir-fold-range-excludes-z",
        "file": FILE,
        "old": "    return (value >= 'A' && value <= 'Z') ? (unsigned char)(value + 32u) : value;",
        "new": "    return (value >= 'A' && value < 'Z') ? (unsigned char)(value + 32u) : value;",
        "targets": ["test_hdd_backing"],
        "why": "the case fold must cover the whole alphabet. Leaving out 'Z' makes \"Zebra\" miss a"
        ' mask "zebra" and sorts it before every lowercase name.',
    },
    {
        "id": "t8-querydir-case-only-different-names-equal",
        "file": FILE,
        "old": "            return strcmp(a, b);",
        "new": "            return 0;",
        "targets": ["test_hdd_backing"],
        "why": "the byte-order tie-break is what makes the cursor strictly increasing. Equal names "
        "make the cursor skip the second of two names that differ only in case.",
    },
    {
        "id": "t8-querydir-directory-reports-the-host-inode-size",
        "file": FILE,
        "old": "    out->size = out->is_directory ? 0u : (uint64_t)best_info.st_size;",
        "new": "    out->size = (uint64_t)best_info.st_size;",
        "targets": ["test_hdd_backing"],
        "why": "a HOST_DIR directo"
        "ry reports 0 (FATX"
        " has no directory size). The inode's own st_size is 0 on overlayfs and nonzero on ex"
        "t4 and tmpfs, so only a listing on a filesystem with sized directories sees it.",
    },
]
