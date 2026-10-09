/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The NV2A pushbuffer, as D3D8 keeps it: a ring in guest memory with a cursor and a limit in
 * the device struct. NOTHING HERE TALKS TO A GPU AND NOTHING INTERPRETS WHAT IS WRITTEN.
 *
 * WHAT THE EVIDENCE SUPPORTS. D3D8 is the driver, so every entry point ends in writing NV2A
 * method headers and parameters at `[device+0]` and advancing it. MEASURED over all of
 * `.text`: the game never reads the cursor, the limit or the base (zero references to
 * 0x3E3F58, 0x3E3F60 or 0x3E3F64), and the only pointer it holds into the ring is the one
 * BeginPush returns. It does two things with it. It calls the library's primitive 0x003D6C90,
 * which appends one header and one value, and it asks `BeginPush` (0x003D6660) for a raw pointer
 * into the ring and writes a 34-dword block through it (docs/d3d8-usage.md section 9.2). So the
 * ring has to exist as writable guest memory with a real cursor, and that is all this module
 * provides.
 *
 * WHAT IS REPLACED, AND WHAT THAT MEANS. The original, when the cursor reaches the limit
 * (0x003D6B20 into 0x003D69E0), kicks the GPU through DMA_PUT, waits for DMA_GET to leave the
 * region it is about to reuse, and moves the limit. There is no GPU here, so the wait is
 * dropped: this port assumes everything before the cursor has been consumed. It keeps the
 * arithmetic that does not depend on the hardware (segment length, the 0x204-byte slack, the
 * wrap and the jump word written at the wrap point) and calls an optional consumer with the
 * range being handed off. With no consumer the handed-off bytes are simply overwritten when
 * the ring wraps, so a run longer than the ring loses the oldest commands. That is a design
 * decision for whoever builds the GPU side: install a consumer, or enlarge the ring.
 */

#ifndef TSFP_GPU_D3D8_PUSHBUFFER_H
#define TSFP_GPU_D3D8_PUSHBUFFER_H

#include <stdbool.h>
#include <stdint.h>

/* Bytes between the limit and the end of the segment. A writer checks `cursor < limit` once
 * and then writes up to this much, so no emitter in the library bounds-checks again. MEASURED:
 * 0x003D6ADD adds -0x204 to the segment end to form the limit. */
#define D3D8_PUSHBUFFER_SLACK_BYTES 0x204u

/* Where the cursor is wrapped to once the ring is exhausted is never below this margin from
 * the end (0x003D6A37 adds 0x4000 to the candidate end before comparing). */
#define D3D8_PUSHBUFFER_WRAP_MARGIN 0x4000u

/**
 * Called with the guest range [begin, end) of commands being handed to the "GPU". When the
 * hand-off is a wrap, the library's ring-jump dword is written at `end` AFTER this returns
 * and is not part of the range. Nothing else about the commands is promised.
 */
typedef void (*d3d8_pushbuffer_consumer)(void *context, uint32_t begin, uint32_t end);

/** Install (or with NULL, remove) the consumer. */
void d3d8_pushbuffer_set_consumer(d3d8_pushbuffer_consumer consumer, void *context);

/* 0x003D6530 stores the GET it read at device+0x5C and the wrap generation it belongs to at +0x60. */
#define D3D8_DEV_GET_SHADOW 0x005Cu
#define D3D8_DEV_GET_GENERATION 0x0060u

/* The eight dwords 0x003D67B0 writes at the cursor for every fence (T391). */
#define D3D8_PUSHBUFFER_FENCE_PACKET_BYTES 0x20u

/* Device flag 0x800: the refill sets 0x1000 and kicks instead of inserting a fence (0x003D6AE3). */
#define D3D8_PUSHBUFFER_FLAG_REFILL_WITHOUT_FENCE 0x800u

/**
 * The end of the original refill (0x003D6AE0 to 0x003D6B14), which is the GPU module's: with device
 * flag 0x800 set `flags |= 0x1000` and kick, otherwise insert a fence with flags 3 and kick. Called
 * after the segment end, the wrap and the limit are final. NULL (no GPU module, the unit tests of this
 * file alone) leaves the refill without the fence, and `d3d8_pushbuffer_set_consumer` is unaffected.
 */
typedef void (*d3d8_pushbuffer_refill_tail)(void);

/** Install (or with NULL, remove) the refill tail. d3d8_gpu_create installs it. */
void d3d8_pushbuffer_set_refill_tail(d3d8_pushbuffer_refill_tail tail);

/**
 * The wait of the original refill (0x003D6A8E to 0x003D6AD4, T486): has the GPU reached `fence`, given the fence
 * counter (device+0x2C) and the semaphore dword the GPU raised? It is the completion test of 0x003D6870, which is
 * all the wait does when the answer is yes (MEASURED against the original, tests/test_d3d8_refill_oracle.py: a
 * reached fence leaves every dword untouched). PURE in its three arguments (T526: a planner that carries the
 * counter and the semaphore across refills asks it with the values the earlier refills leave). A fence that is NOT
 * reached is refused by name before any write: the instantaneous GPU completes every fence at its kick, so only a
 * transplanted state has one, and the original then spins forever or stalls the GPU with ring patches.
 * Without a predicate a refill that would wait is refused as before. d3d8_gpu_attach installs it.
 */
typedef bool (*d3d8_pushbuffer_fence_reached)(uint32_t fence, uint32_t counter, uint32_t semaphore);
void d3d8_pushbuffer_set_fence_reached(d3d8_pushbuffer_fence_reached reached);

/**
 * The ring address the original reads back from DMA_PUT/DMA_GET (`[device+0x1C20]+0x40/+0x44`, its low 28
 * bits in the contiguous window, so the address itself): where the last kick put the GPU. The
 * instantaneous GPU has consumed up to it, so it is also GET. Starts at the ring base after create and
 * moves at every drain. A refill with it outside the ring is fatal, so a test that builds a ring by hand
 * sets it.
 */
uint32_t d3d8_pushbuffer_put(void);
void d3d8_pushbuffer_set_put(uint32_t put);

/**
 * 0x003D6360: allocate the ring from the sizes SetPushBufferSize stored and point the device
 * at it. Writes device+0 (cursor) and device+0x24 (base) to the new base, device+0x28 to the
 * end, device+4 to the first limit and device+0x2C to 5. False when the guest allocator
 * refuses, which the original reports as E_OUTOFMEMORY.
 *
 * The 8-byte-entry kick history the original also allocates (device+0x48) belongs to its GPU
 * wait and is not modelled.
 */
bool d3d8_pushbuffer_create(void);

/**
 * The preamble every emitter has: `if (cursor >= limit) roll over`, then the cursor. This is
 * `cmp [esi], [esi+4]; jb ...; call 0x3D6B20` at the head of each library writer, and it
 * returns the address the writer should store its first dword at.
 */
uint32_t d3d8_pushbuffer_begin(void);

/**
 * Read-only twin of d3d8_pushbuffer_begin for whole-sequence preflight. Given a simulated cursor
 * and limit, returns whether the preamble would roll over and, if so, leaves the cursor and limit
 * the roll-over would produce. Writes no guest or host state. Fatal exactly where the roll-over
 * is (no ring yet, or device flag 4), so a caller can refuse before its first write.
 */
bool d3d8_pushbuffer_preview_begin(uint32_t *cursor, uint32_t *limit);

/**
 * 0x003D6B30(dwords), the sized reservation the original draw and bulk emitters use: nothing
 * happens while cursor + dwords*4 stays below limit + 0x200, otherwise the ring rolls over with
 * both sizes raised to dwords*4 + 0x204 when that exceeds them. Returns the cursor.
 */
uint32_t d3d8_pushbuffer_reserve(uint32_t dwords);

/** Read-only twin of d3d8_pushbuffer_reserve, with the contract of d3d8_pushbuffer_preview_begin. */
bool d3d8_pushbuffer_preview_reserve(uint32_t dwords, uint32_t *cursor, uint32_t *limit);

/**
 * What a refill reads that an earlier refill of the same plan has moved (T526). The original's refill plan
 * (`0x003D6530`, `0x003D69E0`) reads GET (the last kick's put), the wrap count and distance, the fence counter, the
 * semaphore the GPU raised, and walks the second fence history; a refill leaves them changed, through its wrap, its
 * fence packet (`0x003D67B0` with flags 3: a history entry (fence, position of the packet), the counter plus 2) and
 * its kick (the put at the cursor after the fence packet, the semaphore at the last fence inserted). A simulation
 * carries them so the second and later refill of one command site, or of one planned call, is planned from what
 * the earlier ones leave. It models the GPU module's refill tail (`d3d8_gpu_attach`, MEASURED against the original
 * by tests/test_d3d8_refill_oracle.py and tests/test_d3d8_vertex_constants_refill_oracle.py). Loaded from the
 * device at the first refill the simulation plans, so a `d3d8_pushbuffer_sim` built by hand works too.
 */
#define D3D8_PUSHBUFFER_SIM_MAX_FENCES 64u

typedef struct {
    uint32_t index;
    uint32_t fence;
    uint32_t position;
} d3d8_pushbuffer_history_entry;

typedef struct {
    bool loaded;
    uint32_t put;
    uint32_t wrap_count;
    uint32_t wrap_delta;
    uint32_t fence_counter;
    uint32_t history_cursor;
    bool semaphore_known; /* the semaphore dword is read from the device only when a wait needs it */
    uint32_t semaphore_value;
    uint32_t fences; /* history entries the plan added, newest last, over the second history in guest memory */
    d3d8_pushbuffer_history_entry history[D3D8_PUSHBUFFER_SIM_MAX_FENCES];
} d3d8_pushbuffer_refill_state;

/**
 * What the LAST refill the plan made writes outside the commands of the sites, so a caller can check those spans
 * against what it reads (T525): the ring-jump word at `at` when it wraps and the fence packet at `fence_begin`.
 */
typedef struct {
    uint32_t at;
    bool wraps;
    uint32_t fence_begin;
    uint32_t fence_bytes;
} d3d8_pushbuffer_refill_spans;

/** A span of guest memory a plan writes (T546). */
typedef struct {
    uint32_t begin;
    uint32_t bytes;
} d3d8_pushbuffer_span;

/** The most separate spans a plan records (touching spans are merged: a refill adds about one). */
#define D3D8_PUSHBUFFER_SIM_MAX_SPANS 256u

/**
 * A simulated writer position, for planning a whole sequence of emitters before the first write.
 * `refills` counts roll-overs the sequence would take, `bytes` the command bytes it would write, `last_refill` the
 * spans the latest of them writes, `state` what the refills so far have moved (see above). T546: `span` lists
 * EVERYTHING the plan writes (the sites' commands, every refill's jump word and fence packet), so a caller can check
 * each against what it reads, and `flags` are the device flags a refill sees once the plan overrides them (the
 * indexed draw sets 0x800 before its reservation and clears it after; a refill's tail under 0x800 leaves 0x1000).
 */
typedef struct {
    uint32_t cursor;
    uint32_t limit;
    uint32_t refills;
    uint32_t bytes;
    d3d8_pushbuffer_refill_spans last_refill;
    d3d8_pushbuffer_refill_state state;
    bool flags_set;
    uint32_t flags;
    uint32_t span_count;
    d3d8_pushbuffer_span span[D3D8_PUSHBUFFER_SIM_MAX_SPANS];
} d3d8_pushbuffer_sim;

/** From now on the plan's refills see `flags` instead of the device's (T546). */
void d3d8_pushbuffer_sim_set_flags(d3d8_pushbuffer_sim *sim, uint32_t flags);

/** The flags the plan's next refill sees: the ones it was given, or the device's. A refill whose tail ran under flag
 * 0x800 has left 0x1000 in them. */
uint32_t d3d8_pushbuffer_sim_flags(const d3d8_pushbuffer_sim *sim);

/** Start a simulation at the device's actual cursor and limit. */
d3d8_pushbuffer_sim d3d8_pushbuffer_sim_start(void);

/**
 * Simulate one emitter site: the `if (cursor >= limit) roll over` preamble, then `bytes` of
 * commands at the resulting cursor. Returns where the commands start (after the roll-over's fence packet).
 * Fatal at `entry` when the preamble's roll-over would be fatal or the command span is not mapped guest memory.
 * Writes nothing.
 */
uint32_t d3d8_pushbuffer_sim_site(d3d8_pushbuffer_sim *sim, uint32_t entry, uint32_t bytes);

/**
 * Commands written at the cursor with NO limit check of their own (T546: the vertex-program helper 0x003D5C50 writes under
 * the sized reservation it made first, and the fog packet's preamble comes after it). Returns where they start. Fatal at
 * `entry` when the span is not mapped guest memory. Writes nothing.
 */
uint32_t d3d8_pushbuffer_sim_write(d3d8_pushbuffer_sim *sim, uint32_t entry, uint32_t bytes);

/** The sized reservation 0x003D6B30(dwords) over the simulation: the carried twin of d3d8_pushbuffer_preview_reserve. */
bool d3d8_pushbuffer_sim_reserve(d3d8_pushbuffer_sim *sim, uint32_t dwords);

/** The most runs one site may be planned in (the refills of the site, plus one). */
#define D3D8_PUSHBUFFER_MAX_RUNS 64u

/**
 * One run of a site: `bytes` at `begin`. A refill ends the run (T487, T526): it writes (when `wraps`) the
 * ring-jump word at the end of the run, then, `fence_bytes` long, the fence packet at the start of the new
 * segment, after which the next run begins. The last run has neither.
 */
typedef struct {
    uint32_t begin;
    uint32_t bytes;
    bool wraps;
    uint32_t fence_bytes;
} d3d8_pushbuffer_run;

/**
 * Where a site written in units lands. `entered`: the entry preamble rolled the ring over (the first run starts
 * after its fence packet). `runs` runs, `runs - 1` refills between them, in the order the original makes them.
 */
typedef struct {
    bool entered;
    bool split;
    uint32_t begin;
    uint32_t runs;
    d3d8_pushbuffer_run run[D3D8_PUSHBUFFER_MAX_RUNS];
} d3d8_pushbuffer_split;

/**
 * Simulate one emitter site written as `head_bytes`, then `units` equal units of `unit_bytes`, then
 * `tail_bytes`, as the vertex constants emitter 0x003D57D0 writes a packet: the entry preamble
 * (`cursor >= limit` rolls over), and after each unit that another unit follows (never after the last, and never
 * before the tail) `cmp cursor, limit` and, at or past it, the refill 0x003D6B20 with the cursor published,
 * which ends a run. Every refill, the entry roll-over's included, is planned from the state the ones before it
 * left (T526). Fills `plan`. Fatal at `entry` where the roll-overs would be, when a span is not mapped guest
 * memory, and, by name, past `D3D8_PUSHBUFFER_MAX_RUNS` runs. Writes nothing.
 */
void d3d8_pushbuffer_sim_split_site(d3d8_pushbuffer_sim *sim, uint32_t entry, uint32_t head_bytes,
                                    uint32_t unit_bytes, uint32_t units, uint32_t tail_bytes,
                                    d3d8_pushbuffer_split *plan);

/**
 * The fixed vertex constants emitter 0x003D5720 over the simulation (T579): it has no entry preamble. It checks
 * `cursor + bytes >= limit` before it writes anything and, at or past it, calls the refill with the cursor published and
 * checks again from the new cursor and limit, so one call refills any number of times before its `bytes` land. Fills
 * `plan` with one run per refill that is EMPTY (the refill is at its begin) and a last run of `bytes` after the last
 * refill's fence packet. `entered` is always false. Fatal at `entry` where the roll-overs would be, when a span is
 * not mapped guest memory, and, by name, past `D3D8_PUSHBUFFER_MAX_RUNS` runs (a kickoff so small that no refill
 * leaves room for the packet, on which the original does not return). Writes nothing.
 */
void d3d8_pushbuffer_sim_checked_site(d3d8_pushbuffer_sim *sim, uint32_t entry, uint32_t bytes,
                                      d3d8_pushbuffer_split *plan);

/**
 * The refill between two units (0x003D5880 to 0x003D588B): publish `cursor` (the end of the first run), roll the
 * ring over unconditionally and return the cursor the second run starts at. The writer must have written the
 * first run first, since the refill hands [segment start, cursor) to the consumer.
 */
uint32_t d3d8_pushbuffer_refill_at(uint32_t cursor);

/** Publish the cursor after a writer finished (`mov [esi], eax`). */
void d3d8_pushbuffer_end(uint32_t cursor);

/* Commit an emitter's captured device/cursor without reloading a mutable global
 * pointer or cursor. Accounting uses its original packet start. */
void d3d8_pushbuffer_end_at(uint32_t device, uint32_t before, uint32_t cursor);

/**
 * 0x003D6C90, the primitive the game calls with a header in ecx and a value in edx: reserve
 * eight bytes against the limit, rolling over and retrying if that would reach it, then
 * store the pair. It does not check that `header` is a valid command header, and neither
 * does the original.
 */
void d3d8_pushbuffer_emit_pair(uint32_t header, uint32_t value);

/**
 * Hand the consumer everything written since the last hand-off, [segment start, cursor), and start
 * the next range at the cursor. This is the kick: the original writes DMA_PUT with the cursor here.
 * No consumer installed means the range is simply forgotten, as a ring wrap forgets it.
 */
void d3d8_pushbuffer_drain(void);

/** Dwords written through the primitive and the emitters since the last create, for tests
 * and diagnostics. Counted by the host side, not read back out of guest memory. */
uint64_t d3d8_pushbuffer_dwords_written(void);

/** How many times the cursor reached the limit and the roll-over ran. */
uint64_t d3d8_pushbuffer_rollovers(void);

/** Forget host-side bookkeeping. Does not free the guest region. For tests. */
void d3d8_pushbuffer_reset(void);

#endif /* TSFP_GPU_D3D8_PUSHBUFFER_H */
