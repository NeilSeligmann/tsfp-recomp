# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for the T819 playback mode: the audio clock, the picture queue and the guest probe cache.

Grouped by what a survivor would let through:

    clock     the audio sink's clock mode (src/host/present_sink.c): the anchor from the first stamped write, the prefill
              gate, the rebuffer pause, the refill target measured from the guest's speed (floor, cap, fast guest), the give-up, the stall log, the kick, the smooth media clock and its
              clamp, the silence counted while rebuffering
    queue     the window sink's playback queue: shown only when the media clock reached the stamp, the newest due picture
              wins, dropped pictures counted, a picture shown without a clock, the bounded queue and its wait, the
              give-up after a second, the stop flush, late pictures, the guest never sleeping on the vblank hook
    probe     the guest memory probe cache (src/xbox/kernel_call.c): a positive answer remembered, a refusal never
              remembered, the flush

Kills are by `test_present_audio_clock` (clock), `test_present_playback` (queue, SDL dummy video driver, SKIPPED without
SDL3 and then it kills nothing) and `test_kernel_guest_probe` (probe).

EQUIVALENT, removed: the prefill comparison `>=` against `>`, on either side alone. Playback starts at the first of the write-side
check and the pull-side check that sees enough audio, so each side covers the other (both mutated at once would be killed, the
harness mutates one anchor at a time).

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

_SINK = "src/host/present_sink.c"
_CALL = "src/xbox/kernel_call.c"
_CLOCK_TEST = ["test_present_audio_clock"]
_QUEUE_TEST = ["test_present_playback"]
_PROBE_TEST = ["test_kernel_guest_probe"]
_FAST_TEST = ["test_kernel_guest_fast_copy"]


def _row(file: str, mutation_id: str, old: str, new: str, why: str, targets: list[str]) -> dict:
    return {
        "id": f"present-playback-{mutation_id}",
        "file": file,
        "old": old,
        "new": new,
        "targets": list(targets),
        "why": why,
    }


MUTATIONS: list[dict] = [
    # =================================================================== clock
    _row(
        _SINK,
        "clock-anchor-at-end",
        "sink->anchor_vt_ns = end_vt_ns > span ? end_vt_ns - span : 0u;",
        "sink->anchor_vt_ns = end_vt_ns > span ? end_vt_ns - span / 2u : 0u;",
        "the media clock starts half a write too late, every picture shows early or late by that much.",
        _CLOCK_TEST,
    ),
    _row(
        _SINK,
        "clock-rebuffer-does-not-pause",
        "sink->playing = false;\n        stall_begin(sink, false);",
        "sink->playing = true;\n        stall_begin(sink, false);",
        "a dry ring keeps playing: the device chops instead of pausing and the media clock runs through the gap.",
        _CLOCK_TEST,
    ),
    _row(
        _SINK,
        "clock-adapt-ignores-rate",
        "const uint64_t wanted = gained * AUDIO_REFILL_FREEZE_MS / entry->duration_ms;",
        "const uint64_t wanted = gained;",
        "the refill target ignores how fast the guest delivered: a slow guest gets the same short buffer as a fast one.",
        _CLOCK_TEST,
    ),
    _row(
        _SINK,
        "clock-adapt-no-floor",
        "sink->prefill_frames = target < floor_frames ? floor_frames : target;",
        "sink->prefill_frames = target;",
        "a very slow guest drives the target to nothing and the sound chops with a refill of a few frames.",
        _CLOCK_TEST,
    ),
    _row(
        _SINK,
        "clock-adapt-uncapped",
        "target = wanted < sink->prefill_max_frames ? (size_t)wanted : sink->prefill_max_frames;",
        "target = (size_t)wanted;",
        "the target exceeds the maximum (and the ring), so playback could never restart after a stall.",
        _CLOCK_TEST,
    ),
    _row(
        _SINK,
        "clock-adapt-fast-guest-small",
        "size_t target = sink->prefill_max_frames;",
        "size_t target = 0u;",
        "a refill too quick to measure is treated as the slowest guest: the buffer shrinks to the floor on a fast guest.",
        _CLOCK_TEST,
    ),
    _row(
        _SINK,
        "clock-adapt-never-measures",
        "if (entry->duration_ms >= AUDIO_REFILL_MEASURE_MIN_MS) {",
        "if (entry->duration_ms >= AUDIO_REFILL_MEASURE_MIN_MS + 100000u) {",
        "every stall counts as a fast guest: the target is always the maximum, the freeze is never bounded.",
        _CLOCK_TEST,
    ),
    _row(
        _SINK,
        "clock-adapt-on-start",
        "if (sink->clock_mode && !entry->initial) {",
        "if (sink->clock_mode) {",
        "the start of playback changes the refill target before any dry ring was measured.",
        _CLOCK_TEST,
    ),
    _row(
        _SINK,
        "clock-giveup-too-long",
        "#define AUDIO_STALL_RESUME_NS 700000000LL",
        "#define AUDIO_STALL_RESUME_NS 7000000000LL",
        "a refill that never reaches its target freezes the sound and the window for seven seconds instead of 0.7 s.",
        _CLOCK_TEST,
    ),
    _row(
        _SINK,
        "clock-stall-log-kick-reason",
        "        stall_end(sink, 3u);",
        "        stall_end(sink, 1u);",
        "the stop report cannot tell a refill ended by the kick from one that reached its target.",
        _CLOCK_TEST,
    ),
    _row(
        _SINK,
        "clock-stall-log-writes",
        "        sink->stalls[sink->stall_count - 1u].writes_during++;",
        "        sink->stalls[sink->stall_count - 1u].writes_during += 0u;",
        "the stop report cannot tell a stall with a silent guest from one where the guest was writing.",
        _CLOCK_TEST,
    ),
    _row(
        _SINK,
        "clock-kick-inverted",
        "if (sink->clock_mode && !sink->playing && sink->ring_fill != 0u) {\n        sink->playing = true;",
        "if (sink->clock_mode && sink->playing && sink->ring_fill != 0u) {\n        sink->playing = true;",
        "the kick never starts a paused ring, a full picture queue and an unfinished refill deadlock the guest.",
        _CLOCK_TEST,
    ),
    _row(
        _SINK,
        "clock-no-clamp-inside-chunk",
        "if (inside > sink->last_pull_take) {",
        "if (inside > sink->last_pull_take * 2u) {",
        "the interpolated clock runs past the end of the last pulled chunk, it does not stand still while paused.",
        _CLOCK_TEST,
    ),
    _row(
        _SINK,
        "clock-jumps-per-chunk",
        "sink->last_pull_take = consumed;",
        "sink->last_pull_take = 0u;",
        "the clock moves only in whole device chunks (about 20 ms steps), pictures show with chunk jitter.",
        _CLOCK_TEST,
    ),
    _row(
        _SINK,
        "clock-rebuffer-silence-uncounted",
        "sink->counts.rebuffer_frames += frames;\n                if (sink->counts.pulled != 0u) {",
        "sink->counts.rebuffer_frames += 0u;\n                if (sink->counts.pulled != 0u) {",
        "the silence pulled while waiting for the refill is invisible in the stop report.",
        _CLOCK_TEST,
    ),
    # =================================================================== queue
    _row(
        _SINK,
        "queue-ignores-clock",
        "sink->pq[(sink->pq_head + consumed) % PLAYBACK_QUEUE_SLOTS].vt_ns <= media_vt) {",
        "sink->pq[(sink->pq_head + consumed) % PLAYBACK_QUEUE_SLOTS].vt_ns <= media_vt + 100000000000ull) {",
        "every picture is shown as soon as it arrives: video runs ahead of the audio.",
        _QUEUE_TEST,
    ),
    _row(
        _SINK,
        "queue-shows-oldest",
        "const size_t slot = (sink->pq_head + consumed - 1u) % PLAYBACK_QUEUE_SLOTS;",
        "const size_t slot = sink->pq_head % PLAYBACK_QUEUE_SLOTS;",
        "after a stall the window shows the oldest due picture instead of the newest, and stays behind.",
        _QUEUE_TEST,
    ),
    _row(
        _SINK,
        "queue-drops-uncounted",
        "sink->timing.dropped += consumed - 1u;",
        "sink->timing.dropped += 0u;",
        "skipped pictures are invisible in the stop report.",
        _QUEUE_TEST,
    ),
    _row(
        _SINK,
        "queue-late-uncounted",
        "        sink->timing.late_shown++;\n",
        "",
        "pictures shown long after their stamp are not reported.",
        _QUEUE_TEST,
    ),
    _row(
        _SINK,
        "queue-waits-for-clock-when-none",
        "if (!clocked) {\n        consumed = sink->pq_count;",
        "if (!clocked) {\n        consumed = 0u;",
        "before the first audio write nothing is ever shown (a title with no audio shows no movie).",
        _QUEUE_TEST,
    ),
    _row(
        _SINK,
        "queue-unbounded",
        "if (sink->pq_count == PLAYBACK_QUEUE_SLOTS && !sink->closing) {",
        "if (sink->pq_count == PLAYBACK_QUEUE_SLOTS + 1000u && !sink->closing) {",
        "the guest never waits for the presenter, the queue overruns its slots.",
        _QUEUE_TEST,
    ),
    _row(
        _SINK,
        "queue-give-up-keeps-oldest",
        "        if (sink->pq_count == PLAYBACK_QUEUE_SLOTS) {\n            sink->timing.queue_timeouts++;\n            queue_drop_oldest(sink);",
        "        if (sink->pq_count == PLAYBACK_QUEUE_SLOTS) {\n            sink->timing.queue_timeouts++;\n            (void)queue_drop_oldest;",
        "after the wait gives up the full queue is written into anyway.",
        _QUEUE_TEST,
    ),
    _row(
        _SINK,
        "queue-room-not-signalled",
        "    sink->pq_count -= consumed;\n    pthread_cond_broadcast(&sink->queue_room);",
        "    sink->pq_count -= consumed;",
        "a waiting guest is never woken by the presenter, every full queue costs the whole one second timeout.",
        _QUEUE_TEST,
    ),
    _row(
        _SINK,
        "queue-stop-does-not-flush",
        "    (void)argument;\n    playback_tick(sink, true);\n",
        "    (void)argument;\n",
        "the hold at the stop leaves the queued pictures unshown.",
        _QUEUE_TEST,
    ),
    _row(
        _SINK,
        "queue-vblank-still-sleeps",
        "        if (!sink->playback || sink->interactive) {\n            gft_enter(GFT_PACE, 0u);\n            pace_wait(sink);",
        "        if (true) {\n            gft_enter(GFT_PACE, 0u);\n            pace_wait(sink);",
        "the guest sleeps on the vblank hook in playback mode: the presenter paces the guest again.",
        _QUEUE_TEST,
    ),
    _row(
        _SINK,
        "queue-gap-not-tracked",
        "        sink->timing.model_gap_max_ns = vt_ns - sink->last_submit_vt_ns;",
        "        sink->timing.model_gap_max_ns = 0u;",
        "the report cannot tell the title's own pause between two movies from a playback stall.",
        _QUEUE_TEST,
    ),
    _row(
        _SINK,
        "queue-gap-wrong-picture",
        "        sink->timing.model_gap_after = sink->last_submit_number;",
        "        sink->timing.model_gap_after = number;",
        "the report names the wrong picture for the pause.",
        _QUEUE_TEST,
    ),
    # =================================================================== probe
    _row(
        _CALL,
        "probe-positive-not-remembered",
        "            probe_remember(pages[index], epoch);\n",
        "            probe_remember(pages[index], epoch - 1u);\n",
        "the cache never fills: every guest memory access pays a system call again.",
        _PROBE_TEST,
    ),
    _row(
        _CALL,
        "probe-flush-is-a-noop",
        "    atomic_fetch_add_explicit(&g_probe_epoch, 1u, memory_order_release);",
        "    (void)g_probe_epoch;",
        "a page that became a guard band or was unmapped is still reported readable.",
        _PROBE_TEST,
    ),
    _row(
        _CALL,
        "probe-refusal-remembered",
        "        if (process_vm_readv(host_pid(), &local, 1u, remote, count, 0u) != (ssize_t)count) {\n            return false;",
        "        if (process_vm_readv(host_pid(), &local, 1u, remote, count, 0u) != (ssize_t)count) {\n            probe_remember(pages[0], epoch);\n            return false;",
        "a refused page is remembered as readable: the next probe of a guard page says yes and the access faults.",
        _PROBE_TEST,
    ),
    # ============================================================ fast copy (T827)
    _row(
        _CALL,
        "fast-copy-ignores-pending-change",
        "        }\n    }\n    copy_flag_raise(slot);\n    if (atomic_load(&g_change_pending) != 0 || atomic_load_explicit(&g_probe_epoch, memory_order_acquire) != epoch) {",
        "        }\n    }\n    copy_flag_raise(slot);\n    if (atomic_load_explicit(&g_probe_epoch, memory_order_acquire) != epoch) {",
        "a read copies from a page that is being unmapped: the host faults instead of the accessor returning false.",
        _FAST_TEST,
    ),
    _row(
        _CALL,
        "small-fast-ignores-pending-change",
        "        pages++;\n    }\n    copy_flag_raise(slot);\n    if (atomic_load(&g_change_pending) != 0 || atomic_load_explicit(&g_probe_epoch, memory_order_acquire) != epoch) {",
        "        pages++;\n    }\n    copy_flag_raise(slot);\n    if (atomic_load_explicit(&g_probe_epoch, memory_order_acquire) != epoch) {",
        "T1289: a 1 to 16 byte access copies from a page that is being unmapped: the host faults instead of the accessor returning false.",
        _FAST_TEST,
    ),
    _row(
        _CALL,
        "small-fast-ignores-epoch",
        "        if (t_probe_cache[index].page != cursor || t_probe_cache[index].epoch != epoch ||\n            (writing &&",
        "        if (t_probe_cache[index].page != cursor ||\n            (writing &&",
        "T1289: a 1 to 16 byte access trusts a page cached before the last page change: a guard band or an unmapped page is copied from and the host faults.",
        ["test_kernel_guest_small_fast", "test_kernel_guest_fast_copy"],
    ),
    _row(
        _CALL,
        "probe-cache-too-small",
        "#define PROBE_CACHE_SLOTS 4096u",
        "#define PROBE_CACHE_SLOTS 64u",
        "T1289: pages 64 apart evict each other again, 4.8 M probe system calls per 30 s of the Story replay.",
        ["test_kernel_guest_small_fast"],
    ),
    _row(
        _CALL,
        "fast-copy-begin-does-not-announce",
        "    atomic_fetch_add(&g_change_pending, 1);\n#if defined(__linux__) && defined(__NR_membarrier)",
        "#if defined(__linux__) && defined(__NR_membarrier)",
        "the page change is not announced: reads keep copying from the page being unmapped.",
        _FAST_TEST,
    ),
    _row(
        _CALL,
        "fast-copy-flush-leaves-pending",
        "        atomic_fetch_sub(&g_change_pending, 1);",
        "        (void)g_change_pending;",
        "after the first page change every read takes the system call again: the speed-up is silently lost.",
        _FAST_TEST,
    ),
    _row(
        _CALL,
        "fast-copy-slot-never-released",
        "        atomic_store(&g_copy_owned[slot], 0);",
        "        (void)g_copy_owned;",
        "after 256 threads have lived and died no thread gets the fast path: the speed-up is silently lost.",
        _FAST_TEST,
    ),
    _row(
        _CALL,
        "fast-copy-write-without-proof",
        "    write_remember(at, length, epoch);\n    return true;",
        "    (void)write_remember;\n    (void)epoch;\n    return true;",
        "a write never becomes a plain copy (no page is ever remembered as writable).",
        _FAST_TEST,
    ),
    _row(
        _CALL,
        "fast-copy-write-uses-read-cache",
        "const bool known = writing ? (t_write_cache[index].page == cursor && t_write_cache[index].epoch == epoch)\n                                   : probe_cached(cursor, epoch);",
        "const bool known = probe_cached(cursor, epoch) && index < PROBE_CACHE_SLOTS;",
        "a page that was only read is written with a plain copy: a write to a read-only guest page faults the host.",
        _FAST_TEST,
    ),
    _row(
        _CALL,
        "fast-copy-checks-first-page-only",
        "for (uintptr_t cursor = first; cursor <= last; cursor += page) {\n        const unsigned index = probe_index(cursor);\n        const bool known = writing",
        "for (uintptr_t cursor = first; cursor < last || cursor == first; cursor += page) {\n        const unsigned index = probe_index(cursor);\n        const bool known = writing",
        "a write that crosses into a page never proven writable is copied without proof: a read-only page there faults the host.",
        _FAST_TEST,
    ),
    # ============================================================ scope without a mask (T827)
    _row(
        "src/host/host_runtime.c",
        "scope-push-masks-signals",
        "    t_depth=depth+1u;return true;",
        "    {sigset_t guard;sigemptyset(&guard);if(pthread_sigmask(SIG_BLOCK,&guard,NULL)!=0)return false;}\n    t_depth=depth+1u;return true;",
        "push makes a signal mask system call again: four per guest call scope, 28 percent of the guest CPU in the movies.",
        ["test_host_scope_no_mask"],
    ),
    _row(
        "src/host/host_runtime.c",
        "scope-pop-masks-signals",
        "    t_depth=depth;\n    atomic_signal_fence(memory_order_seq_cst);",
        "    {sigset_t guard;sigemptyset(&guard);if(pthread_sigmask(SIG_BLOCK,&guard,NULL)!=0)return false;}\n    t_depth=depth;\n    atomic_signal_fence(memory_order_seq_cst);",
        "pop makes a signal mask system call again.",
        ["test_host_scope_no_mask"],
    ),
]
