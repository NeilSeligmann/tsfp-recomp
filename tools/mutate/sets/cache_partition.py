"""Mutations for the FORMATTABLE CACHE PARTITION: `kernel_file_mount_cache_partition` and the
two control ordinals (196 NtDeviceIoControlFile, 200 NtFsControlFile) that answer its raw view.

WHAT THIS SET PROTECTS. One guest name, `\\Device\\Harddisk0\\Partition5`, is two things. With no
separator it is a raw device the title formats with its own `XapiFormatFATVolumeEx`. With one it
is a real host directory the title then validates and binds to `Z:`. The directory view must not
exist until the title's own format has written "FATX" at offset 0 of the image, because that is
what makes the title format at all. The ways it goes wrong are not symmetric. A directory view
that exists too early means the title validates an empty directory and never formats. A gate that
latches, or compares a prefix, answers the wrong thing about an image the title is about to
rewrite. A directory opened without O_NOFOLLOW leads the title's cache out of the backing
directory. Entries are grouped by the property attacked:

  1. TWO VIEWS, ONE NAME            cache-view-always-raw, cache-walk-from-root,
                                    cache-claim-device-flag-dropped
  2. THE DIRECTORY NEEDS A FORMAT   cache-gate-removed, cache-gate-latched,
                                    cache-magic-three-bytes, cache-nonblock-dropped
  3. A HANDLE IS A DEVICE OR NOT,   cache-write-cap-by-volume, cache-read-by-volume,
     PER HANDLE                     cache-eof-by-volume
  4. NEVER LEAVE THE BACKING DIR    cache-dir-nofollow
  5. WHAT THE CALLER IS TOLD        cache-claim-number-dropped, cache-resolve-number-dropped,
                                    cache-view-flag-dropped, cache-view-count-dropped,
                                    cache-reset-keeps-view-count, cache-unmount-leaks-view-fd
  6. THE MOUNT REFUSES              cache-range-lower, cache-range-upper, cache-name-collision,
                                    cache-dir-slash, cache-dir-backslash
  7. CONTROL REQUESTS               io-only-cache-handles, io-fsctl-code-unchecked,
                                    io-fsctl-handle-unchecked, io-ioctl-any-code,
                                    io-invalid-handle-unchecked, io-geometry-sector-offset,
                                    io-partition-length-offset, io-partition-number-dropped,
                                    io-geometry-length-unchecked, io-partition-length-unchecked,
                                    io-iosb-information-dropped, io-refusal-not-counted,
                                    io-fsctl-not-counted, io-reset-keeps-controls

THREE BRANCHES ARE DELIBERATELY ABSENT, said here rather than left to be noticed.
  - `got == sizeof(magic)` in `cache_image_formatted_locked`. It is redundant by construction:
    `magic` starts zeroed, so a short read leaves a zero where a byte of "FATX" would have to
    be and the four-byte compare fails anyway. A mutation of it alone cannot change a result.
  - `S_ISREG` in the same function. Its job is to refuse a device node swapped in after the
    mount, which an unprivileged suite cannot create. The two special files it CAN create
    (a FIFO and a directory) are already answered "not formatted" by the `pread` that follows
    (ESPIPE and EISDIR), so removing the check survives on every input the suite can build.
  - The `mkdirat` failure arm for an errno other than EEXIST. No suite can make the host
    filesystem refuse a mkdir on demand without privileges.

`src/host/main.c` (the loop that mounts one partition per counted slot) is NOT covered: `tsfp_host`
is not a ctest binary, so no mutation can reach it. It was measured by running the title instead.

THE ENTRIES USE `&& false` rather than `false`, per `_example.py`: a mutation that fails to
compile scores NOT-A-MUTANT, which reads like evidence while meaning nothing was injected.
"""

KERNEL_FILE = "src/xbox/kernel_file.c"
KERNEL_IO = "src/xbox/kernel_io.c"
TARGETS = ["test_hdd_backing"]

MUTATIONS: list[dict] = [
    # ---- 1. two views, one name -----------------------------------------------------------
    {
        "id": "cache-view-always-raw",
        "file": KERNEL_FILE,
        "old": "    return volume->cache_partition == 0u || rest[0] == '\\0';",
        "new": "    return volume->cache_partition == 0u || rest[0] == '\\0' || true;",
        "targets": TARGETS,
        "why": "THE SPLIT ITSELF. If a separator no longer distinguishes the views, "
        "`Partition5\\` is the raw device, so the title's validation opens a device where it "
        "expects a filesystem and the directory view can never be reached.",
    },
    {
        "id": "cache-walk-from-root",
        "file": KERNEL_FILE,
        "old": "    const int start_plus_one = (volume->cache_partition != 0u && rest[0] != '\\0')",
        "new": "    const int start_plus_one = (volume->cache_partition != 0u && rest[0] != '\\0' "
        "&& false)",
        "targets": TARGETS,
        "why": "The directory view must walk from ITS directory. From the `--hdd` root, every "
        "file the title puts in `Z:` lands beside the raw image and `TDATA`, and a name in the "
        "view could shadow the image itself.",
    },
    {
        "id": "cache-claim-device-flag-dropped",
        "file": KERNEL_FILE,
        "old": "    slot->state.device = from->device;",
        "new": "    slot->state.device = false;",
        "targets": TARGETS,
        "why": "Device-ness is carried per HANDLE now. Without it the raw view reads as a "
        "plain file: no capacity bound on writes, no zero-fill on reads, and the control "
        "ordinals no longer see a device.",
    },
    # ---- 2. the directory needs a format --------------------------------------------------
    {
        "id": "cache-gate-removed",
        "file": KERNEL_FILE,
        "old": "    if (!cache_image_formatted_locked(volume)) {",
        "new": "    if (!cache_image_formatted_locked(volume) && false) {",
        "targets": TARGETS,
        "why": "THE FIRST-RUN CONTRACT. Without the gate the directory view opens on a blank "
        "image, so the title validates an empty directory and never formats. Everything after "
        "that is a boot that skipped a step the title meant to take.",
    },
    {
        "id": "cache-gate-latched",
        "file": KERNEL_FILE,
        "old": "    if (!cache_image_formatted_locked(volume)) {",
        "new": "    if (volume->content_fd_plus_one == 0 && "
        "!cache_image_formatted_locked(volume)) {",
        "targets": TARGETS,
        "why": "A gate that is only asked until it first passes keeps answering `formatted` "
        "after something blanked the image, so a title about to re-format is told its "
        "partition is fine. The answer is a property of the image and must be re-read.",
    },
    {
        "id": "cache-magic-three-bytes",
        "file": KERNEL_FILE,
        "old": 'memcmp(magic, "FATX", 4u) == 0;',
        "new": 'memcmp(magic, "FATX", 3u) == 0;',
        "targets": TARGETS,
        "why": "Comparing a prefix accepts any image that begins `FAT`, which includes the "
        "FAT16 and FAT32 boot sectors of a real PC-formatted volume. The title writes "
        "FATX and means the whole word.",
    },
    {
        "id": "cache-nonblock-dropped",
        "file": KERNEL_FILE,
        "old": "                          O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);",
        "new": "                          O_RDONLY | O_NOFOLLOW | O_CLOEXEC);",
        "targets": TARGETS,
        "why": "A FIFO swapped in for the image after the mount blocks an `open(O_RDONLY)` "
        "until a writer appears, with the module lock held, which freezes every file "
        "operation on both guest threads. A hang counts as a kill here.",
    },
    # ---- 3. a handle is a device or not, per handle ---------------------------------------
    {
        "id": "cache-write-cap-by-volume",
        "file": KERNEL_FILE,
        "old": "    if (entry->state.device &&\n        (offset > device->device_capacity ||",
        "new": "    if (device->device_file[0] != '\\0' &&\n        (offset > "
        "device->device_capacity ||",
        "targets": TARGETS,
        "why": "Deciding `is a device` from the VOLUME refuses every write past the image's "
        "capacity to a FILE in the directory view. The title's cache is a directory of "
        "files, so that is the cache failing the first time it grows past the image size.",
    },
    {
        "id": "cache-read-by-volume",
        "file": KERNEL_FILE,
        "old": "        const bool is_device = device->in_use && entry->state.device;",
        "new": "        const bool is_device = device->in_use && device->device_file[0] != '\\0';",
        "targets": TARGETS,
        "why": "The read-side twin of the write cap: a file in the view past the image's "
        "capacity reads as nothing, so a cache the title wrote cannot be read back.",
    },
    {
        "id": "cache-eof-by-volume",
        "file": KERNEL_FILE,
        "old": "    if (entry->state.device) {\n        /* A device has no length",
        "new": "    if (volumes[entry->volume_index].device_file[0] != '\\0') {\n"
        "        /* A device has no length",
        "targets": TARGETS,
        "why": "A file in the directory view must be truncatable. By volume, every "
        "truncation of a cache file is refused as if it were the raw image.",
    },
    # ---- 4. never leave the backing directory ---------------------------------------------
    {
        "id": "cache-dir-nofollow",
        "file": KERNEL_FILE,
        "old": "    const int fd = openat(volume->root_fd_plus_one - 1, volume->content_dir,\n"
        "                          O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);",
        "new": "    const int fd = openat(volume->root_fd_plus_one - 1, volume->content_dir,\n"
        "                          O_RDONLY | O_DIRECTORY | O_CLOEXEC);",
        "targets": TARGETS,
        "why": "THE ESCAPE. A symbolic link planted at the view's name would be followed, "
        "and every create and write the title makes in `Z:` would land wherever it points.",
    },
    # ---- 5. what the caller is told -------------------------------------------------------
    {
        "id": "cache-claim-number-dropped",
        "file": KERNEL_FILE,
        "old": "    slot->state.cache_partition = from->cache_partition;",
        "new": "    slot->state.cache_partition = 0u;",
        "targets": TARGETS,
        "why": "The control ordinals answer only a handle that carries a cache partition "
        "number. Dropped, the format's very first IOCTL is refused and the title reboots.",
    },
    {
        "id": "cache-resolve-number-dropped",
        "file": KERNEL_FILE,
        "old": "        out->cache_partition = volume->cache_partition;",
        "new": "        out->cache_partition = 0u;",
        "targets": TARGETS,
        "why": "The same fact one step earlier, at the resolution. Kept apart from the claim "
        "mutation because the two are separate assignments that could each be dropped.",
    },
    {
        "id": "cache-view-flag-dropped",
        "file": KERNEL_FILE,
        "old": "        out->cache_view = true;",
        "new": "        out->cache_view = false;",
        "targets": TARGETS,
        "why": "The run report's count of directory-view opens is how an operator tells "
        "that the title reached its validated `Z:`. Dropped, the report says it never did.",
    },
    {
        "id": "cache-view-count-dropped",
        "file": KERNEL_FILE,
        "old": "        if (from->cache_view) {",
        "new": "        if (from->cache_view && false) {",
        "targets": TARGETS,
        "why": "Same counter, the other end: the flag is set but never counted.",
    },
    {
        "id": "cache-reset-keeps-view-count",
        "file": KERNEL_FILE,
        "old": "    cache_view_opened_count = 0u;\n    device_opened_count = 0u;",
        "new": "    device_opened_count = 0u;",
        "targets": TARGETS,
        "why": "A counter that survives reset leaks one run's directory opens into the "
        "next run's report.",
    },
    {
        "id": "cache-unmount-leaks-view-fd",
        "file": KERNEL_FILE,
        "old": "        if (volumes[i].content_fd_plus_one != 0) {",
        "new": "        if (volumes[i].content_fd_plus_one != 0 && false) {",
        "targets": TARGETS,
        "why": "The view's directory descriptor outlives the mount, one per reset. A suite "
        "that resets between cases would exhaust descriptors, and a host that remounts would "
        "keep the old directory pinned.",
    },
    # ---- 6. the mount refuses -------------------------------------------------------------
    {
        "id": "cache-range-lower",
        "file": KERNEL_FILE,
        "old": "        if (cache_number < KERNEL_FILE_CACHE_PARTITION_FIRST ||",
        "new": "        if ((cache_number < KERNEL_FILE_CACHE_PARTITION_FIRST && false) ||",
        "targets": TARGETS,
        "why": "Partitions below 3 are the config area and the system partitions. Mounting "
        "a cache partition under one would answer a format request where none is meant.",
    },
    {
        "id": "cache-range-upper",
        "file": KERNEL_FILE,
        "old": "            cache_number > KERNEL_FILE_CACHE_PARTITION_LAST || content_length "
        "== 0u ||",
        "new": "            (cache_number > KERNEL_FILE_CACHE_PARTITION_LAST && false) || "
        "content_length == 0u ||",
        "targets": TARGETS,
        "why": "The upper bound keeps the number inside what the host can mount and what the "
        "title clamps to.",
    },
    {
        "id": "cache-name-collision",
        "file": KERNEL_FILE,
        "old": "            strcmp(content_dir, file_name) == 0) {",
        "new": "            (strcmp(content_dir, file_name) == 0 && false)) {",
        "targets": TARGETS,
        "why": "If the directory and the image share a name, the image (a regular file) is "
        "created first and the directory view can never be made, with no diagnosis that "
        "points at the argument.",
    },
    {
        "id": "cache-dir-slash",
        "file": KERNEL_FILE,
        "old": "            strchr(content_dir, '/') != NULL || strchr(content_dir, '\\\\') != "
        "NULL ||",
        "new": "            (strchr(content_dir, '/') != NULL && false) || "
        "strchr(content_dir, '\\\\') != NULL ||",
        "targets": TARGETS,
        "why": "A directory name with a slash is a PATH, and a path aims the view at a "
        "directory outside the one the mount was given.",
    },
    {
        "id": "cache-dir-backslash",
        "file": KERNEL_FILE,
        "old": "            strchr(content_dir, '/') != NULL || strchr(content_dir, '\\\\') != "
        "NULL ||",
        "new": "            strchr(content_dir, '/') != NULL || "
        "(strchr(content_dir, '\\\\') != NULL && false) ||",
        "targets": TARGETS,
        "why": "The guest's own separator, which the host treats as an ordinary filename "
        "character, so it would silently create a directory named with a backslash.",
    },
    # ---- 7. control requests --------------------------------------------------------------
    {
        "id": "io-only-cache-handles",
        "file": KERNEL_IO,
        "old": "    if (request.file.cache_partition == 0u) {\n        return "
        "control_refuse(&request, "
        '"NtDeviceIoControlFile",',
        "new": "    if (request.file.cache_partition == 0u && false) {\n        return "
        "control_refuse(&request, "
        '"NtDeviceIoControlFile",',
        "targets": TARGETS,
        "why": "Geometry is answered for ANY handle, including partition0 and every file. A "
        "title that asked a save file for its disk geometry would be told a partition's.",
    },
    {
        "id": "io-fsctl-code-unchecked",
        "file": KERNEL_IO,
        "old": "    if (request.file.cache_partition == 0u || request.code != "
        "FSCTL_DISMOUNT_VOLUME) {",
        "new": "    if (request.file.cache_partition == 0u) {",
        "targets": TARGETS,
        "why": "Every FSCTL succeeds. The image issues others (0x9411C at 0x0037DC8F) whose "
        "outputs are not derived, and a bare success is an answer the title would act on.",
    },
    {
        "id": "io-fsctl-handle-unchecked",
        "file": KERNEL_IO,
        "old": "    if (request.file.cache_partition == 0u || request.code != "
        "FSCTL_DISMOUNT_VOLUME) {",
        "new": "    if (request.code != FSCTL_DISMOUNT_VOLUME) {",
        "targets": TARGETS,
        "why": "A dismount of anything is reported successful, including a handle on the "
        "disc or the hard-disk volume.",
    },
    {
        "id": "io-ioctl-any-code",
        "file": KERNEL_IO,
        "old": "    if (request.code == IOCTL_DISK_GET_PARTITION_INFO) {",
        "new": "    if (request.code == IOCTL_DISK_GET_PARTITION_INFO || true) {",
        "targets": TARGETS,
        "why": "An unknown IOCTL code falls into the partition-info arm and is answered with "
        "a partition length. The refusal at the end is the only thing that says a code is "
        "not derived.",
    },
    {
        "id": "io-invalid-handle-unchecked",
        "file": KERNEL_IO,
        "old": "    if (!kernel_file_open_info(file_handle, &out->file)) {",
        "new": "    if (!kernel_file_open_info(file_handle, &out->file) && false) {",
        "targets": TARGETS,
        "why": "A handle nobody issued is reported as a refused CODE instead of a bad "
        "HANDLE, which points the reader at the wrong layer.",
    },
    {
        "id": "io-geometry-sector-offset",
        "file": KERNEL_IO,
        "old": "#define GEOMETRY_BYTES_PER_SECTOR_OFFSET 0x14u",
        "new": "#define GEOMETRY_BYTES_PER_SECTOR_OFFSET 0x10u",
        "targets": TARGETS,
        "why": "THE ONE FIELD THE TITLE READS. It takes its lowest set bit as a shift, so a "
        "value in the wrong slot gives a shift of whatever was there and a FAT laid down "
        "with the wrong sectors-per-cluster.",
    },
    {
        "id": "io-partition-length-offset",
        "file": KERNEL_IO,
        "old": "#define PARTITION_INFO_LENGTH_OFFSET 0x08u",
        "new": "#define PARTITION_INFO_LENGTH_OFFSET 0x10u",
        "targets": TARGETS,
        "why": "The partition length is what the title sizes its FAT from, and the format "
        "fails with ERROR_DISK_FULL when it reads too small a value.",
    },
    {
        "id": "io-partition-number-dropped",
        "file": KERNEL_IO,
        "old": "PARTITION_INFO_NUMBER_OFFSET,\n"
        "                                    request.file.cache_partition) ||",
        "new": "PARTITION_INFO_NUMBER_OFFSET,\n                                    0u) ||",
        "targets": TARGETS,
        "why": "The partition number is not read by the title but is announced as answered. "
        "A structure that reports partition 0 for Partition5 is a wrong answer nobody reads.",
    },
    {
        "id": "io-geometry-length-unchecked",
        "file": KERNEL_IO,
        "old": "        if (request.output == 0u || request.output_length < GEOMETRY_BYTES) {",
        "new": "        if ((request.output == 0u || request.output_length < GEOMETRY_BYTES) && "
        "false) {",
        "targets": TARGETS,
        "why": "A short or NULL output buffer is written past its end, into whatever follows "
        "it on the guest's stack.",
    },
    {
        "id": "io-partition-length-unchecked",
        "file": KERNEL_IO,
        "old": "        if (request.output == 0u || request.output_length < "
        "PARTITION_INFO_BYTES) {",
        "new": "        if ((request.output == 0u || request.output_length < "
        "PARTITION_INFO_BYTES) && "
        "false) {",
        "targets": TARGETS,
        "why": "The same overrun for the 0x20-byte structure, which is the larger of the two.",
    },
    {
        "id": "io-iosb-information-dropped",
        "file": KERNEL_IO,
        "old": "        (void)write_io_status(request.io_status, STATUS_SUCCESS, GEOMETRY_BYTES,",
        "new": "        (void)write_io_status(request.io_status, STATUS_SUCCESS, 0u,",
        "targets": TARGETS,
        "why": "IO_STATUS_BLOCK.information is the byte count the caller reads back. Zero "
        "beside a success is the one answer that cannot be acted on.",
    },
    {
        "id": "io-refusal-not-counted",
        "file": KERNEL_IO,
        "old": "    control_refused_count++;\n    unlock();",
        "new": "    unlock();",
        "targets": TARGETS,
        "why": "A refused format must read as a refusal in the run report, not as a title "
        "that never asked.",
    },
    {
        "id": "io-fsctl-not-counted",
        "file": KERNEL_IO,
        "old": "    lock();\n    control_count++;\n    unlock();\n    (void)write_io_status("
        'request.io_status, STATUS_SUCCESS, 0u, "NtFsControlFile");',
        "new": "    (void)write_io_status(request.io_status, STATUS_SUCCESS, 0u, "
        '"NtFsControlFile");',
        "targets": TARGETS,
        "why": "The answered count is what an operator reads to confirm the whole format "
        "sequence ran, dismount included.",
    },
    {
        "id": "io-reset-keeps-controls",
        "file": KERNEL_IO,
        "old": "    control_count = 0u;\n    control_refused_count = 0u;",
        "new": "    control_refused_count = 0u;",
        "targets": TARGETS,
        "why": "A counter that survives reset leaks one test's, or one run's, control "
        "requests into the next.",
    },
]
