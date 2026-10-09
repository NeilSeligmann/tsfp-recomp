/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* T392: the bounded CPU model of the XMV movie PCM stream. Return values, status words and
 * GetStatus bits are the ones the ORIGINAL code produces (tests/test_dsound_movie_stream_oracle.py).
 * The completion rule is a stated model, tested against hand-computed virtual-clock deadlines. */
#include "test_d3d8_support.h"
#include "dsound_audio_runtime.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_movie_stream.h"
#include "dsound_stream.h"

#define FREQUENCY UINT64_C(733333333)
#define AVERAGE 176400u
#define XMV_CALLER 0x00445128u
#define TITLE_CALLER 0x00030364u
#define CREATE_CALLER 0x004451F4u
#define STARTUP_CALLER 0x00029943u
#define DECODER 0x41376170u
#define PENDING 0x8000000Au
#define ABORT 0x80004004u

static uint32_t device, descriptor, format, output_slot;
static uint8_t irql;
static bool irql_known = true;
static uint64_t fake_now = 1000u;
static bool current_irql(uint8_t *out) { *out = irql; return irql_known; }
static uint64_t clock_now(void) { return fake_now; }

typedef struct callback_record {
    uint32_t callback, stream_context, packet_context, status, completed_seen, status_seen;
    uint64_t at;
} callback_record;
static callback_record records[16];
static unsigned record_count;
static uint32_t watched_completed[16], watched_status[16];
static bool runner_ok = true;
static bool record_callback(uint32_t callback, uint32_t stream_context, uint32_t packet_context, uint32_t status)
{
    callback_record *r = &records[record_count < 16u ? record_count : 15u];
    r->callback = callback;
    r->stream_context = stream_context;
    r->packet_context = packet_context;
    r->status = status;
    r->at = fake_now;
    /* Order proof: the words the packet points at are already written when the callback runs. */
    r->completed_seen = packet_context < 16u ? load(watched_completed[packet_context]) : 0u;
    r->status_seen = packet_context < 16u ? load(watched_status[packet_context]) : 0u;
    record_count++;
    return runner_ok;
}

/* Audio buffer memory the packets point into, one megabyte at a fixed address. */
#define BUFFER_BASE 0x42000000u
static uint32_t buffer_base = BUFFER_BASE;
static uint32_t packet_at(unsigned index) { return SCRATCH_DATA + 0x400u + 0x20u * index; }
static uint32_t completed_at(unsigned index) { return SCRATCH_DATA + 0x600u + 8u * index; }
static uint32_t status_at(unsigned index) { return SCRATCH_DATA + 0x640u + 8u * index; }
/* Packet context is the packet index so the callback record names the packet. */
static uint32_t make_packet(unsigned index, uint32_t size)
{
    store(completed_at(index), 0xDEADBEEFu);
    store(status_at(index), 0xCAFEBABEu);
    watched_completed[index] = completed_at(index);
    watched_status[index] = status_at(index);
    const uint32_t words[6] = {buffer_base + 0x1000u * index, size, completed_at(index), status_at(index), index, 0u};
    CHECK(kernel_guest_write_bytes(packet_at(index), words, sizeof(words)));
    return packet_at(index);
}
static void write_descriptor(uint32_t flags, uint32_t packets, uint32_t callback, uint32_t context, uint32_t mixbins)
{
    const uint32_t desc[6] = {flags, packets, format, callback, context, mixbins};
    CHECK(kernel_guest_write_bytes(descriptor, desc, sizeof(desc)));
}
static void write_format(uint16_t tag, uint16_t channels, uint32_t rate, uint32_t average, uint16_t block,
                         uint16_t bits, uint16_t extra)
{
    uint8_t bytes[18];
    memcpy(bytes, &tag, 2u);
    memcpy(bytes + 2u, &channels, 2u);
    memcpy(bytes + 4u, &rate, 4u);
    memcpy(bytes + 8u, &average, 4u);
    memcpy(bytes + 12u, &block, 2u);
    memcpy(bytes + 14u, &bits, 2u);
    memcpy(bytes + 16u, &extra, 2u);
    CHECK(kernel_guest_write_bytes(format, bytes, sizeof(bytes)));
}
static void measured_descriptor(void)
{
    write_descriptor(0u, 2u, 0x445071u, DECODER, 0u);
    write_format(1u, 2u, 44100u, AVERAGE, 4u, 16u, 0u);
}
static uint32_t create_stream(void)
{
    measured_descriptor();
    store(output_slot, 0xAAAAAAAAu);
    CHECK_EQ_U32(dsound_movie_stream_create(descriptor, output_slot), 0u);
    return load(output_slot);
}
static uint32_t status_of(uint32_t stream)
{
    store(SCRATCH_DATA + 0x700u, 0xEEEEEEEEu);
    CHECK_EQ_U32(dsound_movie_stream_get_status(stream, SCRATCH_DATA + 0x700u), 0u);
    return load(SCRATCH_DATA + 0x700u);
}
static uint64_t ticks_for(uint32_t size) { return ((uint64_t)size * FREQUENCY + AVERAGE - 1u) / AVERAGE; }
static void expect_pending(unsigned index)
{
    CHECK_EQ_U32(load(completed_at(index)), 0u);
    CHECK_EQ_U32(load(status_at(index)), PENDING);
}
static void expect_untouched(unsigned index)
{
    CHECK_EQ_U32(load(completed_at(index)), 0xDEADBEEFu);
    CHECK_EQ_U32(load(status_at(index)), 0xCAFEBABEu);
}
static void expect_done(unsigned index, uint32_t size, uint32_t status)
{
    CHECK_EQ_U32(load(completed_at(index)), size);
    CHECK_EQ_U32(load(status_at(index)), status);
}

static void test_create_and_scope(void)
{
    CHECK_EQ_U32(dsound_movie_stream_count(), 0u);
    const uint32_t refs_before = load(device + 4u);
    const uint32_t stream = create_stream();
    CHECK(stream != 0u && stream != 0xAAAAAAAAu);
    CHECK(dsound_movie_stream_owns(stream));
    CHECK_EQ_U32(dsound_movie_stream_count(), 1u);
    CHECK_EQ_U32(load(stream), 0x4A1D00u);
    CHECK_EQ_U32(load(stream + 4u), 0x4A1CF0u);
    CHECK_EQ_U32(load(stream + 8u), 1u);
    CHECK_EQ_U32(load(device + 4u), refs_before + 1u);
    dsound_movie_stream_snapshot snapshot;
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK_EQ_U32(snapshot.callback, 0x445071u);
    CHECK_EQ_U32(snapshot.context, DECODER);
    CHECK_EQ_U32(snapshot.refs, 1u);
    CHECK_EQ_U32(snapshot.queued, 0u);
    CHECK_EQ_U32(status_of(stream), 1u);
    /* Every field of the measured descriptor and format is pinned: one change refuses the create. */
    struct { const char *name; uint32_t flags, packets, callback, context, mixbins; } desc_cases[] = {
        {"flags", 1u, 2u, 0x445071u, DECODER, 0u}, {"packets 3", 0u, 3u, 0x445071u, DECODER, 0u},
        {"packets 1", 0u, 1u, 0x445071u, DECODER, 0u}, {"callback", 0u, 2u, 0x445072u, DECODER, 0u},
        {"context", 0u, 2u, 0x445071u, 0u, 0u}, {"mixbins", 0u, 2u, 0x445071u, DECODER, 0x1000u},
    };
    for (unsigned i = 0u; i < sizeof(desc_cases) / sizeof(desc_cases[0]); i++) {
        measured_descriptor();
        write_descriptor(desc_cases[i].flags, desc_cases[i].packets, desc_cases[i].callback,
                         desc_cases[i].context, desc_cases[i].mixbins);
        store(SCRATCH_DATA + 0x200u, 0xAAAAAAAAu);
        RUN_EXPECTING_FATAL((void)dsound_movie_stream_create(descriptor, SCRATCH_DATA + 0x200u));
        CHECK(fatal_seen);
        CHECK_EQ_U32(load(SCRATCH_DATA + 0x200u), 0xAAAAAAAAu);
        CHECK_EQ_U32(dsound_movie_stream_count(), 1u);
    }
    struct { uint16_t tag, channels; uint32_t rate, average; uint16_t block, bits, extra; } wave_cases[] = {
        {0x69u, 2u, 44100u, AVERAGE, 4u, 16u, 0u}, {1u, 1u, 44100u, AVERAGE, 4u, 16u, 0u},
        {1u, 2u, 22050u, AVERAGE, 4u, 16u, 0u}, {1u, 2u, 44100u, 88200u, 4u, 16u, 0u},
        {1u, 2u, 44100u, AVERAGE, 2u, 16u, 0u}, {1u, 2u, 44100u, AVERAGE, 4u, 8u, 0u},
        {1u, 2u, 44100u, AVERAGE, 4u, 16u, 2u}, {0u, 2u, 44100u, AVERAGE, 4u, 16u, 0u},
    };
    for (unsigned i = 0u; i < sizeof(wave_cases) / sizeof(wave_cases[0]); i++) {
        measured_descriptor();
        write_format(wave_cases[i].tag, wave_cases[i].channels, wave_cases[i].rate, wave_cases[i].average,
                     wave_cases[i].block, wave_cases[i].bits, wave_cases[i].extra);
        RUN_EXPECTING_FATAL((void)dsound_movie_stream_create(descriptor, SCRATCH_DATA + 0x200u));
        CHECK(fatal_seen);
        CHECK_EQ_U32(dsound_movie_stream_count(), 1u);
    }
    CHECK_EQ_U32(load(device + 4u), refs_before + 1u);
    /* Every original vtable entry must be a compiled stop, one unprotected method refuses the create. */
    measured_descriptor();
    for (unsigned entry = 0u; entry < 15u; entry++) {
        store(0x4A1CF0u + 4u * entry, 0x4093B0u);
        RUN_EXPECTING_FATAL((void)dsound_movie_stream_create(descriptor, SCRATCH_DATA + 0x200u));
        CHECK(fatal_seen);
        store(0x4A1CF0u + 4u * entry, 0x406879u);
        CHECK_EQ_U32(dsound_movie_stream_count(), 1u);
    }
    CHECK_EQ_U32(load(device + 4u), refs_before + 1u);
    /* The startup stream module stays exact: it still refuses the PCM descriptor. */
    measured_descriptor();
    RUN_EXPECTING_FATAL((void)dsound_stream_create(descriptor, SCRATCH_DATA + 0x200u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(dsound_movie_stream_count(), 1u);
    CHECK_EQ_U32(load(device + 4u), refs_before + 1u);
    /* Policy preconditions. */
    measured_descriptor();
    dsound_movie_stream_set_enabled(false);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_create(descriptor, SCRATCH_DATA + 0x200u));
    CHECK(fatal_seen);
    dsound_movie_stream_set_enabled(true);
    irql = 2u;
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_create(descriptor, SCRATCH_DATA + 0x200u));
    CHECK(fatal_seen);
    irql = 0u;
    store(0x4124A8u, 1u);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_create(descriptor, SCRATCH_DATA + 0x200u));
    CHECK(fatal_seen);
    store(0x4124A8u, 0u);
    CHECK_EQ_U32(dsound_movie_stream_count(), 1u);
    CHECK_EQ_U32(load(device + 4u), refs_before + 1u);
}

static void test_references_volume_pause_status(uint32_t stream)
{
    CHECK_EQ_U32(dsound_movie_stream_add_ref(stream), 2u);
    CHECK_EQ_U32(load(stream + 8u), 2u);
    CHECK_EQ_U32(dsound_movie_stream_release(stream), 1u);
    CHECK_EQ_U32(load(stream + 8u), 1u);
    CHECK(dsound_movie_stream_owns(stream));
    const int32_t volumes[] = {0, 1, -1, -10000, -10001, INT32_MAX, INT32_MIN};
    for (unsigned i = 0u; i < sizeof(volumes) / sizeof(volumes[0]); i++) {
        CHECK_EQ_U32(dsound_movie_stream_set_volume(stream, volumes[i]), 0u);
        dsound_movie_stream_snapshot snapshot;
        CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
        CHECK(snapshot.volume_seen && snapshot.volume == volumes[i]);
    }
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 2u), 0u);
    CHECK_EQ_U32(status_of(stream), 1u);
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 0u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 1u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 4u), 0x80004005u);
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 0xFFFFFFFFu), 0x80004005u);
    dsound_movie_stream_snapshot snapshot;
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK_EQ_U32(snapshot.pause_mode, 1u);  /* the failing modes changed nothing */
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_pause(stream, 3u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 0u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_discontinuity(stream), 0u);
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK(snapshot.discontinuity_seen);
    CHECK_EQ_U32(status_of(stream), 1u);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_get_status(stream, 0u));
    CHECK(fatal_seen);
    /* Foreign and unowned streams. */
    const uint32_t foreign[] = {0u, stream + 4u, 0x1000u, device + 8u};
    for (unsigned i = 0u; i < sizeof(foreign) / sizeof(foreign[0]); i++) {
        RUN_EXPECTING_FATAL((void)dsound_movie_stream_add_ref(foreign[i]));
        CHECK(fatal_seen);
        CHECK(!dsound_movie_stream_owns(foreign[i]));
    }
    CHECK_EQ_U32(load(stream + 8u), 1u);
    /* Tampering with the guest header is caught on the next call. */
    for (unsigned word = 0u; word < 10u; word++) {
        if (word == 2u) continue;
        const uint32_t original = load(stream + 4u * word);
        store(stream + 4u * word, original ^ 1u);
        RUN_EXPECTING_FATAL((void)dsound_movie_stream_add_ref(stream));
        CHECK(fatal_seen);
        store(stream + 4u * word, original);
    }
    store(stream + 8u, 7u);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_add_ref(stream));
    CHECK(fatal_seen);
    store(stream + 8u, 1u);
    CHECK_EQ_U32(dsound_movie_stream_add_ref(stream), 2u);
    CHECK_EQ_U32(dsound_movie_stream_release(stream), 1u);
}

static void test_process_scope(uint32_t stream)
{
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 2u), 0u);
    const uint32_t first = make_packet(0u, 0x1000u), second = make_packet(1u, 0x2000u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, first, 0u), 0u);
    expect_pending(0u);
    CHECK_EQ_U32(status_of(stream), 0x20001u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, second, 0u), 0u);
    expect_pending(1u);
    CHECK_EQ_U32(status_of(stream), 0x20000u);
    const uint32_t third = make_packet(2u, 0x1000u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, third, 0u), 0x88780032u);
    expect_untouched(2u);
    CHECK_EQ_U32(status_of(stream), 0x20000u);
    dsound_movie_stream_snapshot snapshot;
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK_EQ_U32(snapshot.queued, 2u);
    CHECK_EQ_U32(snapshot.packet_size[0], 0x1000u);
    CHECK_EQ_U32(snapshot.packet_size[1], 0x2000u);
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 0u), 0u);
    CHECK_EQ_U32(status_of(stream), 0x10000u);
    /* Reset the queue with an async flush then a work pass, for the refusal cases. */
    CHECK_EQ_U32(dsound_movie_stream_flush_ex(stream, 0u, 0u, 1u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 2u);
    CHECK_EQ_U32(status_of(stream), 1u);
    record_count = 0u;
    /* Outside the measured scope. Each refusal leaves the packet words untouched. */
    struct { const char *name; unsigned field; uint32_t value; } bad[] = {
        {"size 0", 1u, 0u}, {"size 3", 1u, 3u}, {"size 5", 1u, 5u}, {"size huge", 1u, 0x02000000u},
        {"null buffer", 0u, 0u}, {"unreadable buffer", 0u, 0x10u}, {"null completed", 2u, 0u},
        {"null status", 3u, 0u},
        {"completed overlaps status", 2u, status_at(0u) + 3u}, {"status overlaps completed", 3u, completed_at(0u) - 3u},
        {"completed is status", 2u, status_at(0u)},
        {"completed aliases the stream", 2u, stream + 8u}, {"status aliases the device", 3u, device + 4u},
    };
    for (unsigned i = 0u; i < sizeof(bad) / sizeof(bad[0]); i++) {
        const uint32_t packet = make_packet(0u, 0x1000u);
        store(packet + 4u * bad[i].field, bad[i].value);
        RUN_EXPECTING_FATAL((void)dsound_movie_stream_process(stream, packet, 0u));
        CHECK(fatal_seen);
        if (bad[i].field != 2u && bad[i].field != 3u) expect_untouched(0u);
        CHECK_EQ_U32(status_of(stream), 1u);
    }
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_process(stream, make_packet(0u, 0x1000u), 0x1234u));
    CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_process(stream, 0x10u, 0u));
    CHECK(fatal_seen);
    /* A completion word shared with an attached packet is refused. */
    const uint32_t keep = make_packet(0u, 0x1000u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, keep, 0u), 0u);
    const uint32_t clash = make_packet(1u, 0x1000u);
    store(clash + 8u, completed_at(0u));
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_process(stream, clash, 0u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(status_of(stream), 0x10001u);
    CHECK_EQ_U32(dsound_movie_stream_flush_ex(stream, 0u, 0u, 1u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 1u);
    record_count = 0u;
}

/* A packet whose completion words are at arbitrary byte addresses. */
static uint32_t make_packet_words(unsigned index, uint32_t size, uint32_t completed, uint32_t status)
{
    store(completed, 0xDEADBEEFu);
    store(status, 0xCAFEBABEu);
    watched_completed[index] = completed;
    watched_status[index] = status;
    const uint32_t words[6] = {buffer_base + 0x1000u * index, size, completed, status, index, 0u};
    CHECK(kernel_guest_write_bytes(packet_at(index), words, sizeof(words)));
    return packet_at(index);
}

/* T394: the retained XMV keeps its completion words at odd addresses inside its decoder object. The
 * original stores dwords through them without aligning (oracle, offsets 1 to 3), so the model admits
 * them, writes exactly four bytes each and refuses only a pair that overlaps. */
static void test_unaligned_completion_words(uint32_t stream)
{
    record_count = 0u;
    fake_now = 2000u * FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 0u), 0u);
    for (uint32_t skew = 1u; skew <= 3u; skew++) {
        const uint32_t completed = SCRATCH_DATA + 0xA00u + skew, status = SCRATCH_DATA + 0xA10u + skew;
        CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet_words(3u, 0x1000u, completed, status), 0u), 0u);
        /* Exactly the four bytes at each address are written, the byte before and after are not. */
        store(completed + 4u, 0x5A5A5A5Au);
        CHECK_EQ_U32(load(completed), 0u);
        CHECK_EQ_U32(load(status), PENDING);
        fake_now += ticks_for(0x1000u);
        CHECK_EQ_U32(dsound_movie_stream_do_work(), 1u);
        CHECK_EQ_U32(load(completed), 0x1000u);
        CHECK_EQ_U32(load(status), 0u);
        CHECK_EQ_U32(load(completed + 4u), 0x5A5A5A5Au);
        CHECK_EQ_U32(record_count, 1u);
        CHECK_EQ_U32(records[0].completed_seen, 0x1000u);
        CHECK_EQ_U32(records[0].status_seen, 0u);
        record_count = 0u;
    }
    /* A pair that overlaps by even one byte is refused, a pair exactly four bytes apart is not. */
    const uint32_t base = SCRATCH_DATA + 0xB01u;
    const uint32_t overlaps[] = {base, base + 1u, base + 2u, base + 3u};
    for (unsigned i = 0u; i < 4u; i++) {
        RUN_EXPECTING_FATAL((void)dsound_movie_stream_process(stream, make_packet_words(4u, 0x1000u, base, overlaps[i]), 0u));
        CHECK(fatal_seen);
    }
    CHECK_EQ_U32(status_of(stream), 1u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet_words(4u, 0x1000u, base, base + 4u), 0u), 0u);
    /* An attached packet's words are protected the same way, by overlap and not only by equality. */
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_process(stream, make_packet_words(5u, 0x1000u, base + 6u, base + 0x20u), 0u));
    CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_process(stream, make_packet_words(5u, 0x1000u, base + 0x20u, base + 7u), 0u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet_words(5u, 0x1000u, base + 8u, base + 0x20u), 0u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_flush_ex(stream, 0u, 0u, 1u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 2u);
    record_count = 0u;
}

/* The stated completion rule, against hand-computed deadlines. */
static void test_completion_rule(uint32_t stream)
{
    record_count = 0u;
    const uint32_t one_second = AVERAGE, half_second = AVERAGE / 2u;
    fake_now = 5000u;
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 2u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(0u, one_second), 0u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(1u, half_second), 0u), 0u);
    /* Paused until SynchPlayback: no amount of time or work completes anything. */
    fake_now = 5000u + 100u * FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 0u);
    expect_pending(0u);
    expect_pending(1u);
    const uint64_t start = fake_now;
    dsound_movie_stream_synch_playback();
    dsound_movie_stream_snapshot snapshot;
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK(snapshot.pause_mode == 0u && snapshot.head_deadline == start + ticks_for(one_second));
    CHECK_EQ_U32((uint32_t)ticks_for(one_second), (uint32_t)FREQUENCY);  /* one second of audio is one second of ticks */
    /* One tick early, nothing. On the deadline, the first packet only. */
    fake_now = start + FREQUENCY - 1u;
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 0u);
    expect_pending(0u);
    fake_now = start + FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 1u);
    expect_done(0u, one_second, 0u);
    expect_pending(1u);
    CHECK_EQ_U32(record_count, 1u);
    CHECK_EQ_U32(records[0].callback, 0x445071u);
    CHECK_EQ_U32(records[0].stream_context, DECODER);
    CHECK_EQ_U32(records[0].packet_context, 0u);
    CHECK_EQ_U32(records[0].status, 0u);
    CHECK_EQ_U32(records[0].completed_seen, one_second);  /* written before the callback ran */
    CHECK_EQ_U32(records[0].status_seen, 0u);
    CHECK_EQ_U32(status_of(stream), 0x10001u);
    /* The second packet plays back to back: its deadline is the first deadline plus half a second,
     * whatever the work cadence was. */
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK(snapshot.head_deadline == start + FREQUENCY + ticks_for(half_second));
    fake_now = snapshot.head_deadline - 1u;
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 0u);
    fake_now = snapshot.head_deadline;
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 1u);
    expect_done(1u, half_second, 0u);
    CHECK_EQ_U32(records[1].packet_context, 1u);
    CHECK_EQ_U32(status_of(stream), 1u);
    /* Late work delivers every due packet in one pass, in order. */
    record_count = 0u;
    fake_now = 10u * FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(2u, half_second), 0u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(3u, half_second), 0u), 0u);
    const uint64_t attach = fake_now;
    fake_now = attach + 50u * FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 2u);
    CHECK_EQ_U32(record_count, 2u);
    CHECK_EQ_U32(records[0].packet_context, 2u);
    CHECK_EQ_U32(records[1].packet_context, 3u);
    expect_done(2u, half_second, 0u);
    expect_done(3u, half_second, 0u);
    /* An idle stream restarts the clock at the attach tick: no credit for the gap. */
    fake_now = attach + 60u * FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(4u, 4u), 0u), 0u);
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK(snapshot.head_deadline == fake_now + ticks_for(4u));
    CHECK_EQ_U32((uint32_t)ticks_for(4u), 16629u);  /* ceil(4 * 733333333 / 176400) */
    fake_now = snapshot.head_deadline;
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 1u);
    expect_done(4u, 4u, 0u);
    record_count = 0u;
}

static void test_pause_freezes_the_remainder(uint32_t stream)
{
    record_count = 0u;
    fake_now = 100u * FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(5u, AVERAGE), 0u), 0u);
    const uint64_t attach = fake_now;
    fake_now = attach + FREQUENCY / 4u;  /* a quarter second played */
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 1u), 0u);
    dsound_movie_stream_snapshot snapshot;
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK_EQ_U32(snapshot.pause_mode, 1u);
    CHECK_EQ_U32(status_of(stream), 0x20001u);
    fake_now = attach + 500u * FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 0u);
    expect_pending(5u);
    /* SynchPlayback does not touch a plain Pause(1). */
    dsound_movie_stream_synch_playback();
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK_EQ_U32(snapshot.pause_mode, 1u);
    CHECK(snapshot.head_deadline == UINT64_MAX);
    CHECK_EQ_U32(status_of(stream), 0x20001u);
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 0u);
    const uint64_t resume = fake_now;
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 0u), 0u);
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK(snapshot.head_deadline == resume + (FREQUENCY - FREQUENCY / 4u));
    fake_now = snapshot.head_deadline;
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 1u);
    expect_done(5u, AVERAGE, 0u);
    record_count = 0u;
}

static void test_flush(uint32_t stream)
{
    record_count = 0u;
    fake_now = 1000u * FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_flush_ex(stream, 0u, 0u, 1u), 0u);  /* empty: nothing changes */
    CHECK_EQ_U32(status_of(stream), 1u);
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 0u);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_flush_ex(stream, 0u, 0u, 0u));
    CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_flush_ex(stream, 1u, 0u, 1u));
    CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_flush_ex(stream, 0u, 1u, 1u));
    CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_flush_ex(stream, 0u, 0u, 3u));
    CHECK(fatal_seen);
    /* A sync-paused stream with two packets: an async flush resumes it and keeps the packets
     * pending until the next DoWork, which aborts both, FIFO, with the full size. */
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 2u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(6u, 0x1000u), 0u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(7u, 0x2000u), 0u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_flush_ex(stream, 0u, 0u, 1u), 0u);
    CHECK_EQ_U32(status_of(stream), 0x10000u);
    expect_pending(6u);
    expect_pending(7u);
    CHECK_EQ_U32(record_count, 0u);
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 2u);
    expect_done(6u, 0x1000u, ABORT);
    expect_done(7u, 0x2000u, ABORT);
    CHECK_EQ_U32(record_count, 2u);
    CHECK_EQ_U32(records[0].packet_context, 6u);
    CHECK_EQ_U32(records[0].status, ABORT);
    CHECK_EQ_U32(records[1].packet_context, 7u);
    CHECK_EQ_U32(records[1].status, ABORT);
    CHECK_EQ_U32(status_of(stream), 1u);
    dsound_movie_stream_snapshot snapshot;
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK(!snapshot.flush_pending);
    /* A packet that finished playing before the flush completes normally, the rest abort. */
    record_count = 0u;
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 0u), 0u);
    fake_now += FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(8u, AVERAGE / 4u), 0u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(9u, AVERAGE), 0u), 0u);
    fake_now += FREQUENCY / 2u;  /* the first quarter second is over, the second packet is half played */
    CHECK_EQ_U32(dsound_movie_stream_flush_ex(stream, 0u, 0u, 1u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 2u);
    expect_done(8u, AVERAGE / 4u, 0u);
    expect_done(9u, AVERAGE, ABORT);
    CHECK_EQ_U32(records[0].status, 0u);
    CHECK_EQ_U32(records[1].status, ABORT);
    record_count = 0u;
}

/* T394: IDirectSoundStream::Flush (0x407388) is the synchronous abort, measured against the original
 * (tests/test_dsound_movie_stream_oracle.py). Everything pending completes at once, FIFO, with the full
 * size and 0x80004004, a paused stream is left running and an empty stream changes nothing else. */
static void test_sync_flush(uint32_t stream)
{
    record_count = 0u;
    fake_now = 3000u * FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 2u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_flush(stream), 0u);  /* empty and paused: it resumes */
    dsound_movie_stream_snapshot snapshot;
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK_EQ_U32(snapshot.pause_mode, 0u);
    CHECK_EQ_U32(record_count, 0u);
    CHECK_EQ_U32(status_of(stream), 1u);
    /* Two packets, the first already played out on the virtual clock but never observed by a DoWork. */
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(6u, AVERAGE / 4u), 0u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(7u, 0x2000u), 0u), 0u);
    fake_now += FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_flush(stream), 0u);
    expect_done(6u, AVERAGE / 4u, ABORT);  /* the model aborts what no DoWork observed, as the final Release does */
    expect_done(7u, 0x2000u, ABORT);
    CHECK_EQ_U32(record_count, 2u);
    CHECK_EQ_U32(records[0].packet_context, 6u);
    CHECK_EQ_U32(records[0].status, ABORT);
    CHECK_EQ_U32(records[0].completed_seen, AVERAGE / 4u);  /* words written before the callback runs */
    CHECK_EQ_U32(records[0].status_seen, ABORT);
    CHECK_EQ_U32(records[1].packet_context, 7u);
    CHECK_EQ_U32(status_of(stream), 1u);
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK_EQ_U32(snapshot.queued, 0u);
    CHECK(!snapshot.flush_pending);
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 0u);  /* nothing is left for a later DoWork */
    /* A paused stream with a pending packet is aborted and left running. */
    record_count = 0u;
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 2u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(8u, 0x1000u), 0u), 0u);
    CHECK_EQ_U32(status_of(stream), 0x20001u);
    CHECK_EQ_U32(dsound_movie_stream_flush(stream), 0u);
    expect_done(8u, 0x1000u, ABORT);
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK_EQ_U32(snapshot.pause_mode, 0u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(9u, 0x1000u), 0u), 0u);
    CHECK_EQ_U32(status_of(stream), 0x10001u);  /* playing, not paused */
    CHECK_EQ_U32(dsound_movie_stream_flush(stream), 0u);
    record_count = 0u;
    /* Scope: not an owned stream, and the original answers 0x80004005 with the global audio state set,
     * which the model has no measured use for and refuses like every other method. */
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_flush(stream + 4u));
    CHECK(fatal_seen);
    store(0x4124A8u, 1u);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_flush(stream));
    CHECK(fatal_seen);
    store(0x4124A8u, 0u);
    /* The raw stack route: [return, this], one argument, popped by the thunk. */
    uint32_t sp = SCRATCH_DATA + 0x800u, result = 0xFFFFFFFFu, pop = 0u;
    store(sp, TITLE_CALLER);
    store(sp + 4u, stream);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(10u, 0x1000u), 0u), 0u);
    CHECK(dsound_movie_stream_route_method(0x407388u, sp, &result, &pop));
    CHECK_EQ_U32(result, 0u);
    CHECK_EQ_U32(pop, 4u);
    expect_done(10u, 0x1000u, ABORT);
    store(sp, 0x29B72u);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_route_method(0x407388u, sp, &result, &pop));
    CHECK(fatal_seen);
    store(sp, TITLE_CALLER);
    store(sp + 4u, 0x41000000u);
    CHECK(!dsound_movie_stream_route_method(0x407388u, sp, &result, &pop));
    record_count = 0u;
}

static void test_work_requires_a_runner(uint32_t stream)
{
    fake_now = 2000u * FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(10u, 4u), 0u), 0u);
    fake_now += FREQUENCY;
    runner_ok = false;
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_do_work());
    CHECK(fatal_seen);
    runner_ok = true;
    CHECK_EQ_U32(status_of(stream), 1u);  /* the packet left the queue before its callback failed */
    dsound_movie_stream_set_callback_runner(NULL);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(11u, 4u), 0u), 0u);
    fake_now += FREQUENCY;
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_do_work());
    CHECK(fatal_seen);
    dsound_movie_stream_set_callback_runner(record_callback);
    record_count = 0u;
    dsound_movie_stream_set_enabled(false);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_do_work());
    CHECK(fatal_seen);
    dsound_movie_stream_set_enabled(true);
}

static void frame_with(kernel_call_frame *frame, uint32_t caller, const uint32_t *args, unsigned count)
{
    CHECK(kernel_frame_build(frame, call_scratch, 0x100u, args, count));
    store(frame->stack_ptr, caller);
}
static void test_routes(uint32_t stream)
{
    kernel_call_frame frame = {0};
    uint32_t result = 0xFFFFFFFFu;
    /* Create is only the one measured EnableAudioStream site, anything else is the startup policy's. */
    measured_descriptor();
    const uint32_t create_args[2] = {descriptor, SCRATCH_DATA + 0x210u};
    store(SCRATCH_DATA + 0x210u, 0xAAAAAAAAu);
    frame_with(&frame, STARTUP_CALLER, create_args, 2u);
    CHECK(!dsound_movie_stream_route_public(0x40967Cu, &frame, STARTUP_CALLER, &result));
    CHECK_EQ_U32(load(SCRATCH_DATA + 0x210u), 0xAAAAAAAAu);
    frame_with(&frame, CREATE_CALLER, create_args, 2u);
    CHECK(dsound_movie_stream_route_public(0x40967Cu, &frame, CREATE_CALLER, &result));
    CHECK_EQ_U32(result, 0u);
    const uint32_t second = load(SCRATCH_DATA + 0x210u);
    CHECK(dsound_movie_stream_owns(second) && second != stream);
    dsound_movie_stream_set_enabled(false);
    CHECK(!dsound_movie_stream_route_public(0x40967Cu, &frame, CREATE_CALLER, &result));
    dsound_movie_stream_set_enabled(true);
    /* Pause, FlushEx and SetVolume: owned movie streams from a movie caller only. */
    const uint32_t pause_args[2] = {stream, 2u};
    frame_with(&frame, XMV_CALLER, pause_args, 2u);
    CHECK(dsound_movie_stream_route_public(0x407B23u, &frame, XMV_CALLER, &result));
    CHECK_EQ_U32(result, 0u);
    const uint32_t volume_args[2] = {stream, (uint32_t)-600};
    frame_with(&frame, TITLE_CALLER, volume_args, 2u);
    CHECK(dsound_movie_stream_route_public(0x407B14u, &frame, TITLE_CALLER, &result));
    dsound_movie_stream_snapshot snapshot;
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK(snapshot.volume == -600 && snapshot.pause_mode == 2u);
    const uint32_t flush_args[4] = {stream, 0u, 0u, 1u};
    frame_with(&frame, XMV_CALLER, flush_args, 4u);
    CHECK(dsound_movie_stream_route_public(0x407B28u, &frame, XMV_CALLER, &result));
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 0u), 0u);
    frame_with(&frame, 0x29B5Au, pause_args, 2u);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_route_public(0x407B23u, &frame, 0x29B5Au, &result));
    CHECK(fatal_seen);
    const uint32_t foreign_args[2] = {0x41000000u, 1u};
    frame_with(&frame, 0x29B5Au, foreign_args, 2u);
    CHECK(!dsound_movie_stream_route_public(0x407B23u, &frame, 0x29B5Au, &result));
    CHECK(!dsound_movie_stream_route_public(0x407B23u, NULL, 0x29B5Au, &result));
    CHECK(!dsound_movie_stream_route_public(0x407B19u, &frame, XMV_CALLER, &result));
    /* The indirect methods through a raw stack: [return, this, ...]. */
    uint32_t sp = SCRATCH_DATA + 0x800u, pop = 0u;
    store(sp, XMV_CALLER);
    store(sp + 4u, stream);
    CHECK(dsound_movie_stream_method_owned(sp));
    CHECK(dsound_movie_stream_route_method(0x40723Fu, sp, &result, &pop));
    CHECK_EQ_U32(result, 2u);
    CHECK_EQ_U32(pop, 4u);
    CHECK(dsound_movie_stream_route_method(0x407286u, sp, &result, &pop));
    CHECK_EQ_U32(result, 1u);
    store(sp + 8u, SCRATCH_DATA + 0x720u);
    CHECK(dsound_movie_stream_route_method(0x4073D3u, sp, &result, &pop));
    CHECK_EQ_U32(result, 0u);
    CHECK_EQ_U32(pop, 8u);
    CHECK_EQ_U32(load(SCRATCH_DATA + 0x720u), 1u);
    CHECK(dsound_movie_stream_route_method(0x40733Bu, sp, &result, &pop));
    CHECK_EQ_U32(pop, 4u);
    store(sp + 8u, make_packet(12u, 4u));
    store(sp + 12u, 0u);
    CHECK(dsound_movie_stream_route_method(0x407424u, sp, &result, &pop));
    CHECK_EQ_U32(result, 0u);
    CHECK_EQ_U32(pop, 12u);
    expect_pending(12u);
    CHECK(!dsound_movie_stream_route_method(0x4072D4u, sp, &result, &pop));  /* GetInfo is not a movie method */
    store(sp, 0x29B72u);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_route_method(0x40723Fu, sp, &result, &pop));
    CHECK(fatal_seen);
    store(sp, XMV_CALLER);
    store(sp + 4u, 0x41000000u);
    CHECK(!dsound_movie_stream_method_owned(sp));
    CHECK(!dsound_movie_stream_route_method(0x40723Fu, sp, &result, &pop));
    store(sp + 4u, stream);
    /* DoWork: only from a movie caller and only while a movie stream exists. */
    CHECK(dsound_movie_stream_route_work(XMV_CALLER));
    CHECK(!dsound_movie_stream_route_work(0x1CE492u));
    CHECK(!dsound_movie_stream_route_work(0u));
    /* Release the second stream, the first stays with packet 12 attached. */
    store(sp + 4u, second);
    CHECK(dsound_movie_stream_route_method(0x407286u, sp, &result, &pop));
    CHECK_EQ_U32(result, 0u);
    CHECK(!dsound_movie_stream_owns(second));
    CHECK_EQ_U32(dsound_movie_stream_count(), 1u);
    record_count = 0u;
    fake_now += FREQUENCY;
    CHECK(dsound_movie_stream_route_work(XMV_CALLER));
    expect_done(12u, 4u, 0u);
    record_count = 0u;
}

static void test_final_release_aborts_pending(uint32_t stream)
{
    record_count = 0u;
    const uint32_t refs = load(device + 4u);
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 2u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(13u, 0x1000u), 0u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, make_packet(14u, 0x2000u), 0u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_add_ref(stream), 2u);
    CHECK_EQ_U32(dsound_movie_stream_release(stream), 1u);
    expect_pending(13u);  /* not the last reference */
    CHECK_EQ_U32(dsound_movie_stream_release(stream), 0u);
    expect_done(13u, 0x1000u, ABORT);
    expect_done(14u, 0x2000u, ABORT);
    CHECK_EQ_U32(record_count, 2u);
    CHECK_EQ_U32(records[0].packet_context, 13u);
    CHECK_EQ_U32(records[1].packet_context, 14u);
    CHECK(!dsound_movie_stream_owns(stream));
    CHECK_EQ_U32(dsound_movie_stream_count(), 0u);
    CHECK_EQ_U32(load(device + 4u), refs - 1u);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_add_ref(stream));
    CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_release(stream));
    CHECK(fatal_seen);
    record_count = 0u;
}

/* T-movie: the movie PCM reaches the audio runtime mixer, on the virtual clock, honouring pause and flush. */
static uint32_t pcm_packet(unsigned index, uint32_t frames, int16_t left, int16_t right)
{
    static int16_t samples[8820u * 2u];
    for (uint32_t i = 0u; i < frames; i++) { samples[2u * i] = left; samples[2u * i + 1u] = right; }
    const uint32_t buffer = BUFFER_BASE + 0x20000u * index;
    CHECK(kernel_guest_write_bytes(buffer, samples, frames * 4u));
    store(completed_at(index), 0u);
    store(status_at(index), 0u);
    const uint32_t words[6] = {buffer, frames * 4u, completed_at(index), status_at(index), index, 0u};
    CHECK(kernel_guest_write_bytes(packet_at(index), words, sizeof(words)));
    return packet_at(index);
}
static size_t render_second(int16_t *out, uint64_t start_tick, uint64_t until_tick)
{
    (void)start_tick;
    size_t written = 0u;
    CHECK(dsound_audio_runtime_render(until_tick, out, 96000u, &written));
    return written;
}
static void skip_to(uint64_t tick)
{
    static int16_t scratch[4096u * 2u];
    size_t written;
    do { CHECK(dsound_audio_runtime_render(tick, scratch, 4096u, &written)); } while (written == 4096u);
}
static void test_pcm_reaches_mixer(uint32_t stream)
{
    static int16_t out[96000u * 2u];
    CHECK(dsound_audio_runtime_start(48000u, FREQUENCY));
    const uint64_t pcm_before = dsound_movie_stream_pcm_frames();
    fake_now = 1000000000u;
    const uint64_t t0 = fake_now;
    CHECK_EQ_U32(dsound_movie_stream_process(stream, pcm_packet(0u, 4410u, 1000, -2000), 0u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, pcm_packet(1u, 2205u, 3000, 4000), 0u), 0u);
    CHECK(dsound_movie_stream_pcm_frames() - pcm_before == 6615u);
    /* 0.1 s of the first packet, then 0.05 s of the second back to back, then silence. Samples are taken 100
     * frames from the join: the band limited resampler (T818) rings for about 26 output frames around a step. */
    const size_t written = render_second(out, t0, t0 + FREQUENCY);
    CHECK(written == 48000u);
    CHECK(out[10u * 2u] == 1000 && out[10u * 2u + 1u] == -2000);
    CHECK(out[4700u * 2u] == 1000);
    CHECK(out[4900u * 2u] == 3000 && out[4900u * 2u + 1u] == 4000);
    CHECK(out[7100u * 2u] == 3000);
    CHECK(out[7210u * 2u] == 0 && out[40000u * 2u] == 0);
    CHECK_EQ_U32(dsound_movie_stream_pcm_refused(), 0u);

    /* Pause(2) holds the audio until SynchPlayback starts it. */
    fake_now = t0 + 2u * FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 2u);
    skip_to(fake_now);
    CHECK_EQ_U32(dsound_movie_stream_pause(stream, 2u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, pcm_packet(0u, 4410u, 500, 600), 0u), 0u);
    fake_now += FREQUENCY / 10u;
    CHECK(render_second(out, 0u, fake_now) > 0u);
    for (unsigned i = 0u; i < 4800u; i++) CHECK(out[2u * i] == 0 && out[2u * i + 1u] == 0);
    dsound_movie_stream_synch_playback();
    fake_now += FREQUENCY / 10u;
    const size_t resumed = render_second(out, 0u, fake_now);
    CHECK(resumed > 4000u && out[100u * 2u] == 500 && out[100u * 2u + 1u] == 600);

    /* A flush aborts the unplayed audio. */
    fake_now += 2u * FREQUENCY;
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 1u);
    skip_to(fake_now);
    CHECK_EQ_U32(dsound_movie_stream_process(stream, pcm_packet(0u, 4410u, 700, 800), 0u), 0u);
    CHECK_EQ_U32(dsound_movie_stream_flush_ex(stream, 0u, 0u, 1u), 0u);
    fake_now += FREQUENCY / 5u;
    (void)render_second(out, 0u, fake_now);
    for (unsigned i = 0u; i < 4800u; i++) CHECK(out[2u * i] == 0);
    CHECK_EQ_U32(dsound_movie_stream_do_work(), 1u);

    /* T818: SetVolume (hundredths of a dB) scales the soundtrack, 0 is bit identical, -10000 mutes. */
    const int32_t volumes[] = {-2000, -10000, 0};
    const int16_t expected[] = {100, 0, 1000};
    for (unsigned i = 0u; i < 3u; i++) {
        fake_now += 2u * FREQUENCY;
        (void)dsound_movie_stream_do_work();
        skip_to(fake_now);
        fake_now += FREQUENCY / 1000u;  /* the packet must start after the frame already rendered */
        CHECK_EQ_U32(dsound_movie_stream_set_volume(stream, volumes[i]), 0u);
        CHECK_EQ_U32(dsound_movie_stream_process(stream, pcm_packet(0u, 4410u, 1000, -1000), 0u), 0u);
        fake_now += FREQUENCY / 20u;
        CHECK(render_second(out, 0u, fake_now) > 2000u);
        CHECK_EQ_U32(dsound_movie_stream_pcm_refused(), 0u);
        CHECK(out[100u * 2u] == expected[i] && out[100u * 2u + 1u] == -expected[i]);
    }
    /* Outside -10000..0 has no measured movie gain: refused by name with the runtime on. */
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_set_volume(stream, 1));
    CHECK(fatal_seen);
    RUN_EXPECTING_FATAL((void)dsound_movie_stream_set_volume(stream, -10001));
    CHECK(fatal_seen);
    dsound_audio_runtime_stop();
}

/* Final Release cuts queued movie audio before resetting its persistent mixer key to unity. Reuse
 * that key directly in the mixer because this guest heap fixture does not recycle movie addresses. */
static void test_release_cuts_pending_movie_audio_before_gain_reset(void)
{
    static int16_t out[96000u * 2u];
    static int16_t unrelated_pcm[9600u * 2u];
    static int16_t replacement_pcm[480u * 2u];
    for (unsigned i = 0u; i < 9600u; i++)
        unrelated_pcm[2u * i] = unrelated_pcm[2u * i + 1u] = 1000;
    for (unsigned i = 0u; i < 480u; i++)
        replacement_pcm[2u * i] = replacement_pcm[2u * i + 1u] = 2000;

    CHECK(dsound_audio_runtime_start(48000u, FREQUENCY));
    fake_now = FREQUENCY * 10u;
    const uint64_t start = fake_now;
    const uint32_t stream_a = create_stream();
    CHECK_EQ_U32(dsound_movie_stream_set_volume(stream_a, -10000), 0u);
    const uint32_t unrelated_key = 0x434F4E54u;
    CHECK(dsound_audio_runtime_set_stream_volume(unrelated_key, -2000));
    CHECK(dsound_audio_runtime_submit_pcm16_stereo(unrelated_key, start, 48000u,
                                                    unrelated_pcm, 9600u));
    CHECK_EQ_U32(dsound_movie_stream_process(stream_a, pcm_packet(0u, 1000u, 500, 600), 0u), 0u);

    const uint64_t release_tick = start + FREQUENCY / 100u;
    CHECK(render_second(out, start, release_tick) == 480u);
    CHECK(out[100u * 2u] == 100 && out[100u * 2u + 1u] == 100);
    fake_now = release_tick;
    CHECK_EQ_U32(dsound_movie_stream_release(stream_a), 0u);
    expect_done(0u, 1000u * 4u, ABORT);
    CHECK(dsound_audio_runtime_submit_pcm16_stereo(stream_a, release_tick, 48000u,
                                                    replacement_pcm, 480u));
    const uint64_t end = release_tick + FREQUENCY / 100u;
    CHECK(render_second(out, release_tick, end) == 480u);
    /* The replaced key is at unity while the unrelated stream remains at -2000 millibels (0.1).
     * A surviving old tail would add 500 to this sum after the key's gain was reset. */
    CHECK(out[100u * 2u] == 2100 && out[100u * 2u + 1u] == 2100);
    fake_now = end;
    dsound_audio_runtime_stop();
}

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    dsound_hle_init();
    dsound_device_set_fatal(catching_fatal);
    dsound_stream_set_fatal(catching_fatal);
    dsound_stream_set_irql_provider(current_irql);
    dsound_stream_set_enabled(true);
    dsound_movie_stream_set_fatal(catching_fatal);
    dsound_movie_stream_set_irql_provider(current_irql);
    dsound_movie_stream_set_clock(clock_now, FREQUENCY);
    dsound_movie_stream_set_callback_runner(record_callback);
    dsound_movie_stream_set_enabled(true);
    dsound_stream_set_extension(dsound_movie_stream_route_public, dsound_movie_stream_reset_checked);
    map_fixed(0x412000u, 4096u);
    map_fixed(0x4A1000u, 4096u);
    for (unsigned i = 0u; i < 15u; i++) store(0x4A1CF0u + 4u * i, 0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_create(0u, SCRATCH_DATA, 0u), 0u);
    device = load(SCRATCH_DATA) - 8u;
    descriptor = SCRATCH_DATA + 256u;
    format = SCRATCH_DATA + 320u;
    output_slot = SCRATCH_DATA + 128u;
    map_fixed(BUFFER_BASE, 0x100000u);
    test_create_and_scope();
    const uint32_t stream = load(output_slot);
    test_references_volume_pause_status(stream);
    test_process_scope(stream);
    test_unaligned_completion_words(stream);
    test_completion_rule(stream);
    test_pause_freezes_the_remainder(stream);
    test_flush(stream);
    test_sync_flush(stream);
    test_work_requires_a_runner(stream);
    test_routes(stream);
    test_final_release_aborts_pending(stream);
    CHECK_EQ_U32(load(device + 4u), 6u);
    /* With no movie stream DoWork is the old owner's call, the movie route does not take it. */
    CHECK(!dsound_movie_stream_route_work(XMV_CALLER));
    const uint32_t again = create_stream();
    CHECK(again != 0u && dsound_movie_stream_owns(again));
    CHECK_EQ_U32(load(device + 4u), 7u);
    test_pcm_reaches_mixer(again);
    CHECK(dsound_movie_stream_reset_checked());
    CHECK_EQ_U32(dsound_movie_stream_count(), 0u);
    CHECK_EQ_U32(load(device + 4u), 6u);
    CHECK(!dsound_movie_stream_owns(again));
    test_release_cuts_pending_movie_audio_before_gain_reset();
    CHECK_EQ_U32(dsound_movie_stream_count(), 0u);
    /* The startup stream reset, which the host calls at shutdown, also frees the movie streams. */
    const uint32_t third = create_stream();
    CHECK(third != 0u && dsound_movie_stream_owns(third));
    CHECK_EQ_U32(load(device + 4u), 7u);
    CHECK(dsound_stream_reset_checked());
    CHECK_EQ_U32(dsound_movie_stream_count(), 0u);
    CHECK_EQ_U32(load(device + 4u), 6u);
    CHECK(!dsound_movie_stream_owns(third));
    /* A tampered movie stream makes the checked reset refuse and keep the allocation. */
    const uint32_t fourth = create_stream();
    store(fourth + 4u, 0x1234u);
    CHECK(!dsound_stream_reset_checked());  /* the startup reset reports the movie refusal */
    CHECK(!dsound_movie_stream_reset_checked());
    CHECK(dsound_movie_stream_owns(fourth));
    CHECK_EQ_U32(load(fourth + 4u), 0x1234u);
    store(fourth + 4u, 0x4A1CF0u);
    CHECK(dsound_movie_stream_reset_checked());
    CHECK(dsound_device_reset_checked());
    environment_end();
    printf("movie stream: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
