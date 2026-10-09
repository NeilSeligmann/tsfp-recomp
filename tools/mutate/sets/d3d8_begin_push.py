# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for BeginPush 0x003D6660 and EndPush 0x003D6680 (T854, src/gpu/d3d8_device.c).

WHAT THIS SET IS FOR. The pair hands the title a raw pointer into the pushbuffer ring and publishes the cursor it
gives back. The failure modes are quiet ones: a reservation one dword short lets the title's last word fall outside the
block (nothing crashes, the stream just loses a word), a dropped flush leaves the library's own deferred vertex state
unwritten, a pointer bound off by one lets a corrupted cursor through. Each mutation breaks one piece and `why` says
what a survivor would hide.

TARGETS. The ctest binary (`test_d3d8_begin_push`, which also guards compilation) and the pytest suite that replays the
ORIGINAL bytes through the same states (`tests/test_d3d8_begin_push_oracle.py`, word for word).

CONVENTION. `&& false` rather than `if (false)`, because `-Wunused-parameter -Werror` turns the latter into
NOT-A-MUTANT, which reads like evidence.
"""

DEVICE = "src/gpu/d3d8_device.c"
T_PUSH = ["test_d3d8_begin_push"]
ORACLE = "pytest:tests/test_d3d8_begin_push_oracle.py"

MUTATIONS: list[dict] = [
    {
        "id": "d3d-beginpush-count-not-incremented",
        "file": DEVICE,
        "old": "    const uint32_t dwords = count + 1u; /* `inc [esp+4]`: 32 bit wrap like the original */",
        "new": "    const uint32_t dwords = count; /* `inc [esp+4]`: 32 bit wrap like the original */",
        "targets": [*T_PUSH, ORACLE],
        "why": (
            "the original reserves Count + 1 dwords (`inc [esp+4]; jmp 0x003D6B30`). One short moves the roll-over "
            "boundary by a dword and lets a block of exactly Count dwords end the reservation, which the title's "
            "34 dword block does."
        ),
    },
    {
        "id": "d3d-beginpush-reservation-skipped",
        "file": DEVICE,
        "old": "    const uint32_t cursor = d3d8_pushbuffer_reserve(dwords);",
        "new": "    const uint32_t cursor = d3d8_device_load32(D3D8_DEV_CURSOR);",
        "targets": [*T_PUSH, ORACLE],
        "why": (
            "without the sized reservation the block is written where the ring may already be at its limit, past "
            "the point the original would have rolled over, and the new limit is never set."
        ),
    },
    {
        "id": "d3d-beginpush-flush-skipped",
        "file": DEVICE,
        "old": "    d3d8_draw_flush_streams(0u);\n    /* `inc [esp+4]; jmp 0x003D6B30`",
        "new": "    /* flush skipped */\n    /* `inc [esp+4]; jmp 0x003D6B30`",
        "targets": [*T_PUSH, ORACLE],
        "why": (
            "BeginPush flushes the deferred state (0x003DEE00 with argument zero) in front of the block. Without it "
            "the library's own format and offset runs and the dirty cascade are never written, and the dirty mask "
            "keeps bits 0x50 the original clears."
        ),
    },
    {
        "id": "d3d-beginpush-flush-wrong-base-vertex",
        "file": DEVICE,
        "old": "    d3d8_draw_flush_streams(0u);\n    /* `inc [esp+4]; jmp 0x003D6B30`",
        "new": "    d3d8_draw_flush_streams(1u);\n    /* `inc [esp+4]; jmp 0x003D6B30`",
        "targets": [*T_PUSH, ORACLE],
        "why": (
            "the flush is called with argument zero. Another base vertex changes the cached value at device+0x20 and "
            "every offset the stream work writes."
        ),
    },
    {
        "id": "d3d-beginpush-plan-skipped",
        "file": DEVICE,
        "old": "    (void)d3d8_pushbuffer_sim_reserve(&sim, dwords);\n    if (kernel_guest_at(sim.cursor, dwords * 4u) == NULL) {",
        "new": "    (void)d3d8_pushbuffer_sim_reserve(&sim, dwords);\n    if (kernel_guest_at(sim.cursor, dwords * 4u) == NULL && false) {",
        "targets": [*T_PUSH],
        "why": (
            "the mapped-span check is what refuses a block that would run past the end of the ring BEFORE the first "
            "write. Without it the title is handed a pointer to memory that is not there."
        ),
    },
    {
        "id": "d3d-beginpush-ring-slack-dropped",
        "file": DEVICE,
        "old": "    if (dwords == 0u || (uint64_t)dwords * 4u + D3D8_PUSHBUFFER_SLACK_BYTES > ring_bytes) {",
        "new": "    if (dwords == 0u || (uint64_t)dwords * 4u > ring_bytes) {",
        "targets": [*T_PUSH],
        "why": (
            "the sized reservation raises both sizes to dwords * 4 + 0x204, so a block that fits the ring only "
            "without the slack still cannot be served: the original loops in its refill."
        ),
    },
    {
        "id": "d3d-beginpush-zero-dwords-accepted",
        "file": DEVICE,
        "old": "    if (dwords == 0u || (uint64_t)dwords * 4u + D3D8_PUSHBUFFER_SLACK_BYTES > ring_bytes) {",
        "new": "    if ((uint64_t)dwords * 4u + D3D8_PUSHBUFFER_SLACK_BYTES > ring_bytes) {",
        "targets": [*T_PUSH],
        "why": "Count 0xFFFFFFFF wraps to no dwords at all; the port refuses it by name rather than reserving nothing.",
    },
    {
        "id": "d3d-beginpush-block-end-short",
        "file": DEVICE,
        "old": "    push_block.end = cursor + dwords * 4u;",
        "new": "    push_block.end = cursor + count * 4u;",
        "targets": [*T_PUSH],
        "why": "the open block spans the whole reservation, Count + 1 dwords; one short refuses a legal EndPush.",
    },
    {
        "id": "d3d-endpush-cursor-not-published",
        "file": DEVICE,
        "old": "    d3d8_pushbuffer_end(pointer);\n    push_block.open = false;",
        "new": "    push_block.open = false;",
        "targets": [*T_PUSH, ORACLE],
        "why": "EndPush stores p as the cursor. Without it the block the title wrote is never part of the stream.",
    },
    {
        "id": "d3d-endpush-block-stays-open",
        "file": DEVICE,
        "old": "    d3d8_pushbuffer_end(pointer);\n    push_block.open = false;",
        "new": "    d3d8_pushbuffer_end(pointer);",
        "targets": [*T_PUSH],
        "why": "a second EndPush for one BeginPush would publish a cursor of the first block again.",
    },
    {
        "id": "d3d-endpush-returns-zero",
        "file": DEVICE,
        "old": "    return pointer; /* `mov eax, [esp+4]` */",
        "new": "    return 0u; /* `mov eax, [esp+4]` */",
        "targets": [*T_PUSH, ORACLE],
        "why": "the original leaves p in eax (`mov eax, [esp+4]`).",
    },
    {
        "id": "d3d-endpush-lower-bound-dropped",
        "file": DEVICE,
        "old": "    if ((pointer & 3u) != 0u || pointer < push_block.begin || pointer > push_block.end) {",
        "new": "    if ((pointer & 3u) != 0u || pointer > push_block.end) {",
        "targets": [*T_PUSH],
        "why": "a pointer below the block would move the cursor backwards over commands already in the ring.",
    },
    {
        "id": "d3d-endpush-upper-bound-open",
        "file": DEVICE,
        "old": "    if ((pointer & 3u) != 0u || pointer < push_block.begin || pointer > push_block.end) {",
        "new": "    if ((pointer & 3u) != 0u || pointer < push_block.begin || pointer > push_block.end + 4u) {",
        "targets": [*T_PUSH],
        "why": "a pointer past the reservation publishes dwords nobody reserved, beyond the slack the writers rely on.",
    },
    {
        "id": "d3d-endpush-alignment-dropped",
        "file": DEVICE,
        "old": "    if ((pointer & 3u) != 0u || pointer < push_block.begin || pointer > push_block.end) {",
        "new": "    if (pointer < push_block.begin || pointer > push_block.end) {",
        "targets": [*T_PUSH],
        "why": "a cursor that is not a dword position splits every later command.",
    },
    {
        "id": "d3d-endpush-no-block-accepted",
        "file": DEVICE,
        "old": '    if (!push_block.open) {\n        d3d8_hle_fatal(0x003D6680u, "EndPush(%#x) with no BeginPush block open", (unsigned)pointer);',
        "new": '    if (!push_block.open && false) {\n        d3d8_hle_fatal(0x003D6680u, "EndPush(%#x) with no BeginPush block open", (unsigned)pointer);',
        "targets": [*T_PUSH],
        "why": "an EndPush with no BeginPush has no reservation behind its pointer.",
    },
    {
        "id": "d3d-beginpush-handler-wrong-address",
        "file": DEVICE,
        "old": "        {0x003D6660u, handler_begin_push},\n",
        "new": "        {0x003D6664u, handler_begin_push},\n",
        "targets": [*T_PUSH, ORACLE],
        "why": "a BeginPush registered under the wrong address is the boot's stop at 0x003D6660 again.",
    },
    {
        "id": "d3d-endpush-handler-wrong-address",
        "file": DEVICE,
        "old": "        {0x003D6680u, handler_end_push},\n",
        "new": "        {0x003D6684u, handler_end_push},\n",
        "targets": [*T_PUSH, ORACLE],
        "why": "an EndPush registered under the wrong address leaves the cursor where BeginPush found it: the block is never published.",
    },
]
