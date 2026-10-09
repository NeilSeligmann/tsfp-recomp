/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include "guest_frame_trace.h"
#include "gpu_phase_timing.h"
#include "present_sink.h"
#include "audio_stretch.h"

#include "audio_sink_sdl.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#ifdef TSFP_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

#ifdef TSFP_T926_TEST_HOOKS
static present_sink_t926_hook s_t926_hook;
static void *s_t926_hook_context;

void present_sink_t926_set_hook(present_sink_t926_hook hook, void *context)
{
    s_t926_hook = hook;
    s_t926_hook_context = context;
}

static void t926_note(present_sink_t926_event event, bool lock_busy)
{
    if (s_t926_hook != NULL) {
        s_t926_hook(event, lock_busy, s_t926_hook_context);
    }
}
#define T926_NOTE(event, busy) t926_note((event), (busy))
#else
#define T926_NOTE(event, busy) ((void)0)
#endif

bool present_video_select(const char *name, bool have_dump_overlay, present_video_kind *kind,
                          const char **reason)
{
    *kind = PRESENT_VIDEO_NONE;
    *reason = NULL;
    if (name == NULL) {
        return true;
    }
    if (strcmp(name, "null") == 0) {
        *kind = PRESENT_VIDEO_NULL;
        return true;
    }
    if (strcmp(name, "png-dir") == 0) {
        if (!have_dump_overlay) {
            *reason = "--present png-dir needs --dump-overlay DIR (the png directory sink is that path)";
            return false;
        }
        *kind = PRESENT_VIDEO_PNG_DIR;
        return true;
    }
    if (strcmp(name, "window") == 0) {
#ifdef TSFP_HAVE_SDL3
        *kind = PRESENT_VIDEO_WINDOW;
        return true;
#else
        *reason = "--present window is not available: this build has no SDL3 (owner decision T744: SDL3 "
                  "is an optional dependency, install libsdl3-dev and reconfigure), nothing is faked";
        return false;
#endif
    }
    *reason = "unknown --present sink (known: null, png-dir, window)";
    return false;
}

bool present_audio_select(const char *name, bool hle_emits_pcm, present_audio_kind *kind,
                          const char **reason)
{
    *kind = PRESENT_AUDIO_NONE;
    *reason = NULL;
    if (name == NULL) {
        return true;
    }
    if (strcmp(name, "null") == 0) {
        *kind = PRESENT_AUDIO_NULL;
        return true;
    }
    if (strcmp(name, "wav-file") == 0) {
        if (!hle_emits_pcm) {
            *reason = "--audio-sink wav-file is not available: the DirectSound HLE emits no PCM "
                      "(it does not mix or resample, src/audio/dsound_hle.h), a mixer is missing";
            return false;
        }
        *kind = PRESENT_AUDIO_WAV_FILE;
        return true;
    }
    if (strcmp(name, "sdl") == 0) {
        if (!present_audio_device_available()) {
            *reason = "--audio-sink sdl is not available: this build has no SDL3 (optional dependency, "
                      "owner decision T744; install libsdl3-dev and rebuild)";
            return false;
        }
        *kind = PRESENT_AUDIO_SDL;
        return true;
    }
    if (strcmp(name, "device") == 0) {
        *reason = "--audio-sink device is not available: use --audio-sink sdl (SDL3 audio device, T761); "
                  "device was the pre-decision name (owner decisions T744)";
        return false;
    }
    *reason = "unknown --audio-sink (known: null, wav-file, sdl)";
    return false;
}

const char *present_video_name(present_video_kind kind)
{
    switch (kind) {
    case PRESENT_VIDEO_NULL: return "null";
    case PRESENT_VIDEO_PNG_DIR: return "png-dir";
    case PRESENT_VIDEO_WINDOW: return "window";
    default: return "none";
    }
}

const char *present_audio_name(present_audio_kind kind)
{
    switch (kind) {
    case PRESENT_AUDIO_NULL: return "null";
    case PRESENT_AUDIO_WAV_FILE: return "wav-file";
    case PRESENT_AUDIO_DEVICE: return "device";
    default: return "none";
    }
}

const char *present_video_announce(present_video_kind kind)
{
    switch (kind) {
    case PRESENT_VIDEO_NULL:
        return "present sink ON (opt-in, T744): null, pictures are discarded";
    case PRESENT_VIDEO_PNG_DIR:
        return "present sink ON (opt-in, T744): png-dir, the --dump-overlay files; no live window, "
               "pictures are written at each UpdateOverlay, not at a timed vblank";
    case PRESENT_VIDEO_WINDOW:
        return "present sink ON (opt-in, T760): window (SDL3), one picture per modelled vblank, overlay "
               "latched one vblank late (INFERRED, T540), colour matrix UNMEASURED";
    default:
        return "";
    }
}

const char *present_audio_announce(present_audio_kind kind)
{
    switch (kind) {
    case PRESENT_AUDIO_NULL:
        return "audio sink ON (opt-in, T744): null, samples are discarded (the HLE emits no PCM yet)";
    case PRESENT_AUDIO_WAV_FILE:
        return "audio sink ON (opt-in, T744): wav-file, sample timing INFERRED";
    case PRESENT_AUDIO_SDL:
        return "audio sink ON (opt-in, T761): SDL3 device pulling from the modelled timeline, timing "
               "INFERRED (T733, T743), the HLE mixer (T759) is the producer, silence until it exists";
    default:
        return "";
    }
}

#define AUDIO_RING_SECONDS 1u
/* A device that stops pulling for this long (suspended, unplugged) no longer holds the producer: the surplus
 * is dropped and counted as overrun, so the boot never deadlocks on audio. */
#define AUDIO_PACE_STALL_MS 2000u
static const char *s_audio_error;
static bool s_ring_only;

struct present_audio_sink {
    present_audio_kind kind;
    pthread_mutex_t lock;
    pthread_cond_t room; /* signalled by pull and close */
    uint32_t pace_timeout_ms; /* 0 = drop on a full ring, else the producer waits this long for room */
    bool closing;
    int16_t *ring;
    size_t ring_frames;
    size_t ring_head; /* next frame to read */
    size_t ring_fill;
    present_audio_counts counts;
    void *device; /* audio_sink_sdl.c state */
    FILE *file;
    uint32_t sample_rate;
    uint16_t channels;
    uint64_t frames;
    bool failed;
    /* T819 clock mode */
    bool clock_mode;
    bool playing;
    bool anchored;
    uint64_t anchor_vt_ns; /* modelled ns of the first frame ever written */
    size_t prefill_frames;
    size_t prefill_max_frames;
    int64_t stalled_since_ns;
    int64_t last_pull_ns; /* wall time of the last real pull and its size, the media clock interpolates inside it */
    size_t last_pull_take;
    /* T827 stall log */
    int64_t first_write_ns;
    int64_t last_write_ns;
    present_audio_stall stalls[PRESENT_AUDIO_STALL_MAX];
    size_t stall_count;
    bool stall_open;
    int64_t stall_start_ns;
    /* T1235 latency governor (0 target = off): holds the ring fill, which IS the audio latency the host adds, near
     * latency_target_frames by consuming faster than the device rate (silence first, then a bounded rate trim). */
    size_t latency_target_frames;
    size_t latency_hysteresis_frames;
    uint32_t latency_trim_permille;
    bool latency_trimming;
    uint32_t trim_fraction_q16;
    uint32_t last_pull_ratio_q16; /* source frames consumed per output frame of the last pull, 65536 = 1 */
    /* T1248 continuous playback: below slow_low_frames of ring fill the device consumes the ring SLOWER than real time
     * (ratio = fill / low, floored at slow_floor_q16, smoothed) instead of draining it dry and pausing everything for a
     * refill, so a guest that produces below real time (or hitches) lowers the pitch instead of cutting every voice. */
    size_t slow_low_frames;
    uint32_t slow_floor_q16;
    uint32_t slow_ratio_q16;
    /* T1250 pitch preserving slow playback (WSOLA, audio_stretch.c) in place of the resampler below the low water. */
    bool stretch_enabled;
    bool stretch_active;
    audio_stretch *stretch;
    uint64_t stretch_pos_q16;  /* nominal start of the next segment, frames from the ring head (q16) */
    size_t stretch_tail_src;   /* ring frame (from the head) where the tail's natural continuation lies */
    size_t stretch_hold_full_frames; /* a hold plays at full gain this long, then decays linearly (T1250 gaps) */
    size_t stretch_hold_max_frames;  /* ... and fades out into a rebuffer at this length */
    uint32_t stretch_gain_q16;       /* output gain of the stretch, ramped per block (a decaying hold) */
    bool fade_in_pending;            /* the next audio after the start or a rebuffer fades in (no click from silence) */
    int64_t stall_first_write_ns;    /* wall time of the first write during the open stall, 0 = none yet */
    size_t stretch_hold_run;   /* consecutive looped frames, bounded: a long gap fades out and rebuffers */
    int16_t *stretch_fifo;     /* one output block, drained across device pulls */
    size_t stretch_fifo_pos;
    size_t stretch_fifo_len;
    int16_t *stretch_scratch;  /* linear copy of the ring head the engine reads */
    size_t stretch_scratch_frames;
    /* T1487 wobble: block length, ratio slew limits per 1024 frame pull and a smoothed fill so the ratio does not follow the
     * production bursts. fill_smooth_frames 0 = the legacy per pull ratio. */
    uint32_t stretch_block_us;
    uint32_t stretch_slew_down_q16;
    uint32_t stretch_slew_up_q16;
    uint32_t stretch_engage_q16;
    uint32_t stretch_leave_q16;
    size_t fill_smooth_frames;
    double fill_ema;
    bool fill_ema_valid;
    bool stretch_tuned;
    int64_t av_offset_ns; /* T1487: the media clock reports the position this much EARLIER (video later), signed */
    /* T1248 cutouts: silence handed to the device after playback began (rebuffer pauses, dry tails, underruns). */
    bool cutout_open;
    uint64_t cutout_run_frames;
};

static int64_t monotonic_ns(void);
#define AUDIO_STALL_RESUME_NS 700000000LL /* a refill that never completes is not waited for longer than this (T827: was 1 s) */
#define AUDIO_REFILL_FREEZE_MS 600u       /* T827: aim for a freeze no longer than this at the guest's measured speed */
#define AUDIO_REFILL_MIN_MS 200u          /* T827: the smallest refill target */
#define AUDIO_REFILL_MEASURE_MIN_MS 20u   /* T827: a stall shorter than this is too brief to measure a rate from */

static size_t refill_target(const present_audio_sink *sink);

static void stall_begin(present_audio_sink *sink, bool initial)
{
    if (sink->stall_open || sink->stall_count >= PRESENT_AUDIO_STALL_MAX) {
        return;
    }
    const int64_t now = monotonic_ns();
    present_audio_stall *entry = &sink->stalls[sink->stall_count++];
    memset(entry, 0, sizeof *entry);
    entry->wall_ms = sink->first_write_ns != 0 ? (uint64_t)(now - sink->first_write_ns) / 1000000u : 0u;
    entry->media_ms = (sink->counts.pulled * 1000u) / sink->sample_rate;
    entry->written_ms = (sink->counts.written * 1000u) / sink->sample_rate;
    entry->since_write_ms = sink->last_write_ns != 0 ? (uint64_t)(now - sink->last_write_ns) / 1000000u : 0u;
    entry->clock_ms = (uint64_t)now / 1000000u;
    entry->fill_start = sink->ring_fill;
    entry->target_frames = refill_target(sink);
    entry->initial = initial;
    sink->stall_first_write_ns = 0;
    sink->stall_open = true;
    sink->stall_start_ns = now;
}

static void stall_end(present_audio_sink *sink, uint8_t reason)
{
    if (!sink->stall_open) {
        return;
    }
    present_audio_stall *entry = &sink->stalls[sink->stall_count - 1u];
    entry->duration_ms = (uint64_t)(monotonic_ns() - sink->stall_start_ns) / 1000000u;
    entry->fill_end = sink->ring_fill;
    entry->reason = reason;
    if (sink->stall_first_write_ns != 0) {
        const int64_t now = monotonic_ns();
        entry->first_write_ms = (uint64_t)(sink->stall_first_write_ns - sink->stall_start_ns) / 1000000u;
        entry->resume_wait_ms = (uint64_t)(now - sink->stall_first_write_ns) / 1000000u;
        if (!entry->initial) {
            sink->counts.resume_events++;
            sink->counts.resume_wait_sum_ms += entry->resume_wait_ms;
            if (entry->resume_wait_ms > sink->counts.resume_wait_max_ms) {
                sink->counts.resume_wait_max_ms = entry->resume_wait_ms;
            }
        }
    }
    sink->fade_in_pending = sink->stretch_enabled && !entry->initial; /* the legacy start and the non stretch paths stay bit exact */
    sink->stall_open = false;
    if (sink->clock_mode && !entry->initial) {
        /* T827: the target is what the guest delivers in AUDIO_REFILL_FREEZE_MS at the speed just measured (frames
         * gained over the stall's wall time), so the next freeze lasts about that long however slow the guest is, and
         * a fast guest gets a deep buffer. Clamped to the floor and the configured maximum. A refill too quick to
         * measure counts as a fast guest. */
        const uint64_t gained = entry->fill_end > entry->fill_start ? entry->fill_end - entry->fill_start : 0u;
        size_t target = sink->prefill_max_frames;
        if (entry->duration_ms >= AUDIO_REFILL_MEASURE_MIN_MS) {
            const uint64_t wanted = gained * AUDIO_REFILL_FREEZE_MS / entry->duration_ms;
            target = wanted < sink->prefill_max_frames ? (size_t)wanted : sink->prefill_max_frames;
        }
        size_t floor_frames = (size_t)sink->sample_rate * AUDIO_REFILL_MIN_MS / 1000u;
        if (floor_frames > sink->prefill_max_frames) {
            floor_frames = sink->prefill_max_frames;
        }
        sink->prefill_frames = target < floor_frames ? floor_frames : target;
        if (sink->slow_low_frames != 0u && sink->prefill_frames > sink->slow_low_frames) {
            /* T1250: with slowed playback on the refill never waits for more than the low water (refill_target), so the
             * adapted target must not ratchet up on bursts either (it only fed the report and a later mode switch). */
            sink->prefill_frames = sink->slow_low_frames;
        }
        if (sink->stretch_enabled) {
            sink->prefill_frames = refill_target(sink); /* the stretch resumes at one block, the adapted value is not used */
        }
        sink->counts.prefill_frames = sink->prefill_frames;
        if (sink->prefill_frames > sink->counts.prefill_peak_frames) {
            sink->counts.prefill_peak_frames = sink->prefill_frames;
        }
    }
}


/* The fill at which a paused playback resumes. T1248: with continuous playback the device adapts its rate to a thin ring,
 * so a deep refill (400 to 1500 ms of silence for every dry ring) buys nothing: resume at the low-water mark. */
static size_t refill_target(const present_audio_sink *sink)
{
    if (sink->stretch_enabled && sink->stretch != NULL && sink->counts.pulled != 0u) {
        /* T1250 gaps (after the start, which keeps its low water prefill): the stretch serves a thin ring (ratio down to the floor, loops when it cannot continue), so a
         * rebuffer resumes as soon as one block can be served instead of waiting 100 ms of a slow guest's output. */
        return 5u * audio_stretch_overlap(sink->stretch) / 2u; /* T1487: 23 ms at the 9.3 ms block, the resume stays under 30 ms */
    }
    if (sink->slow_low_frames != 0u && sink->slow_low_frames < sink->prefill_frames) {
        return sink->slow_low_frames;
    }
    return sink->prefill_frames;
}

/* T1248: account silence handed to the device after playback began. A run (one cutout) ends when real audio flows again. */
static void cutout_add(present_audio_sink *sink, size_t frames)
{
    if (frames == 0u) {
        return;
    }
    if (!sink->cutout_open) {
        sink->cutout_open = true;
        sink->cutout_run_frames = 0u;
        sink->counts.cutout_events++;
    }
    sink->cutout_run_frames += frames;
    sink->counts.cutout_frames += frames;
    if (sink->cutout_run_frames > sink->counts.cutout_max_frames) {
        sink->counts.cutout_max_frames = sink->cutout_run_frames;
    }
}

static void cutout_end(present_audio_sink *sink)
{
    sink->cutout_open = false;
}

static void put16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put32(uint8_t *out, uint32_t value)
{
    put16(out, (uint16_t)value);
    put16(out + 2, (uint16_t)(value >> 16));
}

static bool wav_header(FILE *file, uint32_t rate, uint16_t channels, uint32_t data_bytes)
{
    uint8_t header[44];
    memcpy(header, "RIFF", 4);
    put32(header + 4, 36u + data_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    put32(header + 16, 16u);
    put16(header + 20, 1u);
    put16(header + 22, channels);
    put32(header + 24, rate);
    put32(header + 28, rate * channels * 2u);
    put16(header + 32, (uint16_t)(channels * 2u));
    put16(header + 34, 16u);
    memcpy(header + 36, "data", 4);
    put32(header + 40, data_bytes);
    return fwrite(header, sizeof header, 1, file) == 1;
}

present_audio_sink *present_audio_sink_open(present_audio_kind kind, const char *wav_path,
                                            uint32_t sample_rate, uint16_t channels)
{
    s_audio_error = NULL;
    if (kind != PRESENT_AUDIO_NULL && kind != PRESENT_AUDIO_WAV_FILE && kind != PRESENT_AUDIO_SDL) {
        return NULL;
    }
    if (sample_rate == 0u || channels == 0u) {
        return NULL;
    }
    if (kind == PRESENT_AUDIO_SDL && !s_ring_only && !present_audio_device_available()) {
        s_audio_error = "this build has no SDL3";
        return NULL;
    }
    present_audio_sink *sink = calloc(1, sizeof *sink);
    if (sink == NULL) {
        return NULL;
    }
    pthread_mutex_init(&sink->lock, NULL);
    pthread_cond_init(&sink->room, NULL);
    if (kind == PRESENT_AUDIO_SDL) {
        sink->ring_frames = (size_t)sample_rate * AUDIO_RING_SECONDS;
        sink->ring = calloc(sink->ring_frames * channels, sizeof *sink->ring);
        if (sink->ring == NULL) {
            free(sink);
            return NULL;
        }
    }
    sink->kind = kind;
    sink->counts.ring_frames = sink->ring_frames;
    sink->counts.fill_min = UINT64_MAX;
    /* Real device: the producer runs on the virtual clock, which can burst ahead of wall time (loading).
     * Dropping the surplus skipped the music ahead, so the producer is paced to the device instead. */
    sink->pace_timeout_ms = (kind == PRESENT_AUDIO_SDL && !s_ring_only) ? AUDIO_PACE_STALL_MS : 0u;
    sink->sample_rate = sample_rate;
    sink->channels = channels;
    if (kind == PRESENT_AUDIO_SDL && !s_ring_only &&
        !audio_sink_sdl_start(sink, sample_rate, channels, &sink->device, &s_audio_error)) {
        pthread_mutex_destroy(&sink->lock);
        free(sink->ring);
        free(sink);
        return NULL;
    }
    if (kind == PRESENT_AUDIO_WAV_FILE) {
        sink->file = wav_path != NULL ? fopen(wav_path, "wb") : NULL;
        if (sink->file == NULL || !wav_header(sink->file, sample_rate, channels, 0u)) {
            if (sink->file != NULL) {
                fclose(sink->file);
            }
            free(sink);
            return NULL;
        }
    }
    return sink;
}

void present_audio_sink_set_pacing(present_audio_sink *sink, uint32_t stall_timeout_ms)
{
    if (sink != NULL) {
        pthread_mutex_lock(&sink->lock);
        sink->pace_timeout_ms = stall_timeout_ms;
        pthread_mutex_unlock(&sink->lock);
    }
}

present_audio_sink *present_audio_sink_open_ring(uint32_t sample_rate, uint16_t channels)
{
    s_ring_only = true;
    present_audio_sink *sink = present_audio_sink_open(PRESENT_AUDIO_SDL, NULL, sample_rate, channels);
    s_ring_only = false;
    return sink;
}

bool present_audio_sink_write(present_audio_sink *sink, const int16_t *samples, size_t frames)
{
    return present_audio_sink_write_at(sink, samples, frames, UINT64_MAX);
}

void present_audio_sink_enable_clock(present_audio_sink *sink, uint32_t prefill_ms, uint32_t max_prefill_ms,
                                     uint32_t ring_seconds)
{
    if (sink == NULL || sink->ring == NULL || ring_seconds == 0u) {
        return;
    }
    pthread_mutex_lock(&sink->lock);
    if (sink->counts.written == 0u && !sink->clock_mode) {
        const size_t frames = (size_t)sink->sample_rate * ring_seconds;
        int16_t *grown = calloc(frames * sink->channels, sizeof *grown);
        if (grown != NULL) {
            free(sink->ring);
            sink->ring = grown;
            sink->ring_frames = frames;
            sink->counts.ring_frames = frames;
            sink->ring_head = 0u;
            sink->ring_fill = 0u;
            sink->prefill_frames = (size_t)sink->sample_rate * prefill_ms / 1000u;
            sink->prefill_max_frames = (size_t)sink->sample_rate * max_prefill_ms / 1000u;
            if (sink->prefill_max_frames < sink->prefill_frames) {
                sink->prefill_max_frames = sink->prefill_frames;
            }
            if (sink->prefill_max_frames > frames) {
                sink->prefill_max_frames = frames;
            }
            sink->counts.prefill_frames = sink->prefill_frames;
            sink->clock_mode = true;
        }
    }
    pthread_mutex_unlock(&sink->lock);
}

void present_audio_sink_set_latency_governor(present_audio_sink *sink, uint32_t target_ms, uint32_t hysteresis_ms,
                                             uint32_t max_trim_permille)
{
    if (sink == NULL || sink->ring == NULL) {
        return;
    }
    pthread_mutex_lock(&sink->lock);
    sink->latency_target_frames = (size_t)sink->sample_rate * target_ms / 1000u;
    sink->latency_hysteresis_frames = (size_t)sink->sample_rate * hysteresis_ms / 1000u;
    sink->latency_trim_permille = max_trim_permille > 200u ? 200u : max_trim_permille;
    sink->latency_trimming = false;
    sink->counts.latency_target_frames = sink->latency_target_frames;
    pthread_mutex_unlock(&sink->lock);
}

void present_audio_sink_set_continuous_playback(present_audio_sink *sink, uint32_t low_water_ms, uint32_t min_rate_permille)
{
    if (sink == NULL || sink->ring == NULL) {
        return;
    }
    pthread_mutex_lock(&sink->lock);
    if (min_rate_permille > 1000u) {
        min_rate_permille = 1000u;
    }
    sink->slow_low_frames = min_rate_permille == 0u ? 0u : (size_t)sink->sample_rate * low_water_ms / 1000u;
    sink->slow_floor_q16 = (uint32_t)((uint64_t)min_rate_permille * 65536u / 1000u);
    sink->slow_ratio_q16 = 65536u;
    sink->counts.slow_low_frames = sink->slow_low_frames;
    pthread_mutex_unlock(&sink->lock);
}

/* The consumption ratio the ring fill asks for below the low-water mark (65536 = real time). It falls fast (protect the
 * ring) and recovers slowly (no pitch warble from the guest's bursty writes). Caller holds the lock. */
static uint32_t continuous_ratio(present_audio_sink *sink)
{
    if (sink->slow_low_frames == 0u) {
        return 65536u;
    }
    uint32_t wanted = 65536u;
    if (sink->ring_fill < sink->slow_low_frames) {
        wanted = (uint32_t)((uint64_t)sink->ring_fill * 65536u / sink->slow_low_frames);
        if (wanted < sink->slow_floor_q16) {
            wanted = sink->slow_floor_q16;
        }
    }
    if (wanted < sink->slow_ratio_q16) {
        sink->slow_ratio_q16 -= (sink->slow_ratio_q16 - wanted) / 2u;
    } else {
        sink->slow_ratio_q16 += (wanted - sink->slow_ratio_q16 + 7u) / 8u;
    }
    if (sink->slow_ratio_q16 > 65536u - 64u) {
        sink->slow_ratio_q16 = 65536u; /* back to the exact real time path */
    }
    return sink->slow_ratio_q16;
}

/* A run of digital silence at the head of the ring shorter than this is a waveform's own zero samples, not a gap. */
#define LATENCY_SILENCE_RUN_MIN 128u

static bool ring_frame_silent(const present_audio_sink *sink, size_t offset)
{
    const size_t index = (sink->ring_head + offset) % sink->ring_frames;
    for (uint16_t channel = 0u; channel < sink->channels; channel++) {
        if (sink->ring[index * sink->channels + channel] != 0) {
            return false;
        }
    }
    return true;
}

/* Frames the ring holds above the latency target while the governor is trimming (hysteresis: it starts above
 * target + hysteresis and stops at the target). Caller holds the lock. */
static size_t latency_excess(present_audio_sink *sink)
{
    if (sink->latency_target_frames == 0u) {
        return 0u;
    }
    if (sink->ring_fill > sink->latency_target_frames + sink->latency_hysteresis_frames) {
        sink->latency_trimming = true;
    } else if (sink->ring_fill <= sink->latency_target_frames) {
        sink->latency_trimming = false;
    }
    return sink->latency_trimming ? sink->ring_fill - sink->latency_target_frames : 0u;
}

/* Drop runs of digital silence at the ring head, at most `excess` frames: the cheap, inaudible way to shed latency.
 * They count as played (the media clock moves past them). Caller holds the lock. */
static size_t latency_skip_silence(present_audio_sink *sink, size_t excess)
{
    size_t skipped = 0u;
    while (skipped < excess && skipped < sink->ring_fill) {
        size_t run = 0u;
        while (skipped + run < sink->ring_fill && ring_frame_silent(sink, run)) {
            run++;
        }
        if (run < LATENCY_SILENCE_RUN_MIN) {
            break;
        }
        if (run > excess - skipped) {
            run = excess - skipped;
        }
        sink->ring_head = (sink->ring_head + run) % sink->ring_frames;
        sink->ring_fill -= run;
        sink->counts.pulled += run;
        sink->counts.latency_silence_frames += run;
        skipped += run;
    }
    return skipped;
}

/* ---- T1250 pitch preserving slow playback. Below the low water the pull is served by the WSOLA engine instead of the
 * resampler: the audio plays slower with its natural pitch. All of these hold the sink lock. ---- */
#define STRETCH_HOLD_MAX_MS 100u /* default: a longer gap (ring too thin to continue) fades out and rebuffers */

void present_audio_sink_set_stretch(present_audio_sink *sink, bool enabled)
{
    if (sink == NULL || sink->ring == NULL) {
        return;
    }
    pthread_mutex_lock(&sink->lock);
    if (!sink->stretch_tuned) { /* T1487 defaults, see the header */
        sink->stretch_slew_down_q16 = 30u * 65536u / 1000u;
        sink->stretch_slew_up_q16 = 10u * 65536u / 1000u;
        sink->stretch_engage_q16 = 950u * 65536u / 1000u;
        sink->stretch_leave_q16 = 985u * 65536u / 1000u;
        sink->fill_smooth_frames = (size_t)sink->sample_rate * 150u / 1000u;
    }
    if (enabled && sink->stretch == NULL) {
        size_t overlap = (size_t)((uint64_t)sink->sample_rate * (sink->stretch_block_us != 0u ? sink->stretch_block_us : 9333u) / 1000000u);
        overlap = overlap < 8u ? 8u : overlap;
        sink->stretch = audio_stretch_create(sink->channels, overlap);
        sink->stretch_scratch_frames = 6u * overlap;
        sink->stretch_scratch = calloc(sink->stretch_scratch_frames * sink->channels, sizeof *sink->stretch_scratch);
        sink->stretch_fifo = calloc(overlap * sink->channels, sizeof *sink->stretch_fifo);
        if (sink->stretch == NULL || sink->stretch_scratch == NULL || sink->stretch_fifo == NULL) {
            audio_stretch_destroy(sink->stretch);
            free(sink->stretch_scratch);
            free(sink->stretch_fifo);
            sink->stretch = NULL;
            sink->stretch_scratch = NULL;
            sink->stretch_fifo = NULL;
            enabled = false;
        }
    }
    sink->stretch_enabled = enabled && sink->stretch != NULL;
    if (sink->stretch_hold_max_frames == 0u) {
        sink->stretch_hold_full_frames = sink->stretch_hold_max_frames = (size_t)sink->sample_rate * STRETCH_HOLD_MAX_MS / 1000u;
    }
    sink->stretch_gain_q16 = 65536u;
    if (sink->stretch_enabled) {
        if (sink->slow_low_frames != 0u && sink->prefill_frames > sink->slow_low_frames) {
            sink->prefill_frames = sink->slow_low_frames; /* the start waits for the low water, not the initial 400 ms (the first stall showed 19200) */
        }
        sink->counts.prefill_frames = sink->prefill_frames;
    }
    sink->counts.stretch_enabled = sink->stretch_enabled ? 1u : 0u;
    pthread_mutex_unlock(&sink->lock);
}

void present_audio_sink_set_stretch_tuning(present_audio_sink *sink, const present_audio_stretch_tuning *tuning)
{
    if (sink == NULL || sink->ring == NULL || tuning == NULL) {
        return;
    }
    pthread_mutex_lock(&sink->lock);
    sink->stretch_block_us = tuning->block_us;
    sink->stretch_slew_down_q16 = (uint32_t)((uint64_t)tuning->slew_down_permille * 65536u / 1000u);
    sink->stretch_slew_up_q16 = (uint32_t)((uint64_t)tuning->slew_up_permille * 65536u / 1000u);
    sink->stretch_engage_q16 = (uint32_t)((uint64_t)tuning->engage_permille * 65536u / 1000u);
    sink->stretch_leave_q16 = (uint32_t)((uint64_t)tuning->leave_permille * 65536u / 1000u);
    sink->fill_smooth_frames = (size_t)sink->sample_rate * tuning->smooth_ms / 1000u;
    sink->fill_ema_valid = false;
    sink->stretch_tuned = true;
    pthread_mutex_unlock(&sink->lock);
}

void present_audio_sink_set_av_offset_ms(present_audio_sink *sink, int32_t offset_ms)
{
    if (sink == NULL) {
        return;
    }
    if (sink->ring != NULL) {
        pthread_mutex_lock(&sink->lock);
        sink->av_offset_ns = (int64_t)offset_ms * 1000000;
        pthread_mutex_unlock(&sink->lock);
    }
}

void present_audio_sink_set_stretch_hold(present_audio_sink *sink, uint32_t full_ms, uint32_t max_ms)
{
    if (sink == NULL || sink->ring == NULL) {
        return;
    }
    if (max_ms < full_ms) {
        max_ms = full_ms;
    }
    pthread_mutex_lock(&sink->lock);
    sink->stretch_hold_full_frames = (size_t)sink->sample_rate * full_ms / 1000u;
    sink->stretch_hold_max_frames = (size_t)sink->sample_rate * max_ms / 1000u;
    pthread_mutex_unlock(&sink->lock);
}

/* Ramps the block in the fifo linearly from the current stretch gain to `target_q16` (no click on a fade in, a decaying
 * hold or the return to full gain). */
static void stretch_ramp(present_audio_sink *sink, uint32_t target_q16)
{
    const uint32_t start = sink->stretch_gain_q16;
    if (start == 65536u && target_q16 == 65536u) {
        return;
    }
    const size_t length = sink->stretch_fifo_len;
    for (size_t frame = 0u; frame < length; frame++) {
        const int64_t gain = (int64_t)start + ((int64_t)target_q16 - (int64_t)start) * (int64_t)(frame + 1u) / (int64_t)length;
        for (uint16_t channel = 0u; channel < sink->channels; channel++) {
            int16_t *sample = sink->stretch_fifo + frame * sink->channels + channel;
            *sample = (int16_t)((int64_t)*sample * gain >> 16);
        }
    }
    sink->stretch_gain_q16 = target_q16;
}

/* Copies the first `frames` ring frames into the engine's linear scratch. */
static void stretch_load(present_audio_sink *sink, size_t frames)
{
    for (size_t frame = 0u; frame < frames; frame++) {
        const size_t index = (sink->ring_head + frame) % sink->ring_frames;
        memcpy(sink->stretch_scratch + frame * sink->channels, sink->ring + index * sink->channels,
               sink->channels * sizeof *sink->ring);
    }
}

static void stretch_consume(present_audio_sink *sink, size_t frames, size_t *consumed)
{
    if (frames > sink->ring_fill) {
        frames = sink->ring_fill;
    }
    sink->ring_head = (sink->ring_head + frames) % sink->ring_frames;
    sink->ring_fill -= frames;
    *consumed += frames;
}

/* The smoothed ring fill (T1487): a first order low pass over the pulls so the production bursts of the guest do not
 * modulate the stretch ratio. Updated on every pull, caller holds the lock. */
static void fill_smooth_update(present_audio_sink *sink, size_t pull_frames)
{
    if (sink->fill_smooth_frames == 0u) {
        return;
    }
    if (!sink->fill_ema_valid) {
        sink->fill_ema = (double)sink->ring_fill;
        sink->fill_ema_valid = true;
        return;
    }
    const double alpha = (double)pull_frames / ((double)sink->fill_smooth_frames + (double)pull_frames);
    sink->fill_ema += alpha * ((double)sink->ring_fill - sink->fill_ema);
}

/* The consumption ratio the ring fill asks for: 1 at the low water, falling linearly to the floor as the fill nears
 * what one block needs. T1487: the fill is smoothed, the ratio is slew limited per pull (a guest that dips for a moment
 * is followed by a ramp, not a step) and has a leave/engage hysteresis. Below twice the block's need the legacy fast fall
 * applies so a drying ring still reaches the floor at once (no cutout). */
static uint32_t stretch_ratio(present_audio_sink *sink, size_t pull_frames)
{
    const size_t need = audio_stretch_input_needed(sink->stretch, audio_stretch_search(sink->stretch));
    const size_t panic = 2u * need; /* under this the legacy fast fall: it cuts the response to a guest hitch (T1487: need = 3 half blocks) */
    double fill = (double)sink->ring_fill;
    if (sink->fill_smooth_frames != 0u && sink->fill_ema_valid && sink->ring_fill >= panic) {
        fill = sink->fill_ema; /* the raw fill is a saw tooth of one device pull (21 ms) and the guest's chunks, only a thin ring uses it */
    }
    uint32_t wanted = 65536u;
    if (fill < (double)sink->slow_low_frames) {
        const double above = fill > (double)need ? fill - (double)need : 0.0;
        const double span = sink->slow_low_frames > need ? (double)(sink->slow_low_frames - need) : 1.0;
        wanted = (uint32_t)(above * 65536.0 / span);
        if (wanted < sink->slow_floor_q16) {
            wanted = sink->slow_floor_q16;
        }
    }
    const uint32_t current = sink->slow_ratio_q16;
    if (current == 65536u && wanted < 65536u) {
        sink->counts.stretch_engagements += wanted <= sink->stretch_engage_q16 || sink->ring_fill < panic ? 1u : 0u;
    }
    if (sink->ring_fill < panic) {
        sink->counts.stretch_panic_pulls++;
    }
    if (sink->ring_fill < panic || sink->stretch_slew_down_q16 >= 65536u) {
        /* legacy: falls fast, recovers slowly */
        if (wanted < current) {
            sink->slow_ratio_q16 -= (current - wanted) / 2u;
        } else {
            sink->slow_ratio_q16 += (wanted - current + 7u) / 8u;
        }
    } else {
        const uint64_t scale = pull_frames > 0u ? pull_frames : 1024u;
        uint32_t down = (uint32_t)((uint64_t)sink->stretch_slew_down_q16 * scale / 1024u);
        uint32_t up = (uint32_t)((uint64_t)sink->stretch_slew_up_q16 * scale / 1024u);
        down = down == 0u ? 1u : down;
        up = up == 0u ? 1u : up;
        if (current == 65536u && wanted > sink->stretch_engage_q16) {
            wanted = 65536u; /* a slowdown under the engage threshold is absorbed by the ring, not stretched */
        }
        if (wanted < current) {
            sink->slow_ratio_q16 = current - wanted > down ? current - down : wanted;
        } else {
            sink->slow_ratio_q16 = wanted - current > up ? current + up : wanted;
        }
        const uint32_t step = sink->slow_ratio_q16 > current ? sink->slow_ratio_q16 - current : current - sink->slow_ratio_q16;
        if (step > sink->counts.stretch_max_step_q16) {
            sink->counts.stretch_max_step_q16 = step;
        }
    }
    /* Back to the exact real time path. Legacy: as soon as the ratio is near 1. T1487: only once the ring has recovered to the
     * low water (wanted is 1): a guest at 0.98x that dropped the ring just under it keeps its 2 percent stretch for as long
     * as it stays slow instead of the snap at a ratio near 1 (a leave margin in fill was tried: 22 to 20 onsets, not kept). */
    const bool recovered = sink->stretch_slew_down_q16 >= 65536u
                               ? sink->slow_ratio_q16 >= wanted
                               : wanted == 65536u;
    if (sink->slow_ratio_q16 > sink->stretch_leave_q16 && recovered) {
        sink->slow_ratio_q16 = 65536u;
    }
    return sink->slow_ratio_q16;
}

/* Leaves the engine: crossfades the tail into the ring at its natural source and consumes up to there. Needs the
 * continuation in the ring, else false (the caller keeps stretching or holds). */
static bool stretch_leave(present_audio_sink *sink, size_t *consumed)
{
    const size_t overlap = audio_stretch_overlap(sink->stretch);
    if (sink->ring_fill < sink->stretch_tail_src + overlap) {
        return false;
    }
    stretch_load(sink, sink->stretch_tail_src + overlap);
    if (!audio_stretch_leave(sink->stretch, sink->stretch_scratch + sink->stretch_tail_src * sink->channels,
                             overlap, sink->stretch_fifo)) {
        return false;
    }
    sink->stretch_fifo_pos = 0u;
    sink->stretch_fifo_len = overlap;
    stretch_ramp(sink, 65536u);
    stretch_consume(sink, sink->stretch_tail_src + overlap, consumed);
    sink->stretch_active = false;
    return true;
}

/* One output block into the fifo, or leaving. Returns false when the engine has stopped (left or faded out). */
static void stretch_block(present_audio_sink *sink, uint32_t ratio_q16, size_t *consumed)
{
    audio_stretch *engine = sink->stretch;
    const size_t overlap = audio_stretch_overlap(engine);
    const size_t search = audio_stretch_search(engine);
    if (ratio_q16 == 65536u && stretch_leave(sink, consumed)) {
        return;
    }
    const size_t nominal = (size_t)(sink->stretch_pos_q16 >> 16);
    size_t chosen = 0u;
    size_t window = audio_stretch_input_needed(engine, nominal);
    if (window > sink->ring_fill) {
        window = sink->ring_fill;
    }
    if (window > sink->stretch_scratch_frames) {
        window = sink->stretch_scratch_frames;
    }
    stretch_load(sink, window);
    sink->stretch_fifo_pos = 0u;
    sink->stretch_fifo_len = overlap;
    if (ratio_q16 != 0u && audio_stretch_next(engine, sink->stretch_scratch, window, nominal, sink->stretch_fifo, &chosen)) {
        sink->stretch_hold_run = 0u;
        stretch_ramp(sink, 65536u);
        sink->stretch_tail_src = chosen + overlap;
        sink->stretch_pos_q16 += (uint64_t)ratio_q16 * overlap;
        const size_t position = (size_t)(sink->stretch_pos_q16 >> 16);
        const size_t drop = position > search ? position - search : 0u;
        const size_t before = *consumed;
        stretch_consume(sink, drop, consumed);
        const size_t dropped = *consumed - before;
        sink->stretch_pos_q16 -= (uint64_t)dropped << 16;
        sink->stretch_tail_src = sink->stretch_tail_src > dropped ? sink->stretch_tail_src - dropped : 0u;
        sink->counts.stretch_blocks++;
        return;
    }
    /* The ring is too thin to continue (or the ratio is 0): loop the last segment, at full gain for the full part of the
     * hold and decaying linearly to silence at its end (a gap bridged by a fading sustain instead of a cut), then fade out. */
    if (sink->stretch_hold_run + overlap <= sink->stretch_hold_max_frames && audio_stretch_hold(engine, sink->stretch_fifo)) {
        sink->stretch_hold_run += overlap;
        sink->counts.stretch_hold_frames += overlap;
        uint32_t target = 65536u;
        if (sink->stretch_hold_run > sink->stretch_hold_full_frames) {
            const size_t span = sink->stretch_hold_max_frames - sink->stretch_hold_full_frames;
            const size_t into = sink->stretch_hold_run - sink->stretch_hold_full_frames;
            target = (uint32_t)(65536u - (uint64_t)65536u * (into < span ? into : span) / span);
            sink->counts.stretch_decay_frames += overlap;
        }
        stretch_ramp(sink, target);
        return;
    }
    audio_stretch_fade_out(engine, sink->stretch_fifo);
    stretch_ramp(sink, 0u);
    stretch_consume(sink, sink->ring_fill, consumed); /* what is left cannot continue the faded tail without a click */
    sink->stretch_active = false;
    sink->counts.stretch_fadeouts++;
}

/* Serves up to `frames` from the fifo and the engine. Returns the frames written, adds the ring frames consumed. */
static size_t stretch_serve(present_audio_sink *sink, int16_t *out, size_t frames, uint32_t ratio_q16, size_t *consumed)
{
    const size_t overlap = audio_stretch_overlap(sink->stretch);
    size_t served = 0u;
    if (!sink->stretch_active && sink->stretch_fifo_len == 0u && ratio_q16 < 65536u && sink->ring_fill >= 2u * overlap) {
        stretch_load(sink, 2u * overlap);
        if (audio_stretch_prime(sink->stretch, sink->stretch_scratch, 2u * overlap, sink->stretch_fifo)) {
            sink->stretch_fifo_pos = 0u;
            sink->stretch_fifo_len = overlap;
            if (sink->fade_in_pending) {
                sink->stretch_gain_q16 = 0u; /* a fade in from silence after a rebuffer or the start */
                sink->fade_in_pending = false;
            }
            stretch_ramp(sink, 65536u);
            sink->stretch_tail_src = overlap;
            sink->stretch_pos_q16 = (uint64_t)ratio_q16 * overlap;
            sink->stretch_hold_run = 0u;
            sink->stretch_active = true;
        }
    }
    while (served < frames) {
        if (sink->stretch_fifo_pos >= sink->stretch_fifo_len) {
            sink->stretch_fifo_pos = 0u;
            sink->stretch_fifo_len = 0u;
            if (!sink->stretch_active) {
                break;
            }
            stretch_block(sink, ratio_q16, consumed);
            continue;
        }
        size_t part = sink->stretch_fifo_len - sink->stretch_fifo_pos;
        if (part > frames - served) {
            part = frames - served;
        }
        memcpy(out + served * sink->channels, sink->stretch_fifo + sink->stretch_fifo_pos * sink->channels,
               part * sink->channels * sizeof *out);
        sink->stretch_fifo_pos += part;
        served += part;
        if (sink->stretch_fifo_pos >= sink->stretch_fifo_len && !sink->stretch_active) {
            sink->stretch_fifo_pos = 0u;
            sink->stretch_fifo_len = 0u;
            break;
        }
    }
    return served;
}

void present_audio_sink_kick(present_audio_sink *sink)
{
    if (sink == NULL) {
        return;
    }
    pthread_mutex_lock(&sink->lock);
    if (sink->clock_mode && !sink->playing && sink->ring_fill != 0u) {
        sink->playing = true;
        stall_end(sink, 3u);
    }
    pthread_mutex_unlock(&sink->lock);
}

bool present_audio_sink_media_time(present_audio_sink *sink, uint64_t *vt_ns)
{
    if (sink == NULL || vt_ns == NULL) {
        return false;
    }
    T926_NOTE(PRESENT_SINK_T926_AUDIO_QUERY_ENTER, false);
    pthread_mutex_lock(&sink->lock);
    const bool known = sink->clock_mode && sink->anchored;
    if (known) {
        /* The device pulls in chunks (about 20 ms) and plays each chunk over its own duration, so the clock is the
         * start of the last chunk plus the wall time elapsed in it (at most the chunk). It stands still once the
         * chunk is played and no further real pull comes (a rebuffer). */
        uint64_t played = sink->counts.pulled - sink->last_pull_take;
        if (sink->last_pull_take != 0u) {
            const int64_t since = monotonic_ns() - sink->last_pull_ns;
            uint64_t inside = since > 0 ? (uint64_t)since * sink->sample_rate / 1000000000ULL * sink->last_pull_ratio_q16 / 65536u : 0u;
            if (inside > sink->last_pull_take) {
                inside = sink->last_pull_take;
            }
            played += inside;
        }
        int64_t media = (int64_t)(sink->anchor_vt_ns + played * 1000000000ULL / sink->sample_rate) - sink->av_offset_ns;
        if (media < (int64_t)sink->anchor_vt_ns) {
            media = (int64_t)sink->anchor_vt_ns; /* a positive offset delays the video, never before the first sample */
        }
        *vt_ns = (uint64_t)media;
    }
    pthread_mutex_unlock(&sink->lock);
    T926_NOTE(PRESENT_SINK_T926_AUDIO_QUERY_LEAVE, false);
    return known;
}

bool present_audio_sink_write_at(present_audio_sink *sink, const int16_t *samples, size_t frames,
                                 uint64_t end_vt_ns)
{
    if (sink == NULL || sink->failed || (samples == NULL && frames != 0u)) {
        return false;
    }
    if (sink->ring != NULL) {
        size_t done = 0u;
        pthread_mutex_lock(&sink->lock);
        if (sink->first_write_ns == 0) {
            sink->first_write_ns = monotonic_ns();
        }
        sink->last_write_ns = monotonic_ns();
        if (sink->stall_open) {
            sink->stalls[sink->stall_count - 1u].writes_during++;
            if (sink->stall_first_write_ns == 0) {
                sink->stall_first_write_ns = monotonic_ns();
            }
        }
        if (sink->clock_mode && !sink->anchored && end_vt_ns != UINT64_MAX) {
            const uint64_t span = (uint64_t)frames * 1000000000ULL / sink->sample_rate;
            sink->anchor_vt_ns = end_vt_ns > span ? end_vt_ns - span : 0u;
            sink->anchored = true;
        }
        while (done < frames) {
            size_t room = sink->ring_frames - sink->ring_fill;
            if (room == 0u && sink->pace_timeout_ms != 0u && !sink->closing) {
                /* Backpressure: wait for the device to drain. Progress (a pull) restarts the stall clock. */
                struct timespec deadline;
                clock_gettime(CLOCK_REALTIME, &deadline);
                deadline.tv_sec += sink->pace_timeout_ms / 1000u;
                deadline.tv_nsec += (long)(sink->pace_timeout_ms % 1000u) * 1000000L;
                if (deadline.tv_nsec >= 1000000000L) {
                    deadline.tv_sec++;
                    deadline.tv_nsec -= 1000000000L;
                }
                const int64_t wait_start = monotonic_ns();
                while (sink->ring_fill == sink->ring_frames && !sink->closing) {
                    if (pthread_cond_timedwait(&sink->room, &sink->lock, &deadline) == ETIMEDOUT) {
                        break;
                    }
                }
                const uint64_t waited = (uint64_t)(monotonic_ns() - wait_start);
                sink->counts.block_events++;
                sink->counts.block_ns += waited;
                if (waited > sink->counts.block_max_ns) {
                    sink->counts.block_max_ns = waited;
                }
                room = sink->ring_frames - sink->ring_fill;
            }
            if (room == 0u) {
                break; /* no pacing, closing or a stalled device: drop the rest */
            }
            size_t take = frames - done < room ? frames - done : room;
            size_t tail = (sink->ring_head + sink->ring_fill) % sink->ring_frames;
            for (size_t frame = 0; frame < take; frame++) {
                memcpy(sink->ring + tail * sink->channels, samples + (done + frame) * sink->channels,
                       sink->channels * sizeof *samples);
                tail = (tail + 1u) % sink->ring_frames;
            }
            sink->ring_fill += take;
            sink->counts.written += take;
            done += take;
        }
        sink->counts.overrun_frames += frames - done;
        sink->counts.write_calls++;
        if (sink->ring_fill > sink->counts.fill_max) {
            sink->counts.fill_max = sink->ring_fill;
        }
        if (sink->clock_mode && !sink->playing &&
            (sink->ring_fill >= refill_target(sink) || sink->ring_fill == sink->ring_frames)) {
            sink->playing = true;
            stall_end(sink, sink->ring_fill >= refill_target(sink) ? 1u : 4u);
        }
        pthread_mutex_unlock(&sink->lock);
        sink->frames += frames;
        return true;
    }
    if (sink->file != NULL) {
        for (size_t index = 0; index < frames * sink->channels; index++) {
            uint8_t bytes[2];
            put16(bytes, (uint16_t)samples[index]);
            if (fwrite(bytes, 2, 1, sink->file) != 1) {
                sink->failed = true;
                return false;
            }
        }
    }
    sink->frames += frames;
    return true;
}

size_t present_audio_sink_pull(present_audio_sink *sink, int16_t *out, size_t frames)
{
    if (sink == NULL || out == NULL || sink->ring == NULL) {
        return 0u;
    }
    pthread_mutex_lock(&sink->lock);
    if (sink->clock_mode && !sink->playing) {
        /* Waiting for the ring to refill: the device gets silence and the media clock holds still. */
        const int64_t now = monotonic_ns();
        if (sink->stalled_since_ns == 0) {
            sink->stalled_since_ns = now;
        }
        if (sink->counts.written != 0u) {
            stall_begin(sink, sink->counts.pulled == 0u);
        }
        if (sink->counts.written != 0u && sink->ring_fill != 0u &&
            (sink->ring_fill >= refill_target(sink) || now - sink->stalled_since_ns > AUDIO_STALL_RESUME_NS)) {
            sink->playing = true;
            sink->slow_ratio_q16 = 65536u;
            stall_end(sink, sink->ring_fill >= refill_target(sink) ? 1u : 2u);
        } else {
            sink->counts.pulls++;
            memset(out, 0, frames * sink->channels * sizeof *out);
            if (sink->counts.written != 0u) {
                sink->counts.rebuffer_frames += frames;
                if (sink->counts.pulled != 0u) {
                    cutout_add(sink, frames);
                }
            } else {
                sink->counts.idle_frames += frames;
            }
            pthread_mutex_unlock(&sink->lock);
            return 0u;
        }
    }
    sink->stalled_since_ns = 0;
    const bool started = sink->counts.written != 0u;
    sink->counts.pulls++;
    /* T1235: shed latency before serving the pull (silence first, then a bounded rate trim below). */
    size_t excess = latency_excess(sink);
    fill_smooth_update(sink, frames);
    size_t stretch_consumed = 0u;
    size_t stretch_served = 0u;
    uint32_t stretch_ratio_q16 = 65536u;
    if (sink->stretch_active && excess != 0u) {
        /* The governor wants to shed latency: leave the slowed playback first (the crossfade keeps it click free). */
        stretch_ratio_q16 = 65536u;
        stretch_served = stretch_serve(sink, out, frames, 65536u, &stretch_consumed);
        excess = latency_excess(sink);
    } else if (sink->stretch_enabled && sink->clock_mode && started && sink->slow_low_frames != 0u && excess == 0u) {
        stretch_ratio_q16 = stretch_ratio(sink, frames);
        stretch_served = stretch_serve(sink, out, frames, stretch_ratio_q16, &stretch_consumed);
        if (sink->stretch_active || stretch_served != 0u) {
            if (stretch_served > stretch_consumed) {
                sink->counts.slow_frames += stretch_served - stretch_consumed;
            }
            sink->counts.slow_pulls++;
            if (stretch_ratio_q16 < sink->counts.slow_min_ratio_q16 || sink->counts.slow_min_ratio_q16 == 0u) {
                sink->counts.slow_min_ratio_q16 = stretch_ratio_q16;
            }
        }
    }
    int16_t *const pull_out = out;
    out += stretch_served * sink->channels;
    frames -= stretch_served;
    if (excess != 0u) {
        excess -= latency_skip_silence(sink, excess);
    }
    if (started && sink->ring_fill < sink->counts.fill_min) {
        sink->counts.fill_min = sink->ring_fill;
    }
    size_t take = frames < sink->ring_fill ? frames : sink->ring_fill;
    size_t consumed = take;
    uint32_t ratio_q16 = 65536u;
    if (excess != 0u && sink->latency_trim_permille != 0u && sink->ring_fill > frames + frames * sink->latency_trim_permille / 1000u + 4u) {
        /* Consume `ratio` source frames per output frame, linear interpolation: a speed-up of at most
         * latency_trim_permille that grows with the excess (about a 1.5 s lead takes the whole trim). */
        uint64_t trim = (uint64_t)sink->latency_trim_permille * excess * 2u / 3u / sink->sample_rate;
        if (trim > sink->latency_trim_permille) {
            trim = sink->latency_trim_permille;
        }
        if (trim == 0u) {
            trim = 1u;
        }
        ratio_q16 = 65536u + (uint32_t)(trim * 65536u / 1000u);
    } else if (sink->clock_mode && started && !sink->stretch_enabled) {
        ratio_q16 = continuous_ratio(sink); /* T1248: below real time when the ring is low */
    }
    if (ratio_q16 != 65536u) {
        uint64_t position = sink->trim_fraction_q16;
        size_t produced = 0u;
        while (produced < frames) {
            const size_t base = (size_t)(position >> 16);
            if (base + 1u >= sink->ring_fill) {
                break; /* the interpolation needs the next frame: the ring is dry for this ratio */
            }
            const uint32_t fraction = (uint32_t)(position & 0xFFFFu);
            const size_t first = (sink->ring_head + base) % sink->ring_frames;
            const size_t second = (first + 1u) % sink->ring_frames;
            for (uint16_t channel = 0u; channel < sink->channels; channel++) {
                const int32_t a = sink->ring[first * sink->channels + channel];
                const int32_t b = sink->ring[second * sink->channels + channel];
                out[produced * sink->channels + channel] = (int16_t)(a + (((b - a) * (int32_t)fraction) >> 16));
            }
            position += ratio_q16;
            produced++;
        }
        consumed = (size_t)(position >> 16);
        if (consumed > sink->ring_fill) {
            consumed = sink->ring_fill;
        }
        sink->trim_fraction_q16 = (uint32_t)(position & 0xFFFFu);
        take = produced;
        if (consumed > produced) {
            sink->counts.latency_trim_frames += consumed - produced;
        } else {
            sink->counts.slow_frames += produced - consumed;
            sink->counts.slow_pulls++;
            if (ratio_q16 < sink->counts.slow_min_ratio_q16 || sink->counts.slow_min_ratio_q16 == 0u) {
                sink->counts.slow_min_ratio_q16 = ratio_q16;
            }
        }
        sink->ring_head = (sink->ring_head + consumed) % sink->ring_frames;
        sink->ring_fill -= consumed;
    } else {
        sink->trim_fraction_q16 = 0u;
        for (size_t frame = 0; frame < take; frame++) {
            memcpy(out + frame * sink->channels, sink->ring + sink->ring_head * sink->channels,
                   sink->channels * sizeof *out);
            sink->ring_head = (sink->ring_head + 1u) % sink->ring_frames;
        }
        sink->ring_fill -= take;
    }
    const size_t produced_total = take + stretch_served;
    if (sink->fade_in_pending && produced_total != 0u) {
        sink->fade_in_pending = false;
        if (stretch_served == 0u) {
            /* served without the stretch right after a rebuffer or the start: ramp the first block up from silence */
            const size_t ramp = sink->stretch != NULL ? audio_stretch_overlap(sink->stretch) : (size_t)sink->sample_rate / 150u;
            const size_t length = ramp < produced_total ? ramp : produced_total;
            for (size_t frame = 0u; frame < length; frame++) {
                for (uint16_t channel = 0u; channel < sink->channels; channel++) {
                    int16_t *sample = pull_out + frame * sink->channels + channel;
                    *sample = (int16_t)((int32_t)*sample * (int32_t)(frame + 1u) / (int32_t)length);
                }
            }
        }
    }
    consumed += stretch_consumed;
    if (stretch_served != 0u && ratio_q16 == 65536u) {
        ratio_q16 = stretch_ratio_q16;
    }
    sink->counts.pulled += consumed;
    sink->last_pull_ns = monotonic_ns();
    sink->last_pull_take = consumed;
    sink->last_pull_ratio_q16 = ratio_q16;
    pthread_cond_signal(&sink->room);
    if (produced_total != 0u) {
        cutout_end(sink);
    }
    if (take < frames && sink->clock_mode && started) {
        /* Ran dry: pause and refill. The refill target is adapted from the guest's measured speed when the stall ends. */
        sink->playing = false;
        stall_begin(sink, false);
        sink->counts.rebuffer_events++;
        sink->counts.rebuffer_frames += frames - take;
        cutout_add(sink, frames - take);
        memset(out + take * sink->channels, 0, (frames - take) * sink->channels * sizeof *out);
        sink->stretch_fifo_len = 0u;
        sink->stretch_fifo_pos = 0u;
        sink->stretch_active = false;
    } else if (take < frames) {
        memset(out + take * sink->channels, 0, (frames - take) * sink->channels * sizeof *out);
        if (started) {
            sink->counts.underrun_frames += frames - take;
            sink->counts.underrun_events++;
            if (sink->counts.pulled != 0u) {
                cutout_add(sink, frames - take);
            }
        } else {
            sink->counts.idle_frames += frames - take;
        }
    }
    pthread_mutex_unlock(&sink->lock);
    return take + stretch_served;
}

size_t present_audio_sink_stalls(const present_audio_sink *sink, present_audio_stall *out, size_t max)
{
    if (sink == NULL || out == NULL) {
        return 0u;
    }
    present_audio_sink *mutable_sink = (present_audio_sink *)sink;
    pthread_mutex_lock(&mutable_sink->lock);
    const size_t count = mutable_sink->stall_count < max ? mutable_sink->stall_count : max;
    memcpy(out, mutable_sink->stalls, count * sizeof *out);
    pthread_mutex_unlock(&mutable_sink->lock);
    return count;
}

size_t present_audio_sink_device_frames(const present_audio_sink *sink)
{
    return sink != NULL ? audio_sink_sdl_device_frames(sink->device) : 0u;
}

present_audio_counts present_audio_sink_counts(const present_audio_sink *sink)
{
    present_audio_counts none = {0};
    if (sink == NULL) {
        return none;
    }
    present_audio_sink *mutable_sink = (present_audio_sink *)sink;
    pthread_mutex_lock(&mutable_sink->lock);
    none = sink->counts;
    pthread_mutex_unlock(&mutable_sink->lock);
    return none;
}

const char *present_audio_sink_last_error(void)
{
    return s_audio_error;
}

uint64_t present_audio_sink_frames(const present_audio_sink *sink)
{
    return sink != NULL ? sink->frames : 0u;
}

bool present_audio_sink_close(present_audio_sink *sink)
{
    if (sink == NULL) {
        return false;
    }
    pthread_mutex_lock(&sink->lock);
    sink->closing = true; /* release a producer waiting for room before the device and ring go */
    pthread_cond_broadcast(&sink->room);
    pthread_mutex_unlock(&sink->lock);
    audio_sink_sdl_stop(sink->device);
    sink->device = NULL;
    audio_stretch_destroy(sink->stretch);
    free(sink->stretch_scratch);
    free(sink->stretch_fifo);
    free(sink->ring);
    pthread_cond_destroy(&sink->room);
    pthread_mutex_destroy(&sink->lock);
    bool ok = !sink->failed;
    if (sink->file != NULL) {
        const uint64_t bytes = sink->frames * sink->channels * 2u;
        if (bytes > 0xFFFFFFFFu - 36u) {
            ok = false;
        }
        if (ok) {
            ok = fseek(sink->file, 0, SEEK_SET) == 0 &&
                 wav_header(sink->file, sink->sample_rate, sink->channels, (uint32_t)bytes);
        }
        if (fclose(sink->file) != 0) {
            ok = false;
        }
    }
    free(sink);
    return ok;
}

/* --- video sink objects (T760) ------------------------------------------------------------- */

bool present_audio_device_available(void)
{
#ifdef TSFP_HAVE_SDL3
    return true;
#else
    return false;
#endif
}

bool present_window_available(void)
{
#ifdef TSFP_HAVE_SDL3
    return true;
#else
    return false;
#endif
}

struct present_video_sink {
    pthread_mutex_t lock; /* guest threads call the hooks, SDL is single threaded */
    present_video_kind kind;
    present_video_counts counts;
    bool dirty; /* a good picture arrived since the last vblank */
    uint8_t *pending;
    size_t capacity;
    uint32_t width;
    uint32_t height;
#ifdef TSFP_HAVE_SDL3
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    uint32_t texture_width;
    uint32_t texture_height;
    bool closed_by_user;
    SDL_Gamepad *gamepads[8];
    /* The presenter thread owns every SDL object (window, renderer, texture, pads, events). OpenGL and Wayland
     * bind the renderer to the thread that created it, so a present from a guest thread fails with "The
     * specified window has not been made current" and the window stays black. Callers run their SDL work on
     * that thread with sdl_run(). */
    pthread_t thread;
    pthread_t owner_thread; /* the presenter thread, set by itself */
    bool thread_started;
    pthread_mutex_t job_lock;
    pthread_cond_t job_cond;
    pthread_cond_t done_cond;
    void (*job)(present_video_sink *, void *);
    void *job_arg;
    bool job_pending;
    bool job_done;
    void (*post_fn)(void *);
    void *post_context;
    uint64_t blocked_ns[3], blocked_calls[3], blocked_max_ns[3]; /* T1246: wall time the calling thread spent in sdl_run, sdl_post and sdl_drain (index 0, 1, 2) */
    /* T1262: the synchronous jobs by kind (present_job_kind): how long the calling thread waited for the job slot (the frame job still in
     * flight) and how long it then waited for the job itself, so a report shows which of the two the guest thread paid. */
    uint64_t kind_calls[PRESENT_JOB_KINDS], kind_wait_ns[PRESENT_JOB_KINDS], kind_exec_ns[PRESENT_JOB_KINDS];
    uint64_t kind_wait_max_ns[PRESENT_JOB_KINDS], kind_exec_max_ns[PRESENT_JOB_KINDS];
    bool job_async; /* T1246: the pending job was posted without a waiter, the presenter clears job_pending itself */
    bool stop_thread;
    bool open_finished; /* the thread's window_open handshake */
    bool open_good;
    bool redraw; /* the window was exposed or resized: draw the last picture again */
    bool input_gamepad_init;
    char renderer_name[32];
    int output_width;
    int output_height;
    char last_error[160];
#endif
    present_live_ops live_ops; /* T838 */
    bool live;
    uint32_t pace_millihertz; /* 0: the vblank hook returns at once */
    int64_t pace_deadline_ns;
    int64_t last_vblank_exit_ns; /* T1235 */
    bool pace_started;
    /* T819 timing, wall clock. */
#define TIMING_SAMPLES 8192u
    uint32_t interval_us[TIMING_SAMPLES];
    size_t interval_count;
    int64_t last_presented_ns;
    present_video_timing timing;
    /* T819 playback mode. */
#define PLAYBACK_QUEUE_SLOTS 96u
#define PLAYBACK_ROOM_WAIT_MS 1000
#define PLAYBACK_LATE_NS 40000000ULL
    bool playback;
    bool interactive; /* immutable after startup, wall-paced producer even with audio-master movies */
    atomic_bool user_closed;
    const char *_Atomic close_cause; /* T1492: static text, first cause wins */
    bool closing;
    present_audio_sink *playback_audio;
    pthread_cond_t queue_room; /* waited on with sink->lock */
    struct {
        uint8_t *rgb;
        size_t capacity;
        uint32_t width;
        uint32_t height;
        uint64_t number;
        uint64_t vt_ns;
    } pq[PLAYBACK_QUEUE_SLOTS];
    size_t pq_head;
    size_t pq_count;
    bool have_submit;
    uint64_t last_submit_vt_ns;
    uint64_t last_submit_number;
    bool input_keyboard;
    bool input_gamepad;
    present_input_event queue[PRESENT_INPUT_QUEUE_SIZE];
    size_t queue_head;
    size_t queue_count;
    uint64_t input_dropped;
    const char *held_keys[64];
    size_t held_count;
};

#ifdef TSFP_HAVE_SDL3
static void input_push(present_video_sink *sink, present_input_kind kind, const char *name, int32_t raw,
                       int32_t value)
{
    if (sink->queue_count == PRESENT_INPUT_QUEUE_SIZE) {
        sink->input_dropped++;
        return;
    }
    present_input_event *slot = &sink->queue[(sink->queue_head + sink->queue_count) % PRESENT_INPUT_QUEUE_SIZE];
    slot->kind = kind;
    slot->name = name;
    slot->raw = raw;
    slot->value = value;
    sink->queue_count++;
}
#endif

static present_live_ops s_live_ops;
static bool s_live_ops_set;

void present_video_set_live_ops(const present_live_ops *ops)
{
    s_live_ops_set = ops != NULL;
    if (ops != NULL) {
        s_live_ops = *ops;
    } else {
        memset(&s_live_ops, 0, sizeof s_live_ops);
    }
}

#ifdef TSFP_HAVE_SDL3
static char sdl_error[256];

static void record_error(present_video_sink *sink, const char *what)
{
    sink->counts.present_errors++;
    snprintf(sink->last_error, sizeof sink->last_error, "%s: %s", what, SDL_GetError());
    if (sink->counts.present_errors <= 3u) {
        fprintf(stderr, "present window: %s\n", sink->last_error);
    }
}

/* Runs on the presenter thread. */
static bool window_open(present_video_sink *sink, const char *title)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        snprintf(sdl_error, sizeof sdl_error, "--present window: SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    if (sink->live) {
        sink->window = SDL_CreateWindow(title != NULL ? title : "tsfp", 640, 480, SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN);
        char open_error[200] = "";
        if (sink->window != NULL && sink->live_ops.open(sink->live_ops.context, sink->window, open_error, sizeof open_error)) {
            snprintf(sink->renderer_name, sizeof sink->renderer_name, "vulkan (live, T838)");
            return true;
        }
        snprintf(sdl_error, sizeof sdl_error, "--gpu-live: could not create the Vulkan window: %s",
                 sink->window == NULL ? SDL_GetError() : open_error);
        if (sink->window != NULL) {
            SDL_DestroyWindow(sink->window);
            sink->window = NULL;
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
    sink->window = SDL_CreateWindow(title != NULL ? title : "tsfp", 640, 480, SDL_WINDOW_RESIZABLE);
    if (sink->window != NULL) {
        sink->renderer = SDL_CreateRenderer(sink->window, NULL);
    }
    if (sink->window == NULL || sink->renderer == NULL) {
        snprintf(sdl_error, sizeof sdl_error, "--present window: could not create the window: %s",
                 SDL_GetError());
        if (sink->window != NULL) {
            SDL_DestroyWindow(sink->window);
            sink->window = NULL;
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }
    const char *name = SDL_GetRendererName(sink->renderer);
    snprintf(sink->renderer_name, sizeof sink->renderer_name, "%s", name != NULL ? name : "unknown");
    SDL_SetRenderDrawColor(sink->renderer, 0, 0, 0, 255);
    SDL_RenderClear(sink->renderer);
    SDL_RenderPresent(sink->renderer);
    return true;
}

/* Draw the current texture letterboxed into the window and present it. Presenter thread, sink lock held. */
static void window_draw(present_video_sink *sink)
{
    sink->redraw = false;
    if (!pthread_equal(pthread_self(), sink->owner_thread)) {
        sink->counts.off_thread_sdl_calls++;
    }
    if (sink->live) {
        SDL_GetWindowSizeInPixels(sink->window, &sink->output_width, &sink->output_height);
        sink->live_ops.present(sink->live_ops.context, sink->pq_count != 0u);
        return;
    }
    int out_width = 0;
    int out_height = 0;
    if (!SDL_GetCurrentRenderOutputSize(sink->renderer, &out_width, &out_height)) {
        record_error(sink, "output size");
        return;
    }
    sink->output_width = out_width;
    sink->output_height = out_height;
    SDL_SetRenderDrawColor(sink->renderer, 0, 0, 0, 255);
    if (!SDL_RenderClear(sink->renderer)) {
        record_error(sink, "clear");
    }
    if (sink->texture != NULL && sink->width != 0u && sink->height != 0u) {
        /* Letterbox: the largest rectangle of the picture's aspect that fits the window. */
        float scale = (float)out_width / (float)sink->texture_width;
        const float vertical = (float)out_height / (float)sink->texture_height;
        if (vertical < scale) {
            scale = vertical;
        }
        const SDL_FRect destination = {((float)out_width - (float)sink->texture_width * scale) / 2.0f,
                                       ((float)out_height - (float)sink->texture_height * scale) / 2.0f,
                                       (float)sink->texture_width * scale, (float)sink->texture_height * scale};
        if (!SDL_RenderTexture(sink->renderer, sink->texture, NULL, &destination)) {
            record_error(sink, "render texture");
        }
    }
    if (!SDL_RenderPresent(sink->renderer)) {
        record_error(sink, "present");
    }
}

static bool picture_has_light(const uint8_t *rgb, size_t bytes)
{
    for (size_t i = 0u; i < bytes; i++) {
        if (rgb[i] >= 8u) {
            return true;
        }
    }
    return false;
}

/* Upload the latched picture and draw it. Presenter thread, sink lock held. */
static void window_show(present_video_sink *sink)
{
    if (sink->live) {
        if (picture_has_light(sink->pending, (size_t)sink->width * sink->height * 3u)) {
            sink->counts.presented_nonblack++;
        }
        if (!pthread_equal(pthread_self(), sink->owner_thread)) {
            sink->counts.off_thread_sdl_calls++;
        }
        sink->live_ops.picture(sink->live_ops.context, sink->width, sink->height, sink->pending);
        sink->live_ops.present(sink->live_ops.context, sink->pq_count != 0u);
        return;
    }
    if (sink->texture == NULL || sink->texture_width != sink->width || sink->texture_height != sink->height) {
        if (sink->texture != NULL) {
            SDL_DestroyTexture(sink->texture);
        }
        sink->texture = SDL_CreateTexture(sink->renderer, SDL_PIXELFORMAT_RGB24,
                                          SDL_TEXTUREACCESS_STREAMING, (int)sink->width, (int)sink->height);
        sink->texture_width = sink->width;
        sink->texture_height = sink->height;
        if (sink->texture == NULL) {
            record_error(sink, "create texture");
            return;
        }
        SDL_SetTextureScaleMode(sink->texture, SDL_SCALEMODE_NEAREST);
    }
    if (picture_has_light(sink->pending, (size_t)sink->width * sink->height * 3u)) {
        sink->counts.presented_nonblack++;
    }
    if (!SDL_UpdateTexture(sink->texture, NULL, sink->pending, (int)sink->width * 3)) {
        record_error(sink, "update texture");
    }
    window_draw(sink);
}

/* The jobs the presenter thread runs for a caller (sink lock held by the caller). */
static void window_pump(present_video_sink *sink);

static int64_t monotonic_ns(void);
static void note_presented(present_video_sink *sink);
static void window_show(present_video_sink *sink);

/* T819: show the picture the media clock has reached. Presenter thread, sink lock held. With `everything` (the
 * stop) or while no media clock exists the newest queued picture is shown at once. */
static void playback_tick(present_video_sink *sink, bool everything)
{
    if (!sink->playback || sink->pq_count == 0u) {
        return;
    }
    uint64_t media_vt = 0u;
    const bool clocked = !everything && present_audio_sink_media_time(sink->playback_audio, &media_vt);
    size_t consumed = 0u;
    if (!clocked) {
        consumed = sink->pq_count;
    } else {
        while (consumed < sink->pq_count &&
               sink->pq[(sink->pq_head + consumed) % PLAYBACK_QUEUE_SLOTS].vt_ns <= media_vt) {
            consumed++;
        }
    }
    if (consumed == 0u) {
        return;
    }
    const size_t slot = (sink->pq_head + consumed - 1u) % PLAYBACK_QUEUE_SLOTS;
    uint8_t *shown = sink->pq[slot].rgb;
    const size_t shown_capacity = sink->pq[slot].capacity;
    sink->pq[slot].rgb = sink->pending; /* the slot keeps the old picture's buffer for reuse */
    sink->pq[slot].capacity = sink->capacity;
    sink->pending = shown;
    sink->capacity = shown_capacity;
    sink->width = sink->pq[slot].width;
    sink->height = sink->pq[slot].height;
    if (clocked && media_vt - sink->pq[slot].vt_ns > PLAYBACK_LATE_NS) {
        sink->timing.late_shown++;
    }
    sink->timing.dropped += consumed - 1u;
    sink->pq_head = (sink->pq_head + consumed) % PLAYBACK_QUEUE_SLOTS;
    sink->pq_count -= consumed;
    pthread_cond_broadcast(&sink->queue_room);
    sink->counts.presented++;
    note_presented(sink);
    window_show(sink);
}

static void note_presented(present_video_sink *sink)
{
    const int64_t now = monotonic_ns();
    if (sink->last_presented_ns != 0) {
        const int64_t gap = (now - sink->last_presented_ns) / 1000;
        if (sink->interval_count < TIMING_SAMPLES) {
            sink->interval_us[sink->interval_count++] = gap > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)gap;
        }
        if (gap > 25000) {
            sink->timing.over_25ms++;
        }
    }
    sink->last_presented_ns = now;
}

static void job_vblank(present_video_sink *sink, void *argument)
{
    (void)argument;
    const uint64_t pump_start = gpu_phase_now();
    window_pump(sink);
    gpu_phase_add(GPU_PHASE_WINDOW_PUMP, pump_start);
    if (sink->dirty) {
        sink->dirty = false;
        sink->counts.presented++;
        note_presented(sink);
        window_show(sink);
    } else if (sink->redraw || sink->live) {
        window_draw(sink); /* live: every modelled vblank presents the scheduled front buffer */
    }
}

static void job_pump(present_video_sink *sink, void *argument)
{
    (void)argument;
    window_pump(sink);
}

static void job_present_latched(present_video_sink *sink, void *argument)
{
    (void)argument;
    playback_tick(sink, true);
    if (sink->dirty) { /* a picture latched after the last vblank is shown at the stop */
        sink->dirty = false;
        sink->counts.presented++;
        window_show(sink);
    }
}

/* Presenter thread, between jobs: keep the window alive (events, expose and resize redraws). Never waits for
 * the sink lock, a caller holding it is waiting for the presenter. */
static void window_idle(present_video_sink *sink)
{
    if (pthread_mutex_trylock(&sink->lock) != 0) {
        return;
    }
    playback_tick(sink, false);
    window_pump(sink);
    if (sink->redraw) {
        window_draw(sink);
    }
    pthread_mutex_unlock(&sink->lock);
}

typedef struct {
    present_video_sink *sink;
    const char *title;
} presenter_start;

static void *presenter_main(void *argument)
{
    const presenter_start *start = argument;
    present_video_sink *sink = start->sink;
    sink->owner_thread = pthread_self();
    const bool good = window_open(sink, start->title);
    pthread_mutex_lock(&sink->job_lock);
    sink->open_good = good;
    sink->open_finished = true;
    pthread_cond_broadcast(&sink->done_cond);
    while (good && !sink->stop_thread) {
        if (sink->job_pending && !sink->job_done) {
            void (*job)(present_video_sink *, void *) = sink->job;
            void *job_argument = sink->job_arg;
            pthread_mutex_unlock(&sink->job_lock);
            job(sink, job_argument);
            pthread_mutex_lock(&sink->job_lock);
            sink->job_done = true;
            if (sink->job_async) {
                sink->job_async = false;
                sink->job_pending = false;
            }
            pthread_cond_broadcast(&sink->done_cond);
            continue;
        }
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += (sink->playback ? 2L : 8L) * 1000 * 1000;
        if (until.tv_nsec >= 1000000000L) {
            until.tv_nsec -= 1000000000L;
            until.tv_sec++;
        }
        pthread_cond_timedwait(&sink->job_cond, &sink->job_lock, &until);
        if (!sink->job_pending && !sink->stop_thread) {
            pthread_mutex_unlock(&sink->job_lock);
            window_idle(sink);
            pthread_mutex_lock(&sink->job_lock);
        }
    }
    pthread_mutex_unlock(&sink->job_lock);
    return NULL;
}

/* Run `job` on the presenter thread and wait for it. The caller normally holds sink->lock. */
static void blocked_note(present_video_sink *sink, unsigned kind, int64_t start)
{
    const uint64_t spent = (uint64_t)(monotonic_ns() - start);
    sink->blocked_ns[kind] += spent; /* job_lock held by the caller */
    sink->blocked_calls[kind]++;
    if (spent > sink->blocked_max_ns[kind]) {
        sink->blocked_max_ns[kind] = spent;
    }
}

static void sdl_run_kind(present_video_sink *sink, present_job_kind kind, void (*job)(present_video_sink *, void *), void *argument)
{
    const int64_t start = monotonic_ns();
    gft_enter(GFT_SYNC_SLOT, kind);
    pthread_mutex_lock(&sink->job_lock);
    while (sink->job_pending) {
        pthread_cond_wait(&sink->done_cond, &sink->job_lock);
    }
    const int64_t accepted = monotonic_ns();
    gft_leave();
    gft_enter(GFT_SYNC_EXEC, kind);
    sink->job = job;
    sink->job_arg = argument;
    sink->job_pending = true;
    sink->job_done = false;
    pthread_cond_signal(&sink->job_cond);
    while (!sink->job_done) {
        pthread_cond_wait(&sink->done_cond, &sink->job_lock);
    }
    sink->job_pending = false;
    gft_leave();
    blocked_note(sink, 0u, start);
    {
        const uint64_t waited = (uint64_t)(accepted - start), executed = (uint64_t)(monotonic_ns() - accepted);
        sink->kind_calls[kind]++;
        sink->kind_wait_ns[kind] += waited;
        sink->kind_exec_ns[kind] += executed;
        if (waited > sink->kind_wait_max_ns[kind]) sink->kind_wait_max_ns[kind] = waited;
        if (executed > sink->kind_exec_max_ns[kind]) sink->kind_exec_max_ns[kind] = executed;
    }
    pthread_cond_broadcast(&sink->done_cond);
    pthread_mutex_unlock(&sink->job_lock);
}

static void sdl_run(present_video_sink *sink, void (*job)(present_video_sink *, void *), void *argument)
{
    sdl_run_kind(sink, PRESENT_JOB_OTHER, job, argument);
}

/* T1246: queue `job` on the presenter thread and return at once. At most one job is in flight: a second post, and every sdl_run,
 * waits for it first, so jobs run in the order they were issued and any synchronous call is a drain of the queue. */
static void job_run_posted(present_video_sink *sink, void *argument)
{
    (void)argument;
    sink->post_fn(sink->post_context);
}

static void sdl_post(present_video_sink *sink, void (*fn)(void *), void *context)
{
    const int64_t start = monotonic_ns();
    gft_enter(GFT_SYNC_SLOT, PRESENT_JOB_SERIAL_FRAME); /* a posted frame waits for the previous one */
    pthread_mutex_lock(&sink->job_lock);
    while (sink->job_pending) {
        pthread_cond_wait(&sink->done_cond, &sink->job_lock);
    }
    gft_leave();
    sink->post_fn = fn;
    sink->post_context = context;
    sink->job = job_run_posted;
    sink->job_arg = NULL;
    sink->job_pending = true;
    sink->job_async = true;
    sink->job_done = false;
    blocked_note(sink, 1u, start);
    pthread_cond_signal(&sink->job_cond);
    pthread_mutex_unlock(&sink->job_lock);
}

static void sdl_drain(present_video_sink *sink)
{
    const int64_t start = monotonic_ns();
    gft_enter(GFT_SYNC_SLOT, PRESENT_JOB_OTHER); /* a drain: a guest report slot read or rewritten */
    pthread_mutex_lock(&sink->job_lock);
    while (sink->job_pending) {
        pthread_cond_wait(&sink->done_cond, &sink->job_lock);
    }
    gft_leave();
    blocked_note(sink, 2u, start);
    pthread_mutex_unlock(&sink->job_lock);
}

static const char *const letter_names[26] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
                                             "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z"};
static const char *const digit_names[10] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};

static const char *key_name(SDL_Keycode key)
{
    if (key >= SDLK_A && key <= SDLK_Z) return letter_names[key - SDLK_A];
    if (key >= SDLK_0 && key <= SDLK_9) return digit_names[key - SDLK_0];
    switch (key) {
    case SDLK_UP: return "UP";
    case SDLK_DOWN: return "DOWN";
    case SDLK_LEFT: return "LEFT";
    case SDLK_RIGHT: return "RIGHT";
    case SDLK_RETURN:
    case SDLK_KP_ENTER: return "ENTER";
    case SDLK_BACKSPACE: return "BACKSPACE";
    case SDLK_SPACE: return "SPACE";
    case SDLK_TAB: return "TAB";
    case SDLK_F1: return "F1";
    /* T1627: named so a host hotkey (xinput_hotkey.h) can use them, they stay UNMAPPED in the pad table */
    case SDLK_LCTRL: return "LCTRL";
    case SDLK_RCTRL: return "RCTRL";
    case SDLK_LSHIFT: return "LSHIFT";
    case SDLK_RSHIFT: return "RSHIFT";
    case SDLK_LALT: return "LALT";
    case SDLK_RALT: return "RALT";
    default: return NULL;
    }
}

static const char *button_name(Uint8 button)
{
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "A";
    case SDL_GAMEPAD_BUTTON_EAST: return "B";
    case SDL_GAMEPAD_BUTTON_WEST: return "X";
    case SDL_GAMEPAD_BUTTON_NORTH: return "Y";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB";
    case SDL_GAMEPAD_BUTTON_START: return "START";
    case SDL_GAMEPAD_BUTTON_BACK: return "BACK";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "GUIDE";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "LSTICK";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "RSTICK";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "DPAD_UP";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "DPAD_DOWN";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "DPAD_LEFT";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "DPAD_RIGHT";
    default: return NULL;
    }
}

static const char *axis_name(Uint8 axis, bool *invert)
{
    *invert = false;
    switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: return "LX";
    case SDL_GAMEPAD_AXIS_LEFTY: *invert = true; return "LY";
    case SDL_GAMEPAD_AXIS_RIGHTX: return "RX";
    case SDL_GAMEPAD_AXIS_RIGHTY: *invert = true; return "RY";
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return "LT";
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "RT";
    default: return NULL;
    }
}

static void held_key_down(present_video_sink *sink, const char *name)
{
    for (size_t i = 0u; i < sink->held_count; i++)
        if (sink->held_keys[i] == name) return;
    if (sink->held_count < sizeof sink->held_keys / sizeof sink->held_keys[0]) sink->held_keys[sink->held_count++] = name;
}

static void held_key_up(present_video_sink *sink, const char *name)
{
    for (size_t i = 0u; i < sink->held_count; i++) {
        if (sink->held_keys[i] == name) {
            sink->held_keys[i] = sink->held_keys[--sink->held_count];
            return;
        }
    }
}

static void gamepad_open(present_video_sink *sink, SDL_JoystickID id)
{
    for (size_t i = 0u; i < sizeof sink->gamepads / sizeof sink->gamepads[0]; i++)
        if (sink->gamepads[i] != NULL && SDL_GetGamepadID(sink->gamepads[i]) == id) return; /* ADDED may repeat */
    for (size_t i = 0u; i < sizeof sink->gamepads / sizeof sink->gamepads[0]; i++) {
        if (sink->gamepads[i] == NULL) {
            sink->gamepads[i] = SDL_OpenGamepad(id);
            return;
        }
    }
}

static void gamepad_close(present_video_sink *sink, SDL_JoystickID id)
{
    for (size_t i = 0u; i < sizeof sink->gamepads / sizeof sink->gamepads[0]; i++) {
        if (sink->gamepads[i] != NULL && SDL_GetGamepadID(sink->gamepads[i]) == id) {
            SDL_CloseGamepad(sink->gamepads[i]);
            sink->gamepads[i] = NULL;
        }
    }
}

static void window_input_event(present_video_sink *sink, const SDL_Event *event)
{
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        if (!sink->input_keyboard || event->key.repeat) return;
        const bool down = event->type == SDL_EVENT_KEY_DOWN;
        const char *name = key_name(event->key.key);
        if (name != NULL) {
            if (down) held_key_down(sink, name);
            else held_key_up(sink, name);
        }
        input_push(sink, down ? PRESENT_INPUT_KEY_DOWN : PRESENT_INPUT_KEY_UP, name, (int32_t)event->key.key, 0);
        return;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (!sink->input_keyboard) return;
        while (sink->held_count != 0u) {
            const char *name = sink->held_keys[--sink->held_count];
            input_push(sink, PRESENT_INPUT_KEY_UP, name, 0, 0);
        }
        return;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (sink->input_gamepad) gamepad_open(sink, event->gdevice.which);
        return;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (sink->input_gamepad) gamepad_close(sink, event->gdevice.which);
        return;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (!sink->input_gamepad) return;
        input_push(sink, event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ? PRESENT_INPUT_PAD_DOWN : PRESENT_INPUT_PAD_UP,
                   button_name(event->gbutton.button), (int32_t)event->gbutton.button, 0);
        return;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (!sink->input_gamepad) return;
        bool invert;
        const char *name = axis_name(event->gaxis.axis, &invert);
        int32_t value = event->gaxis.value;
        if (invert) value = value == -32768 ? 32767 : -value;
        input_push(sink, PRESENT_INPUT_PAD_AXIS, name, (int32_t)event->gaxis.axis, value);
        return;
    }
    default:
        return;
    }
}

static void window_pump(present_video_sink *sink)
{
    SDL_Event event;
    if (!pthread_equal(pthread_self(), sink->owner_thread)) {
        sink->counts.off_thread_sdl_calls++;
    }
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
            (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)) {
            const char *cause = event.type == SDL_EVENT_QUIT ? "SDL quit event (SIGINT, SIGTERM or session end)" :
                                event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ? "window close request" :
                                "Escape key";
            const char *none = NULL;
            (void)atomic_compare_exchange_strong(&sink->close_cause, &none, cause);
            sink->closed_by_user = true;
            atomic_store(&sink->user_closed, true);
            continue;
        }
        if (event.type == SDL_EVENT_WINDOW_EXPOSED || event.type == SDL_EVENT_WINDOW_RESIZED ||
            event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_SHOWN ||
            event.type == SDL_EVENT_WINDOW_RESTORED) {
            sink->redraw = true; /* the compositor may have dropped the frame: draw it again */
            continue;
        }
        window_input_event(sink, &event);
    }
}
#endif

present_video_sink *present_video_sink_open(present_video_kind kind, const char *title, const char **error)
{
    const char *ignored = NULL;
    if (error == NULL) {
        error = &ignored;
    }
    *error = NULL;
    if (kind == PRESENT_VIDEO_NONE) {
        return NULL;
    }
    present_video_sink *sink = calloc(1u, sizeof *sink);
    if (sink == NULL) {
        *error = "present sink: out of memory";
        return NULL;
    }
    pthread_mutex_init(&sink->lock, NULL);
    atomic_init(&sink->user_closed, false);
    pthread_cond_init(&sink->queue_room, NULL);
    sink->kind = kind;
    if (kind == PRESENT_VIDEO_WINDOW && s_live_ops_set) {
        sink->live = true;
        sink->live_ops = s_live_ops;
    }
    if (kind == PRESENT_VIDEO_WINDOW) {
#ifdef TSFP_HAVE_SDL3
        pthread_mutex_init(&sink->job_lock, NULL);
        pthread_cond_init(&sink->job_cond, NULL);
        pthread_cond_init(&sink->done_cond, NULL);
        presenter_start start = {sink, title};
        const bool started = pthread_create(&sink->thread, NULL, presenter_main, &start) == 0;
        if (started) {
            pthread_mutex_lock(&sink->job_lock);
            while (!sink->open_finished) {
                pthread_cond_wait(&sink->done_cond, &sink->job_lock);
            }
            pthread_mutex_unlock(&sink->job_lock);
        } else {
            snprintf(sdl_error, sizeof sdl_error, "--present window: could not start the presenter thread");
        }
        if (!started || !sink->open_good) {
            if (started) {
                pthread_join(sink->thread, NULL);
            }
            *error = sdl_error;
            pthread_cond_destroy(&sink->done_cond);
            pthread_cond_destroy(&sink->job_cond);
            pthread_mutex_destroy(&sink->job_lock);
            pthread_cond_destroy(&sink->queue_room);
            pthread_mutex_destroy(&sink->lock);
            free(sink);
            return NULL;
        }
        sink->thread_started = true;
#else
        (void)title;
        *error = "--present window is not available: this build has no SDL3";
        pthread_cond_destroy(&sink->queue_room);
        pthread_mutex_destroy(&sink->lock);
        free(sink);
        return NULL;
#endif
    }
    return sink;
}

static void submit_locked(present_video_sink *sink, uint64_t number, uint32_t width, uint32_t height,
                          const uint8_t *rgb)
{
    if (sink == NULL) {
        return;
    }
    sink->counts.submitted++;
    sink->counts.last_number = number;
    if (rgb == NULL || width == 0u || height == 0u) {
        sink->counts.failed++;
        return;
    }
    if (sink->kind != PRESENT_VIDEO_WINDOW) {
        return; /* null and png-dir only count */
    }
    const size_t bytes = (size_t)width * height * 3u;
    if (bytes > sink->capacity) {
        uint8_t *grown = realloc(sink->pending, bytes);
        if (grown == NULL) {
            sink->counts.failed++;
            return;
        }
        sink->pending = grown;
        sink->capacity = bytes;
    }
    if (sink->dirty) {
        sink->timing.replaced++;
    }
    memcpy(sink->pending, rgb, bytes);
    sink->width = width;
    sink->height = height;
    sink->dirty = true;
}

static void vblank_locked(present_video_sink *sink)
{
    if (sink == NULL) {
        return;
    }
    sink->counts.vblanks++;
    if (sink->kind != PRESENT_VIDEO_WINDOW || (sink->playback && !sink->live)) {
        return; /* playback mode: the presenter thread shows pictures, the guest never waits here */
    }
#ifdef TSFP_HAVE_SDL3
    sdl_run_kind(sink, PRESENT_JOB_VBLANK, job_vblank, NULL);
#endif
}

static int64_t (*s_test_clock)(void);

void present_audio_sink_set_test_clock(int64_t (*now_ns)(void))
{
    s_test_clock = now_ns;
}

static int64_t monotonic_ns(void)
{
    if (s_test_clock != NULL) {
        return s_test_clock();
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
}

/* Sleep to the next wall-clock vblank slot, outside the sink lock. A late slot resynchronises. */
static void pace_wait(present_video_sink *sink)
{
    pthread_mutex_lock(&sink->lock);
    if (sink->pace_millihertz == 0u) {
        pthread_mutex_unlock(&sink->lock);
        return;
    }
    const int64_t period = 1000000000000LL / (int64_t)sink->pace_millihertz;
    const int64_t now = monotonic_ns();
    if (!sink->pace_started) {
        sink->pace_started = true;
        sink->pace_deadline_ns = now + period;
    } else {
        sink->pace_deadline_ns += period;
    }
    if (now > sink->pace_deadline_ns) {
        const uint64_t late_us = (uint64_t)(now - sink->pace_deadline_ns) / 1000u;
        sink->timing.pace_late++;
        if (late_us > sink->timing.pace_late_max_us) {
            sink->timing.pace_late_max_us = late_us;
        }
    }
    if (now > sink->pace_deadline_ns + 250000000LL) {
        sink->pace_deadline_ns = now;
    }
    const struct timespec until = {(time_t)(sink->pace_deadline_ns / 1000000000LL),
                                   (long)(sink->pace_deadline_ns % 1000000000LL)};
    if (sink->pace_deadline_ns > now) {
        sink->timing.pace_sleeps++;
        sink->timing.pace_slept_us += (uint64_t)(sink->pace_deadline_ns - now) / 1000u;
    }
    pthread_mutex_unlock(&sink->lock);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, NULL) == EINTR) {
    }
}

void present_video_sink_set_interactive(present_video_sink *sink, bool enabled)
{
    if (sink == NULL) return;
    pthread_mutex_lock(&sink->lock);
    sink->interactive = enabled;
    pthread_mutex_unlock(&sink->lock);
}
bool present_video_sink_interactive(present_video_sink *sink)
{
    return sink != NULL && sink->interactive; /* immutable after startup */
}
const char *present_video_sink_close_cause(present_video_sink *sink)
{
    return sink == NULL ? NULL : atomic_load(&sink->close_cause);
}
bool present_video_sink_closed(present_video_sink *sink)
{
    return sink != NULL && atomic_load(&sink->user_closed);
}
void present_video_sink_request_close(present_video_sink *sink, const char *cause)
{
    if (sink == NULL) return;
    const char *none = NULL;
    (void)atomic_compare_exchange_strong(&sink->close_cause, &none, cause);
    atomic_store(&sink->user_closed, true);
}

void present_video_sink_set_pace(present_video_sink *sink, uint32_t millihertz)
{
    if (sink != NULL && sink->kind == PRESENT_VIDEO_WINDOW) {
        sink->pace_millihertz = millihertz;
        sink->pace_started = false;
    }
}

#ifdef TSFP_HAVE_SDL3
typedef struct {
    uint32_t x;
    uint32_t y;
    uint8_t rgb[3];
    bool good;
} read_pixel_request;

static void job_read_pixel(present_video_sink *sink, void *argument)
{
    read_pixel_request *request = argument;
    if (sink->live) {
        request->good = sink->live_ops.read_pixel != NULL && sink->live_ops.read_pixel(sink->live_ops.context, request->x, request->y, request->rgb);
        return;
    }
    const SDL_Rect area = {(int)request->x, (int)request->y, 1, 1};
    SDL_Surface *surface = SDL_RenderReadPixels(sink->renderer, &area);
    if (surface == NULL) {
        return;
    }
    Uint8 red = 0;
    Uint8 green = 0;
    Uint8 blue = 0;
    Uint8 alpha = 0;
    request->good = SDL_ReadSurfacePixel(surface, 0, 0, &red, &green, &blue, &alpha);
    SDL_DestroySurface(surface);
    request->rgb[0] = red;
    request->rgb[1] = green;
    request->rgb[2] = blue;
}

typedef struct {
    const char *path;
    bool good;
} capture_request;

static void job_capture(present_video_sink *sink, void *argument)
{
    capture_request *request = argument;
    job_present_latched(sink, NULL);
    if (sink->live) {
        request->good = sink->live_ops.capture != NULL && sink->live_ops.capture(sink->live_ops.context, request->path);
        return;
    }
    SDL_Surface *surface = SDL_RenderReadPixels(sink->renderer, NULL);
    request->good = surface != NULL && SDL_SaveBMP(surface, request->path);
    if (surface != NULL) {
        SDL_DestroySurface(surface);
    }
}

typedef struct {
    bool gamepad;
    bool good;
} input_request;

static void gamepad_open(present_video_sink *sink, SDL_JoystickID id);

static void job_enable_gamepad(present_video_sink *sink, void *argument)
{
    input_request *request = argument;
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        snprintf(sdl_error, sizeof sdl_error, "window input: SDL gamepad init failed: %s", SDL_GetError());
        return;
    }
    sink->input_gamepad_init = true;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != NULL && i < count; i++) gamepad_open(sink, ids[i]);
    SDL_free(ids);
    request->good = true;
}

static void job_teardown(present_video_sink *sink, void *argument)
{
    (void)argument;
    for (size_t i = 0u; i < sizeof sink->gamepads / sizeof sink->gamepads[0]; i++)
        if (sink->gamepads[i] != NULL) SDL_CloseGamepad(sink->gamepads[i]);
    if (sink->input_gamepad_init) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    if (sink->live) {
        sink->live_ops.close(sink->live_ops.context); /* every Vulkan object, before the window it draws to */
    }
    if (sink->texture != NULL) {
        SDL_DestroyTexture(sink->texture);
    }
    if (sink->renderer != NULL) {
        SDL_DestroyRenderer(sink->renderer);
    }
    SDL_DestroyWindow(sink->window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
#endif

static bool read_pixel_locked(present_video_sink *sink, uint32_t x, uint32_t y, uint8_t rgb[3]);

void present_video_sink_submit(present_video_sink *sink, uint64_t number, uint32_t width, uint32_t height,
                               const uint8_t *rgb)
{
    if (sink != NULL) {
        pthread_mutex_lock(&sink->lock);
        submit_locked(sink, number, width, height, rgb);
        pthread_mutex_unlock(&sink->lock);
    }
}

void present_video_sink_set_playback(present_video_sink *sink, present_audio_sink *audio)
{
    if (sink == NULL || sink->kind != PRESENT_VIDEO_WINDOW) {
        return;
    }
#ifdef TSFP_T926_TEST_HOOKS
    const int lock_status = pthread_mutex_trylock(&sink->lock);
    const bool lock_busy = lock_status == EBUSY;
    if (lock_status == 0) {
        pthread_mutex_unlock(&sink->lock);
    }
    T926_NOTE(PRESENT_SINK_T926_PLAYBACK_LOCK_ATTEMPT, lock_busy);
#endif
    pthread_mutex_lock(&sink->lock);
    T926_NOTE(PRESENT_SINK_T926_PLAYBACK_LOCK_ACQUIRED, false);
    sink->playback = audio != NULL;
    sink->playback_audio = audio;
    pthread_mutex_unlock(&sink->lock);
}

#ifdef TSFP_HAVE_SDL3
typedef struct {
    void (*fn)(void *);
    void *context;
} run_request;

static void job_run(present_video_sink *sink, void *argument)
{
    (void)sink;
    const run_request *request = argument;
    request->fn(request->context);
}
#endif

bool present_video_sink_run(present_video_sink *sink, void (*fn)(void *context), void *context)
{
    return present_video_sink_run_kind(sink, PRESENT_JOB_OTHER, fn, context);
}

bool present_video_sink_run_kind(present_video_sink *sink, present_job_kind kind, void (*fn)(void *context), void *context)
{
#ifdef TSFP_HAVE_SDL3
    if (sink == NULL || sink->kind != PRESENT_VIDEO_WINDOW || !sink->thread_started) {
        return false;
    }
    run_request request = {fn, context};
    pthread_mutex_lock(&sink->lock);
    sdl_run_kind(sink, kind < PRESENT_JOB_KINDS ? kind : PRESENT_JOB_OTHER, job_run, &request);
    pthread_mutex_unlock(&sink->lock);
    return true;
#else
    (void)sink;
    (void)kind;
    (void)fn;
    (void)context;
    return false;
#endif
}

bool present_video_sink_post(present_video_sink *sink, void (*fn)(void *context), void *context)
{
#ifdef TSFP_HAVE_SDL3
    if (sink == NULL || sink->kind != PRESENT_VIDEO_WINDOW || !sink->thread_started) {
        return false;
    }
    sdl_post(sink, fn, context);
    return true;
#else
    (void)sink;
    (void)fn;
    (void)context;
    return false;
#endif
}

void present_video_sink_drain(present_video_sink *sink)
{
#ifdef TSFP_HAVE_SDL3
    if (sink != NULL && sink->kind == PRESENT_VIDEO_WINDOW && sink->thread_started) {
        sdl_drain(sink);
    }
#else
    (void)sink;
#endif
}

void present_video_sink_job_kind_stats(present_video_sink *sink, present_job_kind kind, present_job_kind_stats *out)
{
    memset(out, 0, sizeof *out);
    if (sink == NULL || kind >= PRESENT_JOB_KINDS) {
        return;
    }
    pthread_mutex_lock(&sink->job_lock);
    out->calls = sink->kind_calls[kind];
    out->wait_ms = (double)sink->kind_wait_ns[kind] / 1e6;
    out->exec_ms = (double)sink->kind_exec_ns[kind] / 1e6;
    out->wait_max_ms = (double)sink->kind_wait_max_ns[kind] / 1e6;
    out->exec_max_ms = (double)sink->kind_exec_max_ns[kind] / 1e6;
    pthread_mutex_unlock(&sink->job_lock);
}

void present_video_sink_blocked(present_video_sink *sink, unsigned kind, uint64_t *calls, double *total_ms, double *max_ms)
{
    *calls = 0u;
    *total_ms = *max_ms = 0.0;
    if (sink != NULL && kind < 3u) {
        pthread_mutex_lock(&sink->job_lock);
        *calls = sink->blocked_calls[kind];
        *total_ms = (double)sink->blocked_ns[kind] / 1e6;
        *max_ms = (double)sink->blocked_max_ns[kind] / 1e6;
        pthread_mutex_unlock(&sink->job_lock);
    }
}

size_t present_video_sink_pictures_pending(present_video_sink *sink)
{
    if (sink == NULL) {
        return 0u;
    }
    pthread_mutex_lock(&sink->lock);
    const size_t count = sink->pq_count;
    pthread_mutex_unlock(&sink->lock);
    return count;
}

size_t present_video_sink_job_pictures_pending(present_video_sink *sink)
{
    return sink != NULL ? sink->pq_count : 0u; /* inside a job the caller of present_video_sink_run holds sink->lock */
}

bool present_video_sink_playback_enabled(present_video_sink *sink)
{
    if (sink == NULL) {
        return false;
    }
    pthread_mutex_lock(&sink->lock);
    const bool enabled = sink->playback;
    pthread_mutex_unlock(&sink->lock);
    return enabled;
}

bool present_video_sink_job_playback_enabled(present_video_sink *sink)
{
    return sink != NULL && sink->playback; /* only from a presenter job, with sink->lock already held */
}

bool present_video_sink_job_media_time(present_video_sink *sink, uint64_t *vt_ns)
{
    return sink != NULL && sink->playback && present_audio_sink_media_time(sink->playback_audio, vt_ns);
}

static void queue_drop_oldest(present_video_sink *sink)
{
    sink->pq_head = (sink->pq_head + 1u) % PLAYBACK_QUEUE_SLOTS;
    sink->pq_count--;
    sink->timing.dropped++;
}

void present_video_sink_submit_at(present_video_sink *sink, uint64_t number, uint32_t width, uint32_t height,
                                  const uint8_t *rgb, uint64_t vt_ns)
{
    if (sink == NULL) {
        return;
    }
    pthread_mutex_lock(&sink->lock);
    if (!sink->playback || rgb == NULL || width == 0u || height == 0u) {
        submit_locked(sink, number, width, height, rgb);
        pthread_mutex_unlock(&sink->lock);
        return;
    }
    sink->counts.submitted += 1u;
    sink->counts.last_number = number;
    if (sink->pq_count == PLAYBACK_QUEUE_SLOTS && !sink->closing) {
        /* Bounded run-ahead: the guest waits for the presenter to free a slot. The audio clock is kicked first so a
         * refill that never completes cannot hold the picture queue (and so the guest) forever. */
        const int64_t wait_start = monotonic_ns();
        sink->timing.queue_waits++;
        present_audio_sink_kick(sink->playback_audio);
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += PLAYBACK_ROOM_WAIT_MS / 1000;
        deadline.tv_nsec += (long)(PLAYBACK_ROOM_WAIT_MS % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        while (sink->pq_count == PLAYBACK_QUEUE_SLOTS && !sink->closing) {
            if (pthread_cond_timedwait(&sink->queue_room, &sink->lock, &deadline) == ETIMEDOUT) {
                break;
            }
        }
        sink->timing.queue_wait_us += (uint64_t)(monotonic_ns() - wait_start) / 1000u;
        if (sink->pq_count == PLAYBACK_QUEUE_SLOTS) {
            sink->timing.queue_timeouts++;
            queue_drop_oldest(sink);
        }
    }
    const size_t bytes = (size_t)width * height * 3u;
    const size_t tail = (sink->pq_head + sink->pq_count) % PLAYBACK_QUEUE_SLOTS;
    if (bytes > sink->pq[tail].capacity) {
        uint8_t *grown = realloc(sink->pq[tail].rgb, bytes);
        if (grown == NULL) {
            sink->counts.failed++;
            pthread_mutex_unlock(&sink->lock);
            return;
        }
        sink->pq[tail].rgb = grown;
        sink->pq[tail].capacity = bytes;
    }
    memcpy(sink->pq[tail].rgb, rgb, bytes);
    sink->pq[tail].width = width;
    sink->pq[tail].height = height;
    sink->pq[tail].number = number;
    sink->pq[tail].vt_ns = vt_ns;
    if (sink->have_submit && vt_ns > sink->last_submit_vt_ns &&
        vt_ns - sink->last_submit_vt_ns > sink->timing.model_gap_max_ns) {
        sink->timing.model_gap_max_ns = vt_ns - sink->last_submit_vt_ns;
        sink->timing.model_gap_after = sink->last_submit_number;
    }
    sink->have_submit = true;
    sink->last_submit_vt_ns = vt_ns;
    sink->last_submit_number = number;
    sink->pq_count++;
    if (sink->pq_count > sink->timing.queue_max) {
        sink->timing.queue_max = sink->pq_count;
    }
    pthread_mutex_unlock(&sink->lock);
}

void present_video_sink_vblank(present_video_sink *sink)
{
    if (sink != NULL) {
        const int64_t hook_start = monotonic_ns();
        pthread_mutex_lock(&sink->lock);
        vblank_locked(sink);
        pthread_mutex_unlock(&sink->lock);
        const uint64_t hook_us = (uint64_t)(monotonic_ns() - hook_start) / 1000u;
        if (!sink->playback || sink->interactive) {
            gft_enter(GFT_PACE, 0u);
            pace_wait(sink);
            gft_leave();
        }
        const int64_t exit_ns = monotonic_ns();
        pthread_mutex_lock(&sink->lock);
        if (sink->last_vblank_exit_ns != 0) {
            const uint64_t gap_us = (uint64_t)(exit_ns - sink->last_vblank_exit_ns) / 1000u;
            static const uint64_t edges_us[7] = {10000u, 15000u, 18000u, 22000u, 34000u, 50000u, 100000u};
            size_t bucket = 0u;
            while (bucket < 7u && gap_us >= edges_us[bucket]) {
                bucket++;
            }
            sink->timing.vblank_hist[bucket]++;
            sink->timing.vblank_intervals++;
            for (size_t i = 0u; i < 5u; i++) {
                if (gap_us > sink->timing.vblank_spikes_us[i]) {
                    for (size_t j = 4u; j > i; j--) {
                        sink->timing.vblank_spikes_us[j] = sink->timing.vblank_spikes_us[j - 1u];
                    }
                    sink->timing.vblank_spikes_us[i] = gap_us;
                    break;
                }
            }
        }
        sink->last_vblank_exit_ns = exit_ns;
        sink->timing.hook_us_total += hook_us;
        if (hook_us > sink->timing.hook_us_max) {
            sink->timing.hook_us_max = hook_us;
        }
        pthread_mutex_unlock(&sink->lock);
        guest_frame_trace_boundary(); /* T1289: closes the interval of the Swap thread (a no-op on every other thread) */
    }
}

bool present_video_sink_read_pixel(present_video_sink *sink, uint32_t x, uint32_t y, uint8_t rgb[3])
{
    if (sink == NULL) {
        return false;
    }
    pthread_mutex_lock(&sink->lock);
    const bool good = read_pixel_locked(sink, x, y, rgb);
    pthread_mutex_unlock(&sink->lock);
    return good;
}

static int compare_u32(const void *left, const void *right)
{
    const uint32_t a = *(const uint32_t *)left;
    const uint32_t b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

present_video_timing present_video_sink_timing(present_video_sink *sink)
{
    present_video_timing out = {0};
    if (sink == NULL) {
        return out;
    }
    pthread_mutex_lock(&sink->lock);
    out = sink->timing;
    out.queued_at_end = sink->pq_count;
    const size_t count = sink->interval_count;
    if (count != 0u) {
        uint32_t *sorted = malloc(count * sizeof *sorted);
        if (sorted != NULL) {
            memcpy(sorted, sink->interval_us, count * sizeof *sorted);
            qsort(sorted, count, sizeof *sorted, compare_u32);
            out.intervals = count;
            out.min_us = sorted[0];
            out.median_us = sorted[count / 2u];
            out.p95_us = sorted[(count * 95u) / 100u < count ? (count * 95u) / 100u : count - 1u];
            out.max_us = sorted[count - 1u];
            free(sorted);
        }
    }
    pthread_mutex_unlock(&sink->lock);
    return out;
}

present_video_counts present_video_sink_counts(const present_video_sink *sink)
{
    const present_video_counts none = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    return sink != NULL ? sink->counts : none;
}

void present_video_sink_hold(present_video_sink *sink, uint32_t milliseconds)
{
#ifdef TSFP_HAVE_SDL3
    if (sink == NULL || sink->kind != PRESENT_VIDEO_WINDOW) {
        return;
    }
    pthread_mutex_lock(&sink->lock);
    sdl_run(sink, job_present_latched, NULL);
    pthread_mutex_unlock(&sink->lock);
    const uint64_t start = SDL_GetTicks();
    for (;;) {
        pthread_mutex_lock(&sink->lock);
        sdl_run(sink, job_pump, NULL);
        const bool closed = sink->closed_by_user || atomic_load(&sink->user_closed);
        pthread_mutex_unlock(&sink->lock);
        if (closed || SDL_GetTicks() - start >= milliseconds) {
            break;
        }
        SDL_Delay(20);
    }
#else
    (void)sink;
    (void)milliseconds;
#endif
}

static bool read_pixel_locked(present_video_sink *sink, uint32_t x, uint32_t y, uint8_t rgb[3])
{
#ifdef TSFP_HAVE_SDL3
    if (sink == NULL || sink->kind != PRESENT_VIDEO_WINDOW) {
        return false;
    }
    read_pixel_request request = {x, y, {0u, 0u, 0u}, false};
    sdl_run(sink, job_read_pixel, &request);
    rgb[0] = request.rgb[0];
    rgb[1] = request.rgb[1];
    rgb[2] = request.rgb[2];
    return request.good;
#else
    (void)sink;
    (void)x;
    (void)y;
    (void)rgb;
    return false;
#endif
}

bool present_video_sink_capture(present_video_sink *sink, const char *path)
{
#ifdef TSFP_HAVE_SDL3
    if (sink == NULL || path == NULL || sink->kind != PRESENT_VIDEO_WINDOW) {
        return false;
    }
    capture_request request = {path, false};
    pthread_mutex_lock(&sink->lock);
    sdl_run(sink, job_capture, &request);
    pthread_mutex_unlock(&sink->lock);
    return request.good;
#else
    (void)sink;
    (void)path;
    return false;
#endif
}

bool present_video_sink_enable_input(present_video_sink *sink, bool keyboard, bool gamepad, const char **error)
{
    const char *ignored = NULL;
    if (error == NULL) error = &ignored;
    *error = NULL;
#ifdef TSFP_HAVE_SDL3
    if (sink == NULL || sink->kind != PRESENT_VIDEO_WINDOW) {
        *error = "window input needs the --present window sink";
        return false;
    }
    pthread_mutex_lock(&sink->lock);
    if (gamepad && !sink->input_gamepad) {
        input_request request = {true, false};
        sdl_run(sink, job_enable_gamepad, &request);
        if (!request.good) {
            *error = sdl_error;
            pthread_mutex_unlock(&sink->lock);
            return false;
        }
    }
    sink->input_keyboard = sink->input_keyboard || keyboard;
    sink->input_gamepad = sink->input_gamepad || gamepad;
    pthread_mutex_unlock(&sink->lock);
    return true;
#else
    (void)sink;
    (void)keyboard;
    (void)gamepad;
    *error = "window input is not available: this build has no SDL3";
    return false;
#endif
}

bool present_video_sink_input_next(present_video_sink *sink, present_input_event *out)
{
    if (sink == NULL) return false;
    pthread_mutex_lock(&sink->lock);
    const bool have = sink->queue_count != 0u;
    if (have) {
        *out = sink->queue[sink->queue_head];
        sink->queue_head = (sink->queue_head + 1u) % PRESENT_INPUT_QUEUE_SIZE;
        sink->queue_count--;
    }
    pthread_mutex_unlock(&sink->lock);
    return have;
}

uint64_t present_video_sink_input_dropped(const present_video_sink *sink)
{
    return sink != NULL ? sink->input_dropped : 0u;
}

size_t present_video_sink_gamepads_open(const present_video_sink *sink)
{
    size_t open = 0u;
#ifdef TSFP_HAVE_SDL3
    if (sink != NULL)
        for (size_t i = 0u; i < sizeof sink->gamepads / sizeof sink->gamepads[0]; i++)
            if (sink->gamepads[i] != NULL) open++;
#else
    (void)sink;
#endif
    return open;
}

size_t present_video_sink_describe(present_video_sink *sink, char *buffer, size_t size)
{
    if (buffer == NULL || size == 0u) {
        return 0u;
    }
    buffer[0] = '\0';
#ifdef TSFP_HAVE_SDL3
    if (sink == NULL || sink->kind != PRESENT_VIDEO_WINDOW) {
        return 0u;
    }
    pthread_mutex_lock(&sink->lock);
    const int written = snprintf(
        buffer, size,
        "present window: SDL renderer %s, window output %dx%d, texture %ux%u, pictures presented %llu "
        "(non-black %llu), SDL errors %llu%s%s",
        sink->renderer_name, sink->output_width, sink->output_height, sink->texture_width, sink->texture_height,
        (unsigned long long)sink->counts.presented, (unsigned long long)sink->counts.presented_nonblack,
        (unsigned long long)sink->counts.present_errors, sink->counts.present_errors != 0u ? ", last: " : "",
        sink->counts.present_errors != 0u ? sink->last_error : "");
    pthread_mutex_unlock(&sink->lock);
    return written > 0 ? (size_t)written : 0u;
#else
    (void)sink;
    return 0u;
#endif
}

void present_video_sink_close(present_video_sink *sink)
{
    if (sink == NULL) {
        return;
    }
    pthread_mutex_lock(&sink->lock);
    sink->closing = true; /* a guest waiting for queue room is released */
    pthread_cond_broadcast(&sink->queue_room);
    pthread_mutex_unlock(&sink->lock);
#ifdef TSFP_HAVE_SDL3
    if (sink->kind == PRESENT_VIDEO_WINDOW) {
        if (sink->thread_started) {
            pthread_mutex_lock(&sink->lock);
            sdl_run(sink, job_teardown, NULL);
            pthread_mutex_unlock(&sink->lock);
            pthread_mutex_lock(&sink->job_lock);
            sink->stop_thread = true;
            pthread_cond_broadcast(&sink->job_cond);
            pthread_mutex_unlock(&sink->job_lock);
            pthread_join(sink->thread, NULL);
        }
        pthread_cond_destroy(&sink->done_cond);
        pthread_cond_destroy(&sink->job_cond);
        pthread_mutex_destroy(&sink->job_lock);
    }
#endif
    free(sink->pending);
    for (size_t slot = 0u; slot < PLAYBACK_QUEUE_SLOTS; slot++) {
        free(sink->pq[slot].rgb);
    }
    pthread_cond_destroy(&sink->queue_room);
    pthread_mutex_destroy(&sink->lock);
    free(sink);
}

/* ---- T827 timeline sampler ---- */
static pthread_t s_timeline_thread;
static volatile bool s_timeline_run;
static bool s_timeline_started;
static struct {
    present_audio_sink *audio;
    present_video_sink *video;
    present_clock_fn virtual_ns;
    FILE *file;
} s_timeline;

static void timeline_threads(char *out, size_t size, unsigned long *last_ticks, size_t slots)
{
    DIR *dir = opendir("/proc/self/task");
    size_t used = 0u;
    out[0] = '\0';
    if (dir == NULL) {
        return;
    }
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        const long tid = strtol(item->d_name, NULL, 10);
        if (tid <= 0) {
            continue;
        }
        char path[64];
        snprintf(path, sizeof path, "/proc/self/task/%ld/stat", tid);
        FILE *stat_file = fopen(path, "r");
        if (stat_file == NULL) {
            continue;
        }
        char line[512];
        if (fgets(line, sizeof line, stat_file) != NULL) {
            const char *after = strrchr(line, ')');
            unsigned long utime = 0u, stime = 0u;
            if (after != NULL &&
                sscanf(after + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime) == 2) {
                const unsigned long total = utime + stime;
                if (total > last_ticks[(size_t)tid % slots] && used + 24u < size) {
                    used += (size_t)snprintf(out + used, size - used, " %ld:%lu", tid, total - last_ticks[(size_t)tid % slots]);
                }
                last_ticks[(size_t)tid % slots] = total;
            }
        }
        fclose(stat_file);
    }
    closedir(dir);
}

static void *timeline_main(void *argument)
{
    (void)argument;
    enum { THREAD_SLOTS = 1u << 16 };
    unsigned long *last_ticks = calloc(THREAD_SLOTS, sizeof *last_ticks);
    const int64_t start = monotonic_ns();
    while (s_timeline_run && last_ticks != NULL) {
        struct timespec nap = {0, 25 * 1000000L};
        nanosleep(&nap, NULL);
        const present_audio_counts heard = present_audio_sink_counts(s_timeline.audio);
        size_t queued = 0u;
        if (s_timeline.video != NULL) {
            pthread_mutex_lock(&s_timeline.video->lock);
            queued = s_timeline.video->pq_count;
            pthread_mutex_unlock(&s_timeline.video->lock);
        }
        pthread_mutex_lock(&s_timeline.audio->lock);
        const size_t fill = s_timeline.audio->ring_fill;
        const bool playing = s_timeline.audio->playing;
        pthread_mutex_unlock(&s_timeline.audio->lock);
        char threads[256];
        timeline_threads(threads, sizeof threads, last_ticks, THREAD_SLOTS);
        fprintf(s_timeline.file, "%lld,%llu,%zu,%llu,%llu,%zu,%d,%s\n", (long long)((monotonic_ns() - start) / 1000000),
                (unsigned long long)(s_timeline.virtual_ns() / 1000000u), fill,
                (unsigned long long)(heard.written * 1000u / s_timeline.audio->sample_rate),
                (unsigned long long)(heard.pulled * 1000u / s_timeline.audio->sample_rate), queued, playing ? 1 : 0,
                threads);
    }
    free(last_ticks);
    return NULL;
}

bool present_timeline_start(present_audio_sink *audio, present_video_sink *video, present_clock_fn virtual_ns,
                            const char *path)
{
    if (audio == NULL || virtual_ns == NULL || path == NULL || s_timeline_started) {
        return false;
    }
    s_timeline.file = fopen(path, "w");
    if (s_timeline.file == NULL) {
        return false;
    }
    fputs("wall_ms,modelled_ms,ring_fill,written_ms,played_ms,pictures_queued,playing,thread:cpu_ticks\n", s_timeline.file);
    s_timeline.audio = audio;
    s_timeline.video = video;
    s_timeline.virtual_ns = virtual_ns;
    s_timeline_run = true;
    if (pthread_create(&s_timeline_thread, NULL, timeline_main, NULL) != 0) {
        fclose(s_timeline.file);
        return false;
    }
    s_timeline_started = true;
    return true;
}

void present_timeline_stop(void)
{
    if (!s_timeline_started) {
        return;
    }
    s_timeline_run = false;
    pthread_join(s_timeline_thread, NULL);
    fclose(s_timeline.file);
    s_timeline_started = false;
}
