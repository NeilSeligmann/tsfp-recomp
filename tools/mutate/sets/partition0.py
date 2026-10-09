"""Mutations for the VIRTUAL RAW DEVICE behind `\\Device\\Harddisk0\\partition0`.

WHAT THIS SET PROTECTS. `kernel_file_mount_host_device` hands the guest a read/write device
that is really one regular file on the operator's disk. The ways that goes wrong are not
symmetric: a device that resolves for the wrong names, grows without bound, truncates what the
title wrote, or is aimed at a host object that is not a regular file is a safety problem. A
device that reads back the wrong bytes is a correctness one, and the title branches on those
bytes (a magic dword, then a table it rewrites). Entries are grouped by the property attacked:

  1. THE DEVICE ANSWERS ONLY TO ITS NAME  p0-name-inside-resolves, p0-create-guard-removed
  2. READS ARE WHAT WAS WRITTEN, AND THE   p0-zero-fill-skipped, p0-zero-count-dropped,
     REST IS ANNOUNCED ZEROS               p0-claim-does-not-count
  3. A PARTITION ENDS                      p0-read-ignores-capacity, p0-write-cap-removed,
                                           p0-write-cap-offset-half
  4. A DEVICE HAS NO LENGTH TO CHANGE      p0-eof-not-refused, p0-overwrite-not-refused,
                                           p0-mount-truncates
  5. NEVER A RAW HOST OBJECT, NEVER        p0-mount-type-precheck, p0-mount-size-check,
     UNBOUNDED                             p0-capacity-ceiling, p0-name-separator
  6. WHAT THE TITLE SEES IS THE DEVICE'S   p0-size-is-file-size
  7. A RESET FORGETS THE RUN               p0-reset-keeps-opened, p0-reset-keeps-zeros

THREE BRANCHES ARE DELIBERATELY ABSENT, said here rather than left to be noticed.
  - The `..` and `.` arm of the name check in the mount. It is redundant by construction: a
    directory named `..` or `.` already exists, so the `fstatat` type pre-check refuses it
    first, and a mutation of the arm alone cannot change any result. Mutating it would
    survive on every run and be rationalised away.
  - `host_dir` being a file rather than a directory (`O_DIRECTORY`). That open is the same
    line `kernel_file_mount_host_dir` uses and is mutated by the hdd suite's own entries.
  - A failed `openat` for the backing file other than ELOOP and EISDIR (ENOSPC, EACCES). No
    suite can make the host filesystem refuse on demand without privileges.

THE ENTRIES USE `&& false` rather than `false`, per `_example.py`: a mutation that fails to
compile scores NOT-A-MUTANT, which reads like evidence while meaning nothing was injected.
"""

KERNEL_FILE = "src/xbox/kernel_file.c"
TARGETS = ["test_hdd_backing"]

MUTATIONS: list[dict] = [
    # ---- 1. the device answers only to its name -----------------------------------
    {
        "id": "p0-name-inside-resolves",
        "file": KERNEL_FILE,
        "old": "    if (is_device && rest[0] != '\\0') {",
        "new": "    if (is_device && rest[0] != '\\0' && false) {",
        "targets": TARGETS,
        "why": "Without this refusal `partition0\\anything` and `partition0\\` both reach "
        "the device, because the leaf is replaced by the backing file's name afterwards. A "
        "raw device has no namespace, and a title that probed a path inside it would be "
        "told it exists. The exact-name test is the only thing standing between the "
        "device and every name under it.",
    },
    {
        "id": "p0-create-guard-removed",
        "file": KERNEL_FILE,
        "old": "    if (volume->backing != KERNEL_FILE_BACKING_HOST_DIR || "
        "names_raw_view(volume, rest)) {",
        "new": "    if (volume->backing != KERNEL_FILE_BACKING_HOST_DIR || "
        "(names_raw_view(volume, rest) && false)) {",
        "targets": TARGETS,
        "why": "The create path is reachable with the device's own name only when the "
        "backing file has vanished. Without the guard the leaf-less walk answers with a "
        "name collision for an object that does not exist, which is a wrong answer from "
        "the one function that creates things on the operator's disk.",
    },
    # ---- 2. reads are what was written, the rest is announced zeros ----------------
    {
        "id": "p0-zero-fill-skipped",
        "file": KERNEL_FILE,
        "old": "        if (is_device && done < wanted) {",
        "new": "        if (is_device && done < wanted && false) {",
        "targets": TARGETS,
        "why": "THE MEASURED READ. The title reads 0x200 bytes of a fresh device and tests "
        "a magic dword in them. Without the zero-fill the guest's buffer keeps whatever "
        "was on its stack, so the config table is decided by stack garbage and the "
        "result changes from run to run. The count would also still say 0x200 bytes "
        "arrived, which makes it the hardest wrong answer to see.",
    },
    {
        "id": "p0-zero-count-dropped",
        "file": KERNEL_FILE,
        "old": "            device_zero_bytes += (uint64_t)fabricated;",
        "new": "            device_zero_bytes += 0u;",
        "targets": TARGETS,
        "why": "The fabricated-zero counter is how a run report tells ours from a "
        "console's. If it stays at zero the report claims the config area held content "
        "the title never wrote.",
    },
    {
        "id": "p0-claim-does-not-count",
        "file": KERNEL_FILE,
        "old": "        if (from->device) {\n            device_opened_count++;\n        }",
        "new": "        if (from->device && false) {\n"
        "            device_opened_count++;\n"
        "        }",
        "targets": TARGETS,
        "why": "The device-open counter is the only record that a run opened the "
        "fabricated device at all. A run report built on it would say the title never "
        "touched partition0.",
    },
    # ---- 3. a partition ends -------------------------------------------------------
    {
        "id": "p0-read-ignores-capacity",
        "file": KERNEL_FILE,
        "old": "            wanted = (uint64_t)length > left ? (uint32_t)left : length;",
        "new": "            wanted = ((uint64_t)length > left && false) ? (uint32_t)left : length;",
        "targets": TARGETS,
        "why": "A read that straddles the end must stop at it. Without the clamp the "
        "device serves fabricated zeros forever past its own size, so a title that "
        "reads until short count never sees the end.",
    },
    {
        "id": "p0-write-cap-removed",
        "file": KERNEL_FILE,
        "old": "    if (entry->state.device &&\n        (offset > device->device_capacity ||",
        "new": "    if (entry->state.device && false &&\n"
        "        (offset > device->device_capacity ||",
        "targets": TARGETS,
        "why": "THE BOUND ON THE OPERATOR'S DISK. Without it a write at any 64-bit offset "
        "extends the backing file to that size, so a guest can make a sparse file as "
        "large as the host filesystem allows.",
    },
    {
        "id": "p0-write-cap-offset-half",
        "file": KERNEL_FILE,
        "old": "        (offset > device->device_capacity ||",
        "new": "        ((offset > device->device_capacity && false) ||",
        "targets": TARGETS,
        "why": "The second half of the bound subtracts the offset from the capacity, which "
        "wraps for an offset past the end. Without the first half a write at an offset "
        "beyond the capacity passes the length test through the wrapped value, which is "
        "the classic way this kind of bound is bypassed.",
    },
    # ---- 4. a device has no length to change ---------------------------------------
    {
        "id": "p0-eof-not-refused",
        "file": KERNEL_FILE,
        "old": "    if (entry->state.device) {\n        /* A device has no length",
        "new": "    if (entry->state.device && false) {\n        /* A device has no length",
        "targets": TARGETS,
        "why": "Setting the end of file on the handle truncates the backing file, which "
        "silently erases what the title wrote there. A destructive operation reaching "
        "an object that has no length to change.",
    },
    {
        "id": "p0-overwrite-not-refused",
        "file": KERNEL_FILE,
        "old": "            !resolved_to.writable || resolved_to.device) {",
        "new": "            !resolved_to.writable) {",
        "targets": TARGETS,
        "why": "FILE_SUPERSEDE on the device name truncates the backing file through the "
        "generic overwrite arm, which cannot tell a device from a file. This is the "
        "other way to erase the title's config without ever asking to change a length.",
    },
    {
        "id": "p0-mount-truncates",
        "file": KERNEL_FILE,
        "old": "O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);",
        "new": "O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);",
        "targets": TARGETS,
        "why": "The whole point of a file-backed device is that what the title wrote "
        "survives the run. Truncating at mount makes every boot a first boot, so the "
        "title re-selects and re-formats its cache partition every time.",
    },
    # ---- 5. never a raw host object, never unbounded -------------------------------
    {
        "id": "p0-mount-type-precheck",
        "file": KERNEL_FILE,
        "old": "    if (fstatat(root_fd, file_name, &existing, AT_SYMLINK_NOFOLLOW) == 0 &&\n"
        "        !S_ISREG(existing.st_mode)) {",
        "new": "    if (fstatat(root_fd, file_name, &existing, AT_SYMLINK_NOFOLLOW) == 0 &&\n"
        "        !S_ISREG(existing.st_mode) && false) {",
        "targets": TARGETS,
        "why": "THE CLAIM 'LOOKED AT BEFORE IT IS OPENED'. Opening a device node or a FIFO "
        "read/write is already an action. The post-open `fstat` still refuses, so the "
        "RESULT does not change, which is why the test asserts the refusal's wording "
        "and not only that it refused. If this survives, the ordering claim in "
        "`kernel_file.h` is a comment.",
    },
    {
        "id": "p0-mount-size-check",
        "file": KERNEL_FILE,
        "old": "                        (uint64_t)opened.st_size <= capacity_bytes;",
        "new": "                        true;",
        "targets": TARGETS,
        "why": "An existing file larger than the capacity would be adopted as a device, "
        "so a title could be handed (and write over) a big file the operator already "
        "had there under that name.",
    },
    {
        "id": "p0-capacity-ceiling",
        "file": KERNEL_FILE,
        "old": "    if (capacity_bytes == 0u || "
        "capacity_bytes > KERNEL_FILE_DEVICE_CAPACITY_MAX) {",
        "new": "    if (capacity_bytes == 0u || "
        "(capacity_bytes > KERNEL_FILE_DEVICE_CAPACITY_MAX && false)) {",
        "targets": TARGETS,
        "why": "The ceiling is what turns a wrong capacity argument into a refusal instead "
        "of permission to write a sparse file of any size.",
    },
    {
        "id": "p0-name-separator",
        "file": KERNEL_FILE,
        "old": "        strchr(file_name, '/') != NULL || strchr(file_name, '\\\\') != NULL ||",
        "new": "        false || strchr(file_name, '\\\\') != NULL ||",
        "targets": TARGETS,
        "why": "A separator in the backing name lets the caller aim the device at a file in "
        "a subdirectory, and a `/` is the spelling a host path actually uses. The test "
        "plants a real subdirectory so the mutant SUCCEEDS in creating the file rather "
        "than failing on a missing parent.",
    },
    # ---- 6. what the title sees is the device's size -------------------------------
    {
        "id": "p0-size-is-file-size",
        "file": KERNEL_FILE,
        "old": "        out->size = volume->device_capacity;",
        "new": "        out->size = (uint64_t)info.st_size;",
        "targets": TARGETS,
        "why": "NtQueryInformationFile would report however much of the device has been "
        "written, and the guest tests that size against zero to decide a file is empty. "
        "A partition does not grow as it is used.",
    },
    # ---- 7. a reset forgets the run ------------------------------------------------
    {
        "id": "p0-reset-keeps-opened",
        "file": KERNEL_FILE,
        "old": "    device_opened_count = 0u;\n    device_zero_bytes = 0u;",
        "new": "    device_zero_bytes = 0u;",
        "targets": TARGETS,
        "why": "A counter that survives reset leaks one test's opens into the next, and in "
        "the host would leak a previous mount's count into a new run's report.",
    },
    {
        "id": "p0-reset-keeps-zeros",
        "file": KERNEL_FILE,
        "old": "    device_opened_count = 0u;\n    device_zero_bytes = 0u;",
        "new": "    device_opened_count = 0u;",
        "targets": TARGETS,
        "why": "Same property for the fabricated-zero total, which a report quotes as how "
        "much of what the title read was ours.",
    },
]
