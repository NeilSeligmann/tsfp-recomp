/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_device.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "dsound_hle.h"
#include "guest_mem.h"
#include "kernel_call.h"

#define CREATE_ENTRY 0x00409635u
#define CREATE_CALLER 0x000279D5u
/* T421: the XMV GetNextFrame returns here from its DirectSoundCreate and device Release calls. */
#define RELEASE_ENTRY 0x00406A8Au
#define MOVIE_CREATE_CALLER 0x0044570Bu
#define MOVIE_RELEASE_CALLER 0x00445726u
/* The original device Release that would leave this many references destroys the device: five belong
 * to its own children and the sixth to the title. Measured in tests/test_dsound_device_calls_oracle.py. */
#define DESTROY_REFERENCES 6u
#define SINGLETON 0x00412B30u
#define DEVICE_VTABLE 0x004A1CB0u
#define DEVICE_BYTES 44u
#define E_OUTOFMEMORY 0x8007000Eu
#define DSERR_NODRIVER 0x88780078u
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t owned_heap, owned_address, shadow_reference;
typedef struct lease_record {
    dsound_device_lease token;
    dsound_device_lease_child child;
    struct lease_record *next;
} lease_record;
static lease_record *leases;
static size_t lease_count;
/* Survives reset; refuses exhaustion rather than reissuing an identity. */
static uint64_t lease_serial;
static bool announced;
static dsound_device_fatal_fn fatal_handler;
static dsound_device_create_note_fn create_note;
void dsound_device_set_create_note(dsound_device_create_note_fn note)
{pthread_mutex_lock(&lock);create_note=note;pthread_mutex_unlock(&lock);}

/* T421: references the XMV movie calls hold (second DirectSoundCreate, not yet Released). */
static bool movie_calls_enabled;
static uint32_t movie_references;

static void refuse_at(uint32_t entry, const char *message) __attribute__((noreturn));
static void refuse_at(uint32_t entry, const char *message)
{
    dsound_hle_log()("dsound SILENT facade: %#x refused: %s\n", entry, message);
    if (fatal_handler != NULL) fatal_handler(entry, message);
    abort();
}
static void refuse(const char *message) __attribute__((noreturn));
static void refuse(const char *message) { refuse_at(CREATE_ENTRY, message); }
static void refuse_locked(const char *message) __attribute__((noreturn));
static void refuse_locked(const char *message)
{ pthread_mutex_unlock(&lock); refuse(message); }
static void refuse_locked_at(uint32_t entry, const char *message) __attribute__((noreturn));
static void refuse_locked_at(uint32_t entry, const char *message)
{ pthread_mutex_unlock(&lock); refuse_at(entry, message); }
void dsound_device_set_fatal(dsound_device_fatal_fn handler) { fatal_handler = handler; }
static bool overlaps(uint32_t a, uint32_t size_a, uint32_t b, uint32_t size_b)
{
    return (uint64_t)a < (uint64_t)b + size_b && (uint64_t)b < (uint64_t)a + size_a;
}
static void header_words(uint32_t address, uint32_t reference, uint32_t words[11])
{
    memset(words, 0, DEVICE_BYTES);
    words[0] = DEVICE_VTABLE;
    words[1] = reference;
    words[4] = address + 16u;
    words[5] = address + 16u;
}
static bool validate_owned(uint32_t interface)
{
    uint32_t singleton, requested, expected[11], actual[11];
    if (owned_address == 0u || interface != owned_address + 8u || shadow_reference == 0u ||
        !kernel_guest_read_bytes(owned_address, actual, sizeof(actual)) ||
        !kernel_guest_read_u32(SINGLETON, &singleton) || singleton != owned_address ||
        !guest_heap_valid(owned_heap) ||
        !guest_heap_block_size(owned_heap, owned_address, &requested) ||
        requested != DEVICE_BYTES) return false;
    header_words(owned_address, shadow_reference, expected);
    return memcmp(actual, expected, sizeof(actual)) == 0;
}
bool dsound_device_with_owned_interface(uint32_t interface, dsound_owned_interface_fn callback,
                                        void *userdata, uint32_t *result)
{
    pthread_mutex_lock(&lock);
    const bool valid = callback != NULL && result != NULL &&
        shadow_reference != UINT32_MAX && validate_owned(interface);
    if (valid) callback(owned_address, userdata, result);
    pthread_mutex_unlock(&lock);
    return valid;
}
bool dsound_device_with_owned_identity(uint32_t interface, dsound_owned_identity_fn callback,
                                      void *userdata, uint32_t *result)
{
    pthread_mutex_lock(&lock);
    const bool valid = callback != NULL && result != NULL &&
        shadow_reference != UINT32_MAX && validate_owned(interface);
    if (valid) {
        const dsound_device_identity identity = {owned_heap, owned_address};
        callback(&identity, userdata, result);
    }
    pthread_mutex_unlock(&lock);
    return valid;
}
static bool valid_ops(const dsound_device_lease_ops *ops)
{ return ops != NULL && ops->prepare != NULL && ops->abort != NULL && ops->finalize != NULL; }
static bool valid_child(const dsound_device_lease_child *child)
{
    uint32_t bytes;
    return child->heap != 0u && child->heap != owned_heap && child->address != 0u &&
        child->bytes != 0u && (uint64_t)child->address + child->bytes <= UINT64_C(0x100000000) &&
        guest_heap_valid(child->heap) &&
        guest_heap_block_size(child->heap, child->address, &bytes) && bytes == child->bytes &&
        !overlaps(child->address, child->bytes, owned_address, DEVICE_BYTES);
}
static bool same_token(const dsound_device_lease *a, const dsound_device_lease *b)
{ return a->device_heap == b->device_heap && a->internal_address == b->internal_address &&
         a->serial == b->serial; }
static bool same_child(const dsound_device_lease_child *a, const dsound_device_lease_child *b)
{ return a->heap == b->heap && a->address == b->address && a->bytes == b->bytes; }
static bool probe_reference(void)
{ return kernel_guest_write_u32(owned_address + 4u, shadow_reference); }
dsound_device_lease_status dsound_device_acquire_lease(uint32_t interface,
    const dsound_device_lease_ops *ops, void *userdata, dsound_device_lease *output)
{
    dsound_device_lease_status status = DSOUND_LEASE_INVALID;
    pthread_mutex_lock(&lock);
    if (!valid_ops(ops) || output == NULL || !validate_owned(interface)) goto done;
    if (shadow_reference == UINT32_MAX) { status = DSOUND_LEASE_REFERENCE_LIMIT; goto done; }
    if (lease_serial == UINT64_MAX) { status = DSOUND_LEASE_SERIAL_LIMIT; goto done; }
    if (!probe_reference()) { status = DSOUND_LEASE_WRITE_REFUSED; goto done; }
    lease_record *record = malloc(sizeof(*record));
    if (record == NULL) { status = DSOUND_LEASE_OUT_OF_MEMORY; goto done; }
    record->child = (dsound_device_lease_child){0};
    record->token = (dsound_device_lease){owned_heap, owned_address, lease_serial + 1u};
    if (!ops->prepare(&record->token, userdata, &record->child)) {
        status = DSOUND_LEASE_PREPARE_FAILED; goto abort_prepare;
    }
    if (!valid_child(&record->child)) goto abort_prepare;
    for (lease_record *other = leases; other != NULL; other = other->next)
        if (same_child(&other->child, &record->child)) goto abort_prepare;
    /* No callback may change the device or its heap. Validate again before commit. */
    if (!validate_owned(interface)) goto abort_prepare;
    if (!kernel_guest_write_u32(owned_address + 4u, shadow_reference + 1u)) {
        status = DSOUND_LEASE_WRITE_REFUSED; goto abort_prepare;
    }
    shadow_reference++;
    lease_serial = record->token.serial;
    record->next = leases; leases = record; lease_count++;
    ops->finalize(&record->token, userdata);
    *output = record->token;
    status = DSOUND_LEASE_OK;
    goto done;
abort_prepare:
    ops->abort(userdata);
    free(record);
done:
    pthread_mutex_unlock(&lock);
    return status;
}
dsound_device_lease_status dsound_device_release_lease(const dsound_device_lease *lease,
    const dsound_device_lease_ops *ops, void *userdata)
{
    dsound_device_lease_status status = DSOUND_LEASE_INVALID;
    pthread_mutex_lock(&lock);
    if (lease == NULL || !valid_ops(ops)) goto done;
    lease_record **link = &leases;
    while (*link != NULL && !same_token(lease, &(*link)->token)) link = &(*link)->next;
    lease_record *record = *link;
    if (record == NULL || record->token.device_heap != owned_heap ||
        record->token.internal_address != owned_address ||
        !validate_owned(owned_address + 8u) || !valid_child(&record->child) ||
        shadow_reference <= lease_count) goto done;
    if (!probe_reference()) { status = DSOUND_LEASE_WRITE_REFUSED; goto done; }
    dsound_device_lease_child child = {0};
    if (!ops->prepare(&record->token, userdata, &child)) {
        status = DSOUND_LEASE_PREPARE_FAILED; goto abort_prepare;
    }
    if (!same_child(&child, &record->child) || !valid_child(&child) ||
        !validate_owned(owned_address + 8u)) goto abort_prepare;
    if (!kernel_guest_write_u32(owned_address + 4u, shadow_reference - 1u)) {
        status = DSOUND_LEASE_WRITE_REFUSED; goto abort_prepare;
    }
    shadow_reference--; *link = record->next; lease_count--;
    ops->finalize(&record->token, userdata);
    free(record);
    status = DSOUND_LEASE_OK;
    goto done;
abort_prepare:
    ops->abort(userdata);
done:
    pthread_mutex_unlock(&lock);
    return status;
}
bool dsound_device_reset_checked(void)
{
    pthread_mutex_lock(&lock);
    if (lease_count != 0u) { pthread_mutex_unlock(&lock); return false; }
    if (owned_address != 0u && guest_heap_valid(owned_heap)) {
        /* A live but changed allocation/header/singleton may be foreign state.
         * Keep both guest state and sidecar intact so reset is reviewable/retryable. */
        if (!validate_owned(owned_address + 8u) ||
            !kernel_guest_write_u32(SINGLETON, 0u)) {
            pthread_mutex_unlock(&lock); return false;
        }
        /* Quiescent heap ownership makes this destruction deterministic. */
        if (!guest_heap_destroy(owned_heap)) {
            (void)kernel_guest_write_u32(SINGLETON, owned_address);
            pthread_mutex_unlock(&lock); return false;
        }
    }
    owned_heap = owned_address = shadow_reference = 0u;
    movie_references = 0u;
    announced = false;
    pthread_mutex_unlock(&lock);
    return true;
}
void dsound_device_reset(void) { (void)dsound_device_reset_checked(); }
static uint32_t create_device(uint32_t guid, uint32_t output, uint32_t outer, bool movie)
{
    pthread_mutex_lock(&lock);
    if (guid != 0u || outer != 0u) refuse_locked("only NULL GUID/outer are recovered");
    void *output_at = kernel_guest_at(output, 4u);
    void *singleton_at = kernel_guest_at(SINGLETON, 4u);
    if (output_at == NULL || singleton_at == NULL || kernel_guest_at(DEVICE_VTABLE, 12u) == NULL)
        refuse_locked("output, singleton or original vtable is not mapped");
    if (overlaps(output, 4u, SINGLETON, 4u) || overlaps(output, 4u, DEVICE_VTABLE, 12u) ||
        (owned_address != 0u && overlaps(output, 4u, owned_address, DEVICE_BYTES)))
        refuse_locked("output alias with device state is not recovered");
    uint32_t singleton;
    if (!kernel_guest_read_u32(SINGLETON, &singleton)) refuse_locked("singleton is unreadable");
    uint32_t status = 0u;
    if (movie && owned_address == 0u)
        refuse_locked("a movie DirectSoundCreate needs the device the startup already created");
    if (owned_address != 0u) {
        if (!validate_owned(owned_address + 8u) || shadow_reference == UINT32_MAX)
            refuse_locked("cached singleton/header/heap generation/reference changed");
        if (movie && movie_references == UINT32_MAX)
            refuse_locked("movie reference count would overflow");
        /* Cached success remains independent of codec readiness. Use a guarded
         * ref write and update its shadow only after the guest write succeeds. */
        if (!kernel_guest_write_u32(owned_address + 4u, shadow_reference + 1u))
            refuse_locked("cached reference is not writable");
        shadow_reference++;
        if (movie) movie_references++;
    } else {
        if (singleton != 0u) refuse_locked("existing device is not owned by this facade");
        const uint32_t heap = guest_heap_create(0u, GUEST_HEAP_CHUNK_MIN, 0u);
        if (heap == 0u) { pthread_mutex_unlock(&lock); return E_OUTOFMEMORY; }
        const uint32_t address = guest_heap_alloc(heap, DEVICE_BYTES);
        if (address == 0u) {
            (void)guest_heap_destroy(heap);
            pthread_mutex_unlock(&lock);
            return E_OUTOFMEMORY;
        }
        void *mapped = kernel_guest_at(address, DEVICE_BYTES);
        if (mapped == NULL) {
            (void)guest_heap_destroy(heap);
            refuse_locked("new owned device is not mapped");
        }
        if(create_note!=NULL && !create_note(heap,address)) {
            const bool freed=guest_heap_destroy(heap);
            refuse_locked(freed?"PCM epoch rejected retained audio or inactive renderer":
                                  "PCM epoch rejected and unpublished allocation rollback failed");
        }
        /* These reference values are original observations. The omitted internal
         * references/buffers are NOT constructed by this SILENT policy. */
        const bool codec_ready = dsound_hle_codec_ready();
        owned_heap = heap;
        owned_address = address;
        shadow_reference = codec_ready ? 6u : 1u;
        uint32_t words[11];
        header_words(address, shadow_reference, words);
        memcpy(mapped, words, DEVICE_BYTES);
        memcpy(singleton_at, &address, 4u);
        if (!codec_ready) status = DSERR_NODRIVER;
    }
    if (status == 0u) {
        const uint32_t interface = owned_address + 8u;
        memcpy(output_at, &interface, 4u);
    }
    const bool announce = !announced;
    announced = true;
    pthread_mutex_unlock(&lock);
    if (announce)
        dsound_hle_log()("dsound: explicit SILENT public-device facade; omitted B4/540/APU, "
            "IRQ/DPC/timer, silent-buffer and guest critical-section internals; "
            "no audio output or DSP acknowledgement; nested fields remain unsupported\n");
    return status;
}
uint32_t dsound_device_create(uint32_t guid, uint32_t output, uint32_t outer)
{ return create_device(guid, output, outer, false); }
void dsound_device_set_movie_calls(bool enabled)
{ pthread_mutex_lock(&lock); movie_calls_enabled = enabled; pthread_mutex_unlock(&lock); }
bool dsound_device_movie_calls_enabled(void)
{
    pthread_mutex_lock(&lock);
    const bool value = movie_calls_enabled;
    pthread_mutex_unlock(&lock);
    return value;
}
uint32_t dsound_device_movie_references(void)
{
    pthread_mutex_lock(&lock);
    const uint32_t value = movie_references;
    pthread_mutex_unlock(&lock);
    return value;
}
uint32_t dsound_device_release_movie(uint32_t interface)
{
    pthread_mutex_lock(&lock);
    if (!movie_calls_enabled) refuse_locked_at(RELEASE_ENTRY, "movie device calls are disabled");
    if (shadow_reference == UINT32_MAX || !validate_owned(interface))
        refuse_locked_at(RELEASE_ENTRY, "Release argument is not the owned device interface");
    if (movie_references == 0u)
        refuse_locked_at(RELEASE_ENTRY, "no movie reference is outstanding, releasing the startup "
                                        "reference would run the original device destructor");
    /* The original destroys the device when the count would fall to DESTROY_REFERENCES - 1. Every stream
     * lease and movie reference sits above the base count, so this also keeps them. */
    if (shadow_reference - 1u < DESTROY_REFERENCES)
        refuse_locked_at(RELEASE_ENTRY, "Release would reach the original device destructor");
    if (!kernel_guest_write_u32(owned_address + 4u, shadow_reference - 1u))
        refuse_locked_at(RELEASE_ENTRY, "device reference is not writable");
    shadow_reference--;
    movie_references--;
    const uint32_t result = shadow_reference;
    pthread_mutex_unlock(&lock);
    return result;
}
static uint32_t create_handler(void *context)
{
    const kernel_call_frame *frame = context;
    uint32_t caller, arguments[3];
    if (frame == NULL || !kernel_guest_read_u32(frame->stack_ptr, &caller))
        refuse("unreadable Create frame");
    const bool startup = caller == CREATE_CALLER;
    if (!startup && !(caller == MOVIE_CREATE_CALLER && dsound_device_movie_calls_enabled()))
        refuse("only measured startup caller return 0x000279D5 is recovered");
    for (unsigned i = 0u; i < 3u; i++)
        if (!kernel_frame_arg(frame, i, &arguments[i])) refuse("unreadable Create argument");
    return create_device(arguments[0], arguments[1], arguments[2], !startup);
}
static uint32_t release_handler(void *context)
{
    const kernel_call_frame *frame = context;
    uint32_t caller, interface;
    if (frame == NULL || !kernel_guest_read_u32(frame->stack_ptr, &caller) ||
        caller != MOVIE_RELEASE_CALLER)
        refuse_at(RELEASE_ENTRY, "only the measured movie caller return 0x00445726 of device Release "
                                 "is recovered");
    if (!kernel_frame_arg(frame, 0u, &interface))
        refuse_at(RELEASE_ENTRY, "unreadable Release argument");
    return dsound_device_release_movie(interface);
}
size_t dsound_device_register(void)
{ return dsound_hle_register(CREATE_ENTRY, create_handler) ? 1u : 0u; }
size_t dsound_device_register_movie(void)
{ return dsound_hle_register(RELEASE_ENTRY, release_handler) ? 1u : 0u; }
