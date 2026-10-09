/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See dsound_hle.h for why the reporting matters more than the dispatch, why the
 * codec default is NOT_READY, and why no DSP acknowledgement offset is guessed.
 */

#include "dsound_hle.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * THE MEASURED SURFACE.
 *
 * Thirty-nine rows recovered by `tools/gen_d3d8_surface.py` from the retail
 * executable, reproduced from the generated `src/xbox/xdk_surface.c`. Declared in
 * the generator's own order -- site count descending, address ascending within a
 * count -- so a diff against the generated file is a plain diff.
 *
 * Not read from the generated table at build time ON PURPOSE: that table is derived
 * from the user's own executable and is gitignored, so depending on it would mean
 * this module and its tests could not build in a fresh clone. `dsound_hle_crosscheck()`
 * is the guard against the drift that duplication invites.
 *
 * ============== WHICH ROWS TO DOUBT, AND IN WHICH DIRECTION ==============
 *
 * The brief's warning is that naive E8/E9 scanning over-counts and that a
 * single-site row deserves suspicion. For DSOUND specifically that warning mostly
 * does not bite, and the honest answer is that the risk here runs the other way.
 *
 * WHY OVER-COUNTING IS UNLIKELY FOR THIS SECTION. Every one of the 41 addresses is
 * an `.XTLID` symbol start, and every one begins at a real function entry -- 30 with
 * a prologue and 9 with a five-byte `jmp rel32` thunk (listed in the header). A
 * false-positive `E8` lands at whatever byte the scanner's misdecode produced; it
 * does not land on a named symbol boundary 39 times out of 39 (now 39 of 41 named;
 * the 2 rows added by the corrected scanner are thunk targets and unnamed). Contrast D3D, where
 * 74 of 85 rows are unnamed and the clustering argument is all there is.
 *
 * WHY UNDER-COUNTING IS THE REAL RISK. Two mechanisms, both already documented in
 * this codebase against other measurements:
 *   1. INDIRECT CALLS ARE INVISIBLE. An `E8`/`E9` scan sees `call rel32` only. Any
 *      site reaching DirectSound through a function pointer, a vtable, or a
 *      jump-table dispatch contributes nothing to any row and could also hide an
 *      entire function from the table.
 *   2. TAIL-JUMP THUNKS DEFEAT SITE ATTRIBUTION. `kernel_config.h` documents
 *      `callsites.py` losing ALL TWELVE sites of ordinal 24 because each went
 *      through a stub whose body is one `jmp`. Nine of the rows below ARE such
 *      thunks, so any game-side wrapper of the same shape is a blind spot of exactly
 *      the kind that has already cost this project a measurement once.
 *
 * THE SPECIFIC ROWS I DOUBT, with the reason rather than a blanket caveat:
 *
 *   - `IDirectSoundBuffer_Play`, 1 site. This is the row I doubt most, and the shape
 *     of the doubt is "too low", not "not real". `Pause` has 6 sites and `Stop` has
 *     3, and no game pauses six times as often as it starts a sound. Either the
 *     title starts buffers through one shared helper -- which is one `call` to
 *     DSOUND and an arbitrary number of game-side callers, and would be the mundane
 *     explanation -- or some starts are indirect and invisible. Whichever it is, 1
 *     is not a measure of how important `Play` is, and ranking work by it would be a
 *     mistake. The report's runtime count exists for precisely this row.
 *   - `DirectSoundCreate`, 3 sites. Three direct calls to a once-per-process
 *     initialiser is odd. Benign readings: a retry arm and a failure arm, or one
 *     call compiled into two reachable tails. This is the most plausible place for
 *     an over-count among the high rows, and it matters because it is gate one of
 *     upstream's recipe -- but the FUNCTION is certainly real and certainly called,
 *     and that is what the boundary needs.
 *   - The nine `IDirectSoundStream_*` thunk rows. Their site counts are honest
 *     counts of calls to the THUNK. The bodies are shared with nothing in the table,
 *     so these rows measure reachability correctly and say nothing about the cost or
 *     complexity behind them.
 *   - `DirectSoundUseLightHRTF`, 1 site, and the other 16 single-site rows. Called
 *     once at init is exactly what most of these are for (`SetDopplerFactor`,
 *     `SetOrientation`, `DownloadEffectsImage`, `SetFormat`). A site count of 1 is
 *     the expected value for a configuration call, not a smell. I do not doubt them.
 *
 * NET: I doubt no row's existence. I doubt `Play`'s count as too low, `DirectSoundCreate`'s
 * as possibly too high, and the 80-site total by one (see the header's note on
 * `context.md` reporting 79).
 * =========================================================================== */
static const dsound_surface_ref DSOUND_SURFACE[DSOUND_FUNCTION_COUNT] = {
    {0x00407abc, "IDirectSoundBuffer_Pause", 6},
    {0x00407b40, "DirectSoundDoWork", 5},
    {0x00407b23, "IDirectSoundStream_Pause", 4},
    {0x00407b28, "IDirectSoundStream_FlushEx", 4},
    {0x00407aa4, "IDirectSoundBuffer_Stop", 3},
    {0x00407af8, "IDirectSoundBuffer_GetStatus", 3},
    {0x00407b14, "IDirectSoundStream_SetVolume", 3},
    {0x00407b1e, "IDirectSoundStream_SetMixBinVolumes", 3},
    {0x00408556, "IDirectSoundBuffer_SetPosition", 3},
    {0x004085d4, "IDirectSoundStream_SetMixBins", 3},
    {0x00408609, "IDirectSoundStream_SetPosition", 3},
    {0x00408632, "IDirectSoundStream_SetRolloffCurve", 3},
    {0x00409635, "DirectSoundCreate", 3},
    {0x00406a8a, "IDirectSound_Release", 2},
    {0x00407a64, "IDirectSoundBuffer_SetVolume", 2},
    {0x00407ad8, "IDirectSoundBuffer_SetLoopRegion", 2},
    {0x00407b19, "IDirectSoundStream_SetHeadroom", 2},
    {0x00408532, "IDirectSoundBuffer_SetMinDistance", 2},
    {0x0040858b, "IDirectSoundBuffer_SetRolloffCurve", 2},
    {0x004085d9, "IDirectSoundStream_SetMaxDistance", 2},
    {0x004093c8, "IDirectSound_CreateSoundBuffer", 2},
    {0x0040967c, "DirectSoundCreateStream", 2},
    {0x00406ab6, "DirectSoundUseLightHRTF", 1},
    {0x004079db, "IDirectSound_DownloadEffectsImage", 1},
    {0x00407a02, "IDirectSound_SetEffectData", 1},
    {0x00407a2c, "IDirectSound_SetMixBinHeadroom", 1},
    {0x00407a80, "IDirectSoundBuffer_Play", 1},
    {0x0040805b, NULL, 1},
    {0x004084f2, "IDirectSoundBuffer_SetFrequency", 1},
    {0x0040850e, "IDirectSoundBuffer_SetMaxDistance", 1},
    {0x004085af, "IDirectSoundBuffer_SetI3DL2Source", 1},
    {0x004085cf, "IDirectSoundStream_SetFrequency", 1},
    {0x004085f1, "IDirectSoundStream_SetMinDistance", 1},
    {0x00408637, "IDirectSoundStream_SetI3DL2Source", 1},
    {0x00408c0d, "IDirectSoundBuffer_SetBufferData", 1},
    {0x00408c2d, "IDirectSoundStream_SetFormat", 1},
    {0x00409109, "IDirectSound_CommitDeferredSettings", 1},
    {0x004093ec, "IDirectSound_SetDopplerFactor", 1},
    {0x00409410, "IDirectSound_SetOrientation", 1},
    {0x0040945a, "IDirectSound_SetPosition", 1},
    {0x0040af54, NULL, 1},
};

static dsound_entry entries[DSOUND_FUNCTION_COUNT];
static bool initialised;

/* AC97 global status register (MCPX offset 0x130). Codec readiness IS bit 8 of this
 * register: the title's readiness poll reads the bit, nothing returns a constant. */
static uint32_t ac97_global_status;
static dsound_codec_state codec_state;
static bool codec_state_announced;
static uint64_t not_ready_queries;
static bool not_ready_reported;

static dsound_dsp_write_fn dsp_writer;
static void *dsp_writer_user;
static uint32_t dsp_ack_base = DSOUND_DSP_ACK_UNSET;
static uint32_t dsp_ack_offset;
static uint32_t dsp_ack_address = DSOUND_DSP_ACK_UNSET;
static uint64_t dsp_ack_writes;
static uint64_t dsp_ack_refusals;
static bool dsp_ack_unconfigured_reported;

static uint64_t unknown_calls;

static int default_printer(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vfprintf(stderr, format, args);
    va_end(args);
    return written;
}

static dsound_log_fn log_printer = default_printer;

void dsound_hle_set_log(dsound_log_fn printer)
{
    log_printer = printer ? printer : default_printer;
}

dsound_log_fn dsound_hle_log(void)
{
    return log_printer;
}

/* The log must never have to decide what to do with a NULL name. Most rows are
 * named today, but a table regenerated from a different image may not be, and a
 * crash in the diagnostic layer is the worst possible place for one. */
static const char *entry_label(const dsound_entry *entry)
{
    return entry->name ? entry->name : "<unnamed in .XTLID>";
}

void dsound_hle_init(void)
{
    memset(entries, 0, sizeof(entries));
    for (size_t i = 0; i < DSOUND_FUNCTION_COUNT; i++) {
        entries[i].address = DSOUND_SURFACE[i].address;
        entries[i].name = DSOUND_SURFACE[i].name;
        entries[i].sites = DSOUND_SURFACE[i].sites;
        entries[i].state = DSOUND_ENTRY_STUB;
    }
    initialised = true;

    ac97_global_status = 0u;
    codec_state = DSOUND_CODEC_NOT_READY;
    codec_state_announced = false;
    not_ready_queries = 0;
    not_ready_reported = false;

    dsp_writer = NULL;
    dsp_writer_user = NULL;
    dsp_ack_base = DSOUND_DSP_ACK_UNSET;
    dsp_ack_offset = 0;
    dsp_ack_address = DSOUND_DSP_ACK_UNSET;
    dsp_ack_writes = 0;
    dsp_ack_refusals = 0;
    dsp_ack_unconfigured_reported = false;

    unknown_calls = 0;
}

static void ensure_initialised(void)
{
    if (!initialised) {
        dsound_hle_init();
    }
}

/*
 * EXACT-MATCH LOOKUP, LINEAR ON PURPOSE.
 *
 * Thirty-nine rows, and the table is in site-count order rather than address order,
 * so a binary search would need a second sorted index to exist at all. A linear scan
 * over 41 entries costs nothing next to the guest work around it, and it cannot
 * silently return a neighbour the way an off-by-one bound in a binary search can --
 * which is the specific failure the suite mutation-tests for, because a dispatcher
 * that answers with the adjacent function is far worse than one that answers with
 * nothing: the five-byte thunk runs mean several of these rows have a neighbour only
 * five bytes away, with completely unrelated semantics.
 */
static dsound_entry *mutable_entry(uint32_t address)
{
    ensure_initialised();
    for (size_t i = 0; i < DSOUND_FUNCTION_COUNT; i++) {
        if (entries[i].address == address) {
            return &entries[i];
        }
    }
    return NULL;
}

bool dsound_hle_register(uint32_t address, dsound_fn handler)
{
    dsound_entry *entry = mutable_entry(address);
    if (!entry || !handler) {
        return false;
    }
    entry->handler = handler;
    entry->state = DSOUND_ENTRY_IMPLEMENTED;
    return true;
}

bool dsound_hle_set_default_return(uint32_t address, uint32_t value)
{
    dsound_entry *entry = mutable_entry(address);
    if (!entry) {
        return false;
    }
    entry->default_return = value;
    return true;
}

const dsound_entry *dsound_hle_entry(uint32_t address)
{
    return mutable_entry(address);
}

const dsound_entry *dsound_hle_table(size_t *out_count)
{
    ensure_initialised();
    if (out_count) {
        *out_count = DSOUND_FUNCTION_COUNT;
    }
    return entries;
}

uint32_t dsound_hle_call(uint32_t address, void *context)
{
    dsound_entry *entry = mutable_entry(address);
    if (!entry) {
        /* NOT a missing implementation. Our surface table and the binary disagree,
         * which is a worse and differently-shaped problem, so it reports EVERY time
         * rather than once. The two messages are deliberately distinct: inside the
         * section means the table is incomplete and the fix is to regenerate or
         * extend it; outside means the caller dispatched something that is not
         * DirectSound at all and the fix is at the call site. */
        unknown_calls++;
        if (address >= DSOUND_SECTION_VA_BEGIN && address < DSOUND_SECTION_VA_END) {
            log_printer("dsound: call to UNKNOWN target %#010x -- inside the DSOUND "
                        "section but absent from our %u-entry surface table\n",
                        address, (unsigned)DSOUND_FUNCTION_COUNT);
        } else {
            log_printer("dsound: call to UNKNOWN target %#010x -- OUTSIDE the DSOUND "
                        "section (%#010x..%#010x); this is not a DirectSound address\n",
                        address, (unsigned)DSOUND_SECTION_VA_BEGIN,
                        (unsigned)DSOUND_SECTION_VA_END);
        }
        return 0;
    }

    entry->call_count++;

    if (entry->state == DSOUND_ENTRY_IMPLEMENTED && entry->handler) {
        return entry->handler(context);
    }

    if (!entry->reported) {
        entry->reported = true;
        /* Once per function. `DirectSoundDoWork` is a mixer tick; per-call reporting
         * would bury the one-shot init calls that are the interesting ones. The
         * measured site count rides along so the log is readable without the table
         * next to it. */
        log_printer("dsound: %#010x %s is not implemented (%u measured call site%s), "
                    "returning %#x\n",
                    entry->address, entry_label(entry), entry->sites,
                    entry->sites == 1u ? "" : "s", entry->default_return);
    }
    return entry->default_return;
}

size_t dsound_hle_implemented_count(void)
{
    ensure_initialised();
    size_t done = 0;
    for (size_t i = 0; i < DSOUND_FUNCTION_COUNT; i++) {
        if (entries[i].state == DSOUND_ENTRY_IMPLEMENTED) {
            done++;
        }
    }
    return done;
}

size_t dsound_hle_touched_count(void)
{
    ensure_initialised();
    size_t touched = 0;
    for (size_t i = 0; i < DSOUND_FUNCTION_COUNT; i++) {
        if (entries[i].call_count > 0) {
            touched++;
        }
    }
    return touched;
}

uint64_t dsound_hle_unknown_call_count(void)
{
    ensure_initialised();
    return unknown_calls;
}

/*
 * Is `candidate` a better place in the report than `best`?
 *
 * Runtime calls, then measured sites, then address ascending for determinism. Split
 * out as its own function because the ordering IS the product here: a report sorted
 * the wrong way round is not a cosmetic defect, it is a work queue that points at
 * the least important function first.
 */
static bool ranks_above(const dsound_entry *candidate, const dsound_entry *best)
{
    if (candidate->call_count != best->call_count) {
        return candidate->call_count > best->call_count;
    }
    if (candidate->sites != best->sites) {
        return candidate->sites > best->sites;
    }
    return candidate->address < best->address;
}

void dsound_hle_report(void)
{
    ensure_initialised();

    size_t missing = 0;
    uint64_t observed = 0;
    for (size_t i = 0; i < DSOUND_FUNCTION_COUNT; i++) {
        if (entries[i].state != DSOUND_ENTRY_IMPLEMENTED) {
            missing++;
        }
        observed += entries[i].call_count;
    }

    log_printer("dsound: %zu of %u measured functions still need implementations "
                "(%u call sites measured, %llu calls observed this run)\n",
                missing, (unsigned)DSOUND_FUNCTION_COUNT, (unsigned)DSOUND_SITE_COUNT,
                (unsigned long long)observed);

    /* SAY WHAT THE AUDIO ACTUALLY DID. A reader of a run log must not be able to
     * mistake this for a device that failed to open: nothing is mixed and nothing is
     * played, by construction and not by accident. */
    log_printer("dsound: output is RENDER-TO-NOTHING -- no host audio device is "
                "opened, no samples are mixed and none are emitted\n");

    if ((ac97_global_status & DSOUND_AC97_GLOBAL_STATUS_PRIMARY_CODEC_READY) != 0u) {
        log_printer("dsound: codec reports READY (FABRICATED -- there is no AC97 "
                    "codec; DirectSoundCreate is being allowed to succeed)\n");
    } else {
        log_printer("dsound: codec reports NOT READY (honest default; %llu "
                    "readiness quer%s answered no) -- expect DirectSoundCreate to "
                    "fail; upstream titles then fault at guest %#010x, but that address "
                    "is AMBIGUOUS (on this title it was a missing fs:[4], not audio), so "
                    "a fault there is not by itself evidence of this cause\n",
                    (unsigned long long)not_ready_queries,
                    not_ready_queries == 1u ? "y" : "ies",
                    (unsigned)DSOUND_NULL_DEVICE_FAULT_VA);
    }

    if (dsp_ack_address == DSOUND_DSP_ACK_UNSET) {
        log_printer("dsound: DSP command-word ack NOT CONFIGURED -- the scratch "
                    "offset for this title has not been derived from the binary "
                    "(%llu acknowledgement%s refused)\n",
                    (unsigned long long)dsp_ack_refusals,
                    dsp_ack_refusals == 1u ? "" : "s");
    } else {
        log_printer("dsound: DSP command-word ack at %#010x (base %#010x + %#x, "
                    "DERIVED) -- %llu acknowledged\n",
                    dsp_ack_address, dsp_ack_base, dsp_ack_offset,
                    (unsigned long long)dsp_ack_writes);
    }

    if (unknown_calls > 0) {
        log_printer("dsound: %llu call%s landed on an address absent from the "
                    "surface table -- the table and the binary DISAGREE\n",
                    (unsigned long long)unknown_calls,
                    unknown_calls == 1u ? "" : "s");
    }

    /* Selection sort. Thirty-nine entries, once per run, so clarity beats
     * cleverness -- the same trade `kernel_hle_report_missing` makes. */
    bool emitted[DSOUND_FUNCTION_COUNT] = {false};
    for (size_t printed = 0; printed < missing; printed++) {
        size_t best = 0;
        bool found = false;
        for (size_t i = 0; i < DSOUND_FUNCTION_COUNT; i++) {
            if (emitted[i] || entries[i].state == DSOUND_ENTRY_IMPLEMENTED) {
                continue;
            }
            if (!found || ranks_above(&entries[i], &entries[best])) {
                found = true;
                best = i;
            }
        }
        if (!found) {
            break;
        }
        emitted[best] = true;
        log_printer("  %#010x  %-40s sites %2u  calls %llu\n", entries[best].address,
                    entry_label(&entries[best]), entries[best].sites,
                    (unsigned long long)entries[best].call_count);
    }
}

/* ===================== READINESS ===================== */

void dsound_hle_set_codec_state(dsound_codec_state state)
{
    ensure_initialised();
    if (state == codec_state && codec_state_announced) {
        return;
    }
    codec_state = state;
    codec_state_announced = true;
    if (state == DSOUND_CODEC_READY) {
        ac97_global_status |= DSOUND_AC97_GLOBAL_STATUS_PRIMARY_CODEC_READY;
    } else {
        ac97_global_status &= ~DSOUND_AC97_GLOBAL_STATUS_PRIMARY_CODEC_READY;
    }
    if (state == DSOUND_CODEC_READY) {
        /* FABRICATED, in the same voice as --stub-status and the config zero-fill.
         * This single bit decides which branch the title's entire audio init takes,
         * so a run log that does not record it cannot be interpreted at all. */
        log_printer("dsound: codec readiness FORCED READY (FABRICATED -- there is no "
                    "AC97 codec and no audio device; this exists because two upstream "
                    "titles fault at guest %#010x on the NULL device a not-ready "
                    "codec produces)\n",
                    (unsigned)DSOUND_NULL_DEVICE_FAULT_VA);
    } else {
        log_printer("dsound: codec readiness set NOT READY (the honest default)\n");
    }
}

uint32_t dsound_hle_ac97_global_status(void)
{
    ensure_initialised();
    return ac97_global_status;
}

dsound_codec_state dsound_hle_codec_state(void)
{
    ensure_initialised();
    return codec_state;
}

bool dsound_hle_codec_ready(void)
{
    ensure_initialised();
    if ((ac97_global_status & DSOUND_AC97_GLOBAL_STATUS_PRIMARY_CODEC_READY) != 0u) {
        return true;
    }
    not_ready_queries++;
    if (!not_ready_reported) {
        not_ready_reported = true;
        /* Once, not per query -- this can be polled in a spin loop. The honest
         * default costs a crash, so the crash is signposted here instead of being
         * left for someone to find in the renderer three days later. */
        log_printer("dsound: codec is NOT READY -- DirectSoundCreate will fail, the "
                    "NULL device will be carried forward. Upstream titles then fault at "
                    "guest %#010x, but that address is AMBIGUOUS (a missing fs:[4] "
                    "produces it too), or run and never draw having skipped engine "
                    "init. Enable the fabricated ready state if you want past this.\n",
                    (unsigned)DSOUND_NULL_DEVICE_FAULT_VA);
    }
    return false;
}

uint64_t dsound_hle_not_ready_query_count(void)
{
    ensure_initialised();
    return not_ready_queries;
}

/* ===================== DSP COMMAND-WORD ACK ===================== */

void dsound_hle_set_dsp_writer(dsound_dsp_write_fn writer, void *user)
{
    ensure_initialised();
    dsp_writer = writer;
    dsp_writer_user = user;
}

bool dsound_hle_set_dsp_ack(uint32_t scratch_base, uint32_t offset)
{
    ensure_initialised();
    if (scratch_base == DSOUND_DSP_ACK_UNSET) {
        return false;
    }
    /* A wrapped sum is a wrong address, and a wrong address here is a silent write
     * into whatever does live there rather than a visible failure. */
    if (offset > UINT32_MAX - scratch_base) {
        return false;
    }
    dsp_ack_base = scratch_base;
    dsp_ack_offset = offset;
    dsp_ack_address = scratch_base + offset;
    log_printer("dsound: DSP command-word ack address DERIVED as %#010x "
                "(base %#010x + %#x)%s\n",
                dsp_ack_address, scratch_base, offset,
                offset == DSOUND_DSP_ACK_UPSTREAM_OFFSET
                        ? " -- matches the +0x810 upstream measured in four titles"
                        : " -- offset DIFFERS from upstream's +0x810");
    return true;
}

bool dsound_hle_dsp_ack_configured(void)
{
    ensure_initialised();
    return dsp_ack_address != DSOUND_DSP_ACK_UNSET;
}

uint32_t dsound_hle_dsp_ack_address(void)
{
    ensure_initialised();
    return dsp_ack_address;
}

bool dsound_hle_ack_dsp_command(void)
{
    ensure_initialised();
    if (dsp_ack_address == DSOUND_DSP_ACK_UNSET) {
        dsp_ack_refusals++;
        if (!dsp_ack_unconfigured_reported) {
            dsp_ack_unconfigured_reported = true;
            /* Refuse rather than fall back to upstream's offset against a guessed
             * base. The +0x810 is the one part that travels between titles; the base
             * is a per-title scratch allocation, and zeroing a guessed address
             * corrupts whatever really lives there with no diagnostic at all. */
            log_printer("dsound: DSP command-word ack REFUSED -- no address derived "
                        "for this title. Upstream acknowledges at its DirectSound "
                        "scratch + %#x in all four of its titles, but the BASE is "
                        "per-title and is not guessed here. Derive it from the spin "
                        "loop the guest is sitting in.\n",
                        (unsigned)DSOUND_DSP_ACK_UPSTREAM_OFFSET);
        }
        return false;
    }
    if (!dsp_writer) {
        dsp_ack_refusals++;
        log_printer("dsound: DSP command-word ack at %#010x has no writer installed; "
                    "nothing was written\n",
                    dsp_ack_address);
        return false;
    }
    dsp_writer(dsp_ack_address, 0u, dsp_writer_user);
    dsp_ack_writes++;
    return true;
}

uint64_t dsound_hle_dsp_ack_count(void)
{
    ensure_initialised();
    return dsp_ack_writes;
}

uint64_t dsound_hle_dsp_ack_refused_count(void)
{
    ensure_initialised();
    return dsp_ack_refusals;
}

/* ===================== CROSS-CHECK ===================== */

unsigned dsound_hle_crosscheck(const dsound_surface_ref *refs, size_t count)
{
    ensure_initialised();
    if (!refs && count > 0) {
        log_printer("dsound: crosscheck given %zu rows and a NULL pointer\n", count);
        return 1u;
    }

    unsigned disagreements = 0;

    for (size_t i = 0; i < count; i++) {
        const dsound_entry *mine = dsound_hle_entry(refs[i].address);
        if (!mine) {
            disagreements++;
            log_printer("dsound: crosscheck -- generated table has %#010x (%s, %u "
                        "sites) and we do not\n",
                        refs[i].address, refs[i].name ? refs[i].name : "unnamed",
                        refs[i].sites);
            continue;
        }
        if (mine->sites != refs[i].sites) {
            disagreements++;
            log_printer("dsound: crosscheck -- %#010x site count %u here, %u in the "
                        "generated table\n",
                        refs[i].address, mine->sites, refs[i].sites);
        }
        bool names_agree = (mine->name == NULL && refs[i].name == NULL) ||
                           (mine->name && refs[i].name &&
                            strcmp(mine->name, refs[i].name) == 0);
        if (!names_agree) {
            disagreements++;
            log_printer("dsound: crosscheck -- %#010x named \"%s\" here, \"%s\" in "
                        "the generated table\n",
                        refs[i].address, mine->name ? mine->name : "(null)",
                        refs[i].name ? refs[i].name : "(null)");
        }
    }

    /* The other direction. A check that only walked `refs` would pass against a
     * table of ours that had grown an invented row, which is the drift most likely
     * to happen by hand. */
    for (size_t i = 0; i < DSOUND_FUNCTION_COUNT; i++) {
        bool present = false;
        for (size_t j = 0; j < count; j++) {
            if (refs[j].address == entries[i].address) {
                present = true;
                break;
            }
        }
        if (!present) {
            disagreements++;
            log_printer("dsound: crosscheck -- we have %#010x (%s) and the generated "
                        "table does not\n",
                        entries[i].address, entry_label(&entries[i]));
        }
    }

    return disagreements;
}

/* Explicit measured stream, buffer and listener startup surface: no prefix/name guesses and no coverage claim. */
bool dsound_hle_requires_implementation(uint32_t address)
{
    switch (address) {
    case 0x00407B14u: case 0x00407B19u: case 0x00407B1Eu:
    case 0x00407B23u: case 0x00407B28u: case 0x004085CFu:
    case 0x004085D4u: case 0x004085D9u: case 0x004085F1u:
    case 0x00408609u: case 0x00408632u: case 0x00408637u:
    case 0x00408C2Du: case 0x0040967Cu:
    case 0x00407ABCu: case 0x00407AA4u: case 0x00407AF8u:
    case 0x00408556u: case 0x00407A64u: case 0x00407AD8u:
    case 0x00408532u: case 0x0040858Bu: case 0x00407A80u:
    case 0x004084F2u: case 0x0040850Eu: case 0x004085AFu:
    case 0x00408C0Du: case 0x004093C8u:
    case 0x004093ECu: case 0x0040945Au: case 0x00409410u:
        return true;
    default:
        return false;
    }
}
