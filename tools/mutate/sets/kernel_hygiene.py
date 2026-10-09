"""Mutations for the kernel hygiene bundle: T54 handle recycling, T55 guest accessors, T57
test cleanup.

OWNED BY THE KERNEL-HYGIENE TASK. One file per owner, see `_example.py`.

WHAT IS DELIBERATELY ABSENT, so a missing mutation is never mistaken for a lost one:
  - `GUEST_FIELD` / `kernel_guest_add` at the call sites in `kernel_file.c` and
    `kernel_object.c`, swapped back to plain `+`. That mutant SURVIVES by construction and
    is equivalent on this host: a wrapped sum lands below the field size (at most 65535 for
    the `buffer + i` loops), every such address is under `vm.mmap_min_addr`, and the mapping
    probe refuses them anyway. The unit test of `kernel_guest_add` itself is what pins the
    wrap rule, and its mutations are below.
  - The `0xE1` marker and 4-byte alignment of a handle. Lookup compares the WHOLE handle
    value against the entry, so a check on those bits would be redundant and a mutation of
    it equivalent. They were removed rather than mutation-tested.
  - Python code (`tools/lift/arity_check.py`): this harness mutates C and runs ctest
    binaries, so it cannot reach it. Those mutations were run by hand with `python -B`.
"""

RETIRE_BRANCH = (
    "    if (generation[slot] >= KERNEL_OBJECT_GENERATION_COUNT) {\n        retired_count++;"
)

MUTATIONS: list[dict] = [
    # ------------------------------------------------------------------ T54
    {
        "id": "hyg-close-keeps-the-slot",
        "file": "src/xbox/kernel_object.c",
        "old": "    release_file_entry_nolock(entry);\n}",
        "new": "    if (entry->references > 100000u) {\n"
        "        release_file_entry_nolock(entry);\n    }\n}",
        "targets": ["test_kernel_object"],
        "why": "the old lifetime cap in new clothes: a close that never frees the slot "
        "leaves the table exhausted after 256 handles however many were closed, and "
        "no boot-sized run would show it because the boot uses about 13.",
    },
    {
        "id": "hyg-generation-never-bumped",
        "file": "src/xbox/kernel_object.c",
        "old": "    generation[slot]++;\n",
        "new": "    generation[slot] += 0u;\n",
        "targets": ["test_kernel_object"],
        "why": "recycling WITHOUT a generation reissues the identical value, so a stale "
        "handle names the new object: a use-after-free the guest cannot detect, and "
        "kernel_file.c's lazy slot reclaim would see a closed file as live again.",
    },
    {
        "id": "hyg-generation-wraps-instead-of-retiring",
        "file": "src/xbox/kernel_object.c",
        "old": RETIRE_BRANCH,
        "new": "    if (generation[slot] >= KERNEL_OBJECT_GENERATION_COUNT) {\n"
        "        generation[slot] = 0u;\n    }\n" + RETIRE_BRANCH,
        "targets": ["test_kernel_object"],
        "why": "wrapping lets a handle issued 16384 closes ago alias a live one, the one "
        "property the generation exists to rule out, and it only shows after four "
        "million issues, so nothing short of the exhaustive test can see it.",
    },
    {
        "id": "hyg-retire-one-generation-late",
        "file": "src/xbox/kernel_object.c",
        "old": RETIRE_BRANCH,
        "new": RETIRE_BRANCH.replace(">=", ">"),
        "targets": ["test_kernel_object"],
        "why": "an off-by-one at the end of the generation range issues generation 16384, "
        "whose high bits overflow the 8-bit field and repeat generation 0's value.",
    },
    {
        "id": "hyg-fifo-queue-head-never-advances",
        "file": "src/xbox/kernel_object.c",
        "old": "        free_head = (free_head + 1u) % KERNEL_OBJECT_MAX;\n        free_count--;",
        "new": "        free_count--;",
        "targets": ["test_kernel_object"],
        "why": "reissuing the same queue entry hands out a slot that is still live or "
        "skips the oldest-freed order, so two live objects can share a handle.",
    },
    {
        "id": "hyg-lookup-compares-the-slot-only",
        "file": "src/xbox/kernel_object.c",
        "old": "    return (entry->in_use && entry->handle == handle) ? entry : NULL;",
        "new": "    return entry->in_use ? entry : NULL;",
        "targets": ["test_kernel_object"],
        "why": "the generation is only a defence if lookup compares the whole value. A "
        "slot-only lookup finds the NEW object for a stale handle, which is the alias "
        "the whole scheme exists to prevent.",
    },
    {
        "id": "hyg-mutant-keeps-the-handle-the-guest-never-learned",
        "file": "src/xbox/kernel_object.c",
        "old": "        close_entry_nolock(mutable_find(handle));\n"
        '        kernel_hle_log()("kernel: NtCreateMutant could not write',
        "new": "        (void)mutable_find(handle);\n"
        '        kernel_hle_log()("kernel: NtCreateMutant could not write',
        "targets": ["test_kernel_object"],
        "why": "a failed copy-out of the handle leaves an object nothing can ever close, "
        "so every bad call costs one of 256 live slots for good.",
    },
    # ------------------------------------------------------------------ T55
    {
        "id": "hyg-guest-at-does-not-probe",
        "file": "src/xbox/kernel_call.c",
        "old": "    if (length != 0u && !kernel_guest_range_readable(addr, length)) {",
        "new": "    if (length != 0u && !kernel_guest_range_readable(addr, length) && false) {",
        "targets": ["test_kernel_object"],
        "why": "without the probe an in-range unmapped address is accepted and the host "
        "faults on the next read, which is the crash this task exists to remove.",
    },
    {
        "id": "hyg-probe-rounds-the-end-down",
        "file": "src/xbox/kernel_call.c",
        "old": "((uintptr_t)addr + length + page - 1u) & ~(page - 1u)",
        "new": "((uintptr_t)addr + length) & ~(page - 1u)",
        "targets": ["test_kernel_object"],
        "why": "rounding the end DOWN stops the probe at the last whole page, so a range "
        "that crosses into an unmapped page passes. A word at the end of a mapping "
        "is the shape a bad guest pointer takes.",
    },
    {
        "id": "hyg-guest-add-lets-a-small-wrap-through",
        "file": "src/xbox/kernel_call.c",
        "old": "    if (base == 0u || sum >= GUEST_ADDRESS_LIMIT) {",
        "new": "    if (base == 0u || sum > GUEST_ADDRESS_LIMIT + 0x10000u) {",
        "targets": ["test_kernel_object"],
        "why": "0xFFFFFFFE + 4 wraps to address 2, a low address the accessors would "
        "accept. A bound that is a little too generous looks right for every sum "
        "that does not wrap, so only a wrapping input can tell.",
    },
    {
        "id": "hyg-guest-add-accepts-a-null-base",
        "file": "src/xbox/kernel_call.c",
        "old": "    if (base == 0u || sum >= GUEST_ADDRESS_LIMIT) {",
        "new": "    if (base == 1u && sum >= GUEST_ADDRESS_LIMIT) {",
        "targets": ["test_kernel_object"],
        "why": "a NULL struct pointer plus an offset is a plausible low address, not "
        "NULL, so the field read would be attempted instead of refused.",
    },
    # ------------------------------------------------------------------ T56
    {
        "id": "hyg-attr-report-never-fires",
        "file": "src/xbox/kernel_file.c",
        "old": "    if (attributes == GUEST_OBJ_CASE_INSENSITIVE_INFERRED) {\n        return;",
        "new": "    if (true || attributes == GUEST_OBJ_CASE_INSENSITIVE_INFERRED) {\n"
        "        return;",
        "targets": ["test_kernel_file"],
        "why": "attributes are read by every file handler and honoured by none, so the "
        "report is the only trace of a flag we silently drop.",
    },
    {
        "id": "hyg-attr-report-inverted",
        "file": "src/xbox/kernel_file.c",
        "old": "    if (attributes == GUEST_OBJ_CASE_INSENSITIVE_INFERRED) {\n        return;",
        "new": "    if (attributes != GUEST_OBJ_CASE_INSENSITIVE_INFERRED) {\n        return;",
        "targets": ["test_kernel_file"],
        "why": "reporting the value the host agrees with and hiding the rest is the exact "
        "opposite of the point, and would put noise into every boot.",
    },
    {
        "id": "hyg-attr-report-does-not-count",
        "file": "src/xbox/kernel_file.c",
        "old": "    unmodelled_attributes_count++;\n",
        "new": "    (void)0;\n",
        "targets": ["test_kernel_file"],
        "why": "a log line alone is lost in a long run, the counter is what a test or a "
        "later summary can read.",
    },
    {
        "id": "hyg-attr-open-file-skips-report",
        "file": "src/xbox/kernel_file.c",
        "old": '    report_unmodelled_attributes("NtOpenFile", path, attributes);\n',
        "new": "",
        "targets": ["test_kernel_file"],
        "why": "one of three handlers reading attributes; each needs its own report.",
    },
    {
        "id": "hyg-attr-create-file-skips-report",
        "file": "src/xbox/kernel_file.c",
        "old": '    report_unmodelled_attributes("NtCreateFile", path, attributes);\n',
        "new": "",
        "targets": ["test_kernel_file"],
        "why": "one of three handlers reading attributes; each needs its own report.",
    },
    {
        "id": "hyg-attr-symlink-skips-report",
        "file": "src/xbox/kernel_file.c",
        "old": '    report_unmodelled_attributes("NtOpenSymbolicLinkObject", name, attributes);\n',
        "new": "",
        "targets": ["test_kernel_file"],
        "why": "one of three handlers reading attributes; each needs its own report.",
    },
    {
        "id": "hyg-attr-report-rejects-the-open",
        "file": "src/xbox/kernel_file.c",
        "old": "                     ordinal_name, name, (unsigned)attributes);\n}",
        "new": "                     ordinal_name, name, (unsigned)attributes);\n"
        "    lock();\n    refused_count++;\n    unlock();\n}",
        "targets": ["test_kernel_file"],
        "why": "the report must not change the answer: an open the host can serve stays "
        "served, and a refusal counter that moves on it hides real refusals.",
    },
    {
        "id": "hyg-mutant-unnamed-report-needs-both",
        "file": "src/xbox/kernel_object.c",
        "old": "(root_directory != 0u || attributes != 0u)) {\n        /* Unnamed",
        "new": "(root_directory != 0u && attributes != 0u)) {\n        /* Unnamed",
        "targets": ["test_kernel_object"],
        "why": "either field alone is a request the host drops, so both must be reported.",
    },
    {
        "id": "hyg-mutant-unnamed-report-never",
        "file": "src/xbox/kernel_object.c",
        "old": "(root_directory != 0u || attributes != 0u)) {\n        /* Unnamed",
        "new": "(root_directory != 0u || attributes != 0u) && false) {\n        /* Unnamed",
        "targets": ["test_kernel_object"],
        "why": "an ignored root or flag on an unnamed mutant would vanish unreported.",
    },
    {
        "id": "hyg-mutant-unnamed-report-on-null-attributes",
        "file": "src/xbox/kernel_object.c",
        "old": "    if (object_attributes != 0u && (root_directory != 0u || attributes != 0u)) {",
        "new": "    if (true) {",
        "targets": ["test_kernel_object"],
        "why": "the guest's real call passes no OBJECT_ATTRIBUTES, and a report on it would "
        "print on every boot for a shape that has nothing to ignore.",
    },
    {
        "id": "hyg-mutant-refusal-omits-root",
        "file": "src/xbox/kernel_object.c",
        "old": "%#x, root %#x, name %#x, attributes %#x) is NOT IMPLEMENTED",
        "new": "%#x, r00t %#x, name %#x, attributes %#x) is NOT IMPLEMENTED",
        "targets": ["test_kernel_object"],
        "why": "the refusal is where a later reader first meets the 0xFFFFFFFC root and the "
        "0x80 attributes the old header said never occur.",
    },
    {
        "id": "hyg-mutant-reads-attributes-at-the-root-offset",
        "file": "src/xbox/kernel_object.c",
        "old": "         !kernel_guest_read_u32(GUEST_FIELD(object_attributes, attributes), "
        "&attributes))) {",
        "new": "         !kernel_guest_read_u32(GUEST_FIELD(object_attributes, root_directory), "
        "&attributes))) {",
        "targets": ["test_kernel_object"],
        "why": "root and attributes are adjacent dwords, so a wrong offset returns a "
        "plausible value for the wrong field.",
    },
    # ------------------------------------------------------------------ T57
    {
        "id": "hyg-hdd-test-registers-no-exit-cleanup",
        "file": "tests/c/test_hdd_backing.c",
        "old": "        (void)atexit(drop_host_root);",
        "new": "        (void)0;",
        "targets": ["test_hdd_backing"],
        "why": "without the atexit handler every FATAL exit path leaves a tsfp-hdd-test "
        "directory behind, which is the leak this task reports.",
    },
    {
        "id": "hyg-hdd-test-teardown-skips-the-removal",
        "file": "tests/c/test_hdd_backing.c",
        "old": "    drop_host_root();\n    /* Asked of",
        "new": "    /* Asked of",
        "targets": ["test_hdd_backing"],
        "why": "a teardown that does not remove the tree leaks one directory per case, "
        "and the access() assertion after it is the only thing that notices.",
    },
    {
        "id": "hyg-hdd-test-root-is-cwd-relative-again",
        "file": "tests/c/test_hdd_backing.c",
        "old": '"%s/%s", binary_directory,\n                                 HOST_ROOT_TEMPLATE);',
        "new": '"%s%s", "",\n                                 HOST_ROOT_TEMPLATE);',
        "targets": ["test_hdd_backing"],
        "why": "a cwd-relative template puts the scratch tree in whatever directory ctest "
        "was started from, which is how two of them reached the repository root.",
    },
    {
        "id": "hyg-hdd-test-removal-does-not-recurse",
        "file": "tests/c/test_hdd_backing.c",
        "old": "            remove_tree(dir_fd, entry->d_name, depth - 1u);",
        "new": "            (void)0;",
        "targets": ["test_hdd_backing"],
        "why": "a non-recursive removal fails silently on any tree with a subdirectory, "
        "so the cleanup looks present and leaves the directories it was written for.",
    },
]
