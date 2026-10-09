# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for the draw dirty-state cascade refill (T368): the pushbuffer refill model, its
read-only previews and simulated writer, and the per-site reservation of every cascade emitter
(src/gpu/d3d8_{pushbuffer,dirty,texture_dirty,resource}.c).

WHAT THIS SET IS FOR. The cascade 0x003DED80 writes five emitters' packets between the title's
draw calls and each emitter opens with its own `if (cursor >= limit) roll over`, so a roll-over can
fall between any two packets, between two texture stages, or at the draw's sized reservation. The
failure mode here is a roll-over at the wrong SITE: the bytes still look right, only the cursor the
limit moves from differs, and the boot advances either way. Each mutation moves, drops or mis-sizes
one site (or one piece of refill arithmetic) and `why` says what a survivor would hide.

TARGETS. Each names the ctest binary (which also guards compilation, a pytest target alone cannot)
and the pytest suite that replays the ORIGINAL code through the same states:
`tests/test_d3d8_cascade_refill_oracle.py` (draws near a limit, byte for byte) and
`tests/test_d3d8_refill_oracle.py` (the refill arithmetic against 0x003D6B20/0x003D6B30).

CONVENTION. `if (cond && false)` rather than `if (false)`, because `-Wunused-parameter -Werror`
turns the latter into NOT-A-MUTANT, which reads like evidence.
"""

PUSH = "src/gpu/d3d8_pushbuffer.c"
DIRTY = "src/gpu/d3d8_dirty.c"
TEXTURE = "src/gpu/d3d8_texture_dirty.c"
RESOURCE = "src/gpu/d3d8_resource.c"

T_CASCADE = ["test_d3d8_cascade_refill"]
T_PUSH = ["test_d3d8_pushbuffer"]
T_DIRTY = ["test_d3d8_dirty"]
T_TEXTURE = ["test_d3d8_texture_dirty"]
ORACLE = 'pytest:tests/test_d3d8_cascade_refill_oracle.py -k "every_cursor or single_family"'
ORACLE_SIZED = 'pytest:tests/test_d3d8_cascade_refill_oracle.py -k "sized or wraps"'
ORACLE_REFILL = "pytest:tests/test_d3d8_refill_oracle.py"

MUTATIONS: list[dict] = [
    {
        "id": "d3d-cascade-point-preamble-dropped",
        "file": DIRTY,
        "old": "    uint32_t cursor = d3d8_pushbuffer_begin();\n    const float minimum",
        "new": "    uint32_t cursor = d3d8_device_load32(D3D8_DEV_CURSOR);\n    const float minimum",
        "targets": [*T_DIRTY, ORACLE],
        "why": (
            "without its preamble the point emitter writes past a limit the original would have "
            "rolled over at, so the new limit is never set and every later site sees a stale one."
        ),
    },
    {
        "id": "d3d-cascade-stage-program-preamble-dropped",
        "file": DIRTY,
        "old": "    const uint32_t cursor = d3d8_pushbuffer_begin();\n    d3d8_guest_store32(cursor, 0x00041E70u);",
        "new": "    const uint32_t cursor = d3d8_device_load32(D3D8_DEV_CURSOR);\n    d3d8_guest_store32(cursor, 0x00041E70u);",
        "targets": [*T_DIRTY, ORACLE],
        "why": (
            "the stage program is the FIRST site of the live mask, so dropping its preamble moves "
            "the roll-over to the next site and the limit differs by the program's 8 bytes."
        ),
    },
    {
        "id": "d3d-cascade-lighting-preamble-dropped",
        "file": DIRTY,
        "old": "    const uint32_t start = d3d8_pushbuffer_begin();\n    const bool defaults",
        "new": "    const uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);\n    const bool defaults",
        "targets": [*T_DIRTY, ORACLE],
        "why": (
            "the lighting packets are the last site of the live mask: without the preamble a "
            "cursor that reaches the limit exactly there is written past it unrolled."
        ),
    },
    {
        "id": "d3d-cascade-texture-stage-single-preamble",
        "file": TEXTURE,
        "old": "        uint32_t cursor = d3d8_pushbuffer_begin();\n        /* Original cache",
        "new": "        uint32_t cursor = d3d8_device_load32(D3D8_DEV_CURSOR);\n        /* Original cache",
        "targets": [*T_TEXTURE, *T_CASCADE, ORACLE],
        "why": (
            "each texture stage has its own preamble in the original (0x003DDE2C), so a limit "
            "reached between stage 0 and stage 1 rolls there. A shared or missing preamble "
            "writes the later stages past the limit and the boot never notices."
        ),
    },
    {
        "id": "d3d-cascade-fog-preamble-dropped",
        "file": TEXTURE,
        "old": "    cursor = d3d8_pushbuffer_begin();\n    if (cursor != sites.fog)",
        "new": "    cursor = d3d8_device_load32(D3D8_DEV_CURSOR);\n    if (cursor != sites.fog)",
        "targets": [*T_TEXTURE, ORACLE],
        "why": "the fog packet has one preamble at its head; dropping it leaves the limit stale.",
    },
    {
        "id": "d3d-cascade-texture-plan-stage-bytes",
        "file": TEXTURE,
        "old": "    for (uint32_t i = 0u; i < count; i++) d3d8_pushbuffer_sim_site(sim, 0x003DDCB0u, 20u);",
        "new": "    for (uint32_t i = 0u; i < count; i++) d3d8_pushbuffer_sim_site(sim, 0x003DDCB0u, 16u);",
        "targets": [*T_CASCADE],
        "why": (
            "a stage packet is 20 bytes. If the plan counts fewer, the simulated writer reaches "
            "later sites early and predicts a different set of roll-overs than will happen, so "
            "the refusal is no longer before the first write."
        ),
    },
    {
        "id": "d3d-cascade-plan-fog-dropped",
        "file": RESOURCE,
        "old": "    if ((dirty & 0x2000u) != 0u) d3d8_plan_fog(sim);\n",
        "new": "    if ((dirty & 0x2000u) != 0u && false) d3d8_plan_fog(sim);\n",
        "targets": [*T_CASCADE],
        "why": (
            "an unplanned fog site makes every later site's simulated cursor 8 bytes short of "
            "the real one, so a refusal that belongs before the first write arrives after it."
        ),
    },
    {
        "id": "d3d-cascade-plan-lighting-bytes",
        "file": DIRTY,
        "old": "                             ((dirty & 0xFF8000u) != 0u ? 8u : 0u));",
        "new": "                             ((dirty & 0xFF8000u) != 0u ? 0u : 0u));",
        "targets": [*T_CASCADE],
        "why": "the light-enable pair is 8 bytes of the lighting site, ahead of the stream work.",
    },
    {
        "id": "d3d-cascade-plan-point-attenuated-bytes",
        "file": DIRTY,
        "old": "d3d8_guest_load32(0x003E3E9Cu) == 0u ? 20u : 56u);",
        "new": "d3d8_guest_load32(0x003E3E9Cu) == 0u ? 20u : 20u);",
        "targets": [*T_CASCADE],
        "why": "an attenuated point packet is 56 bytes and moves every following site.",
    },
    {
        "id": "d3d-cascade-plan-stream-site-dropped",
        "file": RESOURCE,
        "old": "        d3d8_pushbuffer_sim_site(&sim, 0x003D4FB0u, stream_bytes);\n",
        "new": "        if (false) d3d8_pushbuffer_sim_site(&sim, 0x003D4FB0u, stream_bytes);\n",
        "targets": [*T_CASCADE],
        "why": (
            "the stream work has its own preamble after the cascade: unplanned, a roll-over "
            "there is only discovered after the cascade and the stream header are written."
        ),
    },
    {
        "id": "d3d-cascade-plan-draw-reservation-dropped",
        "file": RESOURCE,
        "old": "    (void)d3d8_pushbuffer_sim_reserve(&sim, dwords);\n",
        "new": "    if (false) (void)d3d8_pushbuffer_sim_reserve(&sim, dwords);\n",
        "targets": [*T_CASCADE],
        "why": (
            "the draw's sized reservation is part of the plan, so a packet that cannot fit the "
            "ring refuses before the cascade writes, not after."
        ),
    },
    {
        "id": "d3d-cascade-draw-reservation-unsized",
        "file": RESOURCE,
        "old": "    uint32_t cursor = d3d8_pushbuffer_reserve(chunks + 5u);",
        "new": "    uint32_t cursor = d3d8_device_load32(D3D8_DEV_CURSOR);",
        "targets": [*T_CASCADE, ORACLE_SIZED],
        "why": (
            "0x003D6B30 raises the segment for a large draw; without it a big draw runs past the "
            "limit and the ring end."
        ),
    },
    {
        "id": "d3d-refill-sized-boundary",
        "file": PUSH,
        "old": "    if (cursor + dwords * 4u >= limit + 0x200u) {",
        "new": "    if (cursor + dwords * 4u > limit + 0x200u) {",
        "targets": [*T_PUSH, ORACLE_SIZED],
        "why": "0x003D6B30 compares with jb: a request ending exactly at limit + 0x200 refills.",
    },
    {
        "id": "d3d-refill-sized-preview-boundary",
        "file": PUSH,
        "old": "    if (*cursor + dwords * 4u < *limit + 0x200u) return false;",
        "new": "    if (*cursor + dwords * 4u <= *limit + 0x200u) return false;",
        "targets": [*T_PUSH],
        "why": "the preview must agree with the real reservation at the exact boundary.",
    },
    {
        "id": "d3d-refill-sized-half-not-raised",
        "file": PUSH,
        "old": "    *half = need > (kick >> 1) ? need : (kick >> 1);",
        "new": "    *half = kick >> 1;",
        "targets": [*T_PUSH, ORACLE_REFILL],
        "why": (
            "the first size of 0x003D69E0 decides whether the ring wraps; not raising it for a "
            "large request wraps where the original clamps or the reverse."
        ),
    },
    {
        "id": "d3d-refill-sized-kickoff-not-raised",
        "file": PUSH,
        "old": "    *kickoff = need > kick ? need : kick;",
        "new": "    *kickoff = kick;",
        "targets": [*T_PUSH, *T_CASCADE, ORACLE_REFILL],
        "why": "a large request needs a segment that holds it, or the draw writes past the limit.",
    },
    {
        "id": "d3d-refill-alternate-put-allowed",
        "file": PUSH,
        "old": "    if ((d3d8_device_load32(D3D8_DEV_FLAGS) & DEV_FLAG_ALTERNATE_PUT) != 0u) {",
        "new": "    if ((d3d8_device_load32(D3D8_DEV_FLAGS) & DEV_FLAG_ALTERNATE_PUT) != 0u && false) {",
        "targets": [*T_PUSH, *T_DIRTY, *T_CASCADE],
        "why": (
            "device flag 4 selects a put pointer nothing models: rolling over under it is a "
            "silent wrong answer, not a supported path."
        ),
    },
    {
        "id": "d3d-refill-larger-than-ring-allowed",
        "file": PUSH,
        "old": "    if (end < base || kickoff > end - base) {",
        "new": "    if (end < base || (kickoff > end - base && false)) {",
        "targets": [*T_PUSH, *T_CASCADE],
        "why": "a segment longer than the ring runs past its end into whatever follows it.",
    },
    {
        "id": "d3d-refill-wrap-at-equal",
        "file": PUSH,
        "old": "        if (cursor + half > end) {",
        "new": "        if (cursor + half >= end) {",
        "targets": [*T_PUSH, ORACLE_REFILL],
        "why": "the original wraps only when cursor + half is strictly past the ring end (jbe).",
    },
    {
        "id": "d3d-refill-preview-begin-at-limit-only",
        "file": PUSH,
        "old": "    if (*cursor < *limit) return false;",
        "new": "    if (*cursor <= *limit) return false;",
        "targets": [*T_PUSH, *T_TEXTURE],
        "why": "the preamble rolls at cursor >= limit; the preview must not skip the equal case.",
    },
    {
        "id": "d3d-refill-sim-site-bytes-not-counted",
        "file": PUSH,
        "old": "    sim->bytes += bytes;",
        "new": "    sim->bytes += 0u;",
        "targets": [*T_PUSH, *T_TEXTURE, *T_CASCADE],
        "why": "the indexed route lays its packet out after the planned bytes.",
    },
    {
        "id": "d3d-refill-sim-site-unmapped-span-allowed",
        "file": PUSH,
        "old": "    if ((uint64_t)sim->cursor + bytes > UINT32_MAX || kernel_guest_at(sim->cursor, bytes) == NULL)",
        "new": "    if ((uint64_t)sim->cursor + bytes > UINT32_MAX || (kernel_guest_at(sim->cursor, bytes) == NULL && false))",
        "targets": [*T_PUSH],
        "why": "a command span that leaves mapped memory must refuse before a write faults mid-cascade.",
    },
    {
        "id": "d3d-cascade-shader-bind-mode-site-unplanned",
        "file": "src/gpu/d3d8_shader_bind.c",
        "old": "    if (changed) {\n        span_start[spans]=plan_site(&sim,entry,mode_bytes);",
        "new": "    if (changed && false) {\n        span_start[spans]=plan_site(&sim,entry,mode_bytes);",
        "targets": ["test_d3d8_shader_bind"],
        "why": (
            "the mode-change packets are the first reservation site of the vertex shader binder. "
            "Unplanned, a roll-over at the handle write that follows them goes unforeseen and a "
            "refusal arrives after the packets are written."
        ),
    },
    {
        "id": "d3d-cascade-shader-bind-handle-site-unplanned",
        "file": "src/gpu/d3d8_shader_bind.c",
        "old": "    span_bytes[spans++]=8u;",
        "new": "    span_bytes[spans++]=0u;",
        "targets": [
            "test_d3d8_shader_bind",
            "pytest:tests/test_d3d8_shader_refill_oracle.py -k 4019760",
        ],
        "why": "the 8-byte handle write is its own site, 8 bytes long.",
    },
    {
        "id": "d3d-fence-insert-wrapper-second-history",
        "file": "src/gpu/d3d8_gpu.c",
        "old": "    return d3d8_gpu_fence_insert(0u);",
        "new": "    return d3d8_gpu_fence_insert(1u);",
        "targets": ["test_d3d8_gpu", "pytest:tests/test_d3d8_fence_oracle.py"],
        "why": "InsertFence passes flags 0: a flag of 1 also records the second history entry.",
    },
    {
        "id": "d3d-fence-insert-wrapper-no-kick",
        "file": "src/gpu/d3d8_gpu.c",
        "old": "    return d3d8_gpu_fence_insert(0u);",
        "new": "    return d3d8_gpu_fence_insert(2u);",
        "targets": ["test_d3d8_gpu", "pytest:tests/test_d3d8_fence_oracle.py"],
        "why": "InsertFence kicks at once, so the title's wait on the fence completes.",
    },
    {
        "id": "d3d-fence-block-until-wrong-fence",
        "file": "src/gpu/d3d8_gpu.c",
        "old": "    d3d8_gpu_fence_wait(argument(context, 0u, 0x003D34B0u), 4u);",
        "new": "    d3d8_gpu_fence_wait(0u, 4u);",
        "targets": ["test_d3d8_gpu", "pytest:tests/test_d3d8_fence_oracle.py"],
        "why": (
            "BlockUntilFence waits on the fence it is given; waiting on zero returns at once and "
            "the title proceeds before the GPU has finished with what it is about to reuse."
        ),
    },
]
