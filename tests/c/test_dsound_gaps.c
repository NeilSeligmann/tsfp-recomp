/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T452: assertions that close the survivors of the src/audio mutation sweep
 * (tools/mutate/sets/dsound.py). Each scenario below names the mutant ids it kills.
 * Every scenario runs in a forked child with a fresh guest environment, so one
 * scenario's guest state, heaps and policy flags cannot leak into the next. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "test_d3d8_support.h"
#include "dsound_buffer.h"
#include "dsound_buffer_scope.h"
#include "dsound_device.h"
#include "dsound_effects_metadata.h"
#include "dsound_hle.h"
#include "dsound_hrtf.h"
#include "dsound_listener.h"
#include "dsound_stream.h"
#include "dsound_stream_scope.h"
#include <sys/wait.h>
#include <unistd.h>

#define SINGLETON 0x412B30u
#define GLOBAL_STATE 0x4124A8u
#define SECONDARY 0x4A1CF0u
#define BUFFER_VTABLE 0x4A1CE0u
#define STREAM_CURVE 0x4B914Cu
#define DEVICE_VTABLE 0x4A1CB0u

static bool fail_destroy;
bool __real_guest_heap_destroy(uint32_t heap);
bool __wrap_guest_heap_destroy(uint32_t heap)
{
    if (fail_destroy) { fail_destroy = false; return false; }
    return __real_guest_heap_destroy(heap);
}
int recomp_has_stop_boundary(uint32_t address) { (void)address; return 1; }

static uint8_t irql;
static bool current_irql(uint8_t *out) { *out = irql; return true; }
static uint32_t device, desc, format, params;

static unsigned count_of(const char *needle)
{
    unsigned found = 0u;
    for (const char *at = strstr(captured, needle); at != NULL; at = strstr(at + 1, needle)) found++;
    return found;
}
static void boot(bool create_device)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    dsound_hle_init();
    dsound_hle_set_log(capture_printer);
    dsound_device_reset();
    dsound_device_set_fatal(catching_fatal);
    map_fixed(0x412000u, 0x1000u);
    map_fixed(0x4A1000u, 0x1000u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    capture_clear();
    desc = SCRATCH_DATA + 256u;
    format = SCRATCH_DATA + 320u;
    params = SCRATCH_DATA + 0x400u;
    if (create_device) {
        CHECK_EQ_U32(dsound_device_create(0u, SCRATCH_DATA, 0u), 0u);
        device = load(SCRATCH_DATA) - 8u;
    }
}
static void write_stream_scope(uint32_t d, uint32_t f, bool spatial)
{
    uint8_t db[24] = {0}, fb[20] = {0};
    const uint16_t channels = spatial ? 1u : 2u;
    const uint32_t words[6] = {spatial ? 16u : 0u, 3u, f, 0u, 0u, 0u};
    memcpy(db, words, sizeof(db));
    const uint16_t shorts[2] = {0x69u, channels};
    memcpy(fb, shorts, 4u);
    const uint32_t rate = 44100u, average = (44100u * 36u * channels) >> 6u;
    memcpy(fb + 4u, &rate, 4u);
    memcpy(fb + 8u, &average, 4u);
    const uint16_t rest[4] = {(uint16_t)(36u * channels), 4u, 2u, 64u};
    memcpy(fb + 12u, rest, 8u);
    CHECK(kernel_guest_write_bytes(d, db, sizeof(db)));
    CHECK(kernel_guest_write_bytes(f, fb, sizeof(fb)));
}
static void write_buffer_scope(uint32_t d, uint32_t f, bool spatial)
{
    uint8_t db[24] = {0}, fb[20] = {0};
    const uint32_t words[6] = {24u, spatial ? 16u : 0u, 0u, f, 0u, 0u};
    memcpy(db, words, sizeof(db));
    const uint16_t shorts[2] = {0x69u, 1u};
    memcpy(fb, shorts, 4u);
    const uint32_t rate = 44000u, average = (44000u * 36u) >> 6u;
    memcpy(fb + 4u, &rate, 4u);
    memcpy(fb + 8u, &average, 4u);
    const uint16_t rest[4] = {36u, 4u, 2u, 64u};
    memcpy(fb + 12u, rest, 8u);
    CHECK(kernel_guest_write_bytes(d, db, sizeof(db)));
    CHECK(kernel_guest_write_bytes(f, fb, sizeof(fb)));
}
static void write_params(uint32_t at)
{
    const uint32_t p[9] = {0u, 0u, 0xFFFFF448u, 0u, 0u, 0u, 0u, 0u, 0u};
    CHECK(kernel_guest_write_bytes(at, p, sizeof(p)));
}
static uint32_t invoke(uint32_t entry, uint32_t caller, const uint32_t *args, unsigned count)
{
    const uint32_t stack = SCRATCH_DATA + 0x800u;
    store(stack, caller);
    for (unsigned i = 0u; i < count; i++) store(stack + 4u + 4u * i, args[i]);
    kernel_call_frame frame = {0};
    frame.stack_ptr = stack;
    frame.stack_limit = stack + 4u + 4u * count;
    return dsound_hle_call(entry, &frame);
}

/* ------------------------------------------------------------------ dsound_hle.c */
static uint32_t written_at, written_value;
static void ack_writer(uint32_t address, uint32_t value, void *user)
{ (void)user; written_at = address; written_value = value; }
static void test_hle(void)
{
    dsound_hle_init();
    dsound_hle_set_log(capture_printer);
    capture_clear();
    /* dsd-hle-row-create-name */
    const dsound_entry *create = dsound_hle_entry(0x00409635u);
    CHECK(create != NULL && create->name != NULL && strcmp(create->name, "DirectSoundCreate") == 0);
    /* dsd-hle-section-end-inclusive: the section end is exclusive, so END is outside it. */
    CHECK_EQ_U32(dsound_hle_call(DSOUND_SECTION_VA_END, NULL), 0u);
    CHECK(captured_has("OUTSIDE the DSOUND section"));
    capture_clear();
    CHECK_EQ_U32(dsound_hle_call(DSOUND_SECTION_VA_END - 1u, NULL), 0u);
    CHECK(captured_has("inside the DSOUND section"));
    /* dsd-hle-plural-sites */
    capture_clear();
    (void)dsound_hle_call(0x0040805bu, NULL);
    CHECK(captured_has("(1 measured call site)"));
    capture_clear();
    (void)dsound_hle_call(0x00407abcu, NULL);
    CHECK(captured_has("(6 measured call sites)"));
    /* dsd-hle-codec-announce-once: the first set of the default state still announces. */
    dsound_hle_init();
    capture_clear();
    dsound_hle_set_codec_state(DSOUND_CODEC_NOT_READY);
    CHECK(captured_has("codec readiness set NOT READY"));
    /* dsd-hle-ack-write-count */
    dsound_hle_init();
    CHECK(dsound_hle_set_dsp_ack(0x5000u, 0x810u));
    dsound_hle_set_dsp_writer(ack_writer, NULL);
    CHECK(dsound_hle_ack_dsp_command());
    CHECK(dsound_hle_ack_dsp_command());
    CHECK_EQ_U32(dsound_hle_dsp_ack_count(), 2u);
    CHECK_EQ_U32(written_at, 0x5810u);
    CHECK_EQ_U32(written_value, 0u);
    /* dsd-hle-crosscheck-null-guard: nothing was given, so every row of ours is missing. */
    CHECK_EQ_U32(dsound_hle_crosscheck(NULL, 0u), DSOUND_FUNCTION_COUNT);
    /* dsd-hle-requires-listener, dsd-hle-requires-buffer */
    CHECK(dsound_hle_requires_implementation(0x00409410u));
    CHECK(dsound_hle_requires_implementation(0x004093C8u));
    CHECK(!dsound_hle_requires_implementation(0x00409411u));
    CHECK(!dsound_hle_requires_implementation(0x004093C9u));
}

/* ---------------------------------------------------------------- dsound_device.c */
typedef struct lctx {
    dsound_device_lease_child child;
    uint32_t tamper_address;
    unsigned aborted;
} lctx;
static bool lprepare(const dsound_device_lease *candidate, void *userdata, dsound_device_lease_child *child)
{
    (void)candidate;
    lctx *x = userdata;
    if (x->tamper_address != 0u) store(x->tamper_address, 0xDEADu);
    *child = x->child;
    return true;
}
static void labort(void *userdata) { ((lctx *)userdata)->aborted++; }
static void lfinalize(const dsound_device_lease *token, void *userdata) { (void)token; (void)userdata; }
static const dsound_device_lease_ops lops = {lprepare, labort, lfinalize};
static const dsound_device_lease_ops lops_no_abort = {lprepare, NULL, lfinalize};
static void take_identity(const dsound_device_identity *identity, void *userdata, uint32_t *result)
{ *(dsound_device_identity *)userdata = *identity; *result = 0u; }
static lctx make_child(void)
{
    lctx x = {0};
    x.child.heap = guest_heap_create(0u, GUEST_HEAP_CHUNK_MIN, 0u);
    x.child.address = guest_heap_alloc(x.child.heap, 40u);
    x.child.bytes = 40u;
    CHECK(x.child.heap != 0u && x.child.address != 0u);
    return x;
}
static void test_device_state(void)
{
    boot(true);
    const uint32_t internal = device;
    /* dsd-dev-header-vtable, dsd-dev-header-list, dsd-dev-header-reference: the header is
     * pinned to the observed original literals, not to the facade's own constants. */
    CHECK_EQ_U32(load(internal), 0x4A1CB0u);
    CHECK_EQ_U32(load(internal + 4u), 6u);
    CHECK_EQ_U32(load(internal + 16u), internal + 16u);
    CHECK_EQ_U32(load(internal + 20u), internal + 16u);
    for (unsigned word = 2u; word < 11u; word++)
        if (word != 4u && word != 5u) CHECK_EQ_U32(load(internal + 4u * word), 0u);
    /* dsd-dev-validate-size: a same-address block of another size is not our device. */
    dsound_device_identity identity;
    uint32_t result = 0u;
    CHECK(dsound_device_with_owned_identity(device + 8u, take_identity, &identity, &result));
    uint8_t saved[44];
    CHECK(kernel_guest_read_bytes(internal, saved, sizeof(saved)));
    CHECK(guest_heap_free(identity.device_heap, internal));
    CHECK_EQ_U32(guest_heap_alloc(identity.device_heap, 48u), internal);
    CHECK(kernel_guest_write_bytes(internal, saved, sizeof(saved)));
    CHECK(!dsound_device_with_owned_identity(device + 8u, take_identity, &identity, &result));
}
static void test_device_leases(void)
{
    boot(true);
    dsound_device_lease token;
    /* dsd-dev-ops-abort */
    lctx a = make_child();
    CHECK_EQ_U32(dsound_device_acquire_lease(device + 8u, &lops_no_abort, &a, &token), DSOUND_LEASE_INVALID);
    /* dsd-dev-child-heap: a child may not live in the device's own heap. */
    dsound_device_identity identity;
    uint32_t result = 0u;
    CHECK(dsound_device_with_owned_identity(device + 8u, take_identity, &identity, &result));
    lctx own = {0};
    own.child.heap = identity.device_heap;
    own.child.address = guest_heap_alloc(identity.device_heap, 40u);
    own.child.bytes = 40u;
    CHECK(own.child.address != 0u);
    CHECK_EQ_U32(dsound_device_acquire_lease(device + 8u, &lops, &own, &token), DSOUND_LEASE_INVALID);
    CHECK_EQ_U32(load(device + 4u), 6u);
    /* dsd-dev-child-size: a child declaring fewer bytes than its block is not the block. */
    lctx small = make_child();
    small.child.bytes = 39u;
    CHECK_EQ_U32(dsound_device_acquire_lease(device + 8u, &lops, &small, &token), DSOUND_LEASE_INVALID);
    /* dsd-dev-lease-revalidate: a callback that changes the device must not commit. */
    lctx tampering = make_child();
    tampering.tamper_address = device + 8u;
    const unsigned before = tampering.aborted;
    CHECK_EQ_U32(dsound_device_acquire_lease(device + 8u, &lops, &tampering, &token), DSOUND_LEASE_INVALID);
    CHECK_EQ_U32(tampering.aborted, before + 1u);
    CHECK_EQ_U32(load(device + 4u), 6u);
    store(device + 8u, 0u);
    /* dsd-dev-release-same-child: the release callback must name the recorded child. */
    lctx first = make_child();
    CHECK_EQ_U32(dsound_device_acquire_lease(device + 8u, &lops, &first, &token), DSOUND_LEASE_OK);
    /* dsd-dev-release-null-lease: only a live lease list makes the dereference reachable. */
    CHECK_EQ_U32(dsound_device_release_lease(NULL, &lops, &a), DSOUND_LEASE_INVALID);
    lctx other = make_child();
    CHECK_EQ_U32(dsound_device_release_lease(&token, &lops, &other), DSOUND_LEASE_INVALID);
    CHECK_EQ_U32(load(device + 4u), 7u);
    CHECK_EQ_U32(dsound_device_release_lease(&token, &lops, &first), DSOUND_LEASE_OK);
    CHECK_EQ_U32(load(device + 4u), 6u);
}
static void test_device_reset_and_create(void)
{
    boot(false);
    dsound_hle_set_log(capture_printer);
    /* dsd-dev-create-announce: the banner prints once until a reset re-arms it. */
    CHECK_EQ_U32(dsound_device_create(0u, SCRATCH_DATA, 0u), 0u);
    CHECK_EQ_U32(dsound_device_create(0u, SCRATCH_DATA, 0u), 0u);
    CHECK_EQ_U32(count_of("explicit SILENT public-device facade"), 1u);
    /* dsd-dev-create-vtable-span: the output may not alias any of the 12 vtable bytes. */
    RUN_EXPECTING_FATAL((void)dsound_device_create(0u, DEVICE_VTABLE + 8u, 0u));
    CHECK(fatal_seen);
    const uint32_t internal = load(SINGLETON);
    CHECK(internal != 0u);
    /* dsd-dev-reset-restore: a refused heap destroy puts the singleton back for a retry. */
    fail_destroy = true;
    CHECK(!dsound_device_reset_checked());
    CHECK_EQ_U32(load(SINGLETON), internal);
    CHECK(dsound_device_reset_checked());
    CHECK_EQ_U32(load(SINGLETON), 0u);
    capture_clear();
    CHECK_EQ_U32(dsound_device_create(0u, SCRATCH_DATA, 0u), 0u);
    CHECK_EQ_U32(count_of("explicit SILENT public-device facade"), 1u);
}

/* ---------------------------------------------------------------- dsound_stream.c */
static void boot_stream(void)
{
    boot(false);
    dsound_stream_set_fatal(catching_fatal);
    dsound_stream_set_irql_provider(current_irql);
    dsound_stream_set_enabled(true);
    for (unsigned i = 0u; i < 15u; i++) store(SECONDARY + 4u * i, 0x406879u);
    CHECK_EQ_U32(dsound_device_create(0u, SCRATCH_DATA, 0u), 0u);
    device = load(SCRATCH_DATA) - 8u;
}
static uint32_t stream_at(uint32_t d, uint32_t f, bool spatial, uint32_t output)
{
    write_stream_scope(d, f, spatial);
    store(output, 0xAABBCCDDu);
    CHECK_EQ_U32(dsound_stream_create(d, output), 0u);
    return load(output);
}
static void stream_refused(uint32_t d, uint32_t output)
{
    const uint32_t references = load(device + 4u);
    RUN_EXPECTING_FATAL((void)dsound_stream_create(d, output));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(device + 4u), references);
}
static void test_stream_aliases(void)
{
    boot_stream();
    const uint32_t desc2 = SCRATCH_DATA + 0x600u, format2 = SCRATCH_DATA + 0x640u;
    map_fixed(0x4B9000u, 0x1000u);
    /* dsd-str-alias-curve: the whole 16-byte curve table is protected. */
    write_stream_scope(desc, format, true);
    stream_refused(desc, STREAM_CURVE + 12u);
    /* dsd-str-create-output-descriptor: the output may not alias the descriptor's last bytes. */
    stream_refused(desc, desc + 20u);
    /* dsd-str-alias-descriptor, dsd-str-alias-format: a live stream's own spans. */
    const uint32_t first = stream_at(desc, format, true, SCRATCH_DATA + 0x500u);
    CHECK(first != 0u);
    write_stream_scope(desc2, format2, true);
    stream_refused(desc2, desc + 20u);
    stream_refused(desc2, format + 16u);
    /* dsd-str-alias-i3dl2: the cached parameter block is protected to its last byte. */
    write_params(params);
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(first, params, 0u), 0u);
    stream_refused(desc2, params + 32u);
    /* dsd-str-alias-detached: a detached node still owns its guest block until cleanup. */
    dsound_stream_snapshot snapshot;
    CHECK(dsound_stream_get_snapshot(first, &snapshot));
    fail_destroy = true;
    CHECK(!dsound_stream_reset_checked());
    stream_refused(desc2, first);
    CHECK(dsound_stream_reset_checked());
}
static void test_stream_headers(void)
{
    boot_stream();
    /* dsd-str-alias-headers-secondary, dsd-str-i3dl2-alias: parameter blocks may not touch
     * the original secondary table or the global audio state, to the last byte. */
    const uint32_t stream = stream_at(desc, format, true, SCRATCH_DATA + 0x500u);
    write_params(SECONDARY + 56u);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_i3dl2(stream, SECONDARY + 56u, 0u));
    CHECK(fatal_seen);
    write_params(GLOBAL_STATE - 32u);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_i3dl2(stream, GLOBAL_STATE - 32u, 0u));
    CHECK(fatal_seen);
    /* dsd-str-i3dl2-compare: every word of the exact block is compared. */
    write_params(params);
    store(params + 32u, 1u);
    RUN_EXPECTING_FATAL((void)dsound_stream_cache_i3dl2(stream, params, 0u));
    CHECK(fatal_seen);
    write_params(params);
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(stream, params, 0u), 0u);
}
static void test_stream_tables(void)
{
    boot_stream();
    write_stream_scope(desc, format, true);
    /* dsd-str-tables-count: the last of the 15 table words is checked. */
    store(SECONDARY + 56u, 0x12345678u);
    stream_refused(desc, SCRATCH_DATA + 0x500u);
    store(SECONDARY + 56u, 0x406879u);
    /* dsd-str-tables-set, dsd-str-tables-last: the last stopped target is still a stop. */
    store(SECONDARY, 0x4093ADu);
    CHECK_EQ_U32(dsound_stream_create(desc, SCRATCH_DATA + 0x500u), 0u);
    /* dsd-str-create-device-limit: device + 8 must not wrap. */
    store(SINGLETON, 0xFFFFFFF8u);
    RUN_EXPECTING_FATAL((void)dsound_stream_create(desc, SCRATCH_DATA + 0x510u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "no owned SILENT device") != NULL);
}
static void test_stream_banner_and_status(void)
{
    boot_stream();
    /* dsd-str-create-finalize-announce, dsd-str-reset-announce */
    (void)stream_at(desc, format, true, SCRATCH_DATA + 0x500u);
    (void)stream_at(SCRATCH_DATA + 0x600u, SCRATCH_DATA + 0x640u, true, SCRATCH_DATA + 0x510u);
    CHECK_EQ_U32(count_of("explicit HEADLESS streams"), 1u);
    CHECK(dsound_stream_reset_checked());
    (void)stream_at(desc, format, true, SCRATCH_DATA + 0x500u);
    CHECK_EQ_U32(count_of("explicit HEADLESS streams"), 2u);
    /* dsd-str-status-curve: the startup status output may not alias the curve table tail. */
    CHECK(dsound_stream_reset_checked());
    map_fixed(0x4B9000u, 0x1000u);
    const uint32_t stream = stream_at(desc, format, true, SCRATCH_DATA + 0x500u);
    write_params(params);
    CHECK_EQ_U32(dsound_stream_cache_i3dl2(stream, params, 0u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_min_distance(stream, 0x3F800000u, 0u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_rolloff(stream, STREAM_CURVE, 4u, 0u), 0u);
    CHECK_EQ_U32(dsound_stream_cache_volume(stream, -10000), 0u);
    RUN_EXPECTING_FATAL((void)dsound_stream_get_startup_status(stream, STREAM_CURVE + 12u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(dsound_stream_get_startup_status(stream, SCRATCH_DATA + 0x520u), 0u);
    CHECK_EQ_U32(load(SCRATCH_DATA + 0x520u), 1u);
}
static void test_stream_node_size(void)
{
    boot_stream();
    /* dsd-str-node-size: a same-address block of another size is not our stream. */
    const uint32_t stream = stream_at(desc, format, true, SCRATCH_DATA + 0x500u);
    dsound_stream_snapshot snapshot;
    CHECK(dsound_stream_get_snapshot(stream, &snapshot));
    uint8_t saved[40];
    CHECK(kernel_guest_read_bytes(stream, saved, sizeof(saved)));
    CHECK(guest_heap_free(snapshot.stream_heap, stream));
    CHECK_EQ_U32(guest_heap_alloc(snapshot.stream_heap, 48u), stream);
    CHECK(kernel_guest_write_bytes(stream, saved, sizeof(saved)));
    CHECK(!dsound_stream_get_snapshot(stream, &snapshot));
}
static bool route_answers;
static bool route_fn(uint32_t entry, const kernel_call_frame *frame, uint32_t return_address, uint32_t *result)
{ (void)entry; (void)frame; (void)return_address; *result = 0x1234u; return route_answers; }
static bool reset_answer;
static bool reset_fn(void) { return reset_answer; }
static void test_stream_extension(void)
{
    boot_stream();
    CHECK_EQ_U32(dsound_stream_register(), 10u);
    dsound_stream_set_extension(route_fn, reset_fn);
    /* dsd-str-frame-route: a true route owns the call, a false route leaves it to the policy. */
    const uint32_t args[4] = {0u, 0u, 0u, 0u};
    route_answers = true;
    CHECK_EQ_U32(invoke(0x40967Cu, 0x388DA5u, args, 2u), 0x1234u);
    route_answers = false;
    RUN_EXPECTING_FATAL((void)invoke(0x40967Cu, 0x388DA5u, args, 2u));
    CHECK(fatal_seen);
    /* dsd-str-reset-extension: the extension's answer is part of the checked result. */
    reset_answer = true;
    CHECK(dsound_stream_reset_checked());
    reset_answer = false;
    CHECK(!dsound_stream_reset_checked());
    dsound_stream_set_extension(NULL, NULL);
}

/* --------------------------------------------------------------- dsound_buffer.c */
static void boot_buffer(void)
{
    boot(false);
    dsound_buffer_set_fatal(catching_fatal);
    dsound_buffer_set_irql_provider(current_irql);
    dsound_buffer_set_enabled(true);
    for (unsigned i = 0u; i < 4u; i++) store(BUFFER_VTABLE + 4u * i, 0x406879u);
    map_fixed(0x581000u, 0x1000u);
    map_fixed(0x583000u, 0x1000u);
    CHECK_EQ_U32(dsound_device_create(0u, SCRATCH_DATA, 0u), 0u);
    device = load(SCRATCH_DATA) - 8u;
}
static void buffer_refused(uint32_t d, uint32_t output)
{
    const uint32_t references = load(device + 4u);
    RUN_EXPECTING_FATAL((void)dsound_buffer_create(device + 8u, d, output, 0u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(load(device + 4u), references);
}
static uint32_t buffer_at(uint32_t d, uint32_t f, bool spatial, uint32_t output)
{
    write_buffer_scope(d, f, spatial);
    store(output, 0xAABBCCDDu);
    CHECK_EQ_U32(dsound_buffer_create(device + 8u, d, output, 0u), 0u);
    return load(output);
}
static void test_buffer_gaps(void)
{
    boot_buffer();
    write_buffer_scope(desc, format, true);
    /* dsd-buf-tables-count: the last of the four table words is checked. */
    store(BUFFER_VTABLE + 12u, 0x12345678u);
    buffer_refused(desc, 0x5835F0u);
    store(BUFFER_VTABLE + 12u, 0x406879u);
    /* dsd-buf-tables-set, dsd-buf-stops-last: the last stopped target is still a stop. */
    store(BUFFER_VTABLE, 0x408040u);
    const uint32_t buffer = buffer_at(desc, format, true, 0x5835F0u);
    store(BUFFER_VTABLE, 0x406879u);
    /* dsd-buf-create-announce, dsd-buf-reset-announce */
    (void)buffer_at(desc, format, true, 0x5835F4u);
    CHECK_EQ_U32(count_of("explicit HEADLESS buffers"), 1u);
    CHECK(dsound_buffer_reset_checked());
    (void)buffer_at(desc, format, true, 0x5835F0u);
    CHECK_EQ_U32(count_of("explicit HEADLESS buffers"), 2u);
    CHECK(dsound_buffer_reset_checked());
    /* dsd-buf-create-descriptor-alias: a descriptor may not reach into the global audio state. */
    write_buffer_scope(GLOBAL_STATE - 20u, format, true);
    buffer_refused(GLOBAL_STATE - 20u, 0x5835F0u);
    /* dsd-buf-i3dl2-alias, dsd-buf-alias-headers-vtable: parameter blocks may not touch the
     * global state or the vtable's last word. */
    const uint32_t live = buffer_at(desc, format, true, 0x5835F0u);
    write_params(GLOBAL_STATE - 32u);
    RUN_EXPECTING_FATAL((void)dsound_buffer_cache_i3dl2(live, GLOBAL_STATE - 32u, 0u));
    CHECK(fatal_seen);
    write_params(BUFFER_VTABLE + 12u);
    RUN_EXPECTING_FATAL((void)dsound_buffer_cache_i3dl2(live, BUFFER_VTABLE + 12u, 0u));
    CHECK(fatal_seen);
    (void)buffer;
}
static void test_buffer_node_and_limit(void)
{
    boot_buffer();
    /* dsd-buf-node-size */
    const uint32_t buffer = buffer_at(desc, format, true, 0x5835F0u);
    dsound_buffer_snapshot snapshot;
    CHECK(dsound_buffer_get_snapshot(buffer, &snapshot));
    uint8_t saved[36];
    CHECK(kernel_guest_read_bytes(snapshot.header_address, saved, sizeof(saved)));
    CHECK(guest_heap_free(snapshot.buffer_heap, snapshot.header_address));
    CHECK_EQ_U32(guest_heap_alloc(snapshot.buffer_heap, 48u), snapshot.header_address);
    CHECK(kernel_guest_write_bytes(snapshot.header_address, saved, sizeof(saved)));
    CHECK(!dsound_buffer_get_snapshot(buffer, &snapshot));
    /* dsd-buf-create-device-limit */
    write_buffer_scope(desc, format, true);
    store(SINGLETON, 0xFFFFFFF8u);
    RUN_EXPECTING_FATAL((void)dsound_buffer_create(0xFFFFFFF8u + 8u, desc, 0x5835F4u, 0u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "no owned SILENT device") != NULL);
}
static void test_buffer_callers(void)
{
    boot_buffer();
    CHECK_EQ_U32(dsound_buffer_register(), 10u); /* Above plus policy-gated SetPosition (T1068). */
    /* dsd-buf-frame-create-caller: both measured Create callers, each with its own class. */
    write_buffer_scope(desc, format, true);
    uint32_t args[4] = {device + 8u, desc, 0x5835F0u, 0u};
    CHECK_EQ_U32(invoke(0x4093C8u, 0x27AA9u, args, 4u), 0u);
    write_buffer_scope(desc, format, false);
    args[2] = 0x5818E8u;
    CHECK_EQ_U32(invoke(0x4093C8u, 0x27B54u, args, 4u), 0u);
}

/* -------------------------------------------------------------- dsound_listener.c */
static bool listener_route(uint32_t return_address) { (void)return_address; return true; }
static void boot_listener(void)
{
    boot(false);
    dsound_listener_set_fatal(catching_fatal);
    dsound_listener_set_irql_provider(current_irql);
    dsound_listener_set_enabled(true);
    CHECK_EQ_U32(dsound_device_create(0u, SCRATCH_DATA, 0u), 0u);
    device = load(SCRATCH_DATA) - 8u;
}
static void test_listener_gaps(void)
{
    boot_listener();
    const uint32_t interface = device + 8u;
    /* dsd-lis-snapshot-identity: even an empty cache carries the live device identity. */
    dsound_listener_snapshot snapshot;
    CHECK(dsound_listener_get_snapshot(interface, &snapshot));
    CHECK_EQ_U32(snapshot.identity.internal_address, device);
    CHECK(guest_heap_valid(snapshot.identity.device_heap));
    /* dsd-lis-announce, dsd-lis-reset-announce */
    CHECK_EQ_U32(dsound_listener_cache_doppler(interface, 0u, 0u), 0u);
    CHECK_EQ_U32(dsound_listener_cache_position(interface, 0u, 0u, 0u, 0u), 0u);
    CHECK_EQ_U32(count_of("explicit passive listener CPU cache"), 1u);
    dsound_listener_reset();
    CHECK_EQ_U32(dsound_listener_cache_doppler(interface, 0u, 0u), 0u);
    CHECK_EQ_U32(count_of("explicit passive listener CPU cache"), 2u);
    CHECK_EQ_U32(dsound_listener_cache_position(interface, 0u, 0u, 0u, 0u), 0u);
    CHECK_EQ_U32(dsound_listener_cache_orientation(interface, 0u, 0u, 0x3F800000u, 0u, 0x3F800000u, 0u, 0u), 0u);
    CHECK_EQ_U32(dsound_listener_cache_commit(interface), 0u);
    /* dsd-lis-work-limit: device + 8 must not wrap. */
    store(SINGLETON, 0xFFFFFFF8u);
    RUN_EXPECTING_FATAL((void)dsound_listener_cache_work());
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "unavailable or invalid device singleton") != NULL);
    store(SINGLETON, device);
    /* dsd-lis-work-route-result: a routed DoWork answers zero. */
    CHECK_EQ_U32(dsound_listener_register(), 5u);
    dsound_listener_set_work_route(listener_route);
    CHECK_EQ_U32(invoke(0x407B40u, 0x12345u, NULL, 0u), 0u);
    dsound_listener_set_work_route(NULL);
}
static void test_listener_stack_end(void)
{
    boot_listener();
    const uint32_t interface = device + 8u;
    CHECK_EQ_U32(dsound_listener_cache_doppler(interface, 0u, 0u), 0u);
    CHECK_EQ_U32(dsound_listener_cache_position(interface, 0u, 0u, 0u, 0u), 0u);
    CHECK_EQ_U32(dsound_listener_cache_orientation(interface, 0u, 0u, 0x3F800000u, 0u, 0x3F800000u, 0u, 0u), 0u);
    CHECK_EQ_U32(dsound_listener_cache_commit(interface), 0u);
    CHECK_EQ_U32(dsound_listener_register(), 5u);
    /* dsd-lis-handler-stack-end: a return slot ending exactly at 4 GiB is inside the space. */
    map_fixed(0xFFFFF000u, 0x1000u);
    store(0xFFFFFFFCu, 0x1CE492u);
    kernel_call_frame frame = {0};
    frame.stack_ptr = 0xFFFFFFFCu;
    CHECK_EQ_U32(dsound_hle_call(0x407B40u, &frame), 0u);
    /* A limit equal to the slot end is also still inside it. */
    dsound_listener_snapshot snapshot;
    CHECK(dsound_listener_get_snapshot(interface, &snapshot));
    CHECK(snapshot.work_seen);
}

/* ------------------------------------------------------------------ the scopes */
static void test_scopes(void)
{
    boot(false);
    uint8_t sd[24], sf[20], bd[24], bf[20];
    dsound_stream_scope stream_scope;
    dsound_buffer_scope buffer_scope;
    const uint32_t scratch_desc = SCRATCH_DATA + 0x100u, scratch_format = SCRATCH_DATA + 0x140u;
    write_stream_scope(scratch_desc, scratch_format, false);
    CHECK(kernel_guest_read_bytes(scratch_desc, sd, sizeof(sd)));
    CHECK(kernel_guest_read_bytes(scratch_format, sf, sizeof(sf)));
    write_buffer_scope(scratch_desc, scratch_format, false);
    CHECK(kernel_guest_read_bytes(scratch_desc, bd, sizeof(bd)));
    CHECK(kernel_guest_read_bytes(scratch_format, bf, sizeof(bf)));
    CHECK(dsound_stream_scope_validate(sd, sizeof(sd), sf, sizeof(sf), &stream_scope));
    CHECK(dsound_buffer_scope_validate(bd, sizeof(bd), bf, sizeof(bf), &buffer_scope));
    /* dsd-sscope-format-bytes, dsd-bscope-format-bytes: the format length is exact. */
    uint8_t long_format[24] = {0};
    memcpy(long_format, sf, sizeof(sf));
    CHECK(!dsound_stream_scope_validate(sd, sizeof(sd), long_format, sizeof(long_format), &stream_scope));
    memcpy(long_format, bf, sizeof(bf));
    CHECK(!dsound_buffer_scope_validate(bd, sizeof(bd), long_format, sizeof(long_format), &buffer_scope));
    /* dsd-sscope-format-null, dsd-bscope-format-null */
    uint8_t changed[24];
    memcpy(changed, sd, sizeof(sd));
    memset(changed + 8u, 0, 4u);
    CHECK(!dsound_stream_scope_validate(changed, sizeof(changed), sf, sizeof(sf), &stream_scope));
    memcpy(changed, bd, sizeof(bd));
    memset(changed + 12u, 0, 4u);
    CHECK(!dsound_buffer_scope_validate(changed, sizeof(changed), bf, sizeof(bf), &buffer_scope));
    /* dsd-sscope-format-end, dsd-bscope-format-end: a format ending exactly at 4 GiB fits. */
    memcpy(changed, sd, sizeof(sd));
    const uint32_t top = 0xFFFFFFECu;
    memcpy(changed + 8u, &top, 4u);
    CHECK(dsound_stream_scope_validate(changed, sizeof(changed), sf, sizeof(sf), &stream_scope));
    memcpy(changed + 8u, &top, 4u);
    const uint32_t beyond = 0xFFFFFFEDu;
    memcpy(changed + 8u, &beyond, 4u);
    CHECK(!dsound_stream_scope_validate(changed, sizeof(changed), sf, sizeof(sf), &stream_scope));
    memcpy(changed, bd, sizeof(bd));
    memcpy(changed + 12u, &top, 4u);
    CHECK(dsound_buffer_scope_validate(changed, sizeof(changed), bf, sizeof(bf), &buffer_scope));
    memcpy(changed + 12u, &beyond, 4u);
    CHECK(!dsound_buffer_scope_validate(changed, sizeof(changed), bf, sizeof(bf), &buffer_scope));
    /* dsd-sscope-snapshot-overlap-format, dsd-bscope-snapshot-overlap-format: a format
     * that ends exactly where the descriptor starts does not overlap it. */
    const uint32_t descriptor_at = SCRATCH_DATA + 0x300u, before_format = descriptor_at - 20u;
    write_stream_scope(descriptor_at, before_format, false);
    CHECK(dsound_stream_scope_snapshot(descriptor_at, &stream_scope));
    CHECK_EQ_U32(stream_scope.format_address, before_format);
    write_buffer_scope(descriptor_at, before_format, false);
    CHECK(dsound_buffer_scope_snapshot(descriptor_at, &buffer_scope));
    CHECK_EQ_U32(buffer_scope.format_address, before_format);
    /* dsd-sscope-snapshot-end, dsd-bscope-snapshot-end: a format ending exactly at 4 GiB is
     * readable guest memory and fits. */
    map_fixed(0xFFFFF000u, 0x1000u);
    write_stream_scope(descriptor_at, 0xFFFFFFECu, false);
    CHECK(dsound_stream_scope_snapshot(descriptor_at, &stream_scope));
    write_buffer_scope(descriptor_at, 0xFFFFFFECu, false);
    CHECK(dsound_buffer_scope_snapshot(descriptor_at, &buffer_scope));
}

/* ------------------------------------------------------------- dsound_hrtf.c */
static void test_hrtf_table(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x412000u, 0x1000u);
    dsound_hle_init();
    irql = 1u;
    dsound_hrtf_set_irql_provider(current_irql);
    CHECK_EQ_U32(dsound_use_light_hrtf(), 0u);
    /* dsd-hrtf-mid-word: every one of the 11 measured table words. */
    static const uint32_t expected[11] = {0x00409D06u, 0x00409D40u, 0x00409A0Bu, 0x00409AA1u, 0x00409AECu,
                                          0x00409BFDu, 0x00409E28u, 0x00409EDFu, 0x00409C8Du, 0x00409F48u, 4u};
    for (unsigned i = 0u; i < 11u; i++) CHECK_EQ_U32(load(0x412B5Cu + 4u * i), expected[i]);
}

/* ------------------------------------------------------ dsound_effects_metadata.c */
#define EFFECTS_HEADER 0x818u
typedef struct synthetic {
    uint8_t bytes[0x1000];
    size_t size;
    uint32_t maps;
} synthetic;
static void put(uint8_t *image, uint32_t offset, uint32_t value) { memcpy(image + offset, &value, 4u); }
/* `maps` maps, each owning 8 code bytes, 8 state bytes and 0x40 workspace bytes. */
static void build_effects(synthetic *s, uint32_t maps)
{
    memset(s, 0, sizeof(*s));
    s->maps = maps;
    const uint32_t code_bytes = 8u * maps, state_bytes = 8u * maps;
    put(s->bytes, 0x804u, code_bytes / 4u);
    put(s->bytes, 0x80Cu, state_bytes / 4u);
    const uint32_t descriptor = EFFECTS_HEADER + code_bytes + state_bytes;
    put(s->bytes, descriptor, maps);
    put(s->bytes, descriptor + 4u, 0x40u * maps);
    for (uint32_t i = 0u; i < maps; i++) {
        const uint32_t p = descriptor + 8u + i * 32u;
        put(s->bytes, p, EFFECTS_HEADER + 8u * i);
        put(s->bytes, p + 4u, 8u);
        put(s->bytes, p + 8u, EFFECTS_HEADER + code_bytes + 8u * i);
        put(s->bytes, p + 12u, 8u);
        put(s->bytes, p + 24u, 0xC000u + 0x40u * i);
        put(s->bytes, p + 28u, 0x40u);
    }
    s->size = descriptor + 8u + 32u * maps + 8u * maps;
}
static uint32_t map_field(const synthetic *s, uint32_t map, uint32_t field)
{
    const uint32_t code_bytes = 8u * s->maps, state_bytes = 8u * s->maps;
    return EFFECTS_HEADER + code_bytes + state_bytes + 8u + map * 32u + field;
}
static bool layout_ok(const synthetic *s, dsound_effects_metadata *out)
{ return dsound_effects_validate_layout(s->bytes, s->size, out); }
static void test_effects_layout(void)
{
    synthetic s;
    dsound_effects_metadata meta;
    build_effects(&s, 2u);
    CHECK(layout_ok(&s, &meta));
    /* dsd-efxmeta-descriptor-bytes, dsd-efxmeta-iv-bytes, dsd-efxmeta-code-words, dsd-efxmeta-state-words */
    CHECK_EQ_U32(meta.code_bytes, 16u);
    CHECK_EQ_U32(meta.state_bytes, 16u);
    CHECK_EQ_U32(meta.state_offset, EFFECTS_HEADER + 16u);
    CHECK_EQ_U32(meta.descriptor_offset, EFFECTS_HEADER + 32u);
    CHECK_EQ_U32(meta.descriptor_bytes, 8u + 2u * 32u);
    CHECK_EQ_U32(meta.iv_offset, EFFECTS_HEADER + 32u + 8u + 2u * 32u);
    CHECK_EQ_U32(meta.iv_bytes, 16u);
    /* dsd-efxmeta-map-workspace-offset, dsd-efxmeta-map-workspace-base: 0xC000 is offset zero. */
    CHECK_EQ_U32(meta.maps[0].workspace_offset, 0u);
    CHECK_EQ_U32(meta.maps[1].workspace_offset, 0x40u);
    /* dsd-efxmeta-range-start, dsd-efxmeta-range-end: ranges touching both block ends fit. */
    CHECK_EQ_U32(meta.maps[0].code_offset, meta.code_offset);
    CHECK_EQ_U32(meta.maps[1].code_offset + meta.maps[1].code_bytes, meta.code_offset + meta.code_bytes);
    /* dsd-efxmeta-iv-end: the image ends exactly at the last IV. */
    CHECK(dsound_effects_validate_layout(s.bytes, s.size, &meta));
    CHECK(!dsound_effects_validate_layout(s.bytes, s.size - 1u, &meta));
    /* dsd-efxmeta-maps-max: exactly the maximum fits, one more does not. */
    build_effects(&s, DSOUND_EFFECTS_MAX_MAPS);
    CHECK(layout_ok(&s, &meta));
    CHECK_EQ_U32(meta.map_count, DSOUND_EFFECTS_MAX_MAPS);
    build_effects(&s, DSOUND_EFFECTS_MAX_MAPS + 1u);
    CHECK(!layout_ok(&s, &meta));
    /* dsd-efxmeta-maps-zero: an empty map list is refused, a single map is accepted. */
    build_effects(&s, 1u);
    CHECK(layout_ok(&s, &meta));
    build_effects(&s, 2u);
    put(s.bytes, EFFECTS_HEADER + 32u, 0u);
    CHECK(!layout_ok(&s, &meta));
    /* dsd-efxmeta-workspace-zero: a zero workspace is refused, a one-byte workspace fits
     * a map that needs none. */
    build_effects(&s, 1u);
    put(s.bytes, EFFECTS_HEADER + 16u + 4u, 0u);
    put(s.bytes, map_field(&s, 0u, 28u), 0u);
    CHECK(!layout_ok(&s, &meta));
    put(s.bytes, EFFECTS_HEADER + 16u + 4u, 1u);
    CHECK(layout_ok(&s, &meta));
    /* dsd-efxmeta-map-y-offset, dsd-efxmeta-map-y-bytes */
    build_effects(&s, 2u);
    put(s.bytes, map_field(&s, 0u, 16u), 4u);
    CHECK(!layout_ok(&s, &meta));
    build_effects(&s, 2u);
    put(s.bytes, map_field(&s, 0u, 20u), 4u);
    CHECK(!layout_ok(&s, &meta));
    /* dsd-efxmeta-overlap-code, -state, -workspace: each overlap alone refuses the image. */
    build_effects(&s, 2u);
    put(s.bytes, map_field(&s, 1u, 0u), EFFECTS_HEADER + 4u);
    CHECK(!layout_ok(&s, &meta));
    build_effects(&s, 2u);
    put(s.bytes, map_field(&s, 1u, 8u), EFFECTS_HEADER + 16u + 4u);
    CHECK(!layout_ok(&s, &meta));
    build_effects(&s, 2u);
    put(s.bytes, map_field(&s, 1u, 24u), 0xC000u + 0x20u);
    CHECK(!layout_ok(&s, &meta));
    /* dsd-efxmeta-overlap-empty-current, -previous: an empty range overlaps nothing. */
    build_effects(&s, 2u);
    put(s.bytes, map_field(&s, 1u, 8u), EFFECTS_HEADER + 16u + 4u);
    put(s.bytes, map_field(&s, 1u, 12u), 0u);
    CHECK(layout_ok(&s, &meta));
    build_effects(&s, 2u);
    put(s.bytes, map_field(&s, 0u, 8u), EFFECTS_HEADER + 16u + 12u);
    put(s.bytes, map_field(&s, 0u, 12u), 0u);
    put(s.bytes, map_field(&s, 1u, 8u), EFFECTS_HEADER + 16u + 8u);
    put(s.bytes, map_field(&s, 1u, 12u), 8u);
    CHECK(layout_ok(&s, &meta));
}

typedef void (*scenario)(void);
static void isolated(scenario test)
{
    fflush(stdout);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        failures = 0;
        test();
        fflush(stdout);
        _exit(failures ? 1 : 0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
/* `test_dsound_gaps N` runs scenario N in-process, for debugging a crash. */
int main(int argc, char **argv)
{
    static const scenario scenarios[] = {
        test_hle, test_device_state, test_device_leases, test_device_reset_and_create,
        test_stream_aliases, test_stream_headers, test_stream_tables, test_stream_banner_and_status,
        test_stream_node_size, test_stream_extension, test_buffer_gaps, test_buffer_node_and_limit,
        test_buffer_callers, test_listener_gaps, test_listener_stack_end, test_scopes,
        test_hrtf_table, test_effects_layout};
    if (argc > 1) {
        scenarios[strtoul(argv[1], NULL, 10)]();
        printf("scenario %s: %d checks, %d failures\n", argv[1], checks, failures);
        return failures ? 1 : 0;
    }
    for (size_t i = 0u; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) isolated(scenarios[i]);
    printf("dsound gaps: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
