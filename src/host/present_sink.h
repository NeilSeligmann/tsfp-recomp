/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * T744: the presentation interface of the host, opt-in and never default.
 *
 * Video sink (--present SINK): null, png-dir, window.
 *   null    discards. Needs nothing. Exists so the seam can be exercised without files.
 *   png-dir the existing --dump-overlay path (T537), named as a sink. Needs --dump-overlay DIR.
 *   window  an SDL3 window (T760), only when the build found SDL3 (TSFP_HAVE_SDL3, optional CMake
 *           dependency), otherwise REFUSED with the named reason. Software blit through the SDL renderer.
 * Every video sink is a present_video_sink object that counts frames: the overlay hook submits one
 * picture per UpdateOverlay and signals each modelled vblank. The picture is latched at submit and shown
 * at the next vblank, so the overlay appears one vblank late (INFERRED, T540, to be measured in xemu).
 * Audio sink (--audio-sink SINK): null, wav-file, device.
 *   null     discards. Needs nothing.
 *   wav-file REFUSED today: the DirectSound HLE emits no PCM (dsound_hle.h, it does not mix).
 *   device   REFUSED: superseded by `sdl`.
 *   sdl      (T761) the SDL3 audio device, only when the build found SDL3, otherwise REFUSED with the
 *            named reason. A ring buffer sits between the producer (the future HLE mixer, T759, calling
 *            present_audio_sink_write on the modelled timeline) and the device callback, which only PULLS
 *            what the timeline produced (present_audio_sink_pull) and never drives the mix. A shortfall
 *            is filled with silence and counted as an underrun. Timing is the modelled AC97 timeline,
 *            INFERRED until T733, T743 and an xemu reference exist.
 * Absent flags change nothing. Nothing here touches guest state.
 */
#ifndef TSFP_PRESENT_SINK_H
#define TSFP_PRESENT_SINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    PRESENT_VIDEO_NONE = 0,
    PRESENT_VIDEO_NULL,
    PRESENT_VIDEO_PNG_DIR,
    PRESENT_VIDEO_WINDOW
} present_video_kind;

typedef enum {
    PRESENT_AUDIO_NONE = 0,
    PRESENT_AUDIO_NULL,
    PRESENT_AUDIO_WAV_FILE,
    PRESENT_AUDIO_DEVICE,
    PRESENT_AUDIO_SDL
} present_audio_kind;

/* Resolve the flag values. NULL name means absent (kind NONE, true). On refusal returns false and sets
 * *reason to a static text naming the missing prerequisite flag, decision or dependency. */
bool present_video_select(const char *name, bool have_dump_overlay, present_video_kind *kind,
                          const char **reason);
bool present_audio_select(const char *name, bool hle_emits_pcm, present_audio_kind *kind,
                          const char **reason);

const char *present_video_name(present_video_kind kind);
const char *present_audio_name(present_audio_kind kind);

/* One announce line per active sink, empty string for NONE. */
const char *present_video_announce(present_video_kind kind);
const char *present_audio_announce(present_audio_kind kind);

/* True when this build has the SDL3 window sink. */
bool present_window_available(void);
/* True when this build has the SDL3 audio device sink (same dependency). */
bool present_audio_device_available(void);

/* Video sink objects (T760). */
typedef struct present_video_sink present_video_sink;
/* Opens the sink of `kind`. NULL with *error set to a static or buffer-held text on failure (for the
 * window: SDL init failed, no display). NONE returns NULL with *error NULL. */
present_video_sink *present_video_sink_open(present_video_kind kind, const char *title,
                                            const char **error);
/* Latch one picture (copied). rgb NULL counts a failed picture and keeps the last good one. */
void present_video_sink_submit(present_video_sink *sink, uint64_t number, uint32_t width,
                               uint32_t height, const uint8_t *rgb);
/* One modelled vblank: show the latched picture if a new one arrived since the last vblank. */
void present_video_sink_vblank(present_video_sink *sink);
void present_video_sink_set_interactive(present_video_sink *sink, bool enabled);
bool present_video_sink_closed(present_video_sink *sink);
/* T1492: WHY the window asked to close ("Escape key", "window close request" or "SDL quit event
 * (SIGINT, SIGTERM or session end)"), or NULL while it has not. The first cause wins. */
const char *present_video_sink_close_cause(present_video_sink *sink);
/* T1632: ask for the same shutdown as closing the window (the stop hotkey). `cause` is static text, reported by
 * present_video_sink_close_cause, the first cause wins. Safe from the guest thread, no SDL call. */
void present_video_sink_request_close(present_video_sink *sink, const char *cause);
bool present_video_sink_interactive(present_video_sink *sink);
typedef struct {
    uint64_t submitted; /* pictures handed in, failed ones included */
    uint64_t failed;    /* submitted without a picture */
    uint64_t vblanks;   /* modelled vblanks seen */
    uint64_t presented; /* vblanks that showed a new picture */
    uint64_t last_number;
    uint64_t presented_nonblack; /* presented pictures with at least one channel >= 8 (a black window with this > 0 is the display path, not the picture) */
    uint64_t present_errors;     /* SDL texture, draw or present calls that reported failure */
    uint64_t off_thread_sdl_calls; /* draws or event pumps that ran on a thread other than the one that made the window, must stay 0 */
} present_video_counts;
present_video_counts present_video_sink_counts(const present_video_sink *sink);
/* T819 timing of the window sink (wall clock, INFERRED): intervals between presented pictures, replaced pictures,
 * and what the guest thread spent inside the vblank hook. Zeros for the other sinks. */
typedef struct {
    uint64_t intervals;       /* presented-picture intervals recorded */
    uint64_t min_us, median_us, p95_us, max_us;
    uint64_t over_25ms;       /* intervals longer than 25 ms */
    uint64_t replaced;        /* pictures overwritten by a newer one before any vblank showed them */
    uint64_t pace_sleeps;     /* vblank hooks that slept to the wall slot */
    uint64_t pace_slept_us;   /* total slept */
    uint64_t pace_late;       /* hooks that arrived after their slot */
    uint64_t pace_late_max_us;
    uint64_t hook_us_total;   /* guest thread time inside present_video_sink_vblank, sleep excluded */
    uint64_t hook_us_max;
    uint64_t dropped;         /* playback: pictures the media clock had already passed, never shown */
    uint64_t queue_max;       /* playback: most pictures waiting at once */
    uint64_t queue_waits;     /* playback: submits that found the queue full */
    uint64_t queue_wait_us;   /* playback: wall time the guest waited for room */
    uint64_t queue_timeouts;  /* playback: waits that gave up (oldest picture dropped) */
    uint64_t late_shown;      /* playback: pictures shown more than one frame (40 ms) after their stamp */
    uint64_t queued_at_end;   /* playback: pictures still waiting when the report was taken */
    uint64_t model_gap_max_ns; /* playback: longest modelled time between two submitted pictures (the title's own pause) */
    uint64_t model_gap_after;  /* playback: number of the picture before that gap */
    /* T1235: wall time between two consecutive vblank hook EXITS (after the pacing sleep), the guest's frame pacing as the
     * player sees it. Buckets: <10, <15, <18, <22, <34, <50, <100, >=100 ms. Spikes: the five longest intervals. */
    uint64_t vblank_hist[8];
    uint64_t vblank_intervals;
    uint64_t vblank_spikes_us[5];
} present_video_timing;
present_video_timing present_video_sink_timing(present_video_sink *sink);

/* T819 playback mode (window only, opt-in, INFERRED timing): the sink stops pacing the guest and presents the
 * pictures itself. Each picture handed to present_video_sink_submit_at carries its modelled (virtual clock)
 * nanosecond, goes into a bounded queue, and the presenter thread shows it when the audio device's media clock
 * (present_audio_sink_media_time of `audio`) has reached that stamp: the audio clock is the master, a picture the
 * clock has already passed is dropped (counted) and the newest due one is shown, a clock that stands still
 * (rebuffering) holds the window. The guest blocks only when the queue is full (it then also kicks the audio
 * clock so a stalled refill cannot deadlock). Without a media clock yet the newest picture is shown at once.
 * `vblank` no longer sleeps. Call before the first submit, with the audio sink in clock mode. */
typedef struct present_audio_sink present_audio_sink;
#ifdef TSFP_T926_TEST_HOOKS
/* Test-only synchronization points. The T926 test-library variant defines these; the
 * production tsfp_host_options library and tsfp_host do not. Configure before threads start. */
typedef enum {
    PRESENT_SINK_T926_AUDIO_QUERY_ENTER,
    PRESENT_SINK_T926_AUDIO_QUERY_LEAVE,
    PRESENT_SINK_T926_PLAYBACK_LOCK_ATTEMPT,
    PRESENT_SINK_T926_PLAYBACK_LOCK_ACQUIRED,
} present_sink_t926_event;
typedef void (*present_sink_t926_hook)(present_sink_t926_event event, bool lock_busy, void *context);
void present_sink_t926_set_hook(present_sink_t926_hook hook, void *context);
#endif
void present_video_sink_set_playback(present_video_sink *sink, present_audio_sink *audio);
void present_video_sink_submit_at(present_video_sink *sink, uint64_t number, uint32_t width, uint32_t height,
                                  const uint8_t *rgb, uint64_t vt_ns);
/* T838 (M9 host hook-up): the LIVE output stage of the window sink, opt-in (--gpu-live). When ops are installed before
 * present_video_sink_open, the window is an SDL_WINDOW_VULKAN window (no SDL_Renderer can share it) and the sink keeps ALL its
 * logic (latch, 59.94 Hz pacing, T819 playback queue with the audio device clock as master, input events, hold, stop) but its
 * output goes to these callbacks instead of the SDL_Renderer. Every callback runs on the presenter thread, which owns the window
 * and every Vulkan object (T815). `present` is called at each modelled vblank, after each picture, and on expose or resize,
 * with whether movie pictures still wait in the playback queue (the title's own frames are held back while they do, so a
 * movie that the audio clock still plays is not covered by the title frames the guest ran ahead to). */
typedef struct {
    bool (*open)(void *context, void *sdl_window, char *error, size_t error_size);
    void (*picture)(void *context, uint32_t width, uint32_t height, const uint8_t *rgb);
    void (*present)(void *context, bool pictures_pending);
    bool (*read_pixel)(void *context, uint32_t x, uint32_t y, uint8_t rgb[3]);
    bool (*capture)(void *context, const char *path);
    void (*close)(void *context);
    void *context;
} present_live_ops;
void present_video_set_live_ops(const present_live_ops *ops); /* NULL removes; read by present_video_sink_open */
/* Run `fn` on the presenter thread and wait for it (the guest thread hooks of the live renderer). False for a sink with no
 * presenter thread. */
bool present_video_sink_run(present_video_sink *sink, void (*fn)(void *context), void *context);
/* T1262: the kind of a synchronous presenter job, for the stop report breakdown (which job the guest thread waits in). */
typedef enum {
    PRESENT_JOB_OTHER = 0,
    PRESENT_JOB_VBLANK,       /* the modelled vblank: pump events, present the scheduled front */
    PRESENT_JOB_OBSERVER,     /* the second observer: record the front buffer the guest presented */
    PRESENT_JOB_SERIAL_FRAME, /* a frame drawn serially (--live-pipeline 0, or a frame that is not self contained) */
    PRESENT_JOB_REGISTER,     /* a render target registration */
    PRESENT_JOB_KINDS
} present_job_kind;
bool present_video_sink_run_kind(present_video_sink *sink, present_job_kind kind, void (*fn)(void *context), void *context);
typedef struct {
    uint64_t calls;
    double wait_ms, exec_ms;         /* total: waiting for the job slot (the frame job in flight), then waiting for the job itself */
    double wait_max_ms, exec_max_ms; /* worst single call */
} present_job_kind_stats;
void present_video_sink_job_kind_stats(present_video_sink *sink, present_job_kind kind, present_job_kind_stats *out);
/* T1246: queue `fn` on the presenter thread and return without waiting. One job is in flight at a time: a later post and every
 * present_video_sink_run wait for it first, so jobs run in issue order and any synchronous call drains the queue. `fn` runs WITHOUT
 * the sink lock, so it must not use the *_job_* accessors below (use present_video_sink_pictures_pending before posting). */
bool present_video_sink_post(present_video_sink *sink, void (*fn)(void *context), void *context);
/* T1246: wait until the posted job (if any) has finished. */
void present_video_sink_drain(present_video_sink *sink);
/* T1246: how long calling threads (the guest thread) spent blocked in presenter jobs, posts and drains: calls, total and worst ms. */
/* kind 0 = synchronous jobs, 1 = posts, 2 = drains */
void present_video_sink_blocked(present_video_sink *sink, unsigned kind, uint64_t *calls, double *total_ms, double *max_ms);
/* Takes the sink lock, for a caller that holds none (the guest thread). */
size_t present_video_sink_pictures_pending(present_video_sink *sink);
/* Pictures waiting in the playback queue (0 outside playback mode). ONLY from inside a job given to present_video_sink_run or from a
 * live op: those run with the sink lock held by the caller, so this does not take it (a second lock would deadlock). */
size_t present_video_sink_job_pictures_pending(present_video_sink *sink);
/* Playback state may be queried from the guest thread. The media clock query is for presenter jobs only; it does not relock. */
bool present_video_sink_playback_enabled(present_video_sink *sink);
bool present_video_sink_job_playback_enabled(present_video_sink *sink);
bool present_video_sink_job_media_time(present_video_sink *sink, uint64_t *vt_ns);

/* Window only: one line for the stop report, the SDL renderer name, window output size, texture size, non-black
 * pictures presented and the count and text of the last SDL error, so a black window can be reported from a host
 * whose display cannot be seen. Writes "" for the other sinks. Returns the length. */
size_t present_video_sink_describe(present_video_sink *sink, char *buffer, size_t size);
/* Window only: pace present_video_sink_vblank to wall clock, `millihertz` vblanks per second (59940 is the NTSC
 * Xbox rate, INFERRED), 0 turns pacing off (the default). Without it a run free-runs the modelled vblanks at CPU
 * speed and a 30 picture per second movie flashes past in a fraction of a second. A late vblank resynchronises
 * instead of bursting to catch up. */
void present_video_sink_set_pace(present_video_sink *sink, uint32_t millihertz);
/* Window only: keep the window open up to `milliseconds`, ends early on close or Escape. Presents a
 * still-latched picture first. Returns at once for the other sinks. */
void present_video_sink_hold(present_video_sink *sink, uint32_t milliseconds);
/* Window only: read back one window pixel from the last presented frame. False for other sinks. */
bool present_video_sink_read_pixel(present_video_sink *sink, uint32_t x, uint32_t y, uint8_t rgb[3]);
/* Window only: show a still-latched picture, then write the window's current frame to `path` as a BMP, read
 * back from the SDL renderer (what the window shows). False for other sinks or on failure. */
bool present_video_sink_capture(present_video_sink *sink, const char *path);
void present_video_sink_close(present_video_sink *sink);

/* T751: host input from the window (opt-in, FABRICATED mapping names, see xinput_host_source.h). The SDL events
 * are pumped on the window pump (every modelled vblank and while holding), UNDER the sink mutex, and converted
 * to SDL-free events with the table names of xinput_host_source.c. The guest poll pops them under the same
 * mutex, so it only ever sees whole events. Keys: arrows UP DOWN LEFT RIGHT, ENTER (also keypad), BACKSPACE,
 * SPACE, TAB, F1, letters A..Z, digits 0..9, anything else has a NULL name and is counted as unknown by the
 * source. ESCAPE closes the window and is not queued. Key repeat is dropped. Losing the keyboard focus queues
 * an UP for every held key. Gamepad: SDL's Xbox layout (SOUTH A, EAST B, WEST X, NORTH Y, shoulders LB RB,
 * START, BACK, GUIDE, sticks LSTICK RSTICK, dpad) and axes LX LY RX RY LT RT, the two Y axes NEGATED (SDL up is
 * negative, XInput up is positive, INFERRED), pads open on SDL_EVENT_GAMEPAD_ADDED and close on REMOVED.
 * The queue holds PRESENT_INPUT_QUEUE_SIZE events, an event past that is DROPPED and counted, never silent. */
#define PRESENT_INPUT_QUEUE_SIZE 256u
typedef enum {
    PRESENT_INPUT_KEY_DOWN = 1,
    PRESENT_INPUT_KEY_UP,
    PRESENT_INPUT_PAD_DOWN,
    PRESENT_INPUT_PAD_UP,
    PRESENT_INPUT_PAD_AXIS
} present_input_kind;
typedef struct {
    present_input_kind kind;
    const char *name; /* table name (static), NULL when the host code has no name */
    int32_t raw;      /* the SDL key, button or axis code */
    int32_t value;    /* AXIS only: LX..RY -32768..32767, triggers 0..32767 */
} present_input_event;
/* Window only. Turns the keyboard and or gamepad events on (a gamepad also initialises the SDL gamepad
 * subsystem and opens the pads already attached). False with *error for a non window sink or an SDL failure,
 * and always in a build without SDL3. */
bool present_video_sink_enable_input(present_video_sink *sink, bool keyboard, bool gamepad, const char **error);
/* Pop the oldest queued event under the sink mutex. False when empty. */
bool present_video_sink_input_next(present_video_sink *sink, present_input_event *out);
uint64_t present_video_sink_input_dropped(const present_video_sink *sink);
/* Number of gamepads currently open. */
size_t present_video_sink_gamepads_open(const present_video_sink *sink);

/* Audio sink objects. write takes interleaved signed 16 bit little endian frames. */
typedef struct present_audio_sink present_audio_sink;
present_audio_sink *present_audio_sink_open(present_audio_kind kind, const char *wav_path,
                                            uint32_t sample_rate, uint16_t channels);
/* The sdl sink's ring without any device (no SDL3 needed): the caller pulls. For tests and a fake device. */
present_audio_sink *present_audio_sink_open_ring(uint32_t sample_rate, uint16_t channels);
/* Backpressure for a ring sink: with a nonzero timeout a write into a full ring WAITS for the consumer to pull
 * (the producer is paced to the device clock, nothing is dropped) and only drops, counted as overrun, if no
 * room appears within the timeout (stalled device). 0 = never block, drop on full. The sdl device sink
 * enables it by default (the virtual clock can burst ahead of wall time, dropping skipped the music ahead);
 * wav-file, null and the device-less ring stay non-blocking so headless runs are unchanged. */
void present_audio_sink_set_pacing(present_audio_sink *sink, uint32_t stall_timeout_ms);
bool present_audio_sink_write(present_audio_sink *sink, const int16_t *samples, size_t frames);
uint64_t present_audio_sink_frames(const present_audio_sink *sink);
/* Pull model (kind sdl; the device callback or a test calls it). Copies up to `frames` queued frames to
 * `out` and fills the rest with silence. Returns the frames that were real. Silence after the first
 * written frame is an underrun, counted. Never blocks, safe against a concurrent write. */
/* T819 audio clock mode (the media clock the window presenter follows, INFERRED timing). The device plays real
 * PCM only while "playing": it starts once the ring holds `prefill_ms` of audio, and when the ring runs dry
 * playback pauses (silence goes to the device, the media clock stops, the window holds its picture) until the
 * ring is refilled, with the refill target doubled each time up to `max_prefill_ms`. The ring becomes
 * `ring_seconds` long. Call before the first write. The audio clock is the master: video follows it. */
void present_audio_sink_enable_clock(present_audio_sink *sink, uint32_t prefill_ms, uint32_t max_prefill_ms,
                                     uint32_t ring_seconds);
/* write, stamped with the modelled (virtual clock) nanosecond at which the LAST frame of `samples` ends. The first
 * stamped write anchors the media clock. */
bool present_audio_sink_write_at(present_audio_sink *sink, const int16_t *samples, size_t frames,
                                 uint64_t end_vt_ns);
/* The modelled nanosecond of the audio the device has played. False before the first stamped write. */
/* Starts playback now if the ring holds any audio (a producer that is blocked elsewhere uses it so a refill cannot
 * wait on a guest that waits on the refill). */
void present_audio_sink_kick(present_audio_sink *sink);
/* T1235: hold the audio latency the host adds. The ring fill is the latency (the guest writes at its own clock, the
 * device drains at wall rate, nothing else ever shortens a lead built by a rebuffer pause or a virtual clock burst).
 * Above target + hysteresis the sink sheds the excess down to the target: runs of digital silence (128 frames or more)
 * at the head are dropped, and the rest is consumed up to max_trim_permille faster than real time (linear
 * interpolation, a 1.5 s lead takes the full trim). The media clock follows the frames consumed. 0 target = off. */
void present_audio_sink_set_latency_governor(present_audio_sink *sink, uint32_t target_ms, uint32_t hysteresis_ms,
                                             uint32_t max_trim_permille);
/* T1248: continuous playback below real time. When the ring fill is under low_water_ms the device consumes it slower
 * (ratio = fill / low water, never below min_rate_permille / 1000, smoothed, linear interpolation: the pitch drops with
 * the guest's own slow motion) instead of draining the ring and pausing every voice for a refill. A ring that still runs
 * dry (a guest stall longer than the slowed ring lasts) keeps the rebuffer pause. 0 min rate = off. Clock mode only. */
void present_audio_sink_set_continuous_playback(present_audio_sink *sink, uint32_t low_water_ms, uint32_t min_rate_permille);
/* T1250: serve the slowed playback (set_continuous_playback sets the low water and the floor) with a pitch preserving
 * WSOLA stretch instead of the resampler. Clock mode only. */
void present_audio_sink_set_stretch(present_audio_sink *sink, bool enabled);
/* T1487 wobble: the stretch's tuning, call BEFORE set_stretch. slew_*: largest change of the consumption ratio per 1024 frame
 * pull in permille of real time (1000 = unlimited, the pre T1487 behaviour of falling by half the gap). smooth_ms: time
 * constant of the ring fill the ratio is computed from (0 = none). engage: a slowdown above this ratio (permille) is not
 * stretched, leave: a ratio above this returns to real time. block_us: the WSOLA block (search range is half of it). */
typedef struct {
    uint32_t block_us;
    uint32_t slew_down_permille;
    uint32_t slew_up_permille;
    uint32_t smooth_ms;
    uint32_t engage_permille;
    uint32_t leave_permille;
} present_audio_stretch_tuning;
void present_audio_sink_set_stretch_tuning(present_audio_sink *sink, const present_audio_stretch_tuning *tuning);
/* T1487: the media clock (what the video follows) reports the position `offset_ms` earlier: positive delays the video
 * against the audio (compensates the audio device's own latency), negative advances it. Default 0. */
void present_audio_sink_set_av_offset_ms(present_audio_sink *sink, int32_t offset_ms);
/* T1250 second half: how long the stretch bridges a ring too thin to continue (a guest production gap) by looping its last
 * segment instead of cutting every voice. Full gain for full_ms, then a linear decay to silence at max_ms, then it fades out
 * and rebuffers. A rebuffer under the stretch resumes as soon as one block is available (not at the refill target) with a
 * short fade in. max_ms 0 = no hold beyond one block. Defaults (no call): 100 / 100 ms (the T1250 behaviour). */
void present_audio_sink_set_stretch_hold(present_audio_sink *sink, uint32_t full_ms, uint32_t max_ms);
/* Tests only: replaces the wall clock the sink's timers read (NULL restores CLOCK_MONOTONIC). */
void present_audio_sink_set_test_clock(int64_t (*now_ns)(void));
bool present_audio_sink_media_time(present_audio_sink *sink, uint64_t *vt_ns);
size_t present_audio_sink_pull(present_audio_sink *sink, int16_t *out, size_t frames);
typedef struct {
    uint64_t written;         /* frames accepted by write */
    uint64_t pulled;          /* real frames handed to the device */
    uint64_t underrun_frames; /* silence frames pulled after the first write */
    uint64_t underrun_events; /* pulls that were short after the first write */
    uint64_t idle_frames;     /* silence pulled before any PCM existed (not an underrun) */
    uint64_t overrun_frames;  /* frames dropped because the ring was full (unpaced ring, or a device stalled past the pacing timeout) */
    uint64_t ring_frames;     /* ring capacity */
    uint64_t fill_min;        /* lowest fill seen at a pull after the first write (UINT64_MAX before any) */
    uint64_t fill_max;        /* highest fill seen after a write */
    uint64_t block_events;    /* writes that had to wait for room (T819) */
    uint64_t block_ns;        /* wall time the producer spent waiting for room */
    uint64_t block_max_ns;    /* longest single wait */
    uint64_t pulls;           /* device pulls */
    uint64_t write_calls;     /* producer writes */
    uint64_t rebuffer_events; /* clock mode: playback stopped because the ring ran dry, resumed after refilling (T819) */
    uint64_t rebuffer_frames; /* clock mode: silence pulled while waiting for the ring to refill */
    uint64_t prefill_frames;  /* clock mode: the fill needed to (re)start playback, grows after each rebuffer */
    uint64_t latency_target_frames;  /* T1235 governor: the ring fill (audio latency) it holds, 0 = off */
    uint64_t latency_silence_frames; /* T1235: digital silence dropped from the ring head to shed latency */
    uint64_t latency_trim_frames;    /* T1235: source frames consumed beyond the device rate by the bounded speed-up */
    uint64_t slow_low_frames;        /* T1248 continuous playback: the ring fill below which playback slows, 0 = off */
    uint64_t slow_frames;            /* T1248: output frames played from fewer source frames (the slowed playback, instead of silence) */
    uint64_t slow_pulls;             /* T1248: device pulls served below real time */
    uint64_t slow_min_ratio_q16;     /* T1248: the lowest consumption ratio used (65536 = real time, 0 = never slowed) */
    uint64_t cutout_events;          /* T1248: runs of silence handed to the device after playback began (every voice is cut) */
    uint64_t cutout_frames;          /* T1248: total frames of those runs */
    uint64_t cutout_max_frames;      /* T1248: the longest run */
    uint64_t stretch_enabled;        /* T1250: 1 when the slowed playback is the pitch preserving stretch (0 = resampler or off) */
    uint64_t stretch_engagements;    /* T1487: times the ratio left real time (each is an audible onset of the stretch) */
    uint64_t stretch_max_step_q16;   /* T1487: largest change of the stretch ratio between two pulls outside the panic zone (65536 = 1.0) */
    uint64_t stretch_panic_pulls;    /* T1487: pulls that found the ring under twice a block's need and used the fast fall (the cutout guard) */
    uint64_t stretch_blocks;         /* T1250: stretched output blocks (a block is about 6.7 ms) */
    uint64_t stretch_hold_frames;    /* T1250: output frames looped from the last segment because the ring was too thin to continue */
    uint64_t stretch_fadeouts;       /* T1250: holds that ran out (about 100 ms) and faded into a rebuffer */
    uint64_t stretch_decay_frames;   /* T1250 gaps: looped frames beyond the full gain part (the decaying tail of a hold) */
    uint64_t resume_events;          /* T1250 gaps: rebuffer resumes after a dry ring (not the start) */
    uint64_t resume_wait_sum_ms;     /* ... wall ms from the guest's first write after the dry to the resume, summed */
    uint64_t resume_wait_max_ms;     /* ... the largest */
    uint64_t prefill_peak_frames;    /* the largest refill target the sink adapted to (the report shows the growth) */
} present_audio_counts;
present_audio_counts present_audio_sink_counts(const present_audio_sink *sink);
/* T1235: the SDL device's own buffer in frames (0 when unknown or not an SDL sink). */
size_t present_audio_sink_device_frames(const present_audio_sink *sink);
/* T827: one record per playback stall (the start and every dry ring), for the stop report. Times are wall ms since the
 * first audio write, media_ms is the modelled ms of the sound played. */
#define PRESENT_AUDIO_STALL_MAX 16u
typedef struct {
    uint64_t wall_ms;       /* when the stall began */
    uint64_t media_ms;      /* modelled ms of sound played when it began */
    uint64_t written_ms;    /* modelled ms of sound written when it began */
    uint64_t since_write_ms; /* wall ms since the guest last wrote audio, at the stall start */
    uint64_t duration_ms;   /* wall ms until playback resumed (0 while open) */
    uint64_t fill_start;    /* ring frames at the start */
    uint64_t fill_end;      /* ring frames at the resume */
    uint64_t target_frames; /* refill target in force */
    uint64_t clock_ms;       /* T1250 gaps: CLOCK_MONOTONIC ms at the stall start (the clock of the call profile's slow call log) */
    uint64_t first_write_ms; /* T1250 gaps: wall ms from the stall start to the guest's first write (0 with none yet) */
    uint64_t resume_wait_ms; /* T1250 gaps: wall ms from that first write to the resume (the sink's own delay) */
    uint64_t writes_during; /* producer writes that arrived during the stall */
    uint8_t reason;         /* 0 open, 1 refill target reached, 2 give-up timeout, 3 kicked, 4 ring full */
    bool initial;           /* the start of playback, not a dry ring */
} present_audio_stall;
size_t present_audio_sink_stalls(const present_audio_sink *sink, present_audio_stall *out, size_t max);
/* sdl only: name of the failure when open returned NULL (static text), else NULL. */
const char *present_audio_sink_last_error(void);
/* Finalises the file header for wav-file. Returns false if the file could not be completed. */
bool present_audio_sink_close(present_audio_sink *sink);


/* T827: --present-timeline FILE. A sampler thread writes one CSV row every 25 ms of wall time:
 * wall_ms, modelled ms (vt_ns), ring fill frames, sound written ms, sound played ms, picture queue count,
 * and per OS thread the CPU ticks (1/100 s) spent since the previous row as tid:user+system. It shows whether the guest
 * was computing (CPU ticks, modelled time moving), blocked (neither moving), or starved. */
typedef uint64_t (*present_clock_fn)(void);
bool present_timeline_start(present_audio_sink *audio, present_video_sink *video, present_clock_fn virtual_ns,
                            const char *path);
void present_timeline_stop(void);

#endif
