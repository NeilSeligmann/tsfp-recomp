# ruff: noqa: E501  (the old/new strings are exact one-line C anchors)
"""Mutations for the live Vulkan texture path (T792): `src/gpu/live_vk_texture.c`, the guest write watch
`src/gpu/live_texture_watch.c` and its hook in `src/gpu/d3d8_lock.c`.

TARGETS. `test_live_vk_texture` needs a Vulkan device: run the harness as
`SDL_VIDEODRIVER=x11 TSFP_TEST_GPU_WINDOW_REQUIRED=1 xvfb-run -a ./.venv/bin/python tools/mutate/c_suites.py --prefix t792-vk`
(llvmpipe is enough). Without a device the suite exits 77, which the harness reports as SKIPPED-NO-DEVICE, not as a kill.
`test_live_texture_watch` and `test_d3d8_lock` need no device. Not mutated: the sampled layout copied from a target
(`layout = target->layout`, llvmpipe does not check it, only a validation layer would) and image usage flags.
"""

VK = "src/gpu/live_vk_texture.c"
WATCH = "src/gpu/live_texture_watch.c"
LOCK = "src/gpu/d3d8_lock.c"


def vk(identifier: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"t792-vk-{identifier}",
        "file": VK,
        "old": old,
        "new": new,
        "targets": ["test_live_vk_texture"],
        "why": why,
    }


def watch(identifier: str, old: str, new: str, why: str) -> dict:
    return {
        "id": f"t792-watch-{identifier}",
        "file": WATCH,
        "old": old,
        "new": new,
        "targets": ["test_live_texture_watch"],
        "why": why,
    }


MUTATIONS: list[dict] = [
    vk(
        "addr-reserved-bits",
        "(address_word & ~0x00000F0Fu) != 0u",
        "(address_word & ~0x0001FF0Fu) != 0u",
        "address bits beyond U and V must refuse",
    ),
    vk(
        "addr-unbound",
        "if (address_word == 0u) {",
        "if (address_word == 1u) {",
        "address word 0 is unbound",
    ),
    vk(
        "addr-v-shift",
        "(address_word >> 8) & 0xFu",
        "(address_word >> 4) & 0xFu",
        "V mode lives in bits 8 to 11",
    ),
    vk(
        "addr-wrap",
        "case 1u: *out = VK_SAMPLER_ADDRESS_MODE_REPEAT;",
        "case 1u: *out = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;",
        "mode 1 is wrap",
    ),
    vk(
        "addr-mirror",
        "case 2u: *out = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;",
        "case 2u: *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;",
        "mode 2 is mirror",
    ),
    vk(
        "addr-border",
        "case 4u: *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;",
        "case 4u: *out = VK_SAMPLER_ADDRESS_MODE_REPEAT;",
        "mode 4 is border",
    ),
    vk(
        "addr-clamp-ogl",
        "case 5u: *out = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;",
        "case 5u: *out = VK_SAMPLER_ADDRESS_MODE_REPEAT;",
        "mode 5 clamps to the edge",
    ),
    vk(
        "filter-lod-bias",
        "(filter_word & 0x1FFFu) != 0u",
        "(filter_word & 0x1FFEu) != 0u",
        "LOD bias bit 0 must refuse",
    ),
    vk(
        "filter-lod-bias-high",
        "(filter_word & 0x1FFFu) != 0u",
        "(filter_word & 0x0FFFu) != 0u",
        "LOD bias bit 12 must refuse",
    ),
    vk(
        "filter-top-nibble",
        "(filter_word & 0xF0000000u) != 0u",
        "(filter_word & 0xE0000000u) != 0u",
        "the top nibble must refuse",
    ),
    vk("filter-min-zero", "min < 1u || min > 7u", "min > 7u", "min filter 0 is invalid"),
    vk(
        "filter-min-eight",
        "min < 1u || min > 7u",
        "min < 1u || min > 8u",
        "min filter 8 is invalid",
    ),
    vk(
        "filter-mag-four",
        "mag != 1u && mag != 2u && mag != 4u",
        "mag != 1u && mag != 2u",
        "mag 4 (convolution) maps to linear",
    ),
    vk(
        "filter-mag-three",
        "mag != 1u && mag != 2u && mag != 4u",
        "mag != 1u && mag != 2u && mag != 3u && mag != 4u",
        "mag 3 is refused",
    ),
    vk(
        "filter-min-nearest-five",
        "(min == 1u || min == 3u || min == 5u)",
        "(min == 1u || min == 3u)",
        "min 5 is nearest on the base level",
    ),
    vk(
        "filter-min-nearest-three",
        "(min == 1u || min == 3u || min == 5u)",
        "(min == 1u || min == 5u)",
        "min 3 is nearest on the base level",
    ),
    vk(
        "filter-mag-nearest",
        "desc.mag_filter = mag == 1u ?",
        "desc.mag_filter = mag == 2u ?",
        "mag 1 is nearest",
    ),
    vk(
        "measured-filter",
        "filter_word == 0x02062000u;",
        "filter_word != 0x02062000u;",
        "only the measured filter word is measured",
    ),
    vk(
        "measured-clamp",
        "address_word == 0x00000303u)",
        "address_word == 0x00000304u)",
        "the measured clamp word",
    ),
    vk(
        "staging-copy",
        "memcpy(set->staging_map, rgba, bytes);",
        "memcpy(set->staging_map, rgba, bytes - 4u);",
        "the whole decode reaches the staging buffer",
    ),
    vk(
        "copy-extent",
        "region->imageExtent = (VkExtent3D){plan->mip[level].width, plan->mip[level].height, 1u};",
        "region->imageExtent = (VkExtent3D){plan->mip[level].width / 2u, plan->mip[level].height, 1u};",
        "the copy covers the whole image",
    ),
    vk(
        "upload-on-need",
        "if (!versioned && (missing || result->needs_upload)) {",
        "if (!versioned && missing) {",
        "a rewritten guest texture must upload again",
    ),
    vk(
        "upload-on-missing",
        "if (!versioned && (missing || result->needs_upload)) {",
        "if (!versioned && result->needs_upload) {",
        "a set without the image must upload even when the cache says uploaded",
    ),
    vk(
        "mark-uploaded",
        (
            "live_texture_mark_uploaded(cache, result->entry, result->generation);\n"
            "            set->stats.uploads++;\n            set->stats.upload_bytes += bytes;"
        ),
        (
            "(void)cache;\n            set->stats.uploads++;\n"
            "            set->stats.upload_bytes += bytes;"
        ),
        "the cache must learn the image is current",
    ),
    vk(
        "uploaded-flag",
        "bound->uploaded = true;\n        }\n        descriptor_index = result->entry;",
        "bound->uploaded = false;\n        }\n        descriptor_index = result->entry;",
        "bound reports the upload",
    ),
    vk(
        "upload-bytes",
        "set->stats.upload_bytes += bytes;",
        "set->stats.upload_bytes += 1u;",
        "the byte count of an upload",
    ),
    vk(
        "resize",
        "const bool resized = slot->image != VK_NULL_HANDLE &&",
        "const bool resized = false && slot->image != VK_NULL_HANDLE &&",
        "an entry reused for another size needs a new image",
    ),
    vk(
        "sampler-ok",
        "if (!sampler->ok) {",
        "if (sampler->ok && false) {",
        "an undecodable sampler is never bound",
    ),
    vk(
        "descriptor-sampler",
        "state->view == view && state->sampler == sampler && state->layout == layout",
        "state->view == view && state->layout == layout",
        "a changed sampler rewrites the descriptor",
    ),
    vk(
        "descriptor-view",
        "state->view == view && state->sampler == sampler && state->layout == layout",
        "state->sampler == sampler && state->layout == layout",
        "a changed view rewrites the descriptor",
    ),
    vk(
        "target-descriptor",
        "descriptor_index = GUEST_SLOTS + target_index;",
        "descriptor_index = target_index;",
        "a target never shares a guest slot's descriptor",
    ),
    vk(
        "target-id",
        "set->targets[target_index].id == result->target_id",
        "set->targets[target_index].id != result->target_id",
        "targets are found by id",
    ),
    vk(
        "target-update",
        "set->targets[index].view = view;",
        "set->targets[index].view = set->targets[index].view;",
        "registering an id again replaces its view",
    ),
    vk(
        "target-update-id",
        "if (set->targets[index].used && set->targets[index].id == id) {",
        "if (set->targets[index].used && set->targets[index].id != id) {",
        "an id registers into one slot",
    ),
    vk(
        "target-null-view",
        "if (view == VK_NULL_HANDLE) {\n        return false;",
        "if (false) {\n        return false;",
        "a null view is refused",
    ),
    vk(
        "target-full",
        "if (free_index == TARGET_SLOTS) {\n        return false;",
        "if (false) {\n        return false;",
        "the target table is bounded",
    ),
    vk(
        "target-clear",
        "memset(set->targets, 0, sizeof set->targets);",
        "(void)set;",
        "clearing forgets every target",
    ),
    vk(
        "trim-in-use",
        "!cache->entries[index].in_use) {",
        "cache->entries[index].in_use) {",
        "only images of entries that left the cache are destroyed",
    ),
    vk(
        "trim-count",
        "            destroyed++;",
        "            destroyed += 2u;",
        "trim counts what it destroyed",
    ),
    vk(
        "create-gipa",
        "|| get_instance_proc_addr == NULL) {",
        ") {",
        "a missing instance loader is refused",
    ),
    watch("zero-bytes", "if (bytes == 0u) {", "if (bytes == 1u) {", "an empty note is nothing"),
    watch(
        "capacity",
        "if (pending_count < LIVE_TEXTURE_WATCH_PENDING) {",
        "if (pending_count + 1u < LIVE_TEXTURE_WATCH_PENDING) {",
        "the queue holds LIVE_TEXTURE_WATCH_PENDING notes",
    ),
    watch(
        "overflow-once",
        "} else if (!pending_overflow) {",
        "} else if (true) {",
        "an overflow is counted once per drain",
    ),
    watch(
        "overflow-all",
        "if (overflow) {",
        "if (overflow && false) {",
        "an overflow invalidates every texture",
    ),
    watch(
        "overflow-range",
        "live_texture_note_write(cache, 0u, (size_t)1u << 32);",
        "live_texture_note_write(cache, 0u, 1u);",
        "the overflow range covers the whole address space",
    ),
    watch(
        "drain-clears-count",
        "    pending_count = 0u;\n    pending_overflow = false;\n    pthread_mutex_unlock(&watch_lock);\n    size_t stale",
        "    pending_overflow = false;\n    pthread_mutex_unlock(&watch_lock);\n    size_t stale",
        "a drained note is gone",
    ),
    watch(
        "drain-clears-overflow",
        "    pending_count = 0u;\n    pending_overflow = false;\n    pthread_mutex_unlock(&watch_lock);\n    size_t stale",
        "    pending_count = 0u;\n    pthread_mutex_unlock(&watch_lock);\n    size_t stale",
        "the overflow flag is consumed",
    ),
    watch("noted-count", "    noted_total++;", "    (void)noted_total;", "notes are counted"),
    watch(
        "pending-bound",
        "if (index < pending_count) {",
        "if (index <= pending_count) {",
        "pending is bounded by the queue",
    ),
    {
        "id": "t792-lock-note-removed",
        "file": LOCK,
        "old": "    live_texture_watch_note(header[1] & 0x0FFFFFFFu, (uint32_t)extent);\n",
        "new": "",
        "targets": ["test_d3d8_lock"],
        "why": "LockRect must tell the live texture cache",
    },
    {
        "id": "t792-lock-note-address",
        "file": LOCK,
        "old": "live_texture_watch_note(header[1] & 0x0FFFFFFFu, (uint32_t)extent);",
        "new": "live_texture_watch_note(header[1] & 0x00FFFFFFu, (uint32_t)extent);",
        "targets": ["test_d3d8_lock"],
        "why": "the noted range is in the 28 bit Data domain",
    },
    {
        "id": "t792-lock-note-bytes",
        "file": LOCK,
        "old": "live_texture_watch_note(header[1] & 0x0FFFFFFFu, (uint32_t)extent);",
        "new": "live_texture_watch_note(header[1] & 0x0FFFFFFFu, pitch);",
        "targets": ["test_d3d8_lock"],
        "why": "the noted length is the locked extent",
    },
]
for entry in MUTATIONS:
    entry.setdefault("targets", ["test_live_vk_texture"])
