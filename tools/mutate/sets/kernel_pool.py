# SPDX-License-Identifier: GPL-3.0-or-later
"""Mutation set for executive pool ownership and allocator state (T468)."""

_TARGET = ["test_kernel_pool"]
MUTATIONS = [
    {
        "id": "kpool-alloc-zero-refused",
        "file": "src/xbox/kernel_pool.c",
        "old": "    if (bytes == 0u) {",
        "new": "    if (bytes != 0u) {",
        "targets": _TARGET,
        "why": "A zero-byte request has no evidenced policy and must be refused; returning a real block would invent success and make the refusal diagnostic disappear.",  # noqa: E501
    },
    {
        "id": "kpool-heap-created-on-demand",
        "file": "src/xbox/kernel_pool.c",
        "old": "    if (pool_heap_handle == 0u) {\n        /* Growable: maximum_size 0.",
        "new": "    if (pool_heap_handle != 0u) {\n        /* Growable: maximum_size 0.",
        "targets": _TARGET,
        "why": "The first allocation must create the backing heap. Skipping creation silently refuses every valid allocation.",  # noqa: E501
    },
    {
        "id": "kpool-track-live-count",
        "file": "src/xbox/kernel_pool.c",
        "old": "    live_count++;\n    live_bytes += bytes;",
        "new": "    live_count += 2u;\n    live_bytes += bytes;",
        "targets": _TARGET,
        "why": "The live count is used to report owned allocations; counting each block twice misreports live ownership immediately.",  # noqa: E501
    },
    {
        "id": "kpool-track-requested-size",
        "file": "src/xbox/kernel_pool.c",
        "old": "    entries[slot].bytes = bytes;",
        "new": "    entries[slot].bytes = bytes + 1u;",
        "targets": _TARGET,
        "why": "The public size query and live-byte ledger describe requested bytes; adding one loses the exact request and corrupts accounting on free.",  # noqa: E501
    },
    {
        "id": "kpool-free-live-byte-accounting",
        "file": "src/xbox/kernel_pool.c",
        "old": "    live_bytes -= entry->bytes;",
        "new": "    live_bytes += entry->bytes;",
        "targets": _TARGET,
        "why": "Freeing a tracked block must release its bytes from the live ledger, or freed memory remains reported as live.",  # noqa: E501
    },
    {
        "id": "kpool-free-clears-entry",
        "file": "src/xbox/kernel_pool.c",
        "old": "    *entry = (pool_entry){0u, 0u, 0u, false};",
        "new": "    *entry = (pool_entry){entry->address, entry->bytes, entry->tag, true};",
        "targets": _TARGET,
        "why": "A successful free must remove the address from the ownership table; retaining it makes freed pointers appear live and permits a double free to pass.",  # noqa: E501
    },
    {
        "id": "kpool-two-arguments-required",
        "file": "src/xbox/kernel_pool.c",
        "old": 'frame_args(frame, args, 2u, "ExAllocatePoolWithTag")',
        "new": 'frame_args(frame, args, 1u, "ExAllocatePoolWithTag")',
        "targets": _TARGET,
        "why": "The handler needs both size and tag. Accepting a one-slot frame silently supplies an uninitialized tag instead of refusing an incomplete call.",  # noqa: E501
    },
    {
        "id": "kpool-free-null-counted",
        "file": "src/xbox/kernel_pool.c",
        "old": "static void pool_free(kernel_guest_ptr address)\n{\n    /* ExFreePool(NULL) is a bugcheck on hardware, not a no-op like C's free(). It\n     * is called out separately from an unknown address because the causes differ: a\n     * NULL here usually means an allocation failure went unchecked, whereas an\n     * unknown address means a double free or a pointer from somewhere else. */\n    if (address == 0u) {",  # noqa: E501
        "new": "static void pool_free(kernel_guest_ptr address)\n{\n    /* ExFreePool(NULL) is a bugcheck on hardware, not a no-op like C's free(). It\n     * is called out separately from an unknown address because the causes differ: a\n     * NULL here usually means an allocation failure went unchecked, whereas an\n     * unknown address means a double free or a pointer from somewhere else. */\n    if (address != 0u) {",  # noqa: E501
        "targets": _TARGET,
        "why": "ExFreePool(NULL) must be counted and reported as a bad free; this inversion routes valid addresses into the null path.",  # noqa: E501
    },
]
