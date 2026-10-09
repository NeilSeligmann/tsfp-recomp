/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1250 / user report 2026-10-06: audio cut off on a real machine (SDL path) although the T1248 slowed playback exists.
 * A simulated wall clock drives the REAL sink code (present_audio_sink, ring only, 48 kHz stereo) with the SDL device's
 * 1024 frame pulls and a guest that produces audio in bursts at about 0.36x real time (350 ms of audio per 960 ms of
 * wall, the pattern of the user's log, stalls #4 to #10). The output is analysed per 10 ms window: a window without
 * signal is a cutout, a tone's zero crossing rate gives the pitch. Modes: A (the shipped default before the fix: the
 * slowed playback off, pause and refill), B (T1248 resampler), C (T1250 pitch preserving stretch).
 */
#define _DEFAULT_SOURCE 1
#include "audio_stretch.h"
#include "present_sink.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define RATE 48000
#define DEVICE_FRAMES 1024
#define WINDOW 480
#define TONE_HZ 440.0
#define PI 3.14159265358979323846

static int64_t g_now_ns;
static unsigned g_hold_ms = 100u; /* T1250 gaps: the stretch hold (full gain 100 ms, decay to this); 100 = the old plain loop */

static int64_t test_clock(void)
{
    return g_now_ns;
}

typedef struct {
    size_t windows;       /* windows after the first signal */
    size_t silent;        /* of those, without signal */
    size_t runs;          /* runs of silent windows */
    size_t longest;       /* longest run, windows */
    double pitch_hz;      /* mean tone frequency over the windows with signal */
    int max_step;         /* largest jump between two neighbouring non-zero samples (a click), the tone's own step is about 461 */
    present_audio_counts counts;
    size_t silent_ms;     /* silent windows in ms */
    int resume_peak;      /* largest sample in the first 1 ms after a run of 5 ms of digital silence (a fade in keeps it small) */
    size_t resumes;       /* such runs of silence that ended */
    int resume_first;     /* largest first sample magnitude at those resumes (a fade in starts near zero) */
    size_t partial;       /* windows with a peak between 200 and 6500 of the tone's 8000 (a decay has many, a cut at most 2) */
    present_audio_stall last_stall; /* the last stall record (the dry ring) */
    size_t stall_count;
} outcome;

typedef enum { MODE_OFF, MODE_RESAMPLE, MODE_STRETCH } mode;

/* speed: audio frames the guest produces per frame of wall time, burst: wall ms of one production burst cycle. */
static outcome run_scenario(mode which, double speed, unsigned cycle_ms, unsigned burst_ms, unsigned seconds, unsigned gap_from_s,
                            unsigned gap_ms)
{
    outcome result;
    memset(&result, 0, sizeof result);
    present_audio_sink *sink = present_audio_sink_open_ring(RATE, 2u);
    present_audio_sink_enable_clock(sink, 400u, 1500u, 4u); /* the host's values */
    present_audio_sink_set_latency_governor(sink, 150u, 100u, 50u);
    if (which != MODE_OFF) {
        present_audio_sink_set_continuous_playback(sink, 100u, 250u);
        present_audio_sink_set_stretch(sink, which == MODE_STRETCH);
        if (which == MODE_STRETCH) {
            present_audio_sink_set_stretch_hold(sink, 100u, g_hold_ms);
        }
    }
    const size_t total_frames = (size_t)seconds * RATE;
    int16_t *heard = calloc(total_frames + DEVICE_FRAMES, 2u * sizeof *heard);
    size_t heard_frames = 0u;
    double produced_frames = 0.0;
    double phase = 0.0;
    double next_pull_ms = 0.0;
    int16_t chunk[2 * 1200];
    for (unsigned ms = 0u; ms < seconds * 1000u && heard_frames + DEVICE_FRAMES <= total_frames; ms++) {
        g_now_ns = (int64_t)ms * 1000000;
        /* the guest: its audio per wall ms is speed * 48 frames, delivered during the first burst_ms of each cycle in
         * 25 ms (1200 frame) writes, nothing during a gap. */
        const unsigned in_cycle = ms % cycle_ms;
        const bool in_gap = gap_ms != 0u && ms >= gap_from_s * 1000u && ms < gap_from_s * 1000u + gap_ms;
        if (in_cycle < burst_ms && !in_gap) {
            produced_frames += speed * RATE / 1000.0 * (double)cycle_ms / (double)burst_ms;
            while (produced_frames >= 1200.0) {
                for (size_t frame = 0u; frame < 1200u; frame++) {
                    const int16_t sample = (int16_t)(8000.0 * sin(phase));
                    phase += 2.0 * PI * TONE_HZ / RATE;
                    chunk[2u * frame] = sample;
                    chunk[2u * frame + 1u] = sample;
                }
                present_audio_sink_write(sink, chunk, 1200u);
                produced_frames -= 1200.0;
            }
        }
        while ((double)ms >= next_pull_ms) {
            present_audio_sink_pull(sink, heard + 2u * heard_frames, DEVICE_FRAMES);
            heard_frames += DEVICE_FRAMES;
            next_pull_ms += 1000.0 * DEVICE_FRAMES / RATE;
        }
    }
    result.counts = present_audio_sink_counts(sink);
    {
        present_audio_stall stalls[PRESENT_AUDIO_STALL_MAX];
        result.stall_count = present_audio_sink_stalls(sink, stalls, PRESENT_AUDIO_STALL_MAX);
        if (result.stall_count != 0u) {
            result.last_stall = stalls[result.stall_count - 1u];
        }
    }
    present_audio_sink_close(sink);
    bool started = false;
    size_t run = 0u;
    for (size_t frame = 1u; frame < heard_frames; frame++) {
        const int now = heard[2u * frame];
        const int before = heard[2u * (frame - 1u)];
        if (now != 0 && before != 0 && abs(now - before) > result.max_step) {
            result.max_step = abs(now - before);
        }
    }
    {
        size_t zeros = 0u;
        for (size_t frame = (size_t)RATE; frame + 48u < heard_frames; frame++) { /* after the start up */
            if (heard[2u * frame] == 0) {
                zeros++;
                continue;
            }
            if (zeros >= 240u) {
                int peak = 0;
                for (size_t index = 0u; index < 48u; index++) {
                    peak = abs(heard[2u * (frame + index)]) > peak ? abs(heard[2u * (frame + index)]) : peak;
                }
                result.resumes++;
                result.resume_first = abs(heard[2u * frame]) > result.resume_first ? abs(heard[2u * frame]) : result.resume_first;
                result.resume_peak = peak > result.resume_peak ? peak : result.resume_peak;
            }
            zeros = 0u;
        }
    }
    double crossings = 0.0;
    double signal_seconds = 0.0;
    for (size_t first = 0u; first + WINDOW <= heard_frames; first += WINDOW) {
        int peak = 0;
        size_t zero_crossings = 0u;
        for (size_t frame = 0u; frame < WINDOW; frame++) {
            const int value = heard[2u * (first + frame)];
            peak = abs(value) > peak ? abs(value) : peak;
            if (frame != 0u && (value < 0) != (heard[2u * (first + frame - 1u)] < 0)) {
                zero_crossings++;
            }
        }
        const bool silent = peak < 64;
        result.partial += peak >= 200 && peak <= 6500 ? 1u : 0u;
        if (!started) {
            started = !silent;
            if (!started) {
                continue;
            }
        }
        result.windows++;
        if (silent) {
            result.silent++;
            if (run == 0u) {
                result.runs++;
            }
            run++;
            if (run > result.longest) {
                result.longest = run;
            }
        } else {
            run = 0u;
            crossings += (double)zero_crossings;
            signal_seconds += (double)WINDOW / RATE;
        }
    }
    result.silent_ms = result.silent * 10u;
    result.pitch_hz = signal_seconds > 0.0 ? crossings / 2.0 / signal_seconds : 0.0;
    free(heard);
    return result;
}

static void print_outcome(const char *label, const outcome *result)
{
    printf("%-34s windows %zu silent %zu (%.1f%%) runs %zu longest %zu ms pitch %.0f Hz step %d | sink: rebuffers %llu cutouts %llu "
           "slowed %llu ms hold %llu ms\n",
           label, result->windows, result->silent, 100.0 * (double)result->silent / (double)(result->windows ? result->windows : 1u),
           result->runs, result->longest * 10u, result->pitch_hz, result->max_step, (unsigned long long)result->counts.rebuffer_events,
           (unsigned long long)result->counts.cutout_events, (unsigned long long)result->counts.slow_frames * 1000u / RATE,
           (unsigned long long)result->counts.stretch_hold_frames * 1000u / RATE);
}

static void engine_checks(void)
{
    /* A sine through the engine at ratio 1 comes out bit identical (nominal wins), at 0.5 keeps its frequency. */
    audio_stretch *engine = audio_stretch_create(2u, 320u);
    CHECK(engine != NULL);
#define INPUT 48000
    int16_t *in = malloc((size_t)INPUT * 2u * sizeof *in);
    int16_t *out = malloc((size_t)INPUT * 4u * sizeof *out);
    for (size_t frame = 0u; frame < INPUT; frame++) {
        in[2u * frame] = in[2u * frame + 1u] = (int16_t)(8000.0 * sin(2.0 * PI * TONE_HZ * (double)frame / RATE));
    }
    int16_t block[2 * 320];
    CHECK(!audio_stretch_next(engine, in, INPUT, 400u, block, NULL)); /* not primed */
    CHECK(!audio_stretch_prime(engine, in, 100u, block));              /* too short */
    CHECK(audio_stretch_prime(engine, in, INPUT, out));
    size_t produced = 320u;
    double position = 0.0;
    const double ratio = 0.5;
    while (position < 40000.0) {
        position += ratio * 320.0;
        size_t chosen = 0u;
        CHECK(audio_stretch_next(engine, in, INPUT, (size_t)position, out + 2u * produced, &chosen));
        CHECK(chosen + 640u <= INPUT);
        produced += 320u;
    }
    size_t crossings = 0u;
    for (size_t frame = 1u; frame < produced; frame++) {
        crossings += (out[2u * frame] < 0) != (out[2u * (frame - 1u)] < 0);
    }
    const double hz = (double)crossings / 2.0 / ((double)produced / RATE);
    CHECK(hz > TONE_HZ * 0.97 && hz < TONE_HZ * 1.03); /* pitch preserved at half speed */
    CHECK(produced > (size_t)(position / ratio * 0.98)); /* and twice as long */
    int peak = 0;
    for (size_t index = 640u; index < produced * 2u; index++) {
        peak = abs(out[index]) > peak ? abs(out[index]) : peak;
    }
    CHECK(peak < 8400); /* no crossfade overshoot */
    /* min RMS of the stretched blocks: a misaligned crossfade (no waveform search) dips towards zero */
    double min_rms = 1.0e9;
    for (size_t first = 640u; first + 320u <= produced; first += 320u) {
        double sum = 0.0;
        for (size_t frame = 0u; frame < 320u; frame++) {
            sum += (double)out[2u * (first + frame)] * out[2u * (first + frame)];
        }
        const double rms = sqrt(sum / 320.0);
        min_rms = rms < min_rms ? rms : min_rms;
    }
    CHECK(min_rms > 0.9 * 8000.0 / 1.41421356);
    /* ratio 1: every block is the input itself (the nominal start wins, nothing is altered) */
    audio_stretch *straight = audio_stretch_create(2u, 320u);
    CHECK(audio_stretch_prime(straight, in, INPUT, out));
    bool identical = memcmp(out, in, 320u * 2u * sizeof *in) == 0;
    for (size_t block_index = 1u; block_index < 40u; block_index++) {
        CHECK(audio_stretch_next(straight, in, INPUT, block_index * 320u, out, NULL));
        identical = identical && memcmp(out, in + block_index * 320u * 2u, 320u * 2u * sizeof *in) == 0;
    }
    CHECK(identical);
    audio_stretch_destroy(straight);
    /* a two tone, amplitude modulated signal at ratio 0.6: no step beyond the signal's own */
    for (size_t frame = 0u; frame < INPUT; frame++) {
        const double envelope = 0.6 + 0.4 * sin(2.0 * PI * 3.0 * (double)frame / RATE);
        in[2u * frame] = in[2u * frame + 1u] = (int16_t)(envelope * (5000.0 * sin(2.0 * PI * 330.0 * (double)frame / RATE) +
                                                                     3000.0 * sin(2.0 * PI * 1234.0 * (double)frame / RATE)));
    }
    int input_step = 0;
    for (size_t frame = 1u; frame < INPUT; frame++) {
        input_step = abs(in[2u * frame] - in[2u * (frame - 1u)]) > input_step ? abs(in[2u * frame] - in[2u * (frame - 1u)]) : input_step;
    }
    audio_stretch *mixed = audio_stretch_create(2u, 320u);
    CHECK(audio_stretch_prime(mixed, in, INPUT, out));
    size_t mixed_frames = 320u;
    double mixed_position = 0.0;
    while (mixed_position < 40000.0) {
        mixed_position += 0.6 * 320.0;
        CHECK(audio_stretch_next(mixed, in, INPUT, (size_t)mixed_position, out + 2u * mixed_frames, NULL));
        mixed_frames += 320u;
    }
    int output_step = 0;
    for (size_t frame = 1u; frame < mixed_frames; frame++) {
        output_step = abs(out[2u * frame] - out[2u * (frame - 1u)]) > output_step ? abs(out[2u * frame] - out[2u * (frame - 1u)]) : output_step;
    }
    printf("engine two tone: input max step %d, stretched max step %d\n", input_step, output_step);
    CHECK(output_step < input_step * 3 / 2);
    audio_stretch_destroy(mixed);
    CHECK(audio_stretch_hold(engine, block));
    CHECK(audio_stretch_fade_out(engine, block));
    CHECK(!audio_stretch_primed(engine));
    CHECK(!audio_stretch_hold(engine, block));
    audio_stretch_destroy(engine);
    free(in);
    free(out);
}


/* T1250 second half: a guest that stops producing for gap_ms (a Story level or stream load, the user's log: 280 to 360 ms
 * of guest silence ahead of 810 to 880 ms of sink silence). Silence heard = the longest silent run. */
static outcome gap_run(double speed, unsigned gap_ms, unsigned hold_ms)
{
    g_hold_ms = hold_ms;
    const outcome result = run_scenario(MODE_STRETCH, speed, 1000u, 1000u, 20u, 8u, gap_ms);
    g_hold_ms = 100u;
    return result;
}

static void gap_sweep(void)
{
    static const double speeds[] = {0.4, 1.0};
    static const unsigned gaps[] = {350u, 600u, 880u, 1500u};
    for (size_t speed_index = 0u; speed_index < 2u; speed_index++) {
        for (size_t gap_index = 0u; gap_index < 4u; gap_index++) {
            const outcome before = gap_run(speeds[speed_index], gaps[gap_index], 100u);
            const outcome after = gap_run(speeds[speed_index], gaps[gap_index], 400u);
            char label[64];
            snprintf(label, sizeof label, "gap %4u ms at %.1fx hold 100", gaps[gap_index], speeds[speed_index]);
            print_outcome(label, &before);
            snprintf(label, sizeof label, "gap %4u ms at %.1fx hold 400", gaps[gap_index], speeds[speed_index]);
            print_outcome(label, &after);
            /* the silence is never longer than the gap itself plus the sink's own 30 ms, and the longer hold never cuts more */
            CHECK(after.longest * 10u <= gaps[gap_index] + 30u);
            CHECK(after.silent_ms <= before.silent_ms);
            CHECK(after.max_step < 900);
            CHECK(after.counts.resume_wait_max_ms <= 30u); /* resumes within one device pull of the guest's first write */
            if (after.counts.cutout_events != 0u) {
                CHECK(after.resumes == 1u && after.resume_peak < 2000); /* the resume fades in, a tone is 8000 */
                CHECK(after.counts.resume_events == 1u && after.last_stall.first_write_ms > 0u);
                CHECK(after.last_stall.resume_wait_ms <= 30u);
            }
            CHECK(after.counts.stretch_decay_frames > 0u || gaps[gap_index] < 500u);
            if (gaps[gap_index] >= 880u) {
                CHECK(after.partial >= 15u);  /* the hold decays over about 30 windows (tone 8000 falls to 0) */
                CHECK(before.partial <= 6u);  /* control: the 100 ms plain loop does not */
            }
            CHECK(before.counts.stretch_decay_frames == 0u);
            if (gaps[gap_index] <= 350u) {
                CHECK(after.silent_ms == 0u && after.counts.cutout_events == 0u); /* bridged: no digital silence at all */
            }
        }
    }
}

int main(void)
{
    present_audio_sink_set_test_clock(test_clock);
    engine_checks();

    /* The user's pattern (log stalls #4 to #10): the guest writes steadily but at 0.364x, 350 ms of audio per 960 ms of
     * wall, and every dry ring costs a refill pause of about 600 ms. */
    const outcome off = run_scenario(MODE_OFF, 0.364, 960u, 960u, 60u, 0u, 0u);
    const outcome resample = run_scenario(MODE_RESAMPLE, 0.364, 960u, 960u, 60u, 0u, 0u);
    const outcome stretch = run_scenario(MODE_STRETCH, 0.364, 960u, 960u, 60u, 0u, 0u);
    print_outcome("A off (before the fix) 0.364x", &off);
    print_outcome("B resampler 0.364x", &resample);
    print_outcome("C stretch 0.364x", &stretch);
    CHECK(off.runs >= 20u && off.silent * 100u > off.windows * 30u);
    CHECK(off.longest >= 40u);
    CHECK(off.counts.cutout_events >= 20u && off.counts.cutout_frames * 100u > (uint64_t)off.windows * WINDOW * 30u);
    CHECK(resample.silent * 100u < resample.windows * 3u);
    CHECK(resample.pitch_hz < TONE_HZ * 0.8); /* the pitch follows the guest */
    CHECK(stretch.windows > 2000u);
    CHECK(stretch.silent * 100u < stretch.windows * 2u && stretch.longest <= 8u);
    CHECK(stretch.counts.cutout_events <= 3u);
    CHECK(stretch.counts.stretch_enabled == 1u && stretch.counts.stretch_blocks > 1000u);
    CHECK(stretch.pitch_hz > TONE_HZ * 0.9 && stretch.pitch_hz < TONE_HZ * 1.1); /* the pitch stays natural */
    CHECK(stretch.counts.slow_frames > (uint64_t)RATE * 10u);
    CHECK(stretch.counts.prefill_peak_frames <= 4800u);
    CHECK(stretch.counts.prefill_frames != 0u && stretch.counts.prefill_frames <= 4800u); /* the first stall showed 19200 */

    /* The same average speed delivered in 600 ms bursts with 360 ms gaps: harder, the ring must bridge the gaps. */
    const outcome burst_off = run_scenario(MODE_OFF, 0.364, 960u, 600u, 60u, 0u, 0u);
    const outcome burst = run_scenario(MODE_STRETCH, 0.364, 960u, 600u, 60u, 0u, 0u);
    print_outcome("A off bursty 0.364x", &burst_off);
    print_outcome("C stretch bursty 0.364x", &burst);
    CHECK(burst.silent * 2u < burst_off.silent);
    CHECK(burst.longest < burst_off.longest);

    /* A guest at real time is never touched: no slowed playback, no cutout. */
    const outcome fast = run_scenario(MODE_STRETCH, 1.0, 1000u, 1000u, 20u, 0u, 0u);
    print_outcome("C stretch 1.0x steady", &fast);
    CHECK(fast.silent == 0u && fast.counts.stretch_hold_frames == 0u);
    CHECK(fast.counts.slow_frames * 1000u / RATE < 200u); /* the start up trim only, a guest at real time is not slowed */
    CHECK(fast.pitch_hz > TONE_HZ * 0.97 && fast.pitch_hz < TONE_HZ * 1.03);

    /* A 1 s production gap (a Story load): the loop and fade out bridge about 100 ms, then the rebuffer pause, and the
     * pause ends at the low water, not after a deep refill. */
    const outcome gap = run_scenario(MODE_STRETCH, 1.0, 1000u, 1000u, 20u, 8u, 1000u);
    print_outcome("C stretch 1.0x with a 1 s gap", &gap);
    CHECK(gap.runs <= 2u && gap.longest <= 90u); /* the gap itself is 100 windows, the ring and the hold bridge part of it */
    CHECK(gap.counts.stretch_hold_frames > 0u && gap.counts.stretch_fadeouts >= 1u);
    CHECK(gap.max_step < 700);   /* the fade out leaves no click */
    CHECK(stretch.max_step < 700 && fast.max_step < 700);
    CHECK(gap.counts.prefill_peak_frames != 0u && gap.counts.prefill_peak_frames <= 4800u); /* the refill target stays at the low water */
    const outcome gap_off = run_scenario(MODE_OFF, 1.0, 1000u, 1000u, 20u, 8u, 1000u);
    print_outcome("A off 1.0x with a 1 s gap", &gap_off);
    CHECK(gap_off.counts.prefill_peak_frames > 4800u); /* control: without the cap the adapted target grows on a burst */

    /* A guest slower than the floor (0.15x): the stretch hold keeps most of it playing instead of one long pause. */
    const outcome crawl_off = run_scenario(MODE_OFF, 0.15, 960u, 960u, 40u, 0u, 0u);
    const outcome crawl = run_scenario(MODE_STRETCH, 0.15, 960u, 960u, 40u, 0u, 0u);
    print_outcome("A off 0.15x", &crawl_off);
    print_outcome("C stretch 0.15x", &crawl);
    CHECK(crawl.silent < crawl_off.silent);

    gap_sweep();

    /* The resume after a rebuffer fades in: a first sample near zero at every one of five gap lengths (five tone phases). */
    for (unsigned gap_ms = 1200u; gap_ms < 1350u; gap_ms += 30u) {
        const outcome fade = gap_run(1.0, gap_ms, 400u);
        CHECK(fade.resumes == 1u && fade.resume_first < 200 && fade.max_step < 900);
    }

    present_audio_sink_set_test_clock(NULL);
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
