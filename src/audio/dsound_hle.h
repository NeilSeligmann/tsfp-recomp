/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DirectSound HLE: the audio boundary.
 *
 * ====================== WHY THIS IS A BOUNDARY AT ALL ======================
 *
 * The Xbox has no user-mode audio DLL. DSOUND.lib is statically linked into the
 * title, so DirectSound is not something the guest imports -- it is code inside
 * the guest, in its own named section. There is no import table to intercept.
 * The boundary is therefore a set of ADDRESSES, and replacing it means watching
 * for control transfers from game `.text` into the `DSOUND` section.
 *
 * ========================= WHAT IS MEASURED =========================
 *
 * `tools/gen_d3d8_surface.py` recovered the surface from the retail executable by
 * scanning `.text` for `call rel32` / `jmp rel32` and bucketing targets by section:
 *
 *     DSOUND   39 distinct functions, 80 call sites, 0 implemented.
 *     section  VA 0x004067C0 .. 0x00412D14
 *
 * Those 41 entries and their site counts are reproduced in `dsound_hle.c`. They are
 * committed data about a specific executable, in the same sense that the kernel
 * ordinal table is: derived once, cheap to re-derive, and useless to recompute at
 * runtime. `dsound_hle_crosscheck()` exists so the generated table can be diffed
 * against this one when it is present, because the failure mode of duplicated
 * measured data is silent drift.
 *
 * KNOWN DISCREPANCY, NOT RESOLVED HERE. `context.md` section 2 tabulates DSOUND as
 * 39 functions / **79** sites; the generated `src/xbox/xdk_surface.c` header
 * comment says **80**, and its 39 DSOUND rows sum to exactly 80. This file follows
 * the generated table, because that is the artefact the generator actually emitted
 * and its rows are self-consistent. The one-site difference is unexplained and is
 * flagged rather than quietly reconciled. (`context.md` section 2 is stale in the
 * same direction for D3D, 360 vs 363, and XNET, 152 vs 153, so the likeliest
 * explanation is an older generator run, not a bug in either count.)
 *
 * ================== WHAT THE BYTES SAY ABOUT THE TABLE ==================
 *
 * Checked directly against the executable rather than assumed, because the brief's
 * warning that the counts are upper bounds deserves an answer and not a shrug:
 *
 *   - ALL 39 DSOUND TARGETS CARRY AN `.XTLID` NAME. Not one is unnamed. Compare
 *     D3D, where 74 of 85 are unnamed. An `.XTLID` name is a symbol START address,
 *     so a false-positive `E8` -- which lands wherever the scanner's byte happened
 *     to decode -- cannot acquire one. For DSOUND the naming IS the filter, and it
 *     passes 39 of 39.
 *   - EVERY ONE OF THE 39 BEGINS AT A PLAUSIBLE FUNCTION ENTRY. Thirty start with a
 *     real prologue (`mov eax,[esp+4]` loading `this`, `push ebp; mov ebp,esp`, or a
 *     leading argument push). The other nine start with `E9`, a five-byte
 *     `jmp rel32` -- they are tail-jump thunks, not function bodies:
 *
 *         0x00407B14 -> 0x004078E5   IDirectSoundStream_SetVolume
 *         0x00407B19 -> 0x00407937   IDirectSoundStream_SetHeadroom
 *         0x00407B1E -> 0x00407989   IDirectSoundStream_SetMixBinVolumes
 *         0x00407B23 -> 0x0040749B   IDirectSoundStream_Pause
 *         0x004085CF -> 0x0040827F   IDirectSoundStream_SetFrequency
 *         0x004085D4 -> 0x004082D1   IDirectSoundStream_SetMixBins
 *         0x00408632 -> 0x00408442   IDirectSoundStream_SetRolloffCurve
 *         0x00408637 -> 0x0040849C   IDirectSoundStream_SetI3DL2Source
 *         0x00408C2D -> 0x00408BBB   IDirectSoundStream_SetFormat
 *
 *     NONE of those nine destinations appears anywhere in the surface table, which
 *     is consistent and expected: the scan counts transfers out of game `.text`, and
 *     a thunk's own jump originates inside DSOUND. It matters for two reasons.
 *     First, the thunk address is the correct dispatch point, because it is what
 *     game code actually calls. Second, when these are eventually implemented the
 *     body is somewhere else, so address-to-semantics is not one-to-one and nobody
 *     should expect the disassembly at a thunk to explain anything.
 *
 * So the entries are not doubted on existence grounds. What is doubted is the
 * COUNTS, and in the opposite direction to the brief's warning -- see
 * `DSOUND_SURFACE` in `dsound_hle.c` for the specific rows and the reasoning.
 *
 * ===================== BOUNDED NATIVE AUDIO HANDLERS =====================
 *
 * dsound_device.c registers an explicitly silent public-device facade for the
 * measured startup caller. It preserves wrapper, cache and retry behavior while
 * omitting hardware-dependent nested objects. dsound_hrtf.c selects the measured
 * function table using the calling thread's IRQL byte and genuine critical-section
 * handling. Later audio methods remain reported stubs.
 *
 * The reporting discipline is therefore the substance, and it mirrors
 * `src/xbox/kernel_hle.c` exactly:
 *   - A STUB REPORTS ONCE, by name. `DirectSoundDoWork` is called from a mixer tick;
 *     reporting per call would bury everything else in the log.
 *   - AN UNKNOWN TARGET REPORTS EVERY TIME. That is not a missing implementation,
 *     it is our table disagreeing with the binary, which is a worse problem and must
 *     not be rate-limited into invisibility.
 *   - THE REPORT IS THE BACKLOG, busiest first. Whatever logs is what remains.
 *
 * ========================= NO HOST AUDIO OUTPUT =========================
 *
 * There is no audio device in this container and this module does not open one, does
 * not mix, does not resample and does not emit a single sample anywhere. Nothing
 * here plays sound. The silent facade announces its omitted internals explicitly;
 * later unsupported methods stop the run so the trace remains an actionable backlog.
 */

#ifndef TSFP_AUDIO_DSOUND_HLE_H
#define TSFP_AUDIO_DSOUND_HLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Guest pointer width, kept distinct from a host pointer for the same reason
 * `kernel_hle.h` does it: a guest pointer is four bytes and storing a host pointer
 * in one truncates it. */
typedef uint32_t dsound_guest_ptr;

/* Bounds of the DSOUND section in the retail image, measured from its own section
 * header. Used only to sharpen the diagnostic for an unknown target: inside these
 * bounds means our table is incomplete, outside them means the caller dispatched
 * something that is not DirectSound at all. Two different bugs, two messages. */
#define DSOUND_SECTION_VA_BEGIN 0x004067C0u
#define DSOUND_SECTION_VA_END 0x00412D14u

/** How many functions the measured surface has. */
#define DSOUND_FUNCTION_COUNT 41u

/** How many call sites the measured surface has, summed over all 39 functions. */
#define DSOUND_SITE_COUNT 83u

/**
 * A DirectSound function implementation.
 *
 * Arguments are not modelled, for the same reason `kernel_fn` does not model them:
 * the call boundary comes from decompiled guest code which has its own view of each
 * signature, and inventing a uniform one here would be a guess to unpick later.
 * `context` is reserved for the per-call state that boundary will supply.
 */
typedef uint32_t (*dsound_fn)(void *context);

typedef enum {
    /* Not in the measured table: calling it means the table and the binary
     * disagree. Never a legitimate state for one of the 39. */
    DSOUND_ENTRY_ABSENT = 0,
    /* A stub that reports itself once and returns its default. */
    DSOUND_ENTRY_STUB,
    /* A real implementation. There are currently none. */
    DSOUND_ENTRY_IMPLEMENTED,
} dsound_entry_state;

typedef struct {
    /* Guest VA of the call target. The table is keyed by this because the
     * executable carries no import entry for any of them. */
    uint32_t address;
    /* `.XTLID` name, or NULL if the image never named it. All 39 are named today;
     * the field is nullable because a regenerated table on another image may not be
     * so lucky, and a NULL that crashes the log is a bad way to find out. */
    const char *name;
    /* Measured call sites in game `.text`. The static work ranking. */
    uint32_t sites;
    dsound_fn handler;
    dsound_entry_state state;
    uint32_t default_return;
    /* Times called this run. This is the observed ranking, which supersedes
     * `sites` the moment it is nonzero. */
    uint64_t call_count;
    bool reported;
} dsound_entry;

/** Reset every entry to an unreported stub, and reset the codec and DSP state. */
void dsound_hle_init(void);

/**
 * Register a real implementation for a measured address.
 *
 * False for an address not in the table, because registering against one would mean
 * either the table or the caller is wrong and accepting it silently hides that.
 */
bool dsound_hle_register(uint32_t address, dsound_fn handler);

/** Set what a stub at `address` returns. Defaults to 0. False for an unknown address. */
bool dsound_hle_set_default_return(uint32_t address, uint32_t value);

/** Look up an entry by EXACT address, or NULL. Never returns a neighbour. */
const dsound_entry *dsound_hle_entry(uint32_t address);

/** The measured table in its declared order, for callers that want to walk it. */
const dsound_entry *dsound_hle_table(size_t *out_count);

/**
 * Invoke the DirectSound function at `address`.
 *
 * A stub reports itself once by name, then returns its default. An address absent
 * from the table reports EVERY time, because it means our surface and the binary
 * disagree and that must not be rate-limited away.
 */
uint32_t dsound_hle_call(uint32_t address, void *context);

/** How many of the 39 have real implementations. */
size_t dsound_hle_implemented_count(void);

/** How many distinct stubs have been called at least once this run. */
size_t dsound_hle_touched_count(void);

/** How many calls landed on an address absent from the table. */
uint64_t dsound_hle_unknown_call_count(void);

/**
 * Write the backlog, busiest first.
 *
 * ORDERING, BECAUSE IT IS THE WHOLE POINT OF THE REPORT. Primary key is runtime
 * `call_count` descending; ties break on measured `sites` descending; remaining ties
 * break on address ascending so the output is deterministic. Runtime observation
 * dominates a static estimate because it is a fact about this run rather than an
 * upper bound over the whole image -- so before the guest has called anything the
 * report is exactly the static ranking, and after a trace it is the observed one,
 * with the static count still ordering everything the trace never reached.
 */
void dsound_hle_report(void);

/* ===================== READINESS: THE MANDATORY PART =====================
 *
 * Upstream measured this on two separate titles and it is not a diagnostic nicety.
 * When the AC97 codec never reports ready, `DirectSoundCreate` FAILS, the title
 * carries the NULL device forward, and something downstream dereferences it. Both
 * Burnout 3 and Black fault at guest VA 0xFFFFFFEC for that single reason, and on
 * Wreckless the same failure made the title skip its entire engine init -- which
 * presents as "runs, never draws" and sends you hunting in the renderer.
 *
 * So a DirectSound HLE that never signals readiness does not produce silence. It
 * produces a CRASH, a long way from its cause, in a subsystem that is not audio.
 *
 * THE LIBRARY DEFAULT IS NOT READY (the host boot default is READY since T375, see
 * host_options.h `ac97_ready`; `--no-ac97-ready` restores this). We have no AC97 codec, no audio
 * device, and no measurement of what this title does with a ready one. Defaulting to
 * ready would fabricate the single most consequential bit in the subsystem silently,
 * and this codebase's standing rule -- set by `--stub-status`, `--mount` and
 * `ExQueryNonVolatileSetting`'s zero-fill policy -- is that every fabrication is a
 * flag and announces itself. A default that fabricates is the one shape that rule
 * forbids.
 *
 * NOT READY IS ONLY HONEST IF IT IS NOT ALSO A MYSTERY. The cost of the honest
 * default is a fault at 0xFFFFFFEC, so the first time the guest asks whether the
 * codec is ready and the answer is no, this module says so, names 0xFFFFFFEC as the
 * fault to expect, and names the switch that avoids it. The operator then gets a
 * signposted crash instead of an unexplained one, which is strictly better than
 * either a lie or a silence.
 */

/** The guest VA two upstream titles fault at when a NULL DirectSound device is used.
 *
 * AMBIGUOUS, and learned the hard way on this title: 0xFFFFFFEC is `0 - 0x14`, the shape of
 * ANY null-based access at a -5 dword index. Here it was `[fs:[4] + (-5)*4]` with fs:[4]
 * unpublished (kernel_thread_publish_tls_end), nothing to do with audio, and it had been
 * attributed to this module's failure for a while. Treat a fault at this address as a
 * hypothesis to check with gdb, not a diagnosis. */
#define DSOUND_NULL_DEVICE_FAULT_VA 0xFFFFFFECu

typedef enum {
    /* The honest default: we have no codec and do not claim one. */
    DSOUND_CODEC_NOT_READY = 0,
    /* FABRICATED. Announces itself when set. Equivalent to upstream's
     * RECOMP_AC97_READY=1, named after it so the two runtimes' logs line up. */
    DSOUND_CODEC_READY,
} dsound_codec_state;

/**
 * Set the codec readiness, announcing the change once.
 *
 * Setting READY prints a FABRICATED banner naming what is being faked and why,
 * because a run log that does not record this is unreadable: readiness changes
 * which branch the title's whole audio init takes.
 */
void dsound_hle_set_codec_state(dsound_codec_state state);

/** AC97 global status register, primary-codec-ready bit (bit 8). Readiness is this bit of
 * register state (MMIO offset 0x130 of the AC97 block), set by the model and read by the
 * readiness poll. FABRICATED until a xemu-level reference exists (T375). */
#define DSOUND_AC97_GLOBAL_STATUS_PRIMARY_CODEC_READY 0x00000100u

/** The modelled AC97 global status register value. */
uint32_t dsound_hle_ac97_global_status(void);

/** The current readiness. */
dsound_codec_state dsound_hle_codec_state(void);

/**
 * Ask whether the codec is ready, as the guest-facing path would.
 *
 * The first NOT_READY answer reports the expected 0xFFFFFFEC fault and the switch
 * that avoids it. Later NOT_READY answers are silent -- this can be polled.
 */
bool dsound_hle_codec_ready(void);

/** How many times readiness was queried while NOT_READY. */
uint64_t dsound_hle_not_ready_query_count(void);

/* ============ THE DSP COMMAND-WORD ACKNOWLEDGEMENT: NOT GUESSED ============
 *
 * There is no DSP56300 here, so nothing clears the command word DirectSound posts
 * and the guest spins on it forever. Upstream acknowledges it at DirectSound's
 * scratch allocation + 0x810, and has four titles at that same offset against three
 * different bases (Wreckless 0x014F8810, Burnout 3 0x825F8810, Black 0x83218810).
 *
 * THE OFFSET FOR THIS TITLE IS UNKNOWN AND IS NOT GUESSED HERE. The constant that
 * travels between those titles is the +0x810, but the BASE is a per-title scratch
 * allocation and there is no reason to expect ours to match any of the three.
 * Writing a zero to a wrong guest address is not a partial fix, it is a silent
 * memory corruption in whatever does live there.
 *
 * It is therefore explicit configuration with no default, and the report says
 * NOT CONFIGURED until something supplies it. Derive it from the binary when the
 * guest first spins: find the read-compare-loop on a DSOUND-allocated address, and
 * `dsound_hle_set_dsp_ack()` takes the base and the offset separately so that if
 * ours really is +0x810 the artefact records that as a confirmation of upstream's
 * constant rather than burying it in one summed address.
 */

/** Sentinel for "no DSP acknowledgement address has been derived". */
#define DSOUND_DSP_ACK_UNSET UINT32_MAX

/** The offset upstream measured in all four of its titles. Ours is NOT assumed to match. */
#define DSOUND_DSP_ACK_UPSTREAM_OFFSET 0x810u

/**
 * How a DSP acknowledgement reaches guest memory.
 *
 * A callback rather than a direct `guest_mem` call so this module links standalone:
 * the test suite points it at a local buffer and needs no guest address space, and
 * the host points it at the real one. It also means an ack can be observed, which
 * is how the suite proves the unconfigured case writes nothing at all.
 */
typedef void (*dsound_dsp_write_fn)(uint32_t guest_address, uint32_t value, void *user);

/** Install the writer an acknowledgement goes through. NULL disables acknowledging. */
void dsound_hle_set_dsp_writer(dsound_dsp_write_fn writer, void *user);

/**
 * Supply the derived acknowledgement address as base + offset.
 *
 * False if the sum would be outside a 32-bit guest address, or if `base` is the
 * unset sentinel. Both halves are retained for the report so the artefact records
 * what was derived, not just what it added up to.
 */
bool dsound_hle_set_dsp_ack(uint32_t scratch_base, uint32_t offset);

/** True once an acknowledgement address has been derived. */
bool dsound_hle_dsp_ack_configured(void);

/** The derived acknowledgement address, or DSOUND_DSP_ACK_UNSET. */
uint32_t dsound_hle_dsp_ack_address(void);

/**
 * Clear the DSP command word once, standing in for hardware that does not exist.
 *
 * Returns false and reports ONCE when no address has been derived, naming what has
 * to be measured. It deliberately does not fall back to upstream's +0x810 against a
 * guessed base: see the block comment above.
 */
bool dsound_hle_ack_dsp_command(void);

/** How many acknowledgements were written. */
uint64_t dsound_hle_dsp_ack_count(void);

/** How many acknowledgements were refused for want of a derived address. */
uint64_t dsound_hle_dsp_ack_refused_count(void);

/* ===================== CROSS-CHECK AGAINST THE GENERATOR ===================== */

/**
 * One row of an externally supplied surface table.
 *
 * Structurally identical to `xdk_surface_entry` minus the section string, and
 * deliberately a separate type: `src/xbox/xdk_surface.c` is generated from the
 * user's own executable and is not committed, so a compile-time dependency on it
 * would mean this module and its tests could not build in a fresh clone.
 */
typedef struct {
    uint32_t address;
    const char *name;
    uint32_t sites;
} dsound_surface_ref;

/**
 * Diff an externally supplied DSOUND surface against the table compiled in here.
 *
 * Returns the number of disagreements and logs each one: a row we do not have, a row
 * we have that is missing from `refs`, a differing site count, a differing name. The
 * measured surface is duplicated between the generated table and this file, and the
 * failure mode of duplicated measured data is SILENT DRIFT -- so there is a function
 * whose whole job is to make the drift loud. Zero means the two agree exactly.
 */
unsigned dsound_hle_crosscheck(const dsound_surface_ref *refs, size_t count);

/* ===================== DIAGNOSTIC SINK ===================== */

/** A printf-style diagnostic sink. */
typedef int (*dsound_log_fn)(const char *format, ...);

/**
 * Redirect diagnostics. Defaults to stderr; tests use this to capture output.
 *
 * A sink of its own rather than a link against `kernel_hle_log()`, so that this
 * module builds and tests without the kernel layer. The host should point it AT
 * `kernel_hle_log()` during start-up, because two separate sinks means a reader who
 * captures one still misses the other, and a diagnostic nobody reads is the exact
 * failure this reporting discipline exists to prevent.
 */
void dsound_hle_set_log(dsound_log_fn printer);

/** The current sink. Never NULL. */
dsound_log_fn dsound_hle_log(void);

/* Safety policy only: these measured stream, buffer and listener startup boundaries require a registered
 * implementation even under continue-on-missing. This registers no handlers. */
bool dsound_hle_requires_implementation(uint32_t address);
#endif /* TSFP_AUDIO_DSOUND_HLE_H */
