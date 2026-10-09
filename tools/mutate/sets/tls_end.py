"""Mutations for the `fs:[4]` (end of TLS) publication in `src/xbox/kernel_thread.c`.

The defect this guards: with fs:[4] left at 0 the title's first XapiSetLastError faults at
0xFFFFFFEC, a fault that was misattributed to DirectSound. The dangerous mutation is not a
wrong formula but the publisher never being CALLED, so that one is first.
"""

MUTATIONS: list[dict] = [
    {
        "id": "tls-end-never-published-by-thread-creation",
        "file": "src/xbox/kernel_thread.c",
        "old": "    if (!kernel_thread_publish_tls_end(control, tls, record->tls_data_size)) {",
        "new": "    if (false && !kernel_thread_publish_tls_end(control, tls, record->tls_data_size)) {",  # noqa: E501 -- must match the C source exactly
        "targets": ["test_guest_thread"],
        "why": "a correct publisher that nothing calls is the src/gpu failure: complete, "
        "tested in isolation, and the boot still faults at 0xFFFFFFEC.",
    },
    {
        "id": "tls-end-uses-the-page-rounded-size",
        "file": "src/xbox/kernel_thread.c",
        "old": "                                  tls_base + tls_data_size);",
        "new": "                                  tls_base + tls_data_size + 0x1000u);",
        "targets": ["test_guest_thread"],
        "why": "the guest indexes backwards from the end of the REQUESTED block (0x14), "
        "so the page-rounded allocation end puts every TLS slot a page away.",
    },
    {
        "id": "tls-end-written-to-the-wrong-pcr-slot",
        "file": "src/xbox/kernel_thread.h",
        "old": "#define KERNEL_PCR_TLS_END 0x04u",
        "new": "#define KERNEL_PCR_TLS_END 0x08u",
        "targets": ["test_guest_thread"],
        "why": "fs:[8] is a different field; the guest reads fs:[4].",
    },
    {
        "id": "tls-end-ignores-the-size",
        "file": "src/xbox/kernel_thread.c",
        "old": "                                  tls_base + tls_data_size);",
        "new": "                                  tls_base);",
        "targets": ["test_guest_thread"],
        "why": "index -5 would then address 20 bytes BEFORE the block, in whatever the "
        "allocator put there.",
    },
]
