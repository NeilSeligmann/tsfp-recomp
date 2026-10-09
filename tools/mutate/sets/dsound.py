# ruff: noqa: E501  (the anchors are verbatim C source lines)
"""Mutations for `src/audio/`, the DirectSound facade (T452, T351's largest zero-set gap).

Covered files and what each family catches:

    dsound_hle.c              the surface table rows, exact-match lookup and registration,
                              the stub-once versus unknown-every-time reports, the report
                              ranking, codec readiness, the DSP ack derivation boundary and
                              refusals, the two-direction crosscheck, the requires list.
    dsound_device.c           the SILENT device ownership validation (header words, heap
                              size, singleton), the lease transactions (reference and serial
                              limits, child validation, duplicate children, revalidation,
                              abort paths), reset, create refusals, the movie reference
                              accounting and the destructor guard (DESTROY_REFERENCES).
    dsound_stream.c           the passive stream: policy gates, header snapshot validation,
                              the alias families (every protected range), the vtable safety
                              set, every exact startup scope of status, volume, pause, flush,
                              discontinuity and the ordered spatial caches, reset ordering,
                              the measured caller gates and dispatch argument order.
    dsound_buffer.c           the same for the passive 36-byte buffer plus the output scope
                              range and the stop-boundary readiness probe.
    dsound_listener.c         the ordered doppler, position, orientation, commit, DoWork
                              state machine, the incarnation check, stack bounds.
    dsound_stream_scope.c     the descriptor and format validation, each field offset and
    dsound_buffer_scope.c     literal, the end-of-address-space bound, the overlap probe.
    dsound_hrtf.c             the guarded critical section, the 11-word table write.
    dsound_effects_binding.c  the policy and repeat gates, the alias list, the descriptor
                              marshalling, ownership integrity.
    dsound_effects_metadata.c the image layout validation (bounds, overlaps, ranges).

`dsound_movie_stream.c` is NOT in this set: `dsound_movie_stream.py` (T392) and
`dsound_synch_playback.py` (T421) already carry its 50-odd mutants.

Kill suites are the plain-build ctest binaries `test_dsound_*`. The `*_routes` suites need
the lifted tree and the Python oracles need the user's XBE, so neither is a target here.

EQUIVALENT MUTANTS CONSIDERED AND LEFT OUT, so nobody re-adds them:
  - `validate_owned` dropping `shadow_reference == 0u`: `owned_address != 0u` already
    implies a nonzero shadow, both are zeroed together in reset and set together in create.
  - `dsound_stream.c` PAUSE storing `1u` instead of `r->argument` as the mode: the gate above
    admits only mode 1, so the stored value is the same.
  - `dsound_listener.c` stage 3/4 dropping the `doppler_bits`, `position` or `orientation`
    comparisons: stages 0..2 only ever cache the exact expected values (the `expected`
    table), so mask 7 already implies them. Likewise the doppler and position `memcpy` of
    all-zero values.
  - `dsound_buffer.c` SetVolume and Pause carry no scope check of their own: a recorded SetBufferData
    (`data_sets`) already proves one of the two startup scopes and caches never go away, so a scope
    check there could only be an equivalent mutant (the first SetVolume draft had one, two survivors).
    Pause checks only `volume_sets` and SetFrequency only `pause_sets` for the same reason (a recorded
    SetVolume implies a recorded SetBufferData, a Pause a SetVolume), a `data_sets` test there survived.
  - `dsound_stream.c` SetFormat scope fields beyond `flags` (rate, channels, block, average, max packets,
    cache mask): `dsound_stream_scope.c` admits exactly one stereo 44100 scope for flags 0, so the code tests flags
    only. SetFormat dropping the null or unreadable input test (`kernel_guest_read_bytes`): the input array is
    zero filled and zeros are never the measured format, only the refusal text could differ.
  - `dsound_stream.c` GetStatus `fresh` dropping `!flush_seen` or `!discontinuity_seen`: FlushEx is admitted only
    after a recorded Pause1 and Discontinuity only after FlushEx, so `!pause_seen` already implies both (two
    survivors). The same holds for the VOLUME `fresh`.
  - `dsound_buffer.c` SetVolume or SetBufferData announcing on every call (`r->announce=true`): only the
    log text differs, the tests do not capture the log.
  - `dsound_buffer.c` and `dsound_stream.c` observer path returning `*result = 1u`: the
    snapshot getters ignore the result word.
  - `dsound_buffer.c` SetBufferData dropping `r->argument==0u ||` from the null gate: the next
    check, `kernel_guest_at`, refuses address 0 itself, only the refusal text differs.
  - `dsound_hle.c` dropping `&& entry->handler`: `dsound_hle_register` refuses a NULL
    handler so IMPLEMENTED implies a handler.
  - `dsound_effects_metadata.c` `snapshot_guest` header-word and map-count literals and the
    `fnv && crc` to `||`: the image fingerprint (FNV-64 and CRC-32) is checked first and a
    fingerprinted image carries exactly those words, so they are unreachable negatives.
  - `dsound_hrtf.c` / effects `refuse` strings and the announce banners (logging only).

Every `old` here is checked to occur exactly once by `tests/test_mutation_anchors.py`.
"""

from collections.abc import Callable

A = "src/audio/"
HLE = A + "dsound_hle.c"
DEV = A + "dsound_device.c"
STR = A + "dsound_stream.c"
BUF = A + "dsound_buffer.c"
LIS = A + "dsound_listener.c"
SSC = A + "dsound_stream_scope.c"
BSC = A + "dsound_buffer_scope.c"
HRT = A + "dsound_hrtf.c"
EFB = A + "dsound_effects_binding.c"
EFM = A + "dsound_effects_metadata.c"

T_HLE = ["test_dsound_hle"]
T_DEV = [
    "test_dsound_device",
    "test_dsound_device_lease",
    "test_dsound_device_movie_calls",
    "test_dsound_device_identity",
]
T_STR = [
    "test_dsound_stream",
    "test_dsound_stream_status",
    "test_dsound_stream_volume",
    "test_dsound_stream_pause",
    "test_dsound_stream_flush",
    "test_dsound_stream_discontinuity",
    "test_dsound_stream_second",
]
T_BUF = ["test_dsound_buffer"]
T_LIS = ["test_dsound_listener", "test_dsound_listener_commit", "test_dsound_listener_work"]
T_SSC = ["test_dsound_stream_scope", "test_dsound_stream"]
T_BSC = ["test_dsound_buffer_scope", "test_dsound_buffer"]
T_HRT = ["test_dsound_hrtf"]
T_EFB = ["test_dsound_effects_binding"]


def _m(prefix: str, path: str, targets: list[str]) -> Callable[..., dict]:
    def make(
        mutation_id: str, old: str, new: str, why: str, extra: list[str] | None = None
    ) -> dict:
        return {
            "id": f"dsd-{prefix}-{mutation_id}",
            "file": path,
            "old": old,
            "new": new,
            "targets": list(targets) + list(extra or []),
            "why": why,
        }

    return make


hle = _m("hle", HLE, T_HLE)
dev = _m("dev", DEV, T_DEV)
strm = _m("str", STR, T_STR)
buf = _m("buf", BUF, T_BUF)
lis = _m("lis", LIS, T_LIS)
ssc = _m("sscope", SSC, T_SSC)
bsc = _m("bscope", BSC, T_BSC)
hrt = _m("hrtf", HRT, T_HRT)
efb = _m("efx", EFB, T_EFB)
efm = _m("efxmeta", EFM, T_EFB)

MUTATIONS: list[dict] = [
    # ================================================================ dsound_hle.c
    hle(
        "row-pause-sites",
        '    {0x00407abc, "IDirectSoundBuffer_Pause", 6},',
        '    {0x00407abc, "IDirectSoundBuffer_Pause", 5},',
        "the site counts are the measurement and feed the report ranking and the crosscheck.",
    ),
    hle(
        "row-dowork-address",
        '    {0x00407b40, "DirectSoundDoWork", 5},',
        '    {0x00407b41, "DirectSoundDoWork", 5},',
        "an address one byte off makes the real DoWork an unknown target.",
    ),
    hle(
        "row-create-name",
        '    {0x00409635, "DirectSoundCreate", 3},',
        '    {0x00409635, "DirectSoundCreateX", 3},',
        "a renamed row still dispatches but disagrees with the generated table.",
    ),
    hle(
        "row-hrtf-sites",
        '    {0x00406ab6, "DirectSoundUseLightHRTF", 1},',
        '    {0x00406ab6, "DirectSoundUseLightHRTF", 2},',
        "the 80-site total is summed from rows, one row off is a plausible table.",
    ),
    hle(
        "lookup-neighbour",
        "        if (entries[i].address == address) {",
        "        if (entries[i].address <= address) {",
        "the lookup comment names this failure: a dispatcher that answers with another function is worse than none.",
    ),
    hle(
        "register-state",
        "    entry->state = DSOUND_ENTRY_IMPLEMENTED;",
        "    entry->state = DSOUND_ENTRY_STUB;",
        "a registered handler that stays a stub never runs and the implemented count stays wrong.",
    ),
    hle(
        "register-null-handler",
        "    if (!entry || !handler) {\n        return false;\n    }\n    entry->handler = handler;",
        "    if (!entry) {\n        return false;\n    }\n    entry->handler = handler;",
        "registering a NULL handler must be refused or the table claims an implementation that is not there.",
    ),
    hle(
        "default-return-ignored",
        "    entry->default_return = value;",
        "    entry->default_return = value & 0u;",
        "the stub default return is the value the guest sees from an unimplemented function.",
    ),
    hle(
        "unknown-counter",
        "        unknown_calls++;",
        "        unknown_calls = 1;",
        "the unknown-call count is how the report says table and binary disagree.",
    ),
    hle(
        "section-end-inclusive",
        "address >= DSOUND_SECTION_VA_BEGIN && address < DSOUND_SECTION_VA_END",
        "address >= DSOUND_SECTION_VA_BEGIN && address <= DSOUND_SECTION_VA_END",
        "the section end is exclusive: the two unknown-target messages tell the reader whether to fix the table or the caller.",
    ),
    hle(
        "call-count",
        "    entry->call_count++;",
        "    entry->call_count = 1u;",
        "the call count is the runtime half of the report ranking.",
    ),
    hle(
        "report-every-call",
        "    if (!entry->reported) {\n        entry->reported = true;",
        "    if (true) {\n        entry->reported = true;",
        "a mixer tick reported on every call buries the one-shot init calls.",
    ),
    hle(
        "plural-sites",
        'entry->sites == 1u ? "" : "s"',
        'entry->sites == 2u ? "" : "s"',
        "the log text is read by people triaging a run.",
    ),
    hle(
        "rank-calls",
        "        return candidate->call_count > best->call_count;",
        "        return candidate->call_count < best->call_count;",
        "the ranking is the work queue, runtime calls first.",
    ),
    hle(
        "rank-sites",
        "        return candidate->sites > best->sites;",
        "        return candidate->sites < best->sites;",
        "measured sites break the tie between equal runtime counts.",
    ),
    hle(
        "rank-address",
        "    return candidate->address < best->address;",
        "    return candidate->address > best->address;",
        "address ascending keeps the report deterministic.",
    ),
    hle(
        "report-missing",
        "        if (entries[i].state != DSOUND_ENTRY_IMPLEMENTED) {\n            missing++;",
        "        if (entries[i].state == DSOUND_ENTRY_IMPLEMENTED) {\n            missing++;",
        "the missing count is the headline figure of the report.",
    ),
    hle(
        "init-codec-default",
        "    codec_state = DSOUND_CODEC_NOT_READY;\n    codec_state_announced = false;",
        "    codec_state = DSOUND_CODEC_READY;\n    codec_state_announced = false;",
        "NOT_READY is the honest default, a default READY fabricates a codec silently.",
    ),
    hle(
        "codec-announce-once",
        "    if (state == codec_state && codec_state_announced) {",
        "    if (state == codec_state) {",
        "the first set of the default state must still announce itself in the run log.",
    ),
    hle(
        "not-ready-count",
        "    not_ready_queries++;",
        "    not_ready_queries = 1u;",
        "the query count is printed in the report.",
    ),
    hle(
        "not-ready-once",
        "    if (!not_ready_reported) {",
        "    if (true) {",
        "polled in a spin loop, the NOT READY signpost must print once.",
    ),
    hle(
        "ack-base-unset",
        "    if (scratch_base == DSOUND_DSP_ACK_UNSET) {",
        "    if (false) {",
        "the unset sentinel must not be accepted as a base.",
    ),
    hle(
        "ack-wrap-boundary",
        "    if (offset > UINT32_MAX - scratch_base) {",
        "    if (offset >= UINT32_MAX - scratch_base) {",
        "a sum exactly UINT32_MAX is a valid address, a wrapped sum is a silent wrong write.",
    ),
    hle(
        "ack-sum",
        "    dsp_ack_address = scratch_base + offset;",
        "    dsp_ack_address = scratch_base - offset;",
        "the ack address is base plus offset.",
    ),
    hle(
        "ack-upstream-label",
        "offset == DSOUND_DSP_ACK_UPSTREAM_OFFSET",
        "offset != DSOUND_DSP_ACK_UPSTREAM_OFFSET",
        "the log says whether the derived offset matches the upstream measurement.",
    ),
    hle(
        "ack-write-value",
        "    dsp_writer(dsp_ack_address, 0u, dsp_writer_user);",
        "    dsp_writer(dsp_ack_address, 1u, dsp_writer_user);",
        "the acknowledgement is a zero command word.",
    ),
    hle(
        "ack-write-count",
        "    dsp_ack_writes++;",
        "    dsp_ack_writes = 1u;",
        "acknowledgement accounting is reported.",
    ),
    hle(
        "ack-no-writer-count",
        "    if (!dsp_writer) {\n        dsp_ack_refusals++;",
        "    if (!dsp_writer) {\n        dsp_ack_refusals += 0u;",
        "a refused acknowledgement for lack of a writer is counted as a refusal.",
    ),
    hle(
        "ack-unconfigured-once",
        "        if (!dsp_ack_unconfigured_reported) {",
        "        if (true) {",
        "the unconfigured explanation prints once.",
    ),
    hle(
        "crosscheck-null-guard",
        "    if (!refs && count > 0) {",
        "    if (!refs) {",
        "an empty reference with a NULL pointer is a valid empty comparison.",
    ),
    hle(
        "crosscheck-sites",
        "        if (mine->sites != refs[i].sites) {",
        "        if (false) {",
        "site-count drift against the generated table is one of the three disagreements.",
    ),
    hle(
        "crosscheck-names",
        "strcmp(mine->name, refs[i].name) == 0);",
        "strcmp(mine->name, refs[i].name) != 0);",
        "name drift against the generated table.",
    ),
    hle(
        "crosscheck-reverse",
        "        if (!present) {\n            disagreements++;",
        "        if (present) {\n            disagreements++;",
        "the reverse walk catches an invented row of ours.",
    ),
    hle(
        "requires-listener",
        "    case 0x004093ECu: case 0x0040945Au: case 0x00409410u:",
        "    case 0x004093ECu: case 0x0040945Au: case 0x00409411u:",
        "the explicit required-implementation list has no prefix guesses, one address off drops a function.",
    ),
    hle(
        "requires-buffer",
        "    case 0x00408C0Du: case 0x004093C8u:",
        "    case 0x00408C0Du: case 0x004093C9u:",
        "the CreateSoundBuffer entry is required.",
    ),
    # ================================================================ dsound_device.c
    dev(
        "validate-interface",
        "interface != owned_address + 8u ||",
        "interface != owned_address + 4u ||",
        "the interface pointer is the object plus 8, a wrong interface must not be treated as the device.",
    ),
    dev(
        "validate-singleton",
        "singleton != owned_address ||",
        "singleton == owned_address ||",
        "the global singleton must still point at our device.",
    ),
    dev(
        "validate-size",
        "        requested != DEVICE_BYTES) return false;",
        "        requested < DEVICE_BYTES) return false;",
        "the allocation must be exactly the 44-byte device.",
    ),
    dev(
        "header-vtable",
        "    words[0] = DEVICE_VTABLE;",
        "    words[0] = DEVICE_VTABLE + 4u;",
        "the header bytes are compared against the guest, the vtable word identifies the device.",
    ),
    dev(
        "header-list",
        "    words[4] = address + 16u;",
        "    words[4] = address + 12u;",
        "the self-referential list words are part of the original header.",
    ),
    dev(
        "header-reference",
        "    words[1] = reference;",
        "    words[1] = reference + 1u;",
        "the reference word must track the shadow count.",
    ),
    dev(
        "owned-interface-callback",
        "validate_owned(interface);\n    if (valid) callback(owned_address, userdata, result);",
        "validate_owned(interface);\n    if (valid) callback(owned_address + 8u, userdata, result);",
        "the callback receives the internal address and builds interface = internal + 8 itself.",
    ),
    dev(
        "identity-swapped",
        "const dsound_device_identity identity = {owned_heap, owned_address};",
        "const dsound_device_identity identity = {owned_address, owned_heap};",
        "heap and address are distinct fields and a swap passes any test that only checks non-zero.",
    ),
    dev(
        "ops-abort",
        "ops->prepare != NULL && ops->abort != NULL && ops->finalize != NULL",
        "ops->prepare != NULL && ops->finalize != NULL",
        "a lease without an abort callback would strand its mutex on refusal.",
    ),
    dev(
        "child-heap",
        "child->heap != 0u && child->heap != owned_heap && child->address != 0u &&",
        "child->heap != 0u && child->address != 0u &&",
        "a lease child must not live in the device's own heap.",
    ),
    dev(
        "child-size",
        "bytes == child->bytes &&",
        "bytes >= child->bytes &&",
        "the child must be exactly the declared allocation size.",
    ),
    dev(
        "lease-serial-new",
        "(dsound_device_lease){owned_heap, owned_address, lease_serial + 1u};",
        "(dsound_device_lease){owned_heap, owned_address, lease_serial};",
        "every lease needs a fresh serial or a stale token matches a new lease.",
    ),
    dev(
        "lease-child-valid",
        "    if (!valid_child(&record->child)) goto abort_prepare;",
        "    if (false) goto abort_prepare;",
        "a child the prepare callback returned must be validated before commit.",
    ),
    dev(
        "lease-duplicate-child",
        "        if (same_child(&other->child, &record->child)) goto abort_prepare;",
        "        if (false) goto abort_prepare;",
        "two leases on one child would be released twice.",
    ),
    dev(
        "lease-revalidate",
        "Validate again before commit. */\n    if (!validate_owned(interface)) goto abort_prepare;",
        "Validate again before commit. */\n    if (false) goto abort_prepare;",
        "a callback must not be able to change the device and still commit.",
    ),
    dev(
        "lease-write-count",
        "kernel_guest_write_u32(owned_address + 4u, shadow_reference + 1u)) {\n        status = DSOUND_LEASE_WRITE_REFUSED; goto abort_prepare;",
        "kernel_guest_write_u32(owned_address + 4u, shadow_reference + 2u)) {\n        status = DSOUND_LEASE_WRITE_REFUSED; goto abort_prepare;",
        "the guest reference increments by exactly one per lease.",
    ),
    dev(
        "lease-shadow",
        "    shadow_reference++;\n    lease_serial = record->token.serial;",
        "    shadow_reference += 0u;\n    lease_serial = record->token.serial;",
        "the shadow count must follow the guest word.",
    ),
    dev(
        "lease-serial-store",
        "    lease_serial = record->token.serial;",
        "    lease_serial = 0u;",
        "the serial high-water mark survives so identities are never reissued.",
    ),
    dev(
        "lease-count",
        "    record->next = leases; leases = record; lease_count++;",
        "    record->next = leases; leases = record;",
        "the live lease count gates reset and release.",
    ),
    dev(
        "lease-output",
        "    *output = record->token;",
        "    *output = (dsound_device_lease){0};",
        "the caller's token is the committed one.",
    ),
    dev(
        "lease-abort-callback",
        "abort_prepare:\n    ops->abort(userdata);\n    free(record);",
        "abort_prepare:\n    free(record);",
        "the abort callback releases the consumer lock, skipping it deadlocks the next caller.",
    ),
    dev(
        "release-null-lease",
        "    if (lease == NULL || !valid_ops(ops)) goto done;",
        "    if (!valid_ops(ops)) goto done;",
        "a NULL lease is refused, not dereferenced.",
    ),
    dev(
        "release-same-child",
        "    if (!same_child(&child, &record->child) || !valid_child(&child) ||",
        "    if (false || !valid_child(&child) ||",
        "the release callback must name the same child the acquire recorded.",
    ),
    dev(
        "release-write",
        "kernel_guest_write_u32(owned_address + 4u, shadow_reference - 1u)) {\n        status = DSOUND_LEASE_WRITE_REFUSED; goto abort_prepare;",
        "kernel_guest_write_u32(owned_address + 4u, shadow_reference - 2u)) {\n        status = DSOUND_LEASE_WRITE_REFUSED; goto abort_prepare;",
        "the guest reference decrements by exactly one.",
    ),
    dev(
        "release-count",
        "    shadow_reference--; *link = record->next; lease_count--;",
        "    shadow_reference--; *link = record->next;",
        "the live lease count must fall with each release.",
    ),
    dev(
        "reset-leases",
        "    if (lease_count != 0u) { pthread_mutex_unlock(&lock); return false; }",
        "    if (lease_count > 1u) { pthread_mutex_unlock(&lock); return false; }",
        "reset with a live lease would destroy a parent under a child.",
    ),
    dev(
        "reset-singleton",
        "            !kernel_guest_write_u32(SINGLETON, 0u)) {",
        "            !kernel_guest_write_u32(SINGLETON, 1u)) {",
        "reset must clear the guest singleton.",
    ),
    dev(
        "reset-restore",
        "(void)kernel_guest_write_u32(SINGLETON, owned_address);",
        "(void)kernel_guest_write_u32(SINGLETON, 0u);",
        "a refused heap destroy must restore the singleton so reset stays retryable.",
    ),
    dev(
        "reset-movie",
        "    movie_references = 0u;\n    announced = false;",
        "    announced = false;",
        "movie references must not leak across a reset.",
    ),
    dev(
        "create-outer",
        "    if (guid != 0u || outer != 0u) refuse_locked(",
        "    if (guid != 0u || outer > 1u) refuse_locked(",
        "aggregation is unsupported and must be refused.",
    ),
    dev(
        "create-vtable-span",
        "overlaps(output, 4u, DEVICE_VTABLE, 12u) ||",
        "overlaps(output, 4u, DEVICE_VTABLE, 8u) ||",
        "the output pointer must not alias the whole original vtable.",
    ),
    dev(
        "create-owned-span",
        "overlaps(output, 4u, owned_address, DEVICE_BYTES)))",
        "overlaps(output, 4u, owned_address, 4u)))",
        "the output must not alias any byte of the owned device.",
    ),
    dev(
        "create-movie-needs-device",
        "    if (movie && owned_address == 0u)",
        "    if (movie && owned_address == 1u)",
        "a movie create before the startup device exists must refuse.",
    ),
    dev(
        "create-foreign-singleton",
        "        if (singleton != 0u) refuse_locked(",
        "        if (singleton == 0u) refuse_locked(",
        "a singleton we did not create is foreign state.",
    ),
    dev(
        "create-movie-count",
        "        if (movie) movie_references++;",
        "        if (!movie) movie_references++;",
        "only movie creates count as movie references.",
    ),
    dev(
        "create-oom",
        "        if (heap == 0u) { pthread_mutex_unlock(&lock); return E_OUTOFMEMORY; }",
        "        if (heap == 0u) { pthread_mutex_unlock(&lock); return 0u; }",
        "heap exhaustion reports E_OUTOFMEMORY.",
    ),
    dev(
        "create-initial-reference",
        "    shadow_reference = codec_ready ? 6u : 1u;",
        "    shadow_reference = codec_ready ? 5u : 1u;",
        "6 is the observed original reference count, five children and the title.",
    ),
    dev(
        "create-nodriver",
        "        if (!codec_ready) status = DSERR_NODRIVER;",
        "        if (codec_ready) status = DSERR_NODRIVER;",
        "a NOT_READY codec reports DSERR_NODRIVER.",
    ),
    dev(
        "create-announce",
        "    const bool announce = !announced;",
        "    const bool announce = true;",
        "the facade banner prints once.",
    ),
    dev(
        "release-disabled",
        "    if (!movie_calls_enabled) refuse_locked_at(",
        "    if (movie_calls_enabled) refuse_locked_at(",
        "movie device calls are refused unless enabled.",
    ),
    dev(
        "release-none-outstanding",
        "    if (movie_references == 0u)\n",
        "    if (movie_references == 1u)\n",
        "releasing the startup reference would run the original destructor.",
    ),
    dev(
        "release-destructor-floor",
        "if (shadow_reference - 1u < DESTROY_REFERENCES)",
        "if (shadow_reference - 1u <= DESTROY_REFERENCES)",
        "the guard sits exactly at the destructor count, one off is a boundary error.",
    ),
    dev(
        "release-movie-count",
        "    movie_references--;\n    const uint32_t result",
        "    const uint32_t result",
        "each movie release drops one movie reference.",
    ),
    dev(
        "release-result",
        "    const uint32_t result = shadow_reference;",
        "    const uint32_t result = shadow_reference + 1u;",
        "the guest sees the remaining reference count.",
    ),
    dev(
        "create-handler-caller",
        "if (!startup && !(caller == MOVIE_CREATE_CALLER && dsound_device_movie_calls_enabled()))",
        "if (!startup && !(caller == MOVIE_CREATE_CALLER))",
        "the movie caller is admitted only with movie calls enabled.",
    ),
    dev(
        "create-handler-movie-flag",
        "return create_device(arguments[0], arguments[1], arguments[2], !startup);",
        "return create_device(arguments[0], arguments[1], arguments[2], startup);",
        "the movie flag is the negation of startup.",
    ),
    dev(
        "release-handler-caller",
        "        caller != MOVIE_RELEASE_CALLER)",
        "        caller == MOVIE_RELEASE_CALLER)",
        "only the measured movie caller may release.",
    ),
    dev(
        "register-movie",
        "{ return dsound_hle_register(RELEASE_ENTRY, release_handler) ? 1u : 0u; }",
        "{ return dsound_hle_register(RELEASE_ENTRY, release_handler) ? 0u : 1u; }",
        "the registration count is the caller's only signal.",
    ),
    # ================================================================ dsound_stream.c
    strm(
        "policy-enabled",
        'if(!enabled)return "explicit headless-streams policy is disabled";',
        'if(enabled)return "explicit headless-streams policy is disabled";',
        "the explicit opt-in policy gate.",
    ),
    strm(
        "policy-irql",
        'irql!=0u)return "only known IRQL0 is supported";',
        'irql>1u)return "only known IRQL0 is supported";',
        "only IRQL zero is supported.",
    ),
    strm(
        "policy-global",
        'state!=0u)return "original global audio state must be zero";',
        'state>1u)return "original global audio state must be zero";',
        "the original global audio state must be quiescent.",
    ),
    strm(
        "node-size",
        "bytes==STREAM_BYTES;",
        "bytes>=STREAM_BYTES;",
        "a stream heap block must be exactly the 40-byte header.",
    ),
    strm(
        "node-header",
        "memcmp(actual,node->value.header,sizeof(actual))==0",
        "memcmp(actual,node->value.header,sizeof(actual)-4u)==0",
        "the whole header is the ownership evidence, a guest change to the last word is foreign state.",
    ),
    strm(
        "alias-curve",
        "overlap(address,bytes,CURVE,16u))return true;",
        "overlap(address,bytes,CURVE,12u))return true;",
        "the whole curve table is protected.",
    ),
    strm(
        "alias-descriptor",
        "overlap(address,bytes,n->value.scope.descriptor_address,24u) ||",
        "overlap(address,bytes,n->value.scope.descriptor_address,20u) ||",
        "the live stream's descriptor span is protected.",
    ),
    strm(
        "alias-format",
        "overlap(address,bytes,n->value.scope.format_address,20u) ||",
        "overlap(address,bytes,n->value.scope.format_address,16u) ||",
        "the live stream's format span is protected.",
    ),
    strm(
        "alias-i3dl2",
        "overlap(address,bytes,n->value.i3dl2_address,36u)))return true;",
        "overlap(address,bytes,n->value.i3dl2_address,32u)))return true;",
        "the cached I3DL2 parameter block is protected.",
    ),
    strm(
        "alias-detached",
        "n->value.i3dl2_address,36u)))return true;\n    for(stream_node *n=detached;n!=NULL;n=n->next)",
        "n->value.i3dl2_address,36u)))return true;\n    for(stream_node *n=NULL;n!=NULL;n=n->next)",
        "detached nodes still own guest memory until cleanup.",
    ),
    strm(
        "alias-headers-secondary",
        "overlap(address,bytes,SECONDARY,60u))return true;",
        "overlap(address,bytes,SECONDARY,56u))return true;",
        "inputs may not alias the original secondary table.",
    ),
    strm(
        "tables-count",
        "for(unsigned i=0u;i<15u;i++) {",
        "for(unsigned i=0u;i<14u;i++) {",
        "all 15 vtable words are checked.",
    ),
    strm(
        "tables-set",
        "for(unsigned j=0u;j<14u;j++)if(table[i]==stopped[j])found=true;",
        "for(unsigned j=0u;j<13u;j++)if(table[i]==stopped[j])found=true;",
        "the last stopped target must still be recognised.",
    ),
    strm(
        "tables-last",
        "0x00407883u,0x0040788Du,0x004093ADu};",
        "0x00407883u,0x0040788Du,0x004093AEu};",
        "a target address one off admits an unprotected vtable word.",
    ),
    strm(
        "create-output-descriptor",
        "overlap(r->output,4u,r->scope.descriptor_address,24u) ||",
        "overlap(r->output,4u,r->scope.descriptor_address,20u) ||",
        "the published output may not alias the descriptor.",
    ),
    strm(
        "create-header",
        "header[0]=VTABLE;header[1]=SECONDARY;header[2]=1u;header[3]=candidate->internal_address;",
        "header[0]=VTABLE;header[1]=SECONDARY;header[2]=2u;header[3]=candidate->internal_address;",
        "the public header words are compared against the guest on every access.",
    ),
    strm(
        "create-header-parent",
        "header[3]=candidate->internal_address;",
        "header[3]=0u;",
        "the header records the parent device.",
    ),
    strm(
        "create-probe",
        "    if(!kernel_guest_write_u32(r->output,old) ||",
        "    if(false ||",
        "the same-byte probe proves the output is writable before any state is created.",
    ),
    strm(
        "create-heap-oom",
        "if(heap==0u){r->status=OOM;return false;}",
        "if(heap==0u){r->status=0u;return false;}",
        "heap exhaustion reports E_OUTOFMEMORY.",
    ),
    strm(
        "create-alloc-oom",
        "if(address==0u){r->status=OOM;return false;}",
        "if(address==0u){return false;}",
        "allocation exhaustion reports E_OUTOFMEMORY.",
    ),
    strm(
        "cleanup-valid",
        "if(heap==0u || (guest_heap_valid(heap) && guest_heap_destroy(heap))) {",
        "if(heap==0u || (guest_heap_valid(heap) && guest_heap_destroy(heap)) || true) {",
        "a failed rollback destroy must retain the node for retry, not free it.",
    ),
    strm(
        "cleanup-retain",
        "r->node->next=rollback;rollback=r->node;\n        r->error=",
        "r->node->next=rollback;\n        r->error=",
        "the retained node must be linked so reset can retry.",
    ),
    strm(
        "create-finalize-announce",
        "r->announce=!announced;announced=true;",
        "r->announce=!announced;",
        "the banner prints once per reset.",
    ),
    strm(
        "create-oom-status",
        "(status==DSOUND_LEASE_PREPARE_FAILED && r.status==OOM && r.error==NULL))return OOM;",
        "(status==DSOUND_LEASE_PREPARE_FAILED && r.status==OOM))return OOM;",
        "an OOM status with a recorded refusal reason is a refusal, not E_OUTOFMEMORY.",
    ),
    strm(
        "create-device-limit",
        'device>UINT32_MAX-8u)\n        refuse(CREATE,"no owned SILENT device");',
        'device>UINT32_MAX-7u)\n        refuse(CREATE,"no owned SILENT device");',
        "device + 8 must not wrap.",
    ),
    strm(
        "create-cleanup",
        "if(status!=DSOUND_LEASE_OK)cleanup_aborted_create(&r);",
        "if(false)cleanup_aborted_create(&r);",
        "an unpublished candidate must be destroyed after a refusal.",
    ),
    strm(
        "access-node",
        'if(!valid_node(n,&r->identity,internal)){r->error="stream ownership',
        'if(n==NULL){r->error="stream ownership',
        "every access re-validates ownership, header and generation.",
    ),
    strm(
        "status-packets",
        "n->value.scope.max_packets != 3u ||\n            !n->value.volume_seen",
        "n->value.scope.max_packets != 4u ||\n            !n->value.volume_seen",
        "the exact startup scope is three packets.",
    ),
    strm(
        "status-volume",
        "n->value.volume != -10000 ||",
        "n->value.volume != -9999 ||",
        "status is only answered after the exact silence volume.",
    ),
    strm(
        "status-volume-seen",
        "            !n->value.volume_seen || n->value.volume != -10000",
        "            n->value.volume_seen || n->value.volume != -10000",
        "status needs the volume to have been requested.",
    ),
    strm(
        "status-curve",
        "overlap(r->argument,4u,CURVE,16u) ||",
        "overlap(r->argument,4u,CURVE,12u) ||",
        "the status output may not alias the curve table.",
    ),
    strm(
        "status-value",
        "!kernel_guest_write_u32(r->argument,1u)) {",
        "!kernel_guest_write_u32(r->argument,0u)) {",
        "the reached startup response is bit 0 set.",
    ),
    strm(
        "volume-value",
        "r->argument != 0xFFFFD8F0u)) {",
        "r->argument != 0xFFFFD8F1u)) {",
        "only the exact -10000 silence volume is admitted.",
    ),
    strm(
        "volume-store",
        "(int32_t)r->argument : -10000;",
        "(int32_t)r->argument : -9999;",
        "the cached volume is what status later compares.",
    ),
    strm(
        "volume-paused",
        "n->value.flush_flags == 1u &&\n            n->value.discontinuity_seen;",
        "n->value.flush_flags == 1u;",
        "the fully paused state includes the discontinuity.",
    ),
    strm(
        "disc-time",
        "n->value.flush_time_low != 0u ||",
        "n->value.flush_time_low != 1u ||",
        "Discontinuity needs the exact time-0 flush.",
    ),
    strm(
        "disc-flags",
        "n->value.flush_flags != 1u) {",
        "n->value.flush_flags != 2u) {",
        "Discontinuity needs the exact flags-1 flush.",
    ),
    strm(
        "disc-store",
        "n->value.discontinuity_seen = true;",
        "n->value.discontinuity_seen = false;",
        "the observation is what later volume and status read.",
    ),
    strm(
        "flush-count",
        "r->argument != 0u || r->count != 0u || r->apply != 1u) {",
        "r->argument != 0u || r->apply != 1u) {",
        "FlushEx admits only time 0:0.",
    ),
    strm(
        "flush-flags",
        "n->value.flush_flags = r->apply;",
        "n->value.flush_flags = 0u;",
        "the cached flags are compared later.",
    ),
    strm(
        "flush-high",
        "n->value.flush_time_high = r->count;",
        "n->value.flush_time_high = r->count + 1u;",
        "the cached high time is compared later.",
    ),
    strm(
        "pause-mode",
        "if ((!stereo && !spatial) || (r->argument != 1u && !resume_ok)) {",
        "if ((!stereo && !spatial) || (r->argument == 0u && !resume_ok)) {",
        "only Pause mode 1 is the measured startup call.",
    ),
    strm(
        "spatial-apply",
        "if(n->value.scope.flags!=0x10u || r->apply!=0u){",
        "if(n->value.scope.flags!=0x10u){",
        "deferred apply is unsupported.",
    ),
    strm(
        "i3dl2-alias",
        "if(n->value.cache_mask!=0u || aliases_headers(r->argument,36u,internal) ||",
        "if(n->value.cache_mask!=0u || aliases_headers(r->argument,32u,internal) ||",
        "the parameter block span is 36 bytes.",
    ),
    strm(
        "i3dl2-compare",
        "memcmp(words,expected,sizeof(words))!=0)",
        "memcmp(words,expected,sizeof(words)-4u)!=0)",
        "every word of the exact first startup block is compared.",
    ),
    strm(
        "i3dl2-literal",
        "expected[9]={0u,0u,0xFFFFF448u,",
        "expected[9]={0u,0u,0xFFFFF449u,",
        "the room-attenuation literal is the measurement.",
    ),
    strm(
        "i3dl2-mask",
        "n->value.i3dl2_address=r->argument;n->value.cache_mask=1u;",
        "n->value.i3dl2_address=r->argument;n->value.cache_mask=3u;",
        "setters are strictly ordered by the mask.",
    ),
    strm(
        "min-literal",
        'r->argument!=0x3F800000u){r->error="only ordered startup minDistance',
        'r->argument!=0x3F800001u){r->error="only ordered startup minDistance',
        "only minDistance 1.0 is admitted.",
    ),
    strm(
        "min-mask",
        "n->value.min_distance_bits=r->argument;n->value.cache_mask=3u;",
        "n->value.min_distance_bits=r->argument;n->value.cache_mask=7u;",
        "the rolloff setter must only follow minDistance.",
    ),
    strm(
        "rolloff-count",
        "r->argument!=CURVE || r->count!=4u)",
        "r->argument!=CURVE || r->count!=3u)",
        "the stream curve has four points.",
    ),
    strm(
        "rolloff-mask",
        "n->value.curve_count=r->count;n->value.cache_mask=7u;",
        "n->value.curve_count=r->count;n->value.cache_mask=3u;",
        "mask 7 is the completed spatial startup the status gate requires.",
    ),
    strm(
        "access-parent",
        "r.identity.internal_address+8u,access_owned,&r,&result))\n        refuse(entry",
        "r.identity.internal_address+4u,access_owned,&r,&result))\n        refuse(entry",
        "the parent interface is the internal address plus 8.",
    ),
    strm(
        "snapshot-error",
        "access_owned,&r,&result) || r.error!=NULL)return false;",
        "access_owned,&r,&result))return false;",
        "a snapshot of a stream with a refusal reason is not valid.",
    ),
    strm(
        "reset-prepare",
        "if(!token_equal(candidate,&r->identity) || !valid_node(r->node,&r->identity,candidate->internal_address))return false;",
        "if(!token_equal(candidate,&r->identity) && !valid_node(r->node,&r->identity,candidate->internal_address))return false;",
        "release needs both the token and an intact node.",
    ),
    strm(
        "reset-finalize",
        "r->node->next=detached;detached=r->node;",
        "r->node->next=detached;",
        "a detached node stays tracked until its guest heap is destroyed.",
    ),
    strm(
        "reset-release",
        "if(dsound_device_release_lease(&r.identity,&reset_ops,&r)!=DSOUND_LEASE_OK)return false;",
        "if(dsound_device_release_lease(&r.identity,&reset_ops,&r)!=DSOUND_LEASE_OK)return true;",
        "a refused release must make reset report failure.",
    ),
    strm(
        "reset-announce",
        "announced=false;pthread_mutex_unlock(&lock);return true;",
        "pthread_mutex_unlock(&lock);return true;",
        "a reset re-arms the banner.",
    ),
    strm(
        "reset-extension",
        "const bool movies=extension==NULL || extension();",
        "const bool movies=extension==NULL || !extension();",
        "the movie extension reset result is part of the checked result.",
    ),
    strm(
        "reset-result",
        "return movies && result;",
        "return movies || result;",
        "a failed stream reset must fail the checked reset.",
    ),
    strm(
        "frame-route",
        "if(route(entry,frame,actual,&extension))return extension;",
        "if(!route(entry,frame,actual,&extension))return extension;",
        "an extension that answers owns the call.",
    ),
    strm(
        "frame-caller",
        "kernel_guest_read_u32(frame->stack_ptr,&actual) ||\n       (actual!=caller && (second==0u || actual!=second) && (!third_admitted || third==0u || actual!=third) &&\n        !(third_admitted && entry==PAUSE && actual==0x29ED1u))) {",
        "kernel_guest_read_u32(frame->stack_ptr,&actual) ||\n       (actual==caller && (second==0u || actual!=second) && (!third_admitted || third==0u || actual!=third) &&\n        !(third_admitted && entry==PAUSE && actual==0x29ED1u))) {",
        "only the measured caller is supported.",
    ),
    strm(
        "dispatch-volume",
        "return access(VOLUME,args[0],args[1],actual==second?1u:0u,0u);",
        "return access(VOLUME,args[1],args[0],actual==second?1u:0u,0u);",
        "argument order of the dispatch shim.",
    ),
    strm(
        "dispatch-pause",
        "return access(PAUSE,args[0],args[1],actual==second?1u:(actual==0x29ED1u?2u:0u),0u);",
        "return access(PAUSE,args[1],args[0],actual==second?1u:(actual==0x29ED1u?2u:0u),0u);",
        "argument order of the dispatch shim.",
    ),
    strm(
        "dispatch-flush",
        "dsound_stream_cache_flush_ex(args[0],args[1],args[2],args[3]);",
        "dsound_stream_cache_flush_ex(args[0],args[1],args[3],args[2]);",
        "FlushEx time and flags order.",
    ),
    strm(
        "handler-create-caller",
        "frame_handler(c,CREATE,0x29943u,0u,0u,2u)",
        "frame_handler(c,CREATE,0x29944u,0u,0u,2u)",
        "each handler admits only its own measured caller.",
    ),
    strm(
        "handler-pause-caller",
        "frame_handler(c,PAUSE,0x29B5Au,0x29A51u,0x29F13u,2u)",
        "frame_handler(c,PAUSE,0x29B5Bu,0x29A51u,0x29F13u,2u)",
        "each handler admits only its own measured caller.",
    ),
    strm(
        "register-count",
        "count+=dsound_hle_register(VOLUME,volume_handler)?1u:0u;return count;",
        "count+=dsound_hle_register(VOLUME,volume_handler)?0u:1u;return count;",
        "the registration count is the caller's only signal.",
    ),
    # T602 SetFormat 0x408C2D: the stereo startup scope, the started order, the exact measured format, the host
    # record, the route. A guest word is never written, the original changes only omitted settings.
    strm(
        "sfmt-scope-flags",
        "const bool stereo = n->value.scope.flags == 0u;",
        "const bool stereo = true;",
        "a started spatial (flags 0x10, mono) stream is outside the measured request.",
    ),
    strm(
        "sfmt-needs-volume",
        "bool started = n->value.volume_seen && n->value.discontinuity_seen;",
        "bool started = n->value.discontinuity_seen;",
        "the recorded volume -10000 is part of the started state the boot reaches.",
    ),
    strm(
        "sfmt-needs-discontinuity",
        "bool started = n->value.volume_seen && n->value.discontinuity_seen;",
        "bool started = n->value.volume_seen;",
        "Discontinuity implies the recorded Pause1 and FlushEx(0,0,1), a half started stream is refused.",
    ),
    strm(
        "sfmt-alias-headers",
        "if (aliases_headers(r->argument, sizeof(words), internal) ||\n            overlap(r->argument, sizeof(words), CURVE, 16u)) {",
        "if (false ||\n            overlap(r->argument, sizeof(words), CURVE, 16u)) {",
        "the format may not sit on the original SECONDARY table or any owned header.",
    ),
    strm(
        "sfmt-alias-curve",
        "if (aliases_headers(r->argument, sizeof(words), internal) ||\n            overlap(r->argument, sizeof(words), CURVE, 16u)) {",
        "if (aliases_headers(r->argument, sizeof(words), internal) ||\n            false) {",
        "the format may not sit on the rolloff curve table.",
    ),
    strm(
        "sfmt-family-rate-floor",
        "rate >= 8000u",
        "rate >= 7999u",
        "T1159: 8000 Hz is the lowest admitted XADPCM rate.",
    ),
    strm(
        "sfmt-family-rate-ceiling",
        "rate <= 48000u",
        "rate <= 48001u",
        "T1159: 48000 Hz is the highest admitted XADPCM rate.",
    ),
    strm(
        "sfmt-family-average-dropped",
        "average == (uint32_t)((uint64_t)rate * block / 64u) &&",
        "true &&",
        "T1159: the average byte rate must be rate * block / 64.",
    ),
    strm(
        "sfmt-family-average-ratio",
        "(uint64_t)rate * block / 64u",
        "(uint64_t)rate * block / 63u",
        "T1159: a block carries 64 samples.",
    ),
    strm(
        "sfmt-family-tag",
        "words[0] == 0x69u && words[1] == 0u",
        "(words[0] == 0x69u || words[1] == 0u)",
        "T1159: the tag is XADPCM 0x69.",
    ),
    strm(
        "sfmt-family-channels",
        "words[2] == channels && words[3] == 0u",
        "words[2] != 0u && words[3] == 0u",
        "T1159, T1181: the channel count is the stream's own.",
    ),
    strm(
        "sfmt-family-block",
        "words[12] == block && words[13] == 0u",
        "words[12] != 0u && words[13] == 0u",
        "T1159, T1181: the XADPCM block is 36 bytes per channel.",
    ),
    strm(
        "sfmt-family-bits",
        "words[14] == 4u && words[15] == 0u",
        "words[14] != 0u && words[15] == 0u",
        "T1159: 4 bits per sample.",
    ),
    strm(
        "sfmt-family-cbsize",
        "words[16] == 2u && words[17] == 0u",
        "words[16] != 0u && words[17] == 0u",
        "T1159: cbSize 2.",
    ),
    strm(
        "sfmt-family-samples",
        "words[18] == 64u && words[19] == 0u;",
        "words[18] != 0u && words[19] == 0u;",
        "T1159: 64 samples per block.",
    ),
    strm(
        "sfmt-family-dropped",
        "if (!readable || !family) {",
        "if (!readable || !(family || true)) {",
        "T1159: the family check is the only format validation.",
    ),
    strm(
        "sfmt-spatial-scope-dropped",
        "const bool spatial = n->value.scope.flags == 0x10u && n->value.cache_mask == 7u;",
        "const bool spatial = false;",
        "T1181: the started spatial stream is re-formatted by the title.",
    ),
    strm(
        "sfmt-spatial-mono-lost",
        "const uint32_t channels = stereo ? 2u : 1u, block = 36u * channels;",
        "const uint32_t channels = 2u, block = 36u * channels;",
        "T1181: the spatial stream is mono, its format is mono ADPCM (block 36).",
    ),
    strm(
        "sfmt-spatial-block-lost",
        "const uint32_t channels = stereo ? 2u : 1u, block = 36u * channels;",
        "const uint32_t channels = stereo ? 2u : 1u, block = 72u;",
        "T1181: the mono block is 36 bytes.",
    ),
    strm(
        "sfmt-record-format",
        "memcpy(n->value.format, words, sizeof(words));",
        "n->value.format_sets++;",
        "the host record keeps the format the original stored.",
    ),
    strm(
        "sfmt-record-sets",
        "n->value.format_sets++;",
        "n->value.format_sets += 2u;",
        "every admitted call counts once, a repeat replaces the record.",
    ),
    strm(
        "sfmt-result",
        "n->value.source_rate_hz=rate;\n        *result = 0u;",
        "n->value.source_rate_hz=rate;\n        *result = 1u;",
        "the original returns S_OK.",
    ),
    strm(
        "sfmt-guest-write",
        "n->value.format_sets++;",
        "n->value.format_sets++;kernel_guest_write_u32(n->value.stream_address + 4u, 0u);",
        "the original writes nothing in the public header, only omitted settings.",
    ),
    strm(
        "sfmt-access-order",
        "return access(SET_FORMAT,stream,format,0u,0u);",
        "return access(SET_FORMAT,stream,0u,format,0u);",
        "the format address is the argument word.",
    ),
    strm(
        "sfmt-dispatch-order",
        "return dsound_stream_cache_set_format(args[0],args[1]);",
        "return dsound_stream_cache_set_format(args[1],args[0]);",
        "the stdcall order is stream, format.",
    ),
    strm(
        "sfmt-handler-count",
        "frame_handler(c,SET_FORMAT,0x299FAu,0u,0u,2u)",
        "frame_handler(c,SET_FORMAT,0x299FAu,0u,0u,3u)",
        "two stdcall arguments are read.",
    ),
    strm(
        "sfmt-caller",
        "frame_handler(c,SET_FORMAT,0x299FAu,0u,0u,2u)",
        "frame_handler(c,SET_FORMAT,0x299FBu,0u,0u,2u)",
        "only the measured title caller.",
    ),
    strm(
        "sfmt-register-gate",
        "if(policy)count+=dsound_hle_register(SET_FORMAT,set_format_handler)?1u:0u;",
        "if(!policy)count+=dsound_hle_register(SET_FORMAT,set_format_handler)?1u:0u;",
        "a flags-off boot keeps its registry, a flags-on boot registers the route.",
    ),
    # T602 Pause(stream, 1) after SetFormat (return 0x29A51): the second caller, its need for a recorded SetFormat.
    strm(
        "pause2-second-caller",
        "(second==0u || actual!=second) && (!third_admitted",
        "(second==0u || actual==second) && (!third_admitted",
        "a third return address is refused, only the two measured callers pass.",
    ),
    # T743 third caller (Pause 0x29F13, the stream update starts the stream): admitted only with the completion model.
    strm(
        "pause3-third-gate",
        "(!third_admitted || third==0u || actual!=third)",
        "((third_admitted && false) || third==0u || actual!=third)",
        "the start caller 0x29F13 is refused with the completion model off.",
        ["test_dsound_stream_start_caller"],
    ),
    strm(
        "pause3-third-caller",
        "frame_handler(c,PAUSE,0x29B5Au,0x29A51u,0x29F13u,2u)",
        "frame_handler(c,PAUSE,0x29B5Au,0x29A51u,0x29F14u,2u)",
        "only the measured stream-update return address is the third caller.",
        ["test_dsound_stream_start_caller"],
    ),
    strm(
        "pause2-caller",
        "frame_handler(c,PAUSE,0x29B5Au,0x29A51u,0x29F13u,2u)",
        "frame_handler(c,PAUSE,0x29B5Au,0x29A52u,0x29F13u,2u)",
        "only the measured re-format caller.",
    ),
    strm(
        "pause2-flag-off",
        "access(PAUSE,args[0],args[1],actual==second?1u:(actual==0x29ED1u?2u:0u),0u)",
        "access(PAUSE,args[0],args[1],actual==0x29ED1u?2u:0u,0u)",
        "the re-format caller needs a recorded SetFormat.",
    ),
    strm(
        "pause2-flag-inverted",
        "access(PAUSE,args[0],args[1],actual==second?1u:(actual==0x29ED1u?2u:0u),0u)",
        "access(PAUSE,args[0],args[1],actual==second?0u:(actual==0x29ED1u?2u:1u),0u)",
        "the startup caller needs no SetFormat, the re-format caller does.",
    ),
    strm(
        "pause2-needs-format",
        'if (r->count == 1u && n->value.format_sets == 0u) {\n            r->error = "Pause after SetFormat',
        'if (false) {\n            r->error = "Pause after SetFormat',
        "Pause after SetFormat needs a recorded SetFormat.",
    ),
    strm(
        "pause2-needs-format-edge",
        'if (r->count == 1u && n->value.format_sets == 0u) {\n            r->error = "Pause after SetFormat',
        'if (r->count == 1u && n->value.format_sets == 1u) {\n            r->error = "Pause after SetFormat',
        "one recorded SetFormat admits.",
    ),
    strm(
        "pause2-flag-count",
        'if (r->count == 1u && n->value.format_sets == 0u) {\n            r->error = "Pause after SetFormat',
        'if (r->count == 0u && n->value.format_sets == 0u) {\n            r->error = "Pause after SetFormat',
        "the startup Pause stays admitted on a stream with no SetFormat.",
    ),
    # T602 Discontinuity from sub_000299C0 (return 0x29A5A): the second measured caller of the owned virtual route.
    {
        "id": "dsd-thunk-reformat-caller",
        "file": "src/host/xdk_thunk.c",
        "old": "#define STREAM_REFORMAT_CALLER 0x00029A5Au",
        "new": "#define STREAM_REFORMAT_CALLER 0x00029A5Bu",
        "targets": ["test_stream_virtual_dispatch", "test_stream_status_dispatch"],
        "why": "only the measured re-format return address is a caller.",
    },
    {
        "id": "dsd-thunk-reformat-any",
        "file": "src/host/xdk_thunk.c",
        "old": "(is_status ? words[0] != STREAM_STATUS_REFORMAT_CALLER : (words[0] != STREAM_REFORMAT_CALLER && words[0] != STREAM_PUMP_CALLER)))) {",
        "new": "(is_status ? words[0] != STREAM_STATUS_REFORMAT_CALLER : false))) {",
        "targets": ["test_stream_virtual_dispatch", "test_stream_status_dispatch"],
        "why": "Discontinuity keeps its measured callers, a neighbouring return address is still refused.",
    },
    {
        "id": "dsd-thunk-reformat-status",
        "file": "src/host/xdk_thunk.c",
        "old": "(is_status ? words[0] != STREAM_STATUS_REFORMAT_CALLER : (words[0] != STREAM_REFORMAT_CALLER && words[0] != STREAM_PUMP_CALLER)))) {",
        "new": "((words[0] != STREAM_REFORMAT_CALLER && words[0] != STREAM_PUMP_CALLER)))) {",
        "targets": ["test_stream_virtual_dispatch", "test_stream_status_dispatch"],
        "why": "GetStatus never takes the Discontinuity re-format caller.",
    },
    # T602 SetVolume after SetFormat (return 0x29A67): the second caller, its need for a recorded SetFormat.
    strm(
        "vol2-caller",
        "frame_handler(c,VOLUME,0x29F68u,0x29A67u,0u,2u)",
        "frame_handler(c,VOLUME,0x29F68u,0x29A68u,0u,2u)",
        "only the measured re-format caller.",
    ),
    strm(
        "vol2-startup-caller",
        "frame_handler(c,VOLUME,0x29F68u,0x29A67u,0u,2u)",
        "frame_handler(c,VOLUME,0x29F69u,0x29A67u,0u,2u)",
        "the startup caller stays admitted next to the re-format caller.",
    ),
    strm(
        "vol2-flag-off",
        "access(VOLUME,args[0],args[1],actual==second?1u:0u,0u)",
        "access(VOLUME,args[0],args[1],0u,0u)",
        "the re-format caller needs a recorded SetFormat.",
    ),
    strm(
        "vol2-flag-inverted",
        "access(VOLUME,args[0],args[1],actual==second?1u:0u,0u)",
        "access(VOLUME,args[0],args[1],actual==second?0u:1u,0u)",
        "the startup caller needs no SetFormat, the re-format caller does.",
    ),
    strm(
        "vol2-needs-format",
        'if (r->count == 1u && n->value.format_sets == 0u) {\n            r->error = "SetVolume after SetFormat',
        'if (false) {\n            r->error = "SetVolume after SetFormat',
        "SetVolume after SetFormat needs a recorded SetFormat.",
    ),
    strm(
        "vol2-needs-format-edge",
        'if (r->count == 1u && n->value.format_sets == 0u) {\n            r->error = "SetVolume after SetFormat',
        'if (r->count == 1u && n->value.format_sets == 1u) {\n            r->error = "SetVolume after SetFormat',
        "one recorded SetFormat admits.",
    ),
    # T605 GetStatus of a started stream (callers 0x29D28 and 0x29CEA): the recorded start, the fresh scope, the
    # single status word. The stereo stream with or without SetFormat and the spatial one are all answered 1.
    strm(
        "stat2-started-partial",
        "bool started = n->value.discontinuity_seen;",
        "bool started = n->value.pause_seen;",
        "a stream started only up to Pause or FlushEx is refused, the whole start is Discontinuity.",
    ),
    strm(
        "stat2-started-lost",
        "bool started = n->value.discontinuity_seen;",
        "bool started = false;",
        "a started stream (stereo, re-formatted or not, and spatial) is answered.",
    ),
    strm(
        "stat2-started-always",
        "bool started = n->value.discontinuity_seen;",
        "bool started = true;",
        "a half started stream (Pause or FlushEx recorded) is refused.",
    ),
    strm(
        "stat2-fresh-pause",
        "bool fresh = !n->value.pause_seen && !n->value.flush_seen && !n->value.discontinuity_seen;\n        if ((!spatial && !stereo)",
        "bool fresh = !n->value.flush_seen && !n->value.discontinuity_seen;\n        if ((!spatial && !stereo)",
        "a recorded Pause is not the fresh startup status.",
    ),
    strm(
        "stat2-refuse-both",
        "n->value.volume != -10000 || (!fresh && !started)) {",
        "n->value.volume != -10000 || (!fresh || !started)) {",
        "the fresh and the started stream are two admitted states, neither needs the other.",
    ),
    strm(
        "stat2-neighbour",
        "!kernel_guest_write_u32(r->argument,1u)) {",
        "!kernel_guest_write_u32(r->argument,1u) || !kernel_guest_write_u32(r->argument+4u,0u)) {",
        "the original writes the status word and nothing else outside its own frame.",
    ),
    {
        "id": "dsd-thunk-status-reformat-caller",
        "file": "src/host/xdk_thunk.c",
        "old": "#define STREAM_STATUS_REFORMAT_CALLER 0x00029D28u",
        "new": "#define STREAM_STATUS_REFORMAT_CALLER 0x00029D29u",
        "targets": ["test_stream_status_dispatch"],
        "why": "only the measured sub_00029D10 return address is a status caller of the re-formatted stream.",
    },
    {
        "id": "dsd-thunk-status-reformat-any",
        "file": "src/host/xdk_thunk.c",
        "old": "(is_status ? words[0] != STREAM_STATUS_REFORMAT_CALLER : (words[0] != STREAM_REFORMAT_CALLER && words[0] != STREAM_PUMP_CALLER)))) {",
        "new": "(is_status ? false : (words[0] != STREAM_REFORMAT_CALLER && words[0] != STREAM_PUMP_CALLER)))) {",
        "targets": ["test_stream_status_dispatch"],
        "why": "GetStatus keeps its measured callers, a neighbouring return address is still refused.",
    },
    {
        "id": "dsd-thunk-status-reformat-discontinuity",
        "file": "src/host/xdk_thunk.c",
        "old": "(is_status ? words[0] != STREAM_STATUS_REFORMAT_CALLER : (words[0] != STREAM_REFORMAT_CALLER && words[0] != STREAM_PUMP_CALLER)))) {",
        "new": "((words[0] != STREAM_STATUS_REFORMAT_CALLER)))) {",
        "targets": ["test_stream_virtual_dispatch", "test_stream_status_dispatch"],
        "why": "Discontinuity never takes the status caller (and keeps its re-format caller).",
    },
    # ================================================================ dsound_buffer.c
    buf(
        "policy-enabled",
        'if(!enabled)return "explicit headless-buffers policy is disabled";',
        'if(enabled)return "explicit headless-buffers policy is disabled";',
        "the explicit opt-in policy gate.",
    ),
    buf(
        "policy-irql",
        'irql!=0u)return "only known IRQL0 is supported";',
        'irql>1u)return "only known IRQL0 is supported";',
        "only IRQL zero is supported.",
    ),
    buf(
        "node-size",
        "bytes==BUFFER_BYTES;",
        "bytes>=BUFFER_BYTES;",
        "a buffer heap block must be exactly the 36-byte header.",
    ),
    buf(
        "node-header",
        "memcmp(actual,node->value.header,sizeof(actual))==0",
        "memcmp(actual,node->value.header,sizeof(actual)-4u)==0",
        "the whole header is the ownership evidence.",
    ),
    buf(
        "output-base",
        "uint32_t base=flags==16u?0x005835F0u:0x005818E8u;",
        "uint32_t base=flags==16u?0x005818E8u:0x005835F0u;",
        "the two output tables are distinct by descriptor class.",
    ),
    buf(
        "output-range",
        "output>=base && output-base<160u",
        "output>=base && output-base<=160u",
        "the output table is exactly 40 slots.",
    ),
    buf(
        "output-align",
        "(output-base)%4u==0u",
        "(output-base)%2u==0u",
        "outputs sit on DWORD slots.",
    ),
    buf(
        "stops-last",
        "0x00408040u};",
        "0x00408041u};",
        "an address one off in the safe list admits an unprotected vtable word.",
    ),
    buf(
        "stops-probe",
        "recomp_has_stop_boundary(stopped[i])!=1)return false;",
        "recomp_has_stop_boundary(stopped[i])!=0)return false;",
        "a verified unconditional boundary is exactly 1.",
    ),
    buf(
        "stops-missing",
        "    if(recomp_has_stop_boundary==NULL)return false;",
        "    if(false)return false;",
        "a build without the host boundary table must report not ready.",
    ),
    buf(
        "tables-count",
        "for(unsigned i=0u;i<4u;i++) {",
        "for(unsigned i=0u;i<3u;i++) {",
        "all four vtable words are checked.",
    ),
    buf(
        "tables-set",
        "for(unsigned j=0u;j<4u;j++)if(table[i]==stopped[j])found=true;",
        "for(unsigned j=0u;j<3u;j++)if(table[i]==stopped[j])found=true;",
        "the last stopped target must still be recognised.",
    ),
    buf(
        "alias-headers-vtable",
        "overlap(address,bytes,VTABLE,16u))return true;",
        "overlap(address,bytes,VTABLE,12u))return true;",
        "inputs may not alias the original vtable.",
    ),
    buf(
        "create-ready",
        "if(!dsound_buffer_stops_ready()){r->error=",
        "if(false){r->error=",
        "creation needs every stop boundary compiled in.",
    ),
    buf(
        "create-tables",
        "if(!safe_tables(table)){r->error=",
        "if(!safe_tables(table) && false){r->error=",
        "creation needs the original vtable protected.",
    ),
    buf(
        "create-descriptor-alias",
        "aliases_headers(r->scope.descriptor_address,24u,candidate->internal_address) ||",
        "aliases_headers(r->scope.descriptor_address,20u,candidate->internal_address) ||",
        "the descriptor span is 24 bytes.",
    ),
    buf(
        "create-header",
        "header[0]=VTABLE;header[1]=1u;header[2]=candidate->internal_address;",
        "header[0]=VTABLE;header[1]=2u;header[2]=candidate->internal_address;",
        "the public header words are compared on every access.",
    ),
    buf(
        "create-address",
        "r->node->value.buffer_address=address+28u;",
        "r->node->value.buffer_address=address+24u;",
        "the public buffer pointer is 28 bytes into the allocation.",
    ),
    buf(
        "create-announce",
        "r->announce=!announced;announced=true;",
        "r->announce=!announced;",
        "the banner prints once per reset.",
    ),
    buf(
        "create-outer",
        'if(outer!=0u)refuse(CREATE,"aggregation is unsupported");',
        'if(outer>1u)refuse(CREATE,"aggregation is unsupported");',
        "aggregation is unsupported.",
    ),
    buf(
        "create-scope",
        "if(interface!=device+8u || !output_scope(r.scope.flags,output))",
        "if(interface!=device+8u && !output_scope(r.scope.flags,output))",
        "both the interface and the output scope must hold.",
    ),
    buf(
        "create-device-limit",
        'device>UINT32_MAX-8u)\n        refuse(CREATE,"no owned SILENT device");',
        'device>UINT32_MAX-7u)\n        refuse(CREATE,"no owned SILENT device");',
        "device + 8 must not wrap.",
    ),
    buf(
        "create-rollback-destroy",
        "if(heap!=0u && !guest_heap_destroy(heap)) {",
        "if(false && !guest_heap_destroy(heap)) {",
        "an unpublished private heap must be destroyed after a refusal.",
    ),
    buf(
        "create-rollback-link",
        "r.node->next=rollback;rollback=r.node;r.error=",
        "r.node->next=rollback;r.error=",
        "a failed rollback keeps the node for retry.",
    ),
    buf(
        "create-oom-status",
        "(status==DSOUND_LEASE_PREPARE_FAILED && r.status==OOM && r.error==NULL))return OOM;",
        "(status==DSOUND_LEASE_PREPARE_FAILED && r.status==OOM))return OOM;",
        "an OOM status with a recorded refusal reason is a refusal.",
    ),
    buf(
        "access-apply",
        "if(n->value.scope.flags!=0x10u || r->apply!=0u){",
        "if(n->value.scope.flags!=0x10u){",
        "deferred apply is unsupported.",
    ),
    buf(
        "i3dl2-literal",
        "expected[9]={0u,0u,0xFFFFF448u,",
        "expected[9]={0u,0u,0xFFFFF449u,",
        "the room-attenuation literal is the measurement.",
    ),
    buf(
        "i3dl2-alias",
        "aliases_headers(r->argument,36u,internal)",
        "aliases_headers(r->argument,32u,internal)",
        "the parameter block span is 36 bytes.",
    ),
    buf(
        "min-literal",
        "r->argument!=0x3F800000u){r->error=",
        "r->argument!=0x3F800001u){r->error=",
        "only minDistance 1.0 is admitted.",
    ),
    buf(
        "rolloff-count",
        "r->argument!=CURVE || r->count!=1u)",
        "r->argument!=CURVE || r->count!=2u)",
        "the buffer curve has one point.",
    ),
    buf(
        "rolloff-mask",
        "n->value.curve_count=r->count;n->value.cache_mask=7u;",
        "n->value.curve_count=r->count;n->value.cache_mask=3u;",
        "mask 7 is the completed spatial startup.",
    ),
    buf(
        "access-parent",
        "r.identity.internal_address+8u,access_owned,&r,&result))\n        refuse(entry",
        "r.identity.internal_address+4u,access_owned,&r,&result))\n        refuse(entry",
        "the parent interface is the internal address plus 8.",
    ),
    buf(
        "snapshot-error",
        "access_owned,&r,&result) || r.error!=NULL)return false;",
        "access_owned,&r,&result))return false;",
        "a snapshot of a buffer with a refusal reason is not valid.",
    ),
    buf(
        "reset-prepare",
        "if(!token_equal(candidate,&r->identity) || !valid_node(r->node,&r->identity,candidate->internal_address))return false;",
        "if(!token_equal(candidate,&r->identity) && !valid_node(r->node,&r->identity,candidate->internal_address))return false;",
        "release needs both the token and an intact node.",
    ),
    buf(
        "reset-release",
        "if(dsound_device_release_lease(&r.identity,&reset_ops,&r)!=DSOUND_LEASE_OK)return false;",
        "if(dsound_device_release_lease(&r.identity,&reset_ops,&r)!=DSOUND_LEASE_OK)return true;",
        "a refused release must make reset report failure.",
    ),
    buf(
        "reset-announce",
        "announced=false;pthread_mutex_unlock(&lock);return true;",
        "pthread_mutex_unlock(&lock);return true;",
        "a reset re-arms the banner.",
    ),
    buf(
        "frame-create-caller",
        "(actual!=0x27AA9u && actual!=0x27B54u)",
        "(actual!=0x27AA9u && actual!=0x27B55u)",
        "the two measured Create callers.",
    ),
    buf(
        "frame-class",
        "scope.flags!=(actual==0x27AA9u?16u:0u)",
        "scope.flags!=(actual==0x27AA9u?0u:16u)",
        "each caller creates its own descriptor class.",
    ),
    buf(
        "dispatch-rolloff",
        "return dsound_buffer_cache_rolloff(args[0],args[1],args[2],args[3]);",
        "return dsound_buffer_cache_rolloff(args[0],args[2],args[1],args[3]);",
        "curve and count order.",
    ),
    buf(
        "handler-caller",
        "frame_handler(c,I3DL2,0x27AE8u,3u)",
        "frame_handler(c,I3DL2,0x27AE9u,3u)",
        "each handler admits only its own measured caller.",
    ),
    buf(
        "register-count",
        "count+=dsound_hle_register(ROLLOFF,rolloff_handler)?1u:0u;return count;",
        "count+=dsound_hle_register(ROLLOFF,rolloff_handler)?0u:1u;return count;",
        "the registration count is the caller's only signal.",
    ),
    # T597 SetBufferData: the host record, the measured scope, the named refusals, the route.
    buf(
        "setdata-spatial-mask",
        "bool spatial=n->value.scope.flags==0x10u && n->value.cache_mask==7u;",
        "bool spatial=n->value.scope.flags==0x10u && n->value.cache_mask>=3u;",
        "SetBufferData on a spatial buffer needs all three startup caches.",
    ),
    buf(
        "setdata-ordinary-flags",
        "bool ordinary=n->value.scope.flags==0u && n->value.cache_mask==0u;",
        "bool ordinary=n->value.scope.flags<=0x10u && n->value.cache_mask==0u;",
        "an unconfigured spatial buffer is not the ordinary scope.",
    ),
    buf(
        "setdata-scope-or",
        "if(!spatial && !ordinary){r->error=",
        "if(!spatial || !ordinary){r->error=",
        "either measured scope admits SetBufferData.",
    ),
    buf(
        "setdata-null-and",
        "if(r->argument==0u || r->count==0u){r->error=",
        "if(r->argument==0u && r->count==0u){r->error=",
        "a null pointer or a zero length each refuse (the original self-allocates or clears).",
    ),
    buf(
        "setdata-max-edge",
        "if(r->count>DATA_MAX_BYTES ||",
        "if(r->count>=DATA_MAX_BYTES ||",
        "exactly 64 MiB is admitted.",
    ),
    buf(
        "setdata-max-value",
        "#define DATA_MAX_BYTES 0x04000000u",
        "#define DATA_MAX_BYTES 0x04000001u",
        "one byte over 64 MiB is refused.",
    ),
    buf(
        "setdata-readable",
        "kernel_guest_at(r->argument,r->count)==NULL){r->error=",
        "kernel_guest_at(r->argument,r->count)!=NULL){r->error=",
        "the data range must be readable guest memory.",
    ),
    buf(
        "setdata-readable-and",
        "if(r->count>DATA_MAX_BYTES || kernel_guest_at(",
        "if(r->count>DATA_MAX_BYTES && kernel_guest_at(",
        "the bound and the readability probe refuse independently.",
    ),
    buf(
        "setdata-alias-length",
        "if(aliases_state(r->argument,r->count,internal)){r->error=",
        "if(aliases_state(r->argument,1u,internal)){r->error=",
        "the whole data range is checked against owned state.",
    ),
    buf(
        "setdata-record-length",
        "n->value.data_length=r->count;",
        "n->value.data_length=r->argument;",
        "the record keeps the length, not the pointer.",
    ),
    buf(
        "setdata-record-address",
        "n->value.data_address=r->argument;",
        "n->value.data_address=r->count;",
        "the record keeps the pointer, not the length.",
    ),
    buf(
        "setdata-record-sets",
        "n->value.data_sets++;",
        "n->value.data_sets+=2u;",
        "every admitted call counts once.",
    ),
    buf(
        "setdata-access-order",
        "return access(SET_DATA,buffer,data,length,0u);",
        "return access(SET_DATA,buffer,length,data,0u);",
        "pointer then length.",
    ),
    buf(
        "setdata-dispatch-order",
        "return dsound_buffer_set_data(args[0],args[1],args[2]);",
        "return dsound_buffer_set_data(args[0],args[2],args[1]);",
        "the stdcall order is this, data, length.",
    ),
    buf(
        "setdata-handler-count",
        "frame_handler(c,SET_DATA,SET_DATA_CALLER,3u)",
        "frame_handler(c,SET_DATA,SET_DATA_CALLER,2u)",
        "three stdcall arguments are read.",
    ),
    buf(
        "setdata-caller",
        "#define SET_DATA_CALLER 0x000282C0u",
        "#define SET_DATA_CALLER 0x000282C1u",
        "only the measured title caller.",
    ),
    buf(
        "setdata-register-gate",
        "if(policy)count+=dsound_hle_register(SET_DATA,set_data_handler)?1u:0u;",
        "if(!policy)count+=dsound_hle_register(SET_DATA,set_data_handler)?1u:0u;",
        "a flags-off boot keeps its registry, a flags-on boot registers the route.",
    ),
    buf(
        "setdata-register-always",
        "const bool policy=enabled;",
        "const bool policy=true;",
        "no registration while the policy is off.",
    ),
    # T601 SetVolume: the host record, the measured scope and order, the exact volume, the route.
    buf(
        "setvol-needs-data",
        'if(n->value.data_sets==0u){r->error="SetVolume before',
        'if(false){r->error="SetVolume before',
        "SetVolume before SetBufferData is not the measured order.",
    ),
    buf(
        "setvol-needs-data-edge",
        "if(n->value.data_sets==0u){r->error=",
        "if(n->value.data_sets==1u){r->error=",
        "exactly one recorded SetBufferData admits.",
    ),
    buf(
        "setvol-exact-volume",
        "if(r->argument!=VOLUME_STARTUP && r->argument!=VOLUME_UPDATE){r->error=",
        "if(r->argument==VOLUME_STARTUP && r->argument!=VOLUME_UPDATE){r->error=",
        "only the measured volumes are admitted.",
    ),
    buf(
        "setvol-volume-value",
        "#define VOLUME_STARTUP 0xFFFFD8F0u",
        "#define VOLUME_STARTUP 0xFFFFD8F1u",
        "the measured value is -10000 exactly.",
    ),
    buf(
        "setvol-record-volume",
        "n->value.volume=(int32_t)r->argument;",
        "n->value.volume=(int32_t)r->count;",
        "the record keeps the volume argument.",
    ),
    buf(
        "setvol-record-sets",
        "n->value.volume_sets++;",
        "n->value.volume_sets+=2u;",
        "every admitted call counts once.",
    ),
    buf(
        "setvol-result",
        "r->announce=!volume_announced;volume_announced=true;\n        *result=0u;goto done;",
        "r->announce=!volume_announced;volume_announced=true;\n        *result=1u;goto done;",
        "the original returns S_OK.",
    ),
    buf(
        "setvol-guest-write",
        "n->value.volume_sets++;",
        "n->value.volume_sets++;kernel_guest_write_u32(n->value.header_address+4u,0u);",
        "the record writes no guest word, the original writes nothing in the header either.",
    ),
    buf(
        "setvol-access-order",
        "return access(SET_VOLUME,buffer,(uint32_t)volume,0u,0u);",
        "return access(SET_VOLUME,buffer,0u,(uint32_t)volume,0u);",
        "the volume is the argument word.",
    ),
    buf(
        "setvol-dispatch-order",
        "return access(SET_VOLUME,args[0],args[1],",
        "return access(SET_VOLUME,args[1],args[0],",
        "the stdcall order is this, volume.",
    ),
    buf(
        "setvol-handler-count",
        "frame_handler(c,SET_VOLUME,SET_VOLUME_CALLER,2u)",
        "frame_handler(c,SET_VOLUME,SET_VOLUME_CALLER,3u)",
        "two stdcall arguments are read.",
    ),
    buf(
        "setvol-caller",
        "#define SET_VOLUME_CALLER 0x00028348u",
        "#define SET_VOLUME_CALLER 0x00028349u",
        "only the measured title caller.",
    ),
    buf(
        "setvol-register-gate",
        "if(policy)count+=dsound_hle_register(SET_VOLUME,set_volume_handler)?1u:0u;",
        "if(!policy)count+=dsound_hle_register(SET_VOLUME,set_volume_handler)?1u:0u;",
        "a flags-off boot keeps its registry, a flags-on boot registers the route.",
    ),
    # T601 Pause(0): the order, the exact mode, the host count, the route.
    buf(
        "pause-needs-volume",
        "if(n->value.volume_sets==0u){r->error=",
        "if(false){r->error=",
        "Pause needs the recorded SetVolume (which implies the recorded SetBufferData).",
    ),
    buf(
        "pause-needs-volume-edge",
        "if(n->value.volume_sets==0u){r->error=",
        "if(n->value.volume_sets==1u){r->error=",
        "exactly one recorded SetVolume admits.",
    ),
    buf(
        "pause-mode-exact",
        'if(r->argument!=0u){r->error="only Pause(0)',
        'if(r->argument==0u){r->error="only Pause(0)',
        "only mode 0 is admitted.",
    ),
    buf(
        "pause-mode-edge",
        'if(r->argument!=0u){r->error="only Pause(0)',
        'if(r->argument>1u){r->error="only Pause(0)',
        "mode 1 is refused (the original returns 1 for it).",
    ),
    buf(
        "pause-record-sets",
        "\n        n->value.pause_sets++;",
        "\n        n->value.pause_sets+=2u;",
        "every admitted call counts once.",
    ),
    buf(
        "pause-result",
        "r->announce=!pause_announced;pause_announced=true;\n        *result=0u;goto done;",
        "r->announce=!pause_announced;pause_announced=true;\n        *result=1u;goto done;",
        "the original returns the mode, 0.",
    ),
    buf(
        "pause-guest-write",
        "\n        n->value.pause_sets++;",
        "\n        n->value.pause_sets++;kernel_guest_write_u32(n->value.header_address+4u,0u);",
        "the original writes no guest word for an unstarted buffer.",
    ),
    buf(
        "pause-access-order",
        "return access(PAUSE,buffer,mode,0u,0u);",
        "return access(PAUSE,buffer,0u,mode,0u);",
        "the mode is the argument word.",
    ),
    buf(
        "pause-dispatch-order",
        "return dsound_buffer_pause(args[0],args[1]);",
        "return dsound_buffer_pause(args[1],args[0]);",
        "the stdcall order is this, mode.",
    ),
    buf(
        "pause-handler-count",
        "frame_handler(c,PAUSE,PAUSE_CALLER,2u)",
        "frame_handler(c,PAUSE,PAUSE_CALLER,3u)",
        "two stdcall arguments are read.",
    ),
    buf(
        "pause-caller",
        "#define PAUSE_CALLER 0x0002751Eu",
        "#define PAUSE_CALLER 0x0002751Fu",
        "only the measured title caller.",
    ),
    buf(
        "pause-register-gate",
        "if(policy)count+=dsound_hle_register(PAUSE,pause_handler)?1u:0u;",
        "if(!policy)count+=dsound_hle_register(PAUSE,pause_handler)?1u:0u;",
        "a flags-off boot keeps its registry, a flags-on boot registers the route.",
    ),
    # T601 SetFrequency: the order, the exact measured frequency, the host record, the route.
    buf(
        "freq-needs-pause",
        "if(n->value.pause_sets==0u){r->error=",
        "if(false){r->error=",
        "SetFrequency needs the recorded Pause(0) (which implies SetVolume and SetBufferData).",
    ),
    buf(
        "freq-needs-pause-edge",
        "if(n->value.pause_sets==0u){r->error=",
        "if(n->value.pause_sets==1u){r->error=",
        "exactly one recorded Pause admits.",
    ),
    buf(
        "freq-exact",
        "if(r->argument!=FREQUENCY_STARTUP && !(completion",
        "if(r->argument==FREQUENCY_STARTUP && !(completion",
        "only the measured startup frequency is admitted.",
    ),
    buf(
        "freq-value",
        "#define FREQUENCY_STARTUP 22042u",
        "#define FREQUENCY_STARTUP 22043u",
        "the measured value is 22042 exactly.",
    ),
    buf(
        "freq-record-value",
        "n->value.frequency=r->argument;",
        "n->value.frequency=r->count;",
        "the record keeps the frequency argument.",
    ),
    buf(
        "freq-record-sets",
        "n->value.frequency_sets++;",
        "n->value.frequency_sets+=2u;",
        "every admitted call counts once.",
    ),
    buf(
        "freq-result",
        "r->announce=!frequency_announced;frequency_announced=true;\n        *result=0u;goto done;",
        "r->announce=!frequency_announced;frequency_announced=true;\n        *result=1u;goto done;",
        "the original returns S_OK.",
    ),
    buf(
        "freq-guest-write",
        "n->value.frequency_sets++;",
        "n->value.frequency_sets++;kernel_guest_write_u32(n->value.header_address+4u,0u);",
        "the original writes nothing in the header.",
    ),
    buf(
        "freq-access-order",
        "return access(SET_FREQUENCY,buffer,hertz,0u,0u);",
        "return access(SET_FREQUENCY,buffer,0u,hertz,0u);",
        "the frequency is the argument word.",
    ),
    buf(
        "freq-dispatch-order",
        "return dsound_buffer_set_frequency(args[0],args[1]);",
        "return dsound_buffer_set_frequency(args[1],args[0]);",
        "the stdcall order is this, hertz.",
    ),
    buf(
        "freq-handler-count",
        "frame_handler(c,SET_FREQUENCY,SET_FREQUENCY_CALLER,2u)",
        "frame_handler(c,SET_FREQUENCY,SET_FREQUENCY_CALLER,3u)",
        "two stdcall arguments are read.",
    ),
    buf(
        "freq-caller",
        "#define SET_FREQUENCY_CALLER 0x00027547u",
        "#define SET_FREQUENCY_CALLER 0x00027548u",
        "only the measured title caller.",
    ),
    buf(
        "freq-register-gate",
        "if(policy)count+=dsound_hle_register(SET_FREQUENCY,frequency_handler)?1u:0u;",
        "if(!policy)count+=dsound_hle_register(SET_FREQUENCY,frequency_handler)?1u:0u;",
        "a flags-off boot keeps its registry, a flags-on boot registers the route.",
    ),
    # T605 SetVolume(-3204) from the sound update (return 0x28643): the exact value, the whole start, the scope, the callers.
    buf(
        "vol2-literal",
        "#define VOLUME_UPDATE 0xFFFFF37Cu",
        "#define VOLUME_UPDATE 0xFFFFF37Du",
        "the measured update volume is -3204.",
    ),
    buf(
        "vol2-any-volume",
        "if(!completion){\n            if(r->argument!=VOLUME_STARTUP && r->argument!=VOLUME_UPDATE){",
        "if(completion){\n            if(r->argument!=VOLUME_STARTUP && r->argument!=VOLUME_UPDATE){",
        "only the two measured volumes are admitted, the original answers every volume.",
    ),
    buf(
        "vol2-start-lost",
        "if(r->argument!=VOLUME_STARTUP && r->argument!=VOLUME_UPDATE){r->error=",
        "if(r->argument!=VOLUME_UPDATE){r->error=",
        "the start volume -10000 stays admitted.",
    ),
    buf(
        "vol2-update-lost",
        "if(r->argument!=VOLUME_STARTUP && r->argument!=VOLUME_UPDATE){r->error=",
        "if(r->argument!=VOLUME_STARTUP){r->error=",
        "the update volume -3204 is admitted.",
    ),
    buf(
        "vol2-needs-frequency",
        "(n->value.frequency_sets==0u || n->value.scope.flags!=0u)){",
        "(n->value.scope.flags!=0u)){",
        "the update comes after the whole start, SetFrequency included.",
    ),
    buf(
        "vol2-scope",
        "(n->value.frequency_sets==0u || n->value.scope.flags!=0u)){",
        "(n->value.frequency_sets==0u)){",
        "a started spatial buffer (flags 0x10) is not the measured update.",
    ),
    buf(
        "vol2-record",
        "n->value.volume=(int32_t)r->argument;n->value.volume_sets++;",
        "n->value.volume=(int32_t)VOLUME_STARTUP;n->value.volume_sets++;",
        "the host record keeps the volume the original stored.",
    ),
    buf(
        "vol2-caller",
        "#define SET_VOLUME_UPDATE_CALLER 0x00028643u",
        "#define SET_VOLUME_UPDATE_CALLER 0x00028644u",
        "only the measured sound update return address is the second caller.",
    ),
    buf(
        "vol2-any-caller",
        "(actual!=caller && !(entry==SET_VOLUME && actual==SET_VOLUME_UPDATE_CALLER) &&",
        "(actual!=caller && !(entry==SET_VOLUME) &&",
        "SetVolume keeps its two measured callers, neighbouring return addresses are refused.",
    ),
    buf(
        "vol2-pairing",
        "if(entry==SET_VOLUME && (actual==SET_VOLUME_UPDATE_CALLER ? (!completion && args[1]!=VOLUME_UPDATE) : (!completion && args[1]!=VOLUME_STARTUP)))",
        "if(false)",
        "each measured SetVolume caller is paired with its own volume.",
    ),
    buf(
        "max-distance-raw-bits",
        "n->value.max_distance_bits=r->argument;n->value.max_distance_sets++;",
        "n->value.max_distance_bits=0u;n->value.max_distance_sets++;",
        "the host record must retain the exact raw float bits from the title caller.",
    ),
    buf(
        "max-distance-apply-gate",
        "n->value.data_sets==0u || r->apply!=1u) {",
        "n->value.data_sets==0u) {",
        "only the measured apply=1 update is admitted; apply=0 reaches an unmodeled commit helper.",
    ),
    buf(
        "max-distance-title-caller",
        "#define MAX_DISTANCE_CALLER 0x000280B5u",
        "#define MAX_DISTANCE_CALLER 0x000280B4u",
        "the exact sole title return site is admitted, not its preceding byte.",
    ),
    buf(
        "max-distance-registration",
        "if(policy)count+=dsound_hle_register(MAX_DISTANCE,max_distance_handler)?1u:0u;",
        "if(false)count+=dsound_hle_register(MAX_DISTANCE,max_distance_handler)?1u:0u;",
        "SetMaxDistance is registered only under the explicit passive buffer policy.",
    ),
    # ================================================================ dsound_listener.c
    lis(
        "same-or",
        "a->device_heap==b->device_heap && a->internal_address==b->internal_address",
        "a->device_heap==b->device_heap || a->internal_address==b->internal_address",
        "an incarnation needs both the heap and the address to match.",
    ),
    lis(
        "incarnation",
        "if(cache.cache_mask!=0u && !same(identity,&cache.identity)){",
        "if(cache.cache_mask!=0u && same(identity,&cache.identity)){",
        "a cache from another device incarnation must refuse.",
    ),
    lis(
        "snapshot-identity",
        "dsound_listener_snapshot value=cache;value.identity=*identity;*r->output=value;",
        "dsound_listener_snapshot value=cache;*r->output=value;",
        "the snapshot carries the live identity.",
    ),
    lis(
        "policy-enabled",
        "if(!enabled){r->error=",
        "if(enabled){r->error=",
        "the explicit opt-in policy gate.",
    ),
    lis(
        "policy-irql",
        'irql!=0u){r->error="only known IRQL0 supported"',
        'irql>1u){r->error="only known IRQL0 supported"',
        "only IRQL zero is supported.",
    ),
    lis(
        "policy-global",
        'global!=0u){r->error="original global audio state',
        'global>1u){r->error="original global audio state',
        "the original global audio state must be quiescent.",
    ),
    lis(
        "commit-mask",
        "if(cache.cache_mask!=7u ||",
        "if(cache.cache_mask!=3u ||",
        "commit needs doppler, position and orientation all cached.",
    ),
    lis(
        "work-needs-commit",
        "if(!cache.commit_seen){r->error=",
        "if(cache.commit_seen){r->error=",
        "DoWork requires a recorded commit.",
    ),
    lis(
        "work-store",
        "cache.work_seen=true;",
        "cache.work_seen=false;",
        "the DoWork observation is a recorded fact.",
    ),
    lis(
        "commit-store",
        "} else cache.commit_seen=true;",
        "} else cache.commit_seen=false;",
        "the commit observation is a recorded fact.",
    ),
    lis(
        "stage-mask-table",
        "const uint32_t expected_mask[]={0u,1u,3u};",
        "const uint32_t expected_mask[]={0u,1u,1u};",
        "orientation follows position, so mask 3.",
    ),
    lis(
        "stage-basis",
        "{{0u},{0u},{0u,0u,0x3F800000u,0u,0x3F800000u,0u}};",
        "{{0u},{0u},{0u,0u,0x3F800001u,0u,0x3F800000u,0u}};",
        "the exact orientation basis is the measured startup value.",
    ),
    lis(
        "stage-apply",
        "if(r->apply!=0u || cache.cache_mask!=expected_mask[r->stage]",
        "if(cache.cache_mask!=expected_mask[r->stage]",
        "deferred apply is unsupported.",
    ),
    lis(
        "stage-order",
        "cache.cache_mask|=1u<<r->stage;",
        "cache.cache_mask|=1u<<(r->stage+1u);",
        "the mask bit is the stage index.",
    ),
    lis(
        "announce",
        "if(!announced){announced=true;r->announce=true;}",
        "if(!announced){r->announce=true;}",
        "the banner prints once per reset.",
    ),
    lis(
        "work-limit",
        "internal>UINT32_MAX-8u)",
        "internal>UINT32_MAX-7u)",
        "device + 8 must not wrap.",
    ),
    lis(
        "work-interface",
        "return invoke(internal+8u,WORK,&r);",
        "return invoke(internal+4u,WORK,&r);",
        "the interface is the internal address plus 8.",
    ),
    lis(
        "handler-stack-end",
        "((uint64_t)frame->stack_ptr+4u>UINT64_C(0x100000000) ||",
        "((uint64_t)frame->stack_ptr+4u>=UINT64_C(0x100000000) ||",
        "a return slot ending exactly at 4 GiB is inside the address space.",
    ),
    lis(
        "handler-stack-limit",
        "(uint64_t)frame->stack_ptr+4u>frame->stack_limit)",
        "(uint64_t)frame->stack_ptr+4u>=frame->stack_limit)",
        "a return slot ending exactly at the stack limit is still inside it.",
    ),
    lis(
        "handler-caller",
        'actual!=caller)refuse(entry,"unsupported caller");',
        'actual==caller)refuse(entry,"unsupported caller");',
        "only the measured caller is supported.",
    ),
    lis(
        "dispatch-orientation",
        "dsound_listener_cache_orientation(args[0],args[1],args[2],args[3],args[4],args[5],args[6],args[7])",
        "dsound_listener_cache_orientation(args[0],args[1],args[3],args[2],args[4],args[5],args[6],args[7])",
        "front and top vector order.",
    ),
    lis(
        "dispatch-position",
        "dsound_listener_cache_position(args[0],args[1],args[2],args[3],args[4])",
        "dsound_listener_cache_position(args[1],args[0],args[2],args[3],args[4])",
        "interface first.",
    ),
    lis(
        "handler-caller-doppler",
        "handler(c,DOPPLER,0x27B78u,3u)",
        "handler(c,DOPPLER,0x27B79u,3u)",
        "each handler admits only its own measured caller.",
    ),
    lis(
        "work-route-result",
        "&& work_route(caller))return 0u;",
        "&& work_route(caller))return 1u;",
        "a routed DoWork returns zero to the guest.",
    ),
    lis(
        "work-caller",
        ":0x1CE492u,0u);",
        ":0x1CE493u,0u);",
        "the DoWork caller constant.",
    ),
    lis(
        "work-caller-main-loop",
        "have && caller==0x1CE45Fu?0x1CE45Fu:0x1CE492u",
        "have && caller==0x1CE460u?0x1CE460u:0x1CE492u",
        "the second measured DoWork caller is the title's main loop return address 0x1CE45F.",
    ),
    lis(
        "work-caller-any",
        "have && caller==0x1CE45Fu?0x1CE45Fu:0x1CE492u",
        "have?caller:0x1CE492u",
        "DoWork admits exactly two measured callers, any other return address is refused.",
    ),
    lis(
        "work-caller-first-lost",
        "have && caller==0x1CE45Fu?0x1CE45Fu:0x1CE492u",
        "have && caller==0x1CE45Fu?0x1CE45Fu:0x1CE45Fu",
        "the first measured DoWork caller 0x1CE492 stays admitted.",
    ),
    lis(
        "reset-announce",
        "memset(&cache,0,sizeof(cache));announced=false;",
        "memset(&cache,0,sizeof(cache));",
        "a reset re-arms the banner.",
    ),
    lis(
        "snapshot-valid",
        "dsound_device_with_owned_identity(interface,owned,&r,&result) && r.valid;",
        "dsound_device_with_owned_identity(interface,owned,&r,&result);",
        "a refused snapshot request is not a snapshot.",
    ),
    lis(
        "register-count",
        "(dsound_hle_register(WORK,work_handler)?1u:0u)",
        "(dsound_hle_register(WORK,work_handler)?0u:1u)",
        "the registration count is the caller's only signal.",
    ),
    # ================================================================ dsound_stream_scope.c
    ssc(
        "format-bytes",
        "descriptor_bytes!=DSOUND_STREAM_DESCRIPTOR_BYTES || format_bytes!=DSOUND_STREAM_FORMAT_BYTES)",
        "descriptor_bytes!=DSOUND_STREAM_DESCRIPTOR_BYTES || format_bytes<DSOUND_STREAM_FORMAT_BYTES)",
        "an undersized format buffer must be refused.",
    ),
    ssc(
        "packets-offset",
        "result.max_packets=word32(descriptor+4u);",
        "result.max_packets=word32(descriptor+8u);",
        "descriptor field offsets.",
    ),
    ssc(
        "format-offset",
        "result.format_address=word32(descriptor+8u);",
        "result.format_address=word32(descriptor+12u);",
        "descriptor field offsets.",
    ),
    ssc(
        "channels-offset",
        "result.channels=word16(format+2u);",
        "result.channels=word16(format+4u);",
        "format field offsets.",
    ),
    ssc(
        "rate-offset",
        "result.sample_rate=word32(format+4u);",
        "result.sample_rate=word32(format+8u);",
        "format field offsets.",
    ),
    ssc(
        "average-offset",
        "result.average_bytes_per_second=word32(format+8u);",
        "result.average_bytes_per_second=word32(format+4u);",
        "format field offsets.",
    ),
    ssc(
        "align-offset",
        "result.block_align=word16(format+12u);",
        "result.block_align=word16(format+14u);",
        "format field offsets.",
    ),
    ssc(
        "flags",
        "(result.flags!=0u && result.flags!=0x10u)",
        "(result.flags!=0u)",
        "only the stereo and 3D classes are measured.",
    ),
    ssc(
        "packets", "result.max_packets!=3u ||", "result.max_packets>3u ||", "exactly three packets."
    ),
    ssc(
        "format-null",
        "result.format_address==0u ||\n       (uint64_t)result.format_address+DSOUND_STREAM_FORMAT_BYTES",
        "false ||\n       (uint64_t)result.format_address+DSOUND_STREAM_FORMAT_BYTES",
        "a NULL format pointer is refused.",
    ),
    ssc(
        "format-end",
        ">UINT64_C(0x100000000) ||\n       word32(descriptor+12u)",
        ">=UINT64_C(0x100000000) ||\n       word32(descriptor+12u)",
        "a format ending exactly at 4 GiB is inside the address space.",
    ),
    ssc(
        "reserved-12",
        "word32(descriptor+12u)!=0u || word32(descriptor+16u)!=0u ||",
        "word32(descriptor+12u)!=0u ||",
        "reserved descriptor words must be zero.",
    ),
    ssc(
        "reserved-16",
        "word32(descriptor+12u)!=0u || word32(descriptor+16u)!=0u ||",
        "word32(descriptor+16u)!=0u ||",
        "reserved descriptor words must be zero.",
    ),
    ssc(
        "reserved-20",
        "word32(descriptor+20u)!=0u || word16(format)!=0x69u ||",
        "word16(format)!=0x69u ||",
        "reserved descriptor words must be zero.",
    ),
    ssc(
        "tag",
        "word16(format)!=0x69u ||",
        "word16(format)!=0x68u ||",
        "the format tag is Xbox ADPCM 0x69.",
    ),
    ssc(
        "rate",
        "result.sample_rate!=44100u ||",
        "result.sample_rate!=44101u ||",
        "the measured sample rate.",
    ),
    ssc(
        "format-14",
        "word16(format+14u)!=4u ||",
        "word16(format+14u)!=5u ||",
        "the measured ADPCM format tail.",
    ),
    ssc(
        "format-16",
        "word16(format+16u)!=2u ||",
        "word16(format+16u)!=3u ||",
        "the measured ADPCM format tail.",
    ),
    ssc(
        "format-18",
        "word16(format+18u)!=64u)",
        "word16(format+18u)!=65u)",
        "the measured ADPCM format tail.",
    ),
    ssc(
        "channels-class",
        "const uint16_t channels=result.flags==0u?2u:1u;",
        "const uint16_t channels=result.flags==0u?1u:2u;",
        "stereo for the plain class, mono for 3D.",
    ),
    ssc(
        "align",
        "result.block_align!=36u*channels",
        "result.block_align!=36u",
        "block align scales with the channel count.",
    ),
    ssc(
        "average-shift",
        "(((uint32_t)result.block_align*44100u)>>6u)",
        "(((uint32_t)result.block_align*44100u)>>5u)",
        "the average byte rate is block align times rate over 64.",
    ),
    ssc(
        "snapshot-end",
        "(uint64_t)address+sizeof(format)>UINT64_C(0x100000000) ||",
        "(uint64_t)address+sizeof(format)>=UINT64_C(0x100000000) ||",
        "a format ending exactly at 4 GiB is inside the address space.",
    ),
    ssc(
        "snapshot-overlap-desc",
        "((uint64_t)descriptor+sizeof(desc)>address &&",
        "((uint64_t)descriptor+sizeof(desc)>=address &&",
        "a format adjacent after the descriptor does not overlap it.",
    ),
    ssc(
        "snapshot-overlap-format",
        "(uint64_t)address+sizeof(format)>descriptor))",
        "(uint64_t)address+sizeof(format)>=descriptor))",
        "a format adjacent before the descriptor does not overlap it.",
    ),
    ssc(
        "snapshot-address",
        "result.descriptor_address=descriptor;",
        "result.descriptor_address=address;",
        "the recorded descriptor address is the descriptor, not the format.",
    ),
    # ================================================================ dsound_buffer_scope.c
    bsc(
        "format-bytes",
        "descriptor_bytes!=DSOUND_BUFFER_DESCRIPTOR_BYTES || format_bytes!=DSOUND_BUFFER_FORMAT_BYTES)",
        "descriptor_bytes!=DSOUND_BUFFER_DESCRIPTOR_BYTES || format_bytes<DSOUND_BUFFER_FORMAT_BYTES)",
        "an undersized format buffer must be refused.",
    ),
    bsc(
        "flags-offset",
        "result.flags=word32(descriptor+4u);",
        "result.flags=word32(descriptor+8u);",
        "descriptor field offsets.",
    ),
    bsc(
        "bytes-offset",
        "result.buffer_bytes=word32(descriptor+8u);",
        "result.buffer_bytes=word32(descriptor+4u);",
        "descriptor field offsets.",
    ),
    bsc(
        "format-offset",
        "result.format_address=word32(descriptor+12u);",
        "result.format_address=word32(descriptor+8u);",
        "descriptor field offsets.",
    ),
    bsc(
        "channels-offset",
        "result.channels=word16(format+2u);",
        "result.channels=word16(format+4u);",
        "format field offsets.",
    ),
    bsc(
        "rate-offset",
        "result.sample_rate=word32(format+4u);",
        "result.sample_rate=word32(format+8u);",
        "format field offsets.",
    ),
    bsc(
        "average-offset",
        "result.average_bytes_per_second=word32(format+8u);",
        "result.average_bytes_per_second=word32(format+4u);",
        "format field offsets.",
    ),
    bsc(
        "align-offset",
        "result.block_align=word16(format+12u);",
        "result.block_align=word16(format+14u);",
        "format field offsets.",
    ),
    bsc(
        "flags",
        "(result.flags!=0u && result.flags!=0x10u)",
        "(result.flags!=0u)",
        "only the 2D and 3D classes are measured.",
    ),
    bsc(
        "size-word",
        "word32(descriptor)!=24u",
        "word32(descriptor)!=28u",
        "the descriptor size word.",
    ),
    bsc(
        "buffer-bytes",
        "result.buffer_bytes!=0u ||",
        "false ||",
        "a passive buffer has no data bytes.",
    ),
    bsc(
        "format-null",
        "result.format_address==0u ||\n       (uint64_t)result.format_address+DSOUND_BUFFER_FORMAT_BYTES",
        "false ||\n       (uint64_t)result.format_address+DSOUND_BUFFER_FORMAT_BYTES",
        "a NULL format pointer is refused.",
    ),
    bsc(
        "format-end",
        ">UINT64_C(0x100000000) ||\n       word32(descriptor+16u)",
        ">=UINT64_C(0x100000000) ||\n       word32(descriptor+16u)",
        "a format ending exactly at 4 GiB is inside the address space.",
    ),
    bsc(
        "reserved-16",
        "       word32(descriptor+16u)!=0u ||\n",
        "       false ||\n",
        "reserved descriptor words must be zero.",
    ),
    bsc(
        "reserved-20",
        "       word32(descriptor+20u)!=0u || word16(format)!=0x69u",
        "       word16(format)!=0x69u",
        "reserved descriptor words must be zero.",
    ),
    bsc(
        "tag",
        "word16(format)!=0x69u",
        "word16(format)!=0x68u",
        "the format tag is Xbox ADPCM 0x69.",
    ),
    bsc(
        "rate",
        "result.sample_rate!=44000u",
        "result.sample_rate!=44001u",
        "the measured buffer sample rate is 44000, not the stream's 44100.",
    ),
    bsc(
        "format-14",
        "word16(format+14u)!=4u",
        "word16(format+14u)!=5u",
        "the measured ADPCM format tail.",
    ),
    bsc(
        "format-16",
        "word16(format+16u)!=2u",
        "word16(format+16u)!=3u",
        "the measured ADPCM format tail.",
    ),
    bsc(
        "format-18",
        "word16(format+18u)!=64u)",
        "word16(format+18u)!=65u)",
        "the measured ADPCM format tail.",
    ),
    bsc(
        "channels",
        "const uint16_t channels=1u;",
        "const uint16_t channels=2u;",
        "the passive buffer is mono.",
    ),
    bsc(
        "average-shift",
        "(((uint32_t)result.block_align*44000u)>>6u)",
        "(((uint32_t)result.block_align*44000u)>>5u)",
        "the average byte rate is block align times rate over 64.",
    ),
    bsc(
        "snapshot-end",
        "(uint64_t)address+sizeof(format)>UINT64_C(0x100000000) ||",
        "(uint64_t)address+sizeof(format)>=UINT64_C(0x100000000) ||",
        "a format ending exactly at 4 GiB is inside the address space.",
    ),
    bsc(
        "snapshot-overlap-desc",
        "((uint64_t)descriptor+sizeof(desc)>address &&",
        "((uint64_t)descriptor+sizeof(desc)>=address &&",
        "a format adjacent after the descriptor does not overlap it.",
    ),
    bsc(
        "snapshot-overlap-format",
        "(uint64_t)address+sizeof(format)>descriptor))",
        "(uint64_t)address+sizeof(format)>=descriptor))",
        "a format adjacent before the descriptor does not overlap it.",
    ),
    bsc(
        "snapshot-address",
        "result.descriptor_address=descriptor;",
        "result.descriptor_address=address;",
        "the recorded descriptor address is the descriptor, not the format.",
    ),
    # ================================================================ dsound_hrtf.c
    hrt(
        "passive",
        "    if (irql == 0u) {\n        (void)enter_call(CS);",
        "    if (irql <= 1u) {\n        (void)enter_call(CS);",
        "only passive level takes the guest critical section.",
    ),
    hrt(
        "owner",
        "state.owner != kernel_critsec_owner_token() ||",
        "state.owner == kernel_critsec_owner_token() ||",
        "the acquisition must be ours, not merely tracked.",
    ),
    hrt(
        "recursion",
        "state.recursion == 0u) refuse(",
        "state.recursion == 1u) refuse(",
        "a tracked acquisition has a nonzero recursion count.",
    ),
    hrt(
        "first-word",
        "0x00409D06u, 0x00409D40u,",
        "0x00409D07u, 0x00409D40u,",
        "the measured light-HRTF function table.",
    ),
    hrt("mid-word", "0x00409BFDu,", "0x00409BFEu,", "the measured light-HRTF function table."),
    hrt(
        "last-word",
        "0x00409F48u, 4u",
        "0x00409F48u, 5u",
        "the final table word is the measured constant 4.",
    ),
    hrt(
        "count",
        "for (unsigned i = 0u; i < 11u; i++)",
        "for (unsigned i = 0u; i < 10u; i++)",
        "all 11 words are written.",
    ),
    hrt(
        "stride",
        "kernel_guest_write_u32(TABLE + i * 4u, functions[i]);",
        "kernel_guest_write_u32(TABLE + i * 2u, functions[i]);",
        "the table is DWORD-strided.",
    ),
    hrt(
        "leave",
        "return irql == 0u ? leave_call(CS) : 0u;",
        "return irql != 0u ? leave_call(CS) : 0u;",
        "the critical section is left exactly when it was entered.",
    ),
    hrt(
        "enter-default",
        "enter_call = enter != NULL ? enter : kernel_critsec_enter_guest;",
        "enter_call = enter == NULL ? enter : kernel_critsec_enter_guest;",
        "a NULL override restores the guest critical-section call.",
    ),
    hrt(
        "leave-default",
        "leave_call = leave != NULL ? leave : kernel_critsec_leave_guest;",
        "leave_call = leave == NULL ? leave : kernel_critsec_leave_guest;",
        "a NULL override restores the guest critical-section call.",
    ),
    # ================================================================ dsound_effects_binding.c
    # ================================================================ dsound_effects_metadata.c
    efm(
        "code-words", "word(image+0x804u)*4u;", "word(image+0x804u)*2u;", "code size is in DWORDs."
    ),
    efm(
        "state-words",
        "word(image+0x80Cu)*4u;",
        "word(image+0x80Cu)*2u;",
        "state size is in DWORDs.",
    ),
    efm(
        "maps-max",
        "result.map_count>DSOUND_EFFECTS_MAX_MAPS",
        "result.map_count>=DSOUND_EFFECTS_MAX_MAPS",
        "exactly the maximum number of maps is allowed.",
    ),
    efm(
        "maps-zero",
        "result.map_count==0u ||",
        "result.map_count==1u ||",
        "an empty map list is refused.",
    ),
    efm(
        "workspace-zero",
        "result.workspace_bytes==0u)",
        "result.workspace_bytes==1u)",
        "a zero workspace is refused.",
    ),
    efm(
        "descriptor-bytes",
        "result.descriptor_bytes=8u+result.map_count*32u;",
        "result.descriptor_bytes=8u+result.map_count*28u;",
        "map records are 32 bytes.",
    ),
    efm(
        "iv-bytes",
        "result.iv_bytes=result.map_count*8u;",
        "result.iv_bytes=result.map_count*4u;",
        "one 8-byte IV per map.",
    ),
    efm(
        "iv-end",
        "iv_offset+result.iv_bytes>bytes)",
        "iv_offset+result.iv_bytes>=bytes)",
        "IVs ending exactly at the image end are inside it.",
    ),
    efm(
        "range-start",
        "return offset >= start &&",
        "return offset > start &&",
        "a range starting exactly at the block start is inside it.",
    ),
    efm(
        "range-end",
        "<= (uint64_t)start+bytes;",
        "< (uint64_t)start+bytes;",
        "a range ending exactly at the block end is inside it.",
    ),
    efm(
        "overlap-empty-current",
        "return n != 0u && m != 0u && (uint64_t)a+n>b",
        "return m != 0u && (uint64_t)a+n>b",
        "an empty current range overlaps nothing.",
    ),
    efm(
        "overlap-empty-previous",
        "return n != 0u && m != 0u && (uint64_t)a+n>b",
        "return n != 0u && (uint64_t)a+n>b",
        "an empty previous range overlaps nothing.",
    ),
    efm(
        "map-y-offset",
        "m->y_offset!=0u || m->y_bytes!=0u ||",
        "m->y_bytes!=0u ||",
        "the measured maps carry no Y block.",
    ),
    efm(
        "map-y-bytes",
        "m->y_offset!=0u || m->y_bytes!=0u ||",
        "m->y_offset!=0u ||",
        "the measured maps carry no Y block.",
    ),
    efm(
        "map-workspace-base",
        "workspace<0xC000u) return false;",
        "workspace<0xC001u) return false;",
        "a workspace at exactly 0xC000 is offset zero.",
    ),
    efm(
        "map-workspace-offset",
        "m->workspace_offset=workspace-0xC000u;",
        "m->workspace_offset=workspace-0xBFFCu;",
        "the workspace base constant.",
    ),
    efm(
        "overlap-code",
        "            if (overlap(m->code_offset,m->code_bytes,previous->code_offset,previous->code_bytes) ||",
        "            if (false ||",
        "two maps may not share code bytes.",
    ),
    efm(
        "overlap-state",
        "                overlap(m->state_offset,m->state_bytes,previous->state_offset,previous->state_bytes) ||",
        "                false ||",
        "two maps may not share state bytes.",
    ),
    efm(
        "overlap-workspace",
        "                overlap(m->workspace_offset,m->workspace_bytes,\n                        previous->workspace_offset,previous->workspace_bytes)) return false;",
        "                false) return false;",
        "two maps may not share workspace bytes.",
    ),
]

#: Survivors of the first sweep, closed by `tests/c/test_dsound_gaps.c` (one named scenario per id).
GAPS_KILLERS = {
    "dsd-hle-row-create-name",
    "dsd-hle-section-end-inclusive",
    "dsd-hle-plural-sites",
    "dsd-hle-codec-announce-once",
    "dsd-hle-ack-write-count",
    "dsd-hle-crosscheck-null-guard",
    "dsd-hle-requires-listener",
    "dsd-hle-requires-buffer",
    "dsd-dev-validate-size",
    "dsd-dev-header-vtable",
    "dsd-dev-header-list",
    "dsd-dev-header-reference",
    "dsd-dev-ops-abort",
    "dsd-dev-child-heap",
    "dsd-dev-child-size",
    "dsd-dev-lease-revalidate",
    "dsd-dev-release-null-lease",
    "dsd-dev-release-same-child",
    "dsd-dev-reset-restore",
    "dsd-dev-create-vtable-span",
    "dsd-dev-create-announce",
    "dsd-str-node-size",
    "dsd-str-alias-curve",
    "dsd-str-alias-descriptor",
    "dsd-str-alias-format",
    "dsd-str-alias-i3dl2",
    "dsd-str-alias-detached",
    "dsd-str-alias-headers-secondary",
    "dsd-str-tables-count",
    "dsd-str-tables-set",
    "dsd-str-tables-last",
    "dsd-str-create-output-descriptor",
    "dsd-str-create-finalize-announce",
    "dsd-str-create-device-limit",
    "dsd-str-status-curve",
    "dsd-str-i3dl2-alias",
    "dsd-str-i3dl2-compare",
    "dsd-str-reset-announce",
    "dsd-str-reset-extension",
    "dsd-str-frame-route",
    "dsd-buf-node-size",
    "dsd-buf-tables-count",
    "dsd-buf-tables-set",
    "dsd-buf-alias-headers-vtable",
    "dsd-buf-create-descriptor-alias",
    "dsd-buf-create-announce",
    "dsd-buf-create-device-limit",
    "dsd-buf-i3dl2-alias",
    "dsd-buf-reset-announce",
    "dsd-buf-frame-create-caller",
    "dsd-lis-snapshot-identity",
    "dsd-lis-announce",
    "dsd-lis-work-limit",
    "dsd-lis-handler-stack-end",
    "dsd-lis-work-route-result",
    "dsd-lis-reset-announce",
    "dsd-sscope-format-bytes",
    "dsd-sscope-format-null",
    "dsd-sscope-format-end",
    "dsd-sscope-snapshot-end",
    "dsd-sscope-snapshot-overlap-format",
    "dsd-bscope-format-bytes",
    "dsd-bscope-format-null",
    "dsd-bscope-format-end",
    "dsd-bscope-snapshot-end",
    "dsd-bscope-snapshot-overlap-format",
    "dsd-hrtf-mid-word",
    "dsd-efxmeta-maps-max",
    "dsd-efxmeta-maps-zero",
    "dsd-efxmeta-workspace-zero",
    "dsd-efxmeta-descriptor-bytes",
    "dsd-efxmeta-iv-bytes",
    "dsd-efxmeta-iv-end",
    "dsd-efxmeta-code-words",
    "dsd-efxmeta-state-words",
    "dsd-efxmeta-range-start",
    "dsd-efxmeta-range-end",
    "dsd-efxmeta-map-y-offset",
    "dsd-efxmeta-map-y-bytes",
    "dsd-efxmeta-map-workspace-base",
    "dsd-efxmeta-map-workspace-offset",
    "dsd-efxmeta-overlap-code",
    "dsd-efxmeta-overlap-state",
    "dsd-efxmeta-overlap-workspace",
    "dsd-efxmeta-overlap-empty-current",
    "dsd-efxmeta-overlap-empty-previous",
}
for _mutation in MUTATIONS:
    if _mutation["id"] in GAPS_KILLERS:
        _mutation["targets"].append("test_dsound_gaps")
    if _mutation["id"] == "dsd-buf-stops-missing":
        _mutation["targets"] = ["test_dsound_stops_absent"]
