/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The GPU, as D3D8's own code experiences it: a consumer that drains the pushbuffer, a fence
 * semaphore the library polls, and a vertical blank. THERE IS NO NV2A AND NOTHING HERE INTERPRETS
 * A COMMAND. This is the model docs/d3d8-usage.md section 14 describes, and it is deliberately the
 * simplest one that lets every wait the library issues end.
 *
 * AN INSTANTANEOUS GPU. The ring is real guest memory the library and the title write into. When
 * the library kicks (0x003D6690 writes DMA_PUT) or waits, `d3d8_gpu_kick` hands the range
 * [consumed, cursor) to the consumer, which RECORDS it (method and data, bounded and counted) for a
 * future backend, and then every fence the library has inserted is complete. The completion is the
 * one value the library polls: the dword at `[device+0x30]` (the first dword of the 0x60-byte control
 * block 0x003DACB0 allocates), which on hardware the GPU writes when it executes
 * BACK_END_WRITE_SEMAPHORE_RELEASE (method 0x1D70) and which the library compares against its own
 * fence counter at `[device+0x2C]`. MEASURED under the oracle: the original ends CreateDevice with
 * the counter 7 and the dword 3, and each fence the stream releases raises the dword to that fence's
 * value. THE TITLE POLLS NOTHING: it holds no reference into D3D8's BSS beyond the three words in
 * d3d8_guest.h, so this semaphore is observed only by library code, all of which is replaced.
 *
 * WHAT THE RECORDED STREAM IS NOT. Only the pairs the TITLE pushes through 0x003D6C90 and the blocks
 * it writes through BeginPush are in the ring, plus the draw cascade's packets (T368) and, since T440,
 * Clear's commands. The rest of the library's own emitters are not written by their ports
 * (docs/d3d8-usage.md section 13.5, principle P), so the recorded stream is a strict subset of
 * what a console would have run. The fence packet (0x003D67B0, T391) is NOT among the missing: it is written at every
 * fence and recorded, its first command on subchannel 5.
 * `d3d8_gpu_stats` therefore counts library emissions the ports elided, per kick, so the size of the
 * gap is a number and not a caveat.
 *
 * A VIRTUAL VERTICAL BLANK. There is no timer thread. The blank advances at exactly one place, the
 * wait the library issues for it (0x003D3550: clear the event's SignalState, then
 * KeWaitForSingleObject on the event at device+0x1DBC), and at each present. Advancing it signals
 * the event through the kernel module, so the wait is satisfied by the same KeSetEvent a real
 * DPC would make. The ISR (0x003DC0E0) and DPC (0x003DCA90) themselves are not run: what they would
 * update beyond the event is announced where it is skipped.
 */

#ifndef TSFP_GPU_D3D8_GPU_H
#define TSFP_GPU_D3D8_GPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Device fields this module owns (device-relative). All MEASURED from the original's code. */
#define D3D8_DEV_FENCE 0x002Cu         /* the next fence value, +2 per fence (starts at 5) */
#define D3D8_DEV_SEMAPHORE 0x0030u     /* pointer to the dword the GPU raises (control block) */
#define D3D8_DEV_HISTORY_CURSOR 0x0034u
#define D3D8_DEV_HISTORY_MASK 0x0038u
#define D3D8_DEV_FENCE_STALL 0x003Cu
#define D3D8_DEV_HISTORY_RING 0x0048u  /* pointer to the second history: 8-byte entries */
#define D3D8_DEV_HISTORY_TABLE 0x0064u /* 64 entries of (fence, ring address), 8 bytes each */
#define D3D8_DEV_CONTROL_BLOCK 0x2488u
#define D3D8_DEV_VBLANK_EVENT 0x1DBCu  /* the KEVENT the display wait uses */
#define D3D8_DEV_FENCE_EVENT 0x1DCCu   /* the KEVENT 0x003D6870 waits on when the GPU is far behind */

/* The bounded recording. Past the capacity commands are dropped and counted, never overwritten. */
#define D3D8_GPU_STREAM_CAPACITY 65536u
/* Host recording storage cap, not a guest/NV2A memory limit. Opt-in only. */
#define D3D8_GPU_STREAM_MAX_CAPACITY 4194304u

/* The NV2A method header (MEASURED from every header in the ring after the original CreateDevice and from
 * the fence packet 0x003D67B0 writes): bits 2-12 the method, bits 13-15 the SUBCHANNEL, bits 18-28 the
 * count, bit 30 a non-incrementing run. CreateDevice binds objects to subchannels 0 (handle 0xD), 1 (0xE),
 * 2 (0x10), 3 (0x11) and 5 (0x12) with `SET_OBJECT` (method 0). Every 3D method the library's emitters
 * write is on subchannel 0 (INFERRED to be the 3D class's: the method numbers are the 3D class's, nothing
 * measured names the class), so subchannel 0 is the one the decoder replays. Subchannel 5 carries the
 * fence's software method 0x310, written by 0x003D67B0 as its first command. */
#define D3D8_GPU_SUBCHANNEL_3D 0u
#define D3D8_GPU_SUBCHANNEL_SOFTWARE 5u
#define D3D8_GPU_METHOD_FENCE_NOTIFY 0x0310u

typedef struct {
    uint32_t method;
    uint32_t data;
    uint32_t subchannel; /* header bits 13-15, 0 for every 3D command */
} d3d8_gpu_command;

typedef struct {
    uint64_t kicks;               /* d3d8_gpu_kick calls */
    uint64_t fences_inserted;
    uint64_t fence_waits;         /* 0x003D6870 calls, satisfied or not */
    uint64_t fence_waits_blocked; /* waits that needed the instantaneous completion */
    uint64_t vblanks;
    uint64_t dwords_consumed;     /* raw dwords handed to the consumer */
    uint64_t commands_recorded;
    uint64_t stream_growths, stream_commands_peak;
    uint64_t stream_limit_failures, stream_allocation_failures;
    uint64_t commands_dropped;    /* recording full */
    uint64_t commands_discarded;  /* released by d3d8_gpu_stream_discard (T84a), still counted recorded */
    uint64_t malformed_dwords;    /* a header whose count ran past its range, or a jump */
    uint64_t emissions_elided;    /* library emitters a port skipped, see d3d8_gpu_note_elided */
    uint64_t clear_surfaces;      /* CLEAR_SURFACE (0x1D94) commands recorded, one per clipped Clear rectangle (T440) */
    uint64_t commands_other_subchannel; /* recorded commands on a subchannel other than 0, never replayed (T391) */
} d3d8_gpu_stats;

/* How many distinct original functions' skipped emissions are tracked by address. */
#define D3D8_GPU_ELIDED_TABLE 48u

/** Forget host-side state (the recording, the counters). The guest side is CreateDevice's. */
void d3d8_gpu_reset(void);

/** How many times d3d8_gpu_reset has run (CreateDevice calls it). Not cleared by the reset, so a
 * holder of a model decoded from the recording (d3d8_swap_replay, T84a3) can tell it went stale. */
uint64_t d3d8_gpu_reset_count(void);

/**
 * The GPU's part of 0x003DACA0 and 0x003D6360: the control block (a contiguous allocation of 0x60
 * bytes, 4 KiB aligned, protect 4, the first of the original's allocations), its semaphore dword at
 * 3, the second history ring and its mask, the two KEVENTs, and the consumer on the ring. False
 * when an allocation is refused. Must run before `d3d8_pushbuffer_create` to preserve allocation order.
 */
bool d3d8_gpu_create(void);

/**
 * Put the consumer and the refill tail on the ring, which is the last step of d3d8_gpu_create. For a harness
 * that transplants a captured device and ring (the original-vs-port runners) and so cannot run the create.
 */
void d3d8_gpu_attach(void);

/**
 * 0x003D6690: the kick. Hands the unconsumed ring range to the consumer, advances GET to PUT and
 * completes every fence inserted so far. Fatal for a device flagged as using an alternate put.
 */
void d3d8_gpu_kick(void);

/**
 * 0x003D67B0: insert a fence. Returns its value (the fence counter before the insertion). Writes the
 * eight-dword packet at the cursor (T391: the software method 0x310 on subchannel 5 carrying the cursor,
 * the fence and the wrap count, SEMAPHORE_RELEASE 0x1D70 with the fence value, two colour clear value
 * writes of 0), advances the cursor by 0x20, records the (fence, ring address) pair in the 64-entry
 * history, in the second history when `flags` bit 0 is set, adds 2 to the counter, clears the stall word
 * and kicks unless `flags` bit 1 is set.
 */
uint32_t d3d8_gpu_fence_insert(uint32_t flags);

/**
 * 0x003D6870: wait until the GPU has passed `fence`. Returns at once when it already has. When it has
 * not, a fence equal to the counter is inserted first (the original does the same), the GPU is
 * kicked and so completes it. Fatal if the fence still is not reached, which only a fence that was
 * never inserted can cause. The original also patches the ring with stall commands while it waits for a
 * fence the GPU is far from (T486 MEASURED: method 0x100 words over the fence packet's second clear value,
 * and the first as well unless mode bit 8 is set), and calls a user callback at device+0x19FC (fatal if one is
 * installed). The instantaneous GPU is never behind, so the patches are NOT written (not even counted): a
 * fence it has not reached is one nothing kicked, which the original would spin on forever. The refill's
 * wait on the second history (T486) only asks whether the fence is reached, through
 * d3d8_pushbuffer_set_fence_reached, and refuses when it is not.
 */
void d3d8_gpu_fence_wait(uint32_t fence, uint32_t mode);

/**
 * 0x003D3550: the wait for the next vertical blank. Clears the event's SignalState, advances the
 * virtual vblank (which signals the event through KeSetEvent) and waits through KeWaitForSingleObject.
 */
void d3d8_gpu_wait_vblank(void);
/* T696: one blank's derivable device effects (clock frame floor, coupled helper run, overlay retire) with no
 * event pair, for a blank modelled outside a wait. False when the effects are off or nothing was applied. */
bool d3d8_gpu_model_blank(void);
/* Trusted observer, default NULL. Called only after clock/event/wait return,
 * outside their locks. Completion requires implemented145/159, successful clock
 * frame, KeSetEvent previous SignalState0 (LONG, not NTSTATUS), and wait success.
 * Configure quiescently; GPU reset deliberately preserves observer configuration. */
typedef void (*d3d8_vblank_wait_observer)(bool completed);
void d3d8_gpu_set_vblank_observer(d3d8_vblank_wait_observer observer);

/** The virtual vblank counter. */
uint64_t d3d8_gpu_vblank_count(void);

/**
 * A port skipped the commands the original function at `address` writes into the ring. Counted per
 * address, so "which commands the recorded stream is missing" is a table and not a caveat. The dword
 * counts are not kept here because they depend on the branches taken: the harness measures them
 * (tools/d3dscan/port_diff.py, the stream column).
 */
void d3d8_gpu_note_elided(uint32_t address);

/** How many times the emission of `address` was skipped. 0 for an address never noted. */
uint64_t d3d8_gpu_elided_count(uint32_t address);

/** The recording, oldest first. `d3d8_gpu_stream_count` is how many are held. */
/** Startup-only opt-in host storage limit. Default 65536; range 65536..4194304.
 * Refuses after any recorded/dropped command. Persists through CreateDevice/reset;
 * reset frees grown storage and starts at the static 65536 capacity again. Existing
 * indices and refusal on any dropped command remain intact. No thread/payload pin. */
bool d3d8_gpu_set_stream_limit(size_t limit);
size_t d3d8_gpu_stream_capacity(void);
size_t d3d8_gpu_stream_limit(void);
size_t d3d8_gpu_stream_count(void);
d3d8_gpu_command d3d8_gpu_stream_at(size_t index);

/**
 * T262. Called by the consumer after every handed-off range has been recorded, at the kick, with guest
 * memory exactly as it stands when the instantaneous GPU is handed the range. Default NULL (nothing
 * happens). Installed by d3d8_swap_replay when it is enabled so it can decode, and snapshot the vertex
 * bytes of, each draw at the moment the GPU runs it. NOT cleared by d3d8_gpu_reset: the observer
 * is configuration, not recording state. The observer may read the recording but must not kick.
 */
typedef void (*d3d8_gpu_recorded_observer)(void);
void d3d8_gpu_set_recorded_observer(d3d8_gpu_recorded_observer observer);

/**
 * Release the first `count` recorded commands (all of them when it is larger) so a consumer that
 * has decoded them can keep the bounded recording from filling: the rest move to the front, so every
 * index a consumer holds drops by `count`. Only the swap replay (d3d8_swap_replay.h) calls it, and
 * only when enabled, so nothing changes by default.
 */
void d3d8_gpu_stream_discard(size_t count);

/**
 * Register the handlers for the library's internal GPU functions (0x003D6690, 0x003D67B0, 0x003D6870,
 * 0x003D3550). None is in the title's measured surface, so in production d3d8_hle_register skips
 * them. The measurement harness adds their rows so it can call them one at a time.
 */
size_t d3d8_gpu_register(void);

/** The counters. */
d3d8_gpu_stats d3d8_gpu_get_stats(void);

#endif /* TSFP_GPU_D3D8_GPU_H */
