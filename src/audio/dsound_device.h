/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_AUDIO_DSOUND_DEVICE_H
#define TSFP_AUDIO_DSOUND_DEVICE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Explicit SILENT public-device facade; no nested APU/voice/buffer objects,
 * interrupt/DPC/timer objects, guest critical-section state or DSP acknowledgement.
 * Only GUID=NULL, outer=NULL are recovered. Output is one guest pointer. */
uint32_t dsound_device_create(uint32_t guid, uint32_t output, uint32_t outer);
size_t dsound_device_register(void);
/* Optional explicit PCM epoch observer for a newly allocated device, before
 * publication. Called under device lock; no device reentry/fatal. False
 * rolls back the unpublished allocation and reports a named refusal.
 * Configure quiescent; NULL preserves original facade policy. */
typedef bool (*dsound_device_create_note_fn)(uint32_t heap,uint32_t internal);
void dsound_device_set_create_note(dsound_device_create_note_fn note);

/* T421: the device calls the XMV GetNextFrame makes, behind --headless-movie-audio. DirectSoundCreate
 * from the XMV site (return 0x0044570B) adds one reference to the device the startup created, and device
 * Release (0x00406A8A, only from return 0x00445726) takes one such reference back and returns the new
 * count, both measured against the original (tests/test_dsound_device_calls_oracle.py). Everything
 * else stays fatal: a first-time XMV Create, a Release with no outstanding movie reference, a Release
 * that would reach the original destructor (which tears down the omitted children). Default off. The
 * Release handler is registered only by dsound_device_register_movie. */
void dsound_device_set_movie_calls(bool enabled);
bool dsound_device_movie_calls_enabled(void);
size_t dsound_device_register_movie(void);
uint32_t dsound_device_movie_references(void);
/* Release one movie reference on `interface`, the body of the Release handler. Stops the run through
 * the fatal handler when it is outside the measured scope. Returns the new reference count. */
uint32_t dsound_device_release_movie(uint32_t interface);
/* Quiescent shutdown/test reset: releases this adapter's owned heap, clears its
 * singleton only if still owned, preserves dsound_hle policy/registry/counters.
 * External guest heap resets/frees must also be quiescent. */
void dsound_device_reset(void);
/* False, with no mutation, while any child lease remains live or a live
 * device heap/header/singleton changed. A dead heap clears only host sidecar;
 * foreign replacement memory/singleton is preserved. Serial is never reset. */
bool dsound_device_reset_checked(void);
typedef void (*dsound_device_fatal_fn)(uint32_t address, const char *message);
void dsound_device_set_fatal(dsound_device_fatal_fn handler);
/* Callback executes under device mutex after complete ownership/header validation.
 * It must not call Create/reset/fatal. Return any refusal through userdata; callers
 * invoke fatal only after this function releases the mutex. Lock order is device
 * then effects. External guest heap/free/reset remains quiescent. */
typedef void (*dsound_owned_interface_fn)(uint32_t internal_address, void *userdata,
                                        uint32_t *result);
bool dsound_device_with_owned_interface(uint32_t interface, dsound_owned_interface_fn callback,
                                        void *userdata, uint32_t *result);
/* Read-only owned-device incarnation. The heap token includes its complete
 * nonwrapping generation. Identity is observation, not a lease or capability. */
typedef struct dsound_device_identity {
    uint32_t device_heap;
    uint32_t internal_address;
} dsound_device_identity;
/* Same full ownership/header validation as with_owned_interface, under the
 * device mutex. Pointer is valid only during callback; callback may COPY it.
 * No Create/reset/fatal/reentry; lock order device -> child. External mappings,
 * heap operations and content must remain quiescent during the observation.
 * Refusal preserves result and invokes no callback; handle errors after unlock.
 * This API does not allocate, write guest state or retain a reference. */
typedef void (*dsound_owned_identity_fn)(const dsound_device_identity *identity,
                                       void *userdata, uint32_t *result);
bool dsound_device_with_owned_identity(uint32_t interface, dsound_owned_identity_fn callback,
                                      void *userdata, uint32_t *result);
typedef struct dsound_device_lease {
    uint32_t device_heap;
    uint32_t internal_address;
    uint64_t serial;
} dsound_device_lease;
typedef struct dsound_device_lease_child {
    uint32_t heap;
    uint32_t address;
    uint32_t bytes;
} dsound_device_lease_child;
typedef enum dsound_device_lease_status {
    DSOUND_LEASE_OK,
    DSOUND_LEASE_INVALID,
    DSOUND_LEASE_REFERENCE_LIMIT,
    DSOUND_LEASE_SERIAL_LIMIT,
    DSOUND_LEASE_OUT_OF_MEMORY,
    DSOUND_LEASE_PREPARE_FAILED,
    DSOUND_LEASE_WRITE_REFUSED
} dsound_device_lease_status;
/* All callbacks run under the device mutex. prepare acquires a child mutex and
 * retains it until abort/finalize. abort is called even when prepare fails and
 * must clean partially prepared state. Acquire prepares an unpublished allocation;
 * release only validates/prepares cleanup, without freeing or changing it yet.
 * finalize cannot fail: no allocations, fallible guest operations, fatal calls,
 * Create/reset or reentry. Publication comes last. All writable spans must have
 * been probed in prepare. Lock order is device -> child. External mappings and
 * writes/frees to owned state must remain quiescent throughout the transaction.
 * A same-byte writable probe is a write that preserves the original bytes.
 * Refusal leaves the host output token unchanged; errors are handled after unlock.
 * Release consumes the lease but does not free the child. After success, the
 * caller reclaims its detached private allocations and checks cleanup results
 * outside the device mutex; this API does not claim atomic child cleanup.
 * Tokens are ownership identities, not security capabilities. */
typedef struct dsound_device_lease_ops {
    bool (*prepare)(const dsound_device_lease *candidate, void *userdata,
                    dsound_device_lease_child *child);
    void (*abort)(void *userdata);
    void (*finalize)(const dsound_device_lease *committed, void *userdata);
} dsound_device_lease_ops;
dsound_device_lease_status dsound_device_acquire_lease(uint32_t interface,
    const dsound_device_lease_ops *ops, void *userdata, dsound_device_lease *output);
dsound_device_lease_status dsound_device_release_lease(const dsound_device_lease *lease,
    const dsound_device_lease_ops *ops, void *userdata);
#endif
