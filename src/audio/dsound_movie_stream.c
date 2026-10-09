/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_movie_stream.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsound_audio_runtime.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_mixer.h"
#include "dsound_stream.h"
#include "guest_mem.h"
#include "kernel_call.h"
#include "kernel_clock.h"
#define CREATE 0x0040967Cu
#define ADD_REF 0x0040723Fu
#define RELEASE 0x00407286u
#define GET_STATUS 0x004073D3u
#define PROCESS 0x00407424u
#define DISCONTINUITY 0x0040733Bu
#define PAUSE 0x00407B23u
#define FLUSH_EX 0x00407B28u
#define FLUSH 0x00407388u
#define VOLUME 0x00407B14u
#define WORK 0x00407B40u
/* T421: IDirectSound::SynchPlayback, called once from the XMV GetNextFrame (call at 0x445716). */
#define SYNCH 0x00407A4Cu
#define SYNCH_RETURN 0x0044571Bu
#define SINGLETON 0x00412B30u
#define GLOBAL_STATE 0x004124A8u
#define VTABLE 0x004A1D00u
#define SECONDARY 0x004A1CF0u
/* The retained XMV library section and the title function that owns the movie loop. */
#define XMV_LOW 0x00444920u
#define XMV_HIGH 0x0044CE49u
#define TITLE_LOW 0x00030280u
#define TITLE_HIGH 0x00030676u
/* The return address after the single DirectSoundCreateStream call in EnableAudioStream. */
#define CREATE_RETURN 0x004451F4u
#define OOM 0x8007000Eu
#define E_FAIL_STATUS 0x80004005u
/* The only format the retained EnableAudioStream builds for the measured logo movie. */
#define FORMAT_AVERAGE 176400u
#define FORMAT_RATE 44100u
#define FORMAT_BLOCK 4u
#define MAX_PACKET_BYTES 0x01000000u
#define NEVER UINT64_MAX

typedef struct packet {
    uint32_t buffer, size, completed, status, context;
    uint64_t ticks, deadline, remaining;
} packet;
typedef struct movie_node {
    uint32_t stream_address, stream_heap, publication_address, header[10];
    uint32_t callback, context, pause_mode, queued;
    int32_t volume;
    bool volume_seen, discontinuity_seen, flush_pending, paused;
    uint64_t flush_tick;
    packet queue[DSOUND_MOVIE_MAX_PACKETS];
    dsound_device_lease lease;
    struct movie_node *next;
} movie_node;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t reset_lock = PTHREAD_MUTEX_INITIALIZER;
static bool enabled, announced;
static dsound_movie_stream_irql_provider irql_provider;
static dsound_movie_stream_fatal_fn fatal_handler;
static dsound_movie_stream_clock_fn clock_fn;
static uint64_t clock_frequency = KERNEL_CLOCK_FREQUENCY_HZ;
static dsound_movie_stream_callback_fn callback_runner;
static movie_node *nodes, *rollback;
static uint64_t completions, operations;

static void reset_mixer_volume(uint32_t stream_address)
{
    if (dsound_audio_runtime_active())
        (void)dsound_audio_runtime_reset_stream_volume(stream_address);
}

static uint64_t now_ticks(void)
{ return clock_fn != NULL ? clock_fn() : kernel_clock_peek(); }
/* T394 trace (--trace-xmv): one stderr line per model event, with the virtual tick it happened at. */
static bool trace_enabled;
void dsound_movie_stream_set_trace(bool on) { trace_enabled = on; }
#define TRACE_EVENT(...) do { if (trace_enabled) { fprintf(stderr, "movie-stream: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
static bool overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{ return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an; }
static void refuse(uint32_t entry, const char *reason) __attribute__((noreturn));
static void refuse(uint32_t entry, const char *reason)
{
    pthread_mutex_lock(&lock);
    dsound_movie_stream_fatal_fn fatal = fatal_handler;
    pthread_mutex_unlock(&lock);
    dsound_hle_log()("dsound movie stream %#x refused: %s\n", entry, reason);
    if (fatal != NULL) fatal(entry, reason);
    abort();
}
void dsound_movie_stream_set_enabled(bool value)
{ pthread_mutex_lock(&lock); enabled = value; pthread_mutex_unlock(&lock); }
bool dsound_movie_stream_enabled(void)
{ pthread_mutex_lock(&lock); const bool value = enabled; pthread_mutex_unlock(&lock); return value; }
void dsound_movie_stream_set_irql_provider(dsound_movie_stream_irql_provider provider)
{ pthread_mutex_lock(&lock); irql_provider = provider; pthread_mutex_unlock(&lock); }
void dsound_movie_stream_set_fatal(dsound_movie_stream_fatal_fn fatal)
{ pthread_mutex_lock(&lock); fatal_handler = fatal; pthread_mutex_unlock(&lock); }
void dsound_movie_stream_set_clock(dsound_movie_stream_clock_fn clock, uint64_t frequency)
{
    pthread_mutex_lock(&lock);
    clock_fn = clock;
    clock_frequency = clock != NULL ? frequency : KERNEL_CLOCK_FREQUENCY_HZ;
    pthread_mutex_unlock(&lock);
}
void dsound_movie_stream_set_callback_runner(dsound_movie_stream_callback_fn runner)
{ pthread_mutex_lock(&lock); callback_runner = runner; pthread_mutex_unlock(&lock); }
bool dsound_movie_stream_caller_ok(uint32_t address)
{
    return (address >= XMV_LOW && address <= XMV_HIGH) || (address >= TITLE_LOW && address < TITLE_HIGH);
}
bool dsound_movie_stream_create_caller_ok(uint32_t address) { return address == CREATE_RETURN; }

/* Called with the lock held. Same preconditions as the startup streams: policy on, known IRQL0 and
 * the original global audio state zero. */
static const char *policy_error(void)
{
    uint32_t state;
    uint8_t irql;
    if (!enabled) return "explicit headless-movie-audio policy is disabled";
    if (irql_provider == NULL || !irql_provider(&irql) || irql != 0u) return "only known IRQL0 is supported";
    if (!kernel_guest_read_u32(GLOBAL_STATE, &state) || state != 0u) return "original global audio state must be zero";
    return NULL;
}
static movie_node *find(uint32_t address)
{
    for (movie_node *n = nodes; n != NULL; n = n->next)
        if (n->stream_address == address) return n;
    return NULL;
}
bool dsound_movie_stream_owns(uint32_t stream)
{ pthread_mutex_lock(&lock); const bool owned = find(stream) != NULL; pthread_mutex_unlock(&lock); return owned; }
size_t dsound_movie_stream_count(void)
{
    pthread_mutex_lock(&lock);
    size_t count = 0u;
    for (movie_node *n = nodes; n != NULL; n = n->next) count++;
    pthread_mutex_unlock(&lock);
    return count;
}
uint64_t dsound_movie_stream_completion_count(void)
{ pthread_mutex_lock(&lock); const uint64_t value = completions; pthread_mutex_unlock(&lock); return value; }
uint64_t dsound_movie_stream_operation_count(void)
{ pthread_mutex_lock(&lock); const uint64_t value = operations; pthread_mutex_unlock(&lock); return value; }
static void count_operation(void)
{ pthread_mutex_lock(&lock); operations++; pthread_mutex_unlock(&lock); }
static bool token_equal(const dsound_device_lease *a, const dsound_device_lease *b)
{ return a->device_heap == b->device_heap && a->internal_address == b->internal_address && a->serial == b->serial; }
/* The header is guest memory the title changes only through AddRef/Release here. Every word must
 * match the sidecar, which mirrors the reference count in word 2. */
static bool valid_node(const movie_node *node, const dsound_device_lease *identity, uint32_t internal)
{
    uint32_t actual[10], bytes;
    return node != NULL && token_equal(&node->lease, identity) && identity->internal_address == internal &&
        guest_heap_valid(identity->device_heap) &&
        kernel_guest_read_bytes(node->stream_address, actual, sizeof(actual)) &&
        memcmp(actual, node->header, sizeof(actual)) == 0 && guest_heap_valid(node->stream_heap) &&
        guest_heap_block_size(node->stream_heap, node->stream_address, &bytes) &&
        bytes == DSOUND_MOVIE_STREAM_BYTES;
}
static bool aliases_state(uint32_t address, uint32_t bytes, uint32_t internal)
{
    if (overlap(address, bytes, internal, 44u) || overlap(address, bytes, SINGLETON, 4u) ||
        overlap(address, bytes, GLOBAL_STATE, 4u) || overlap(address, bytes, SECONDARY, 60u))
        return true;
    for (movie_node *n = nodes; n != NULL; n = n->next)
        if (overlap(address, bytes, n->stream_address, DSOUND_MOVIE_STREAM_BYTES) ||
            overlap(address, bytes, n->publication_address, 4u))
            return true;
    for (movie_node *n = rollback; n != NULL; n = n->next)
        if (n->stream_address != 0u && overlap(address, bytes, n->stream_address, DSOUND_MOVIE_STREAM_BYTES))
            return true;
    return false;
}
static uint16_t word16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8u); }
static uint32_t word32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1] << 8u | (uint32_t)p[2] << 16u | (uint32_t)p[3] << 24u; }
typedef struct scope { uint32_t descriptor, format, callback, context; } scope;
/* The exact DSSTREAMDESC and WAVEFORMATEX the retained EnableAudioStream builds: flags 0, two
 * packets, callback 0x445071, a nonzero context (the decoder), no mixbins, PCM stereo 44100 Hz
 * 16-bit, block 4, average 176400, cbSize 0. */
static bool scope_snapshot(uint32_t descriptor, scope *out)
{
    uint8_t desc[24], wave[18];
    if (!kernel_guest_read_bytes(descriptor, desc, sizeof(desc))) return false;
    const uint32_t format = word32(desc + 8u);
    if (format == 0u || (uint64_t)format + sizeof(wave) > UINT64_C(0x100000000) ||
        overlap(descriptor, sizeof(desc), format, sizeof(wave)) ||
        !kernel_guest_read_bytes(format, wave, sizeof(wave)))
        return false;
    if (word32(desc) != 0u || word32(desc + 4u) != DSOUND_MOVIE_MAX_PACKETS ||
        word32(desc + 12u) != DSOUND_MOVIE_CALLBACK || word32(desc + 16u) == 0u || word32(desc + 20u) != 0u)
        return false;
    if (word16(wave) != 1u || word16(wave + 2u) != 2u || word32(wave + 4u) != FORMAT_RATE ||
        word32(wave + 8u) != FORMAT_AVERAGE || word16(wave + 12u) != FORMAT_BLOCK ||
        word16(wave + 14u) != 16u || word16(wave + 16u) != 0u)
        return false;
    *out = (scope){descriptor, format, word32(desc + 12u), word32(desc + 16u)};
    return true;
}

typedef struct create_request {
    scope sc;
    uint32_t output, status;
    void *output_at;
    movie_node *node;
    const char *error;
    bool announce;
} create_request;
static bool create_prepare(const dsound_device_lease *candidate, void *userdata, dsound_device_lease_child *child)
{
    create_request *r = userdata;
    pthread_mutex_lock(&lock);
    r->error = policy_error();
    if (r->error != NULL) return false;
    uint32_t old;
    if (!dsound_stream_original_tables_stopped() || !kernel_guest_read_u32(r->output, &old)) {
        r->error = "output unreadable or original vtable contains an unprotected target";
        return false;
    }
    if (aliases_state(r->output, 4u, candidate->internal_address) ||
        overlap(r->output, 4u, r->sc.descriptor, 24u) || overlap(r->output, 4u, r->sc.format, 18u) ||
        overlap(r->sc.descriptor, 24u, candidate->internal_address, 44u) ||
        overlap(r->sc.format, 18u, candidate->internal_address, 44u)) {
        r->error = "unproven output/input alias with owned state";
        return false;
    }
    /* Same-byte probe preserves contents, but is explicitly a guest write. */
    if (!kernel_guest_write_u32(r->output, old) || (r->output_at = kernel_guest_at(r->output, 4u)) == NULL) {
        r->error = "output is not writable";
        return false;
    }
    r->node = calloc(1u, sizeof(*r->node));
    if (r->node == NULL) { r->status = OOM; return false; }
    r->node->publication_address = r->output;
    r->node->callback = r->sc.callback;
    r->node->context = r->sc.context;
    const uint32_t heap = guest_heap_create(0u, GUEST_HEAP_CHUNK_MIN, 0u);
    r->node->stream_heap = heap;
    if (heap == 0u) { r->status = OOM; return false; }
    const uint32_t address = guest_heap_alloc(heap, DSOUND_MOVIE_STREAM_BYTES);
    r->node->stream_address = address;
    if (address == 0u) { r->status = OOM; return false; }
    uint32_t *header = r->node->header;
    header[0] = VTABLE;
    header[1] = SECONDARY;
    header[2] = 1u;
    header[3] = candidate->internal_address;
    if (aliases_state(address, DSOUND_MOVIE_STREAM_BYTES, candidate->internal_address) ||
        overlap(address, DSOUND_MOVIE_STREAM_BYTES, r->output, 4u) ||
        overlap(address, DSOUND_MOVIE_STREAM_BYTES, r->sc.descriptor, 24u) ||
        overlap(address, DSOUND_MOVIE_STREAM_BYTES, r->sc.format, 18u) ||
        !kernel_guest_write_bytes(address, header, DSOUND_MOVIE_STREAM_BYTES)) {
        r->error = "new private stream allocation/alias is invalid";
        return false;
    }
    *child = (dsound_device_lease_child){heap, address, DSOUND_MOVIE_STREAM_BYTES};
    return true;
}
/* Lease callbacks run under the device mutex. The module lock taken in prepare is released
 * here or in finalize, and cleanup and fatal run only after acquire returns. */
static void create_abort(void *userdata)
{
    (void)userdata;
    pthread_mutex_unlock(&lock);
}
static void cleanup_aborted_create(create_request *r)
{
    if (r->node == NULL) return;
    pthread_mutex_lock(&lock);
    const uint32_t heap = r->node->stream_heap;
    if (heap == 0u || (guest_heap_valid(heap) && guest_heap_destroy(heap))) {
        free(r->node);
    } else {
        r->node->next = rollback;
        rollback = r->node;
        r->error = "private allocation rollback failed; unleased cleanup retained";
    }
    r->node = NULL;
    pthread_mutex_unlock(&lock);
}
static void create_finalize(const dsound_device_lease *committed, void *userdata)
{
    create_request *r = userdata;
    movie_node **tail = &nodes;
    while (*tail != NULL) tail = &(*tail)->next;
    r->node->lease = *committed;
    r->node->next = NULL;
    *tail = r->node;
    r->announce = !announced;
    announced = true;
    memcpy(r->output_at, &r->node->stream_address, 4u);
    pthread_mutex_unlock(&lock);
}
static const dsound_device_lease_ops create_ops = {create_prepare, create_abort, create_finalize};
uint32_t dsound_movie_stream_create(uint32_t descriptor, uint32_t output)
{
    create_request r = {0};
    r.output = output;
    count_operation();
    pthread_mutex_lock(&lock);
    const char *error = policy_error();
    pthread_mutex_unlock(&lock);
    if (error != NULL) refuse(CREATE, error);
    if (!scope_snapshot(descriptor, &r.sc)) refuse(CREATE, "descriptor/format outside the measured XMV movie scope");
    uint32_t device;
    if (!kernel_guest_read_u32(SINGLETON, &device) || device == 0u || device > UINT32_MAX - 8u)
        refuse(CREATE, "no owned SILENT device");
    dsound_device_lease lease;
    const dsound_device_lease_status status = dsound_device_acquire_lease(device + 8u, &create_ops, &r, &lease);
    if (status != DSOUND_LEASE_OK) cleanup_aborted_create(&r);
    if (status == DSOUND_LEASE_OUT_OF_MEMORY || (status == DSOUND_LEASE_PREPARE_FAILED && r.status == OOM && r.error == NULL))
        return OOM;
    if (status != DSOUND_LEASE_OK) refuse(CREATE, r.error != NULL ? r.error : "device lease transaction refused");
    /* The mixer keys persistent voice state by guest address. A reused address starts at unity;
     * resetting an absent key must not reserve a slot. */
    reset_mixer_volume(r.node->stream_address);
    if (r.announce && dsound_audio_runtime_active())
        dsound_hle_log()("dsound: explicit HEADLESS movie stream; bounded CPU model of the XMV PCM stream "
                         "(2 packets, 44100 Hz stereo 16-bit). Its PCM packets feed the audio mixer and sink on the "
                         "virtual clock (timing INFERRED, the same as the menu music). Packets complete only "
                         "in DirectSoundDoWork at the format byte rate, callback included. APU, DSP and interrupt "
                         "objects are omitted\n");
    else if (r.announce)
        dsound_hle_log()("dsound: explicit HEADLESS movie stream; bounded CPU model of the XMV PCM stream "
                         "(2 packets, 44100 Hz stereo 16-bit). No audio is produced. Packets complete only "
                         "in DirectSoundDoWork on the virtual clock at the format byte rate, callback "
                         "included. APU, DSP and interrupt objects are omitted\n");
    return 0u;
}

typedef struct access_request access_request;
struct access_request {
    uint32_t address, entry, internal;
    uint32_t arg[3];
    dsound_device_lease identity;
    const char *error;
    bool destroy;
    uint32_t (*apply)(movie_node *node, access_request *request);
};
static bool identify(uint32_t address, dsound_device_lease *identity)
{
    pthread_mutex_lock(&lock);
    movie_node *n = find(address);
    if (n != NULL) *identity = n->lease;
    pthread_mutex_unlock(&lock);
    return n != NULL;
}
static void access_owned(uint32_t internal, void *userdata, uint32_t *result)
{
    access_request *r = userdata;
    pthread_mutex_lock(&lock);
    movie_node *n = find(r->address);
    if (!valid_node(n, &r->identity, internal)) {
        r->error = "stream ownership/header/generation changed";
        goto done;
    }
    r->error = policy_error();
    if (r->error != NULL) goto done;
    r->internal = internal;
    *result = r->apply(n, r);
done:
    pthread_mutex_unlock(&lock);
}
/* Runs `apply` with the owned device interface held and the module lock taken. */
static uint32_t access_node(access_request *r)
{
    count_operation();
    if (!identify(r->address, &r->identity)) refuse(r->entry, "stream is not owned by the movie model");
    uint32_t result = 0u;
    if (!dsound_device_with_owned_interface(r->identity.internal_address + 8u, access_owned, r, &result))
        refuse(r->entry, "parent SILENT device ownership/generation changed");
    if (r->error != NULL) refuse(r->entry, r->error);
    return result;
}
static uint64_t ticks_for(uint32_t size)
{
    const uint64_t numerator = (uint64_t)size * clock_frequency;
    return (numerator + FORMAT_AVERAGE - 1u) / FORMAT_AVERAGE;
}
/* The stream is playing when it holds packets and is not paused. Bit 0 means it can take
 * another packet. These are the bits the original reports (oracle). */
static uint32_t status_word(const movie_node *n)
{
    uint32_t value = n->queued < DSOUND_MOVIE_MAX_PACKETS ? 1u : 0u;
    if (n->queued != 0u) value |= n->paused ? 0x20000u : 0x10000u;
    return value;
}
static bool write_refs(movie_node *n, uint32_t refs)
{
    n->header[2] = refs;
    return kernel_guest_write_u32(n->stream_address + 8u, refs);
}
/* Called with the lock held. A paused stream freezes the unplayed remainder of its head packet. */
static void pause_stream(movie_node *n, uint32_t mode, uint64_t now)
{
    if (!n->paused && n->queued != 0u) {
        n->queue[0].remaining = n->queue[0].deadline > now ? n->queue[0].deadline - now : 0u;
        n->queue[0].deadline = NEVER;
    }
    n->paused = true;
    n->pause_mode = mode;
    (void)dsound_audio_runtime_set_stream_running(n->stream_address, now, false);
}
static void resume_stream(movie_node *n, uint64_t now)
{
    if (n->paused && n->queued != 0u) n->queue[0].deadline = now + n->queue[0].remaining;
    n->paused = false;
    n->pause_mode = 0u;
    (void)dsound_audio_runtime_set_stream_running(n->stream_address, now, true);
}

static uint32_t apply_add_ref(movie_node *n, access_request *r)
{
    if (n->header[2] == UINT32_MAX) { r->error = "reference count overflow"; return 0u; }
    if (!write_refs(n, n->header[2] + 1u)) { r->error = "reference word is not writable"; return 0u; }
    return n->header[2];
}
uint32_t dsound_movie_stream_add_ref(uint32_t stream)
{
    access_request r = {.address = stream, .entry = ADD_REF, .apply = apply_add_ref};
    return access_node(&r);
}
static bool volume_announced;
static uint32_t apply_set_volume(movie_node *n, access_request *r)
{
    /* HOST request cache only. The original also writes its settings object and APU gains, both
     * omitted. It validates nothing (oracle: every int32 returns 0), so neither does this. */
    n->volume = (int32_t)r->arg[0];
    n->volume_seen = true;
    /* With the audio runtime the soundtrack gain follows (T818). The title builds the value as
     * round(2000 * log10(gain)) clamped to -10000..0 (sub_00028180), so anything else is unmeasured
     * for the PCM and refused by name. Without the runtime nothing changes. */
    if (dsound_audio_runtime_active()) {
        uint32_t gain;
        if (!dsound_mixer_volume_to_gain_q16(n->volume, &gain)) {
            r->error = "SetVolume outside the DirectSound range -10000..0 has no measured movie gain";
            return 0u;
        }
        if (!dsound_audio_runtime_set_stream_volume(n->stream_address, n->volume)) {
            r->error = "the audio mixer refused the movie stream volume";
            return 0u;
        }
        if (n->volume != 0 && !volume_announced) {
            volume_announced = true;
            dsound_hle_log()("dsound: movie stream SetVolume is applied to the soundtrack gain "
                             "(10^(hundredths dB / 2000), -10000 mutes)\n");
        }
    }
    return 0u;
}
uint32_t dsound_movie_stream_set_volume(uint32_t stream, int32_t volume)
{
    access_request r = {.address = stream, .entry = VOLUME, .apply = apply_set_volume,
                        .arg = {(uint32_t)volume}};
    return access_node(&r);
}
static uint32_t apply_pause(movie_node *n, access_request *r)
{
    const uint32_t mode = r->arg[0];
    TRACE_EVENT("pause mode %u tick %llu queued %u", mode, (unsigned long long)now_ticks(), n->queued);
    if (mode >= 4u) return E_FAIL_STATUS;  /* the original fails modes 4 and up (oracle) */
    if (mode == 3u) { r->error = "Pause mode 3 has no measured XMV use"; return 0u; }
    const uint64_t now = now_ticks();
    if (mode == 0u) resume_stream(n, now);
    else pause_stream(n, mode, now);
    return 0u;
}
uint32_t dsound_movie_stream_pause(uint32_t stream, uint32_t mode)
{
    access_request r = {.address = stream, .entry = PAUSE, .apply = apply_pause, .arg = {mode}};
    return access_node(&r);
}
static uint32_t apply_flush_ex(movie_node *n, access_request *r)
{
    if (r->arg[0] != 0u || r->arg[1] != 0u || r->arg[2] != 1u) {
        r->error = "only the measured FlushEx(time 0, flags 1) is supported";
        return 0u;
    }
    if (n->queued == 0u) return 0u;  /* nothing to flush, the original changes nothing (oracle) */
    const uint64_t now = now_ticks();
    if (n->paused) resume_stream(n, now);  /* an async flush lets a paused stream drain (oracle) */
    n->flush_pending = true;
    n->flush_tick = now;
    (void)dsound_audio_runtime_cut_stream(n->stream_address, now);
    return 0u;
}
uint32_t dsound_movie_stream_flush_ex(uint32_t stream, uint32_t time_low, uint32_t time_high, uint32_t flags)
{
    access_request r = {.address = stream, .entry = FLUSH_EX, .apply = apply_flush_ex,
                        .arg = {time_low, time_high, flags}};
    return access_node(&r);
}
/* IDirectSoundStream::Flush (0x407388, T394): measured on the original bytes, identical to the synchronous
 * FlushEx(0, 0, 0). Every attached packet is completed at once with its full size and 0x80004004 and its
 * callback, in FIFO order, and a paused stream is left running (oracle). The stated model aborts all
 * of them, a packet whose deadline passed but that no DoWork observed included, exactly as the final
 * Release does. */
static uint32_t apply_flush(movie_node *n, access_request *r)
{
    (void)r;
    TRACE_EVENT("flush tick %llu queued %u", (unsigned long long)now_ticks(), n->queued);
    if (n->paused) resume_stream(n, now_ticks());
    (void)dsound_audio_runtime_cut_stream(n->stream_address, now_ticks());
    return 0u;
}
static size_t drain(uint32_t entry, bool everything, uint32_t only_stream);
uint32_t dsound_movie_stream_flush(uint32_t stream)
{
    access_request r = {.address = stream, .entry = FLUSH, .apply = apply_flush};
    const uint32_t result = access_node(&r);
    (void)drain(FLUSH, true, stream);
    return result;
}
static uint32_t apply_discontinuity(movie_node *n, access_request *r)
{
    (void)r;
    /* HOST request observation only. The original sets a voice flag the omitted APU consumes. */
    n->discontinuity_seen = true;
    return 0u;
}
uint32_t dsound_movie_stream_discontinuity(uint32_t stream)
{
    access_request r = {.address = stream, .entry = DISCONTINUITY, .apply = apply_discontinuity};
    return access_node(&r);
}
static uint32_t apply_get_status(movie_node *n, access_request *r)
{
    const uint32_t output = r->arg[0];
    uint32_t old;
    if (output == 0u || (uint64_t)output + 4u > UINT64_C(0x100000000) || aliases_state(output, 4u, r->internal) ||
        !kernel_guest_read_u32(output, &old) || !kernel_guest_write_u32(output, old) ||
        !kernel_guest_write_u32(output, status_word(n))) {
        r->error = "status output is NULL, inaccessible or aliases protected state";
        return 0u;
    }
    return 0u;
}
uint32_t dsound_movie_stream_get_status(uint32_t stream, uint32_t output)
{
    access_request r = {.address = stream, .entry = GET_STATUS, .apply = apply_get_status, .arg = {output}};
    return access_node(&r);
}
/* The original stores dwords through the completion pointers and never aligns them (oracle, offsets
 * 1 to 3), and the retained XMV keeps them at odd addresses (T394), so only overlap is refused. */
static bool words_overlap(uint32_t a, uint32_t b)
{
    return a < b ? b - a < 4u : a - b < 4u;
}
static bool word_probe(uint32_t address, uint32_t internal)
{
    uint32_t old;
    return address != 0u && (uint64_t)address + 4u <= UINT64_C(0x100000000) &&
        !aliases_state(address, 4u, internal) && kernel_guest_read_u32(address, &old) &&
        kernel_guest_write_u32(address, old);
}
/* The attached packet is the XMV decoder's PCM (the one admitted format: 44100 Hz stereo signed 16-bit
 * little endian, see FORMAT_*). With the opt-in audio runtime active it is copied into the same
 * virtual-clock mixer the menu music uses, starting where the model starts it (the mixer queues behind the
 * stream tail exactly as the deadline chain does, and honours the same Pause/Resume ticks). Without the
 * runtime nothing changes. A mixer refusal never changes guest-visible timing, it is counted and announced. */
static uint64_t pcm_packets, pcm_frames, pcm_refused;
static void submit_pcm(movie_node *n, const packet *p)
{
    if (!dsound_audio_runtime_active()) return;
    int16_t *pcm = malloc(p->size);
    bool ok = pcm != NULL && kernel_guest_read_bytes(p->buffer, pcm, p->size) &&
              dsound_audio_runtime_submit_pcm16_stereo(n->stream_address, now_ticks(), FORMAT_RATE, pcm,
                                                       p->size / FORMAT_BLOCK);
    free(pcm);
    if (ok) {
        pcm_packets++;
        pcm_frames += p->size / FORMAT_BLOCK;
        return;
    }
    if (pcm_refused++ == 0u)
        fprintf(stderr, "dsound: movie PCM packet (0x%X bytes) was refused by the mixer; its audio is dropped, "
                        "guest timing is unchanged\n", p->size);
}
uint64_t dsound_movie_stream_pcm_packets(void) { return pcm_packets; }
uint64_t dsound_movie_stream_pcm_frames(void) { return pcm_frames; }
uint64_t dsound_movie_stream_pcm_refused(void) { return pcm_refused; }
static uint32_t apply_process(movie_node *n, access_request *r)
{
    if (r->arg[1] != 0u) { r->error = "Process output packet must be NULL as the retained XMV passes it"; return 0u; }
    /* The original rejects before reading the packet when two are attached (oracle). */
    if (n->queued >= DSOUND_MOVIE_MAX_PACKETS) return DSOUND_MOVIE_TOO_MANY_PACKETS;
    uint32_t words[6];
    if (!kernel_guest_read_bytes(r->arg[0], words, sizeof(words))) { r->error = "packet is unreadable"; return 0u; }
    /* words[5] is the timestamp slot: the retained XMV leaves it unset and nothing here reads it. */
    packet p = {.buffer = words[0], .size = words[1], .completed = words[2], .status = words[3], .context = words[4]};
    if (p.buffer == 0u || p.size < FORMAT_BLOCK || (p.size % FORMAT_BLOCK) != 0u || p.size > MAX_PACKET_BYTES ||
        !kernel_guest_range_readable(p.buffer, p.size) || words_overlap(p.completed, p.status) ||
        !word_probe(p.completed, r->internal) || !word_probe(p.status, r->internal)) {
        /* Name the measured values, a refusal must say which condition failed (T394). */
        static _Thread_local char detail[256];
        snprintf(detail, sizeof(detail),
                 "packet buffer, size or completion words are outside the measured scope "
                 "(buffer 0x%08X size 0x%X completed 0x%08X status 0x%08X context 0x%08X, queued %u, "
                 "buffer readable %d, completed ok %d, status ok %d)",
                 p.buffer, p.size, p.completed, p.status, p.context, n->queued,
                 p.buffer != 0u && p.size <= MAX_PACKET_BYTES && kernel_guest_range_readable(p.buffer, p.size),
                 word_probe(p.completed, r->internal), word_probe(p.status, r->internal));
        r->error = detail;
        return 0u;
    }
    /* Sub-block sizes (below 4) are accepted by the original but never attached: they would stay
     * pending forever, so they are refused above. Distinct completion words keep order visible. */
    for (uint32_t i = 0u; i < n->queued; i++)
        if (words_overlap(n->queue[i].completed, p.completed) || words_overlap(n->queue[i].status, p.status) ||
            words_overlap(n->queue[i].completed, p.status) || words_overlap(n->queue[i].status, p.completed)) {
            r->error = "completion words alias an attached packet";
            return 0u;
        }
    if (!kernel_guest_write_u32(p.completed, 0u) || !kernel_guest_write_u32(p.status, DSOUND_MOVIE_PENDING_STATUS)) {
        r->error = "completion words are not writable";
        return 0u;
    }
    submit_pcm(n, &p);
    p.ticks = ticks_for(p.size);
    p.deadline = NEVER;
    p.remaining = p.ticks;
    if (n->queued == 0u && !n->paused) p.deadline = now_ticks() + p.ticks;
    n->queue[n->queued++] = p;
    TRACE_EVENT("process size 0x%X context 0x%08X tick %llu ticks %llu deadline %llu queued %u paused %d", p.size, p.context,
                (unsigned long long)now_ticks(), (unsigned long long)p.ticks,
                (unsigned long long)(p.deadline == NEVER ? 0u : p.deadline), n->queued, n->paused);
    return 0u;
}
uint32_t dsound_movie_stream_process(uint32_t stream, uint32_t packet_address, uint32_t output_packet)
{
    access_request r = {.address = stream, .entry = PROCESS, .apply = apply_process,
                        .arg = {packet_address, output_packet}};
    return access_node(&r);
}

/* One packet leaves the queue. Called with the lock held. */
static packet take_head(movie_node *n, uint64_t now, uint32_t *status)
{
    const packet done = n->queue[0];
    for (uint32_t i = 1u; i < n->queued; i++) n->queue[i - 1u] = n->queue[i];
    n->queued--;
    memset(&n->queue[n->queued], 0, sizeof(n->queue[0]));
    /* A flush completes a packet normally only when its deadline passed before the flush. */
    const bool played = done.deadline != NEVER && (!n->flush_pending || done.deadline <= n->flush_tick);
    *status = played ? 0u : DSOUND_MOVIE_ABORT_STATUS;
    if (n->queued != 0u) {
        /* Playback is continuous, the next packet starts where this one ended. A paused stream
         * keeps the full duration in `remaining` and has no deadline. */
        if (n->paused) n->queue[0].deadline = NEVER;
        else n->queue[0].deadline = (played ? done.deadline : now) + n->queue[0].ticks;
    }
    if (n->queued == 0u) n->flush_pending = false;
    return done;
}
static bool due(const movie_node *n, uint64_t now)
{
    return n->queued != 0u && (n->flush_pending || (!n->paused && n->queue[0].deadline <= now));
}
/* The original completion helper 0x40B244: write the completed size and the status, then call
 * back with (stream context, packet context, status). */
static void deliver(uint32_t entry, const packet *done, uint32_t status, uint32_t callback, uint32_t context)
{
    pthread_mutex_lock(&lock);
    const bool written = kernel_guest_write_u32(done->completed, done->size) &&
                         kernel_guest_write_u32(done->status, status);
    dsound_movie_stream_callback_fn runner = callback_runner;
    completions++;
    TRACE_EVENT("complete size 0x%X status 0x%08X context 0x%08X tick %llu", done->size, status, done->context,
                (unsigned long long)now_ticks());
    pthread_mutex_unlock(&lock);
    if (!written) refuse(entry, "packet completion words are no longer writable");
    if (runner == NULL) refuse(entry, "no guest callback runner is installed");
    if (!runner(callback, context, done->context, status)) refuse(entry, "guest completion callback could not run");
}
/* Completes the packets that are due, in node then queue order, until none is. */
static size_t drain(uint32_t entry, bool everything, uint32_t only_stream)
{
    size_t delivered = 0u;
    for (;;) {
        pthread_mutex_lock(&lock);
        const uint64_t now = now_ticks();
        movie_node *pick = NULL;
        for (movie_node *n = nodes; n != NULL && pick == NULL; n = n->next)
            if ((only_stream == 0u || n->stream_address == only_stream) && n->queued != 0u &&
                (everything || due(n, now)))
                pick = n;
        if (pick == NULL) { pthread_mutex_unlock(&lock); return delivered; }
        uint32_t status;
        const uint32_t callback = pick->callback, context = pick->context;
        if (everything) {  /* final release aborts what is left, whatever its deadline */
            pick->flush_pending = true;
            pick->flush_tick = 0u;
            (void)dsound_audio_runtime_cut_stream(pick->stream_address, now);
        }
        const packet done = take_head(pick, now, &status);
        pthread_mutex_unlock(&lock);
        deliver(entry, &done, status, callback, context);
        delivered++;
    }
}
size_t dsound_movie_stream_do_work(void)
{
    count_operation();
    pthread_mutex_lock(&lock);
    const char *error = policy_error();
    pthread_mutex_unlock(&lock);
    if (error != NULL) refuse(WORK, error);
    return drain(WORK, false, 0u);
}
void dsound_movie_stream_synch_playback(void)
{
    pthread_mutex_lock(&lock);
    const uint64_t now = now_ticks();
    TRACE_EVENT("synch playback tick %llu", (unsigned long long)now);
    for (movie_node *n = nodes; n != NULL; n = n->next)
        if (n->paused && n->pause_mode == 2u) resume_stream(n, now);
    pthread_mutex_unlock(&lock);
}

static void synch_owned_device(uint32_t internal_address, void *userdata, uint32_t *result)
{
    (void)internal_address;
    (void)userdata;
    dsound_movie_stream_synch_playback();
    *result = 0u;
}
bool dsound_movie_stream_route_synch(uint32_t stack_pointer, uint32_t *result)
{
    uint32_t caller, interface;
    if (result == NULL) refuse(SYNCH, "no result slot");
    pthread_mutex_lock(&lock);
    const char *error = policy_error();
    pthread_mutex_unlock(&lock);
    if (error != NULL) refuse(SYNCH, error);
    if (stack_pointer > UINT32_MAX - 8u || !kernel_guest_read_u32(stack_pointer, &caller) ||
        !kernel_guest_read_u32(stack_pointer + 4u, &interface))
        refuse(SYNCH, "unreadable SynchPlayback frame");
    if (caller != SYNCH_RETURN) refuse(SYNCH, "SynchPlayback called from outside the measured XMV site");
    count_operation();
    /* The device mutex is held while the model runs, lock order is device then movie. */
    if (!dsound_device_with_owned_interface(interface, synch_owned_device, NULL, result))
        refuse(SYNCH, "SynchPlayback argument is not the owned device interface");
    return true;
}

typedef struct detach_request { uint32_t address; dsound_device_lease identity; movie_node *node; } detach_request;
static bool detach_prepare(const dsound_device_lease *candidate, void *userdata, dsound_device_lease_child *child)
{
    detach_request *r = userdata;
    pthread_mutex_lock(&lock);
    r->node = find(r->address);
    if (!token_equal(candidate, &r->identity) || !valid_node(r->node, &r->identity, candidate->internal_address))
        return false;
    *child = (dsound_device_lease_child){r->node->stream_heap, r->address, DSOUND_MOVIE_STREAM_BYTES};
    return true;
}
static void detach_abort(void *userdata) { (void)userdata; pthread_mutex_unlock(&lock); }
static void detach_finalize(const dsound_device_lease *committed, void *userdata)
{
    detach_request *r = userdata;
    (void)committed;
    movie_node **p = &nodes;
    while (*p != r->node) p = &(*p)->next;
    *p = r->node->next;
    r->node->next = NULL;
    pthread_mutex_unlock(&lock);
}
static const dsound_device_lease_ops detach_ops = {detach_prepare, detach_abort, detach_finalize};
/* Detach one node from its device and free its private allocation. False when refused. */
static bool destroy_node(uint32_t address, const dsound_device_lease *identity)
{
    detach_request r = {.address = address, .identity = *identity};
    if (dsound_device_release_lease(identity, &detach_ops, &r) != DSOUND_LEASE_OK) return false;
    const bool freed = guest_heap_valid(r.node->stream_heap) && guest_heap_destroy(r.node->stream_heap);
    if (freed) free(r.node);
    return freed;
}
static uint32_t apply_release(movie_node *n, access_request *r)
{
    if (n->header[2] == 0u) { r->error = "reference count underflow"; return 0u; }
    if (n->header[2] == 1u) { r->destroy = true; return 0u; }
    if (!write_refs(n, n->header[2] - 1u)) { r->error = "reference word is not writable"; return 0u; }
    return n->header[2];
}
uint32_t dsound_movie_stream_release(uint32_t stream)
{
    access_request r = {.address = stream, .entry = RELEASE, .apply = apply_release};
    const uint32_t result = access_node(&r);
    if (!r.destroy) return result;
    /* The last reference. Pending packets are aborted exactly as the original destructor does,
     * completed size = packet size, status 0x80004004, callback per packet, then the stream is freed. */
    (void)drain(RELEASE, true, stream);
    /* drain cuts any still-queued audio before the persistent mixer slot is returned to unity. */
    reset_mixer_volume(stream);
    if (!destroy_node(stream, &r.identity)) refuse(RELEASE, "stream cleanup refused changed ownership");
    return 0u;
}

bool dsound_movie_stream_get_snapshot(uint32_t stream, dsound_movie_stream_snapshot *output)
{
    if (output == NULL) return false;
    pthread_mutex_lock(&lock);
    const movie_node *n = find(stream);
    if (n != NULL) {
        memset(output, 0, sizeof(*output));
        output->stream_address = n->stream_address;
        output->refs = n->header[2];
        output->callback = n->callback;
        output->context = n->context;
        output->volume = n->volume;
        output->volume_seen = n->volume_seen;
        output->discontinuity_seen = n->discontinuity_seen;
        output->flush_pending = n->flush_pending;
        output->pause_mode = n->paused ? n->pause_mode : 0u;
        output->queued = n->queued;
        for (uint32_t i = 0u; i < n->queued; i++) output->packet_size[i] = n->queue[i].size;
        output->head_deadline = n->queued != 0u ? n->queue[0].deadline : 0u;
    }
    pthread_mutex_unlock(&lock);
    return n != NULL;
}
static bool reset_streams(void)
{
    for (;;) {
        pthread_mutex_lock(&lock);
        const uint32_t address = nodes != NULL ? nodes->stream_address : 0u;
        const dsound_device_lease identity = nodes != NULL ? nodes->lease : (dsound_device_lease){0};
        pthread_mutex_unlock(&lock);
        if (address == 0u) break;
        if (!destroy_node(address, &identity)) return false;
    }
    pthread_mutex_lock(&lock);
    while (rollback != NULL) {
        movie_node *n = rollback;
        if (!guest_heap_valid(n->stream_heap) || !guest_heap_destroy(n->stream_heap)) {
            pthread_mutex_unlock(&lock);
            return false;
        }
        rollback = n->next;
        free(n);
    }
    announced = false;
    completions = 0u;
    pcm_packets = pcm_frames = pcm_refused = 0u;
    operations = 0u;
    pthread_mutex_unlock(&lock);
    return true;
}
bool dsound_movie_stream_reset_checked(void)
{
    pthread_mutex_lock(&reset_lock);
    const bool result = reset_streams();
    pthread_mutex_unlock(&reset_lock);
    return result;
}
void dsound_movie_stream_reset(void) { (void)dsound_movie_stream_reset_checked(); }

static bool movie_policy(void)
{
    pthread_mutex_lock(&lock);
    const bool value = enabled;
    pthread_mutex_unlock(&lock);
    return value;
}
bool dsound_movie_stream_route_public(uint32_t entry, const kernel_call_frame *frame,
                                      uint32_t return_address, uint32_t *result)
{
    uint32_t args[4] = {0u, 0u, 0u, 0u};
    if (frame == NULL || result == NULL || !movie_policy()) return false;
    if (entry == CREATE) {
        if (!dsound_movie_stream_create_caller_ok(return_address)) return false;
        if (!kernel_frame_arg(frame, 0u, &args[0]) || !kernel_frame_arg(frame, 1u, &args[1]))
            refuse(entry, "unreadable argument");
        *result = dsound_movie_stream_create(args[0], args[1]);
        return true;
    }
    if (entry != PAUSE && entry != FLUSH_EX && entry != VOLUME) return false;
    if (!kernel_frame_arg(frame, 0u, &args[0]) || !dsound_movie_stream_owns(args[0])) return false;
    if (!dsound_movie_stream_caller_ok(return_address)) refuse(entry, "movie stream called from outside the movie callers");
    const unsigned count = entry == PAUSE || entry == VOLUME ? 2u : 4u;
    for (unsigned i = 1u; i < count; i++)
        if (!kernel_frame_arg(frame, i, &args[i])) refuse(entry, "unreadable argument");
    if (entry == PAUSE) *result = dsound_movie_stream_pause(args[0], args[1]);
    else if (entry == VOLUME) *result = dsound_movie_stream_set_volume(args[0], (int32_t)args[1]);
    else *result = dsound_movie_stream_flush_ex(args[0], args[1], args[2], args[3]);
    return true;
}
bool dsound_movie_stream_method_owned(uint32_t stack_pointer)
{
    uint32_t stream;
    return movie_policy() && stack_pointer <= UINT32_MAX - 8u &&
           kernel_guest_read_u32(stack_pointer + 4u, &stream) && dsound_movie_stream_owns(stream);
}
bool dsound_movie_stream_route_method(uint32_t entry, uint32_t stack_pointer, uint32_t *result,
                                      uint32_t *pop_bytes)
{
    uint32_t caller, args[3] = {0u, 0u, 0u};
    unsigned count;
    switch (entry) {
    case ADD_REF: case RELEASE: case DISCONTINUITY: case FLUSH: count = 1u; break;
    case GET_STATUS: count = 2u; break;
    case PROCESS: count = 3u; break;
    default: return false;
    }
    if (result == NULL || pop_bytes == NULL || !movie_policy() || stack_pointer > UINT32_MAX - 16u ||
        !kernel_guest_read_u32(stack_pointer, &caller) || !kernel_guest_read_u32(stack_pointer + 4u, &args[0]) ||
        !dsound_movie_stream_owns(args[0]))
        return false;
    if (!dsound_movie_stream_caller_ok(caller)) refuse(entry, "movie stream called from outside the movie callers");
    for (unsigned i = 1u; i < count; i++)
        if (!kernel_guest_read_u32(stack_pointer + 4u + 4u * i, &args[i])) refuse(entry, "unreadable argument");
    switch (entry) {
    case ADD_REF: *result = dsound_movie_stream_add_ref(args[0]); break;
    case RELEASE: *result = dsound_movie_stream_release(args[0]); break;
    case DISCONTINUITY: *result = dsound_movie_stream_discontinuity(args[0]); break;
    case FLUSH: *result = dsound_movie_stream_flush(args[0]); break;
    case GET_STATUS: *result = dsound_movie_stream_get_status(args[0], args[1]); break;
    default: *result = dsound_movie_stream_process(args[0], args[1], args[2]); break;
    }
    *pop_bytes = 4u * count;
    return true;
}
bool dsound_movie_stream_route_work(uint32_t return_address)
{
    if (!movie_policy() || !dsound_movie_stream_caller_ok(return_address) || dsound_movie_stream_count() == 0u)
        return false;
    (void)dsound_movie_stream_do_work();
    return true;
}
