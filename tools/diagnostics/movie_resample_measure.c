/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * T818 measurement: how clean is the mixer's 44100 -> 48000 Hz path for the movie soundtrack?
 * A sine of --frequency Hz is cut into back to back 44100 Hz stereo packets (the XMV packet is 4410
 * frames), submitted like dsound_movie_stream_process does, rendered at 48000 Hz and written through
 * the repo's wav-file sink (--wav PATH). The same samples are analysed in memory:
 *   fundamental dB    level of the tone against the ideal input amplitude
 *   image dB          level of each folded image (k*44100 +- f, folded into 0..24000 Hz) against the tone
 *   residual dB       everything left after removing the fitted tone (images, aliasing, clicks) vs the tone
 *   join / body peak  largest residual sample within 2 output samples of a packet join vs elsewhere
 * Build (from a configured build dir that has libtsfp_xbox.a and libtsfp_host_options.a):
 *   clang -O1 -Isrc/audio -Isrc/host tools/diagnostics/movie_resample_measure.c \
 *     build/libtsfp_xbox.a build/libtsfp_host_options.a -lpthread -lm -o build/movie_resample_measure
 */
#include "dsound_mixer.h"
#include "present_sink.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICKS UINT64_C(733333333)
#define SOURCE_RATE 44100u
#define OUTPUT_RATE 48000u

static double level_at(const double *x, size_t count, double hz)
{
    double re = 0.0, im = 0.0, window_sum = 0.0;
    for (size_t i = 0; i < count; i++) {
        const double window = 0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)(count - 1));
        const double angle = 2.0 * M_PI * hz * (double)i / OUTPUT_RATE;
        re += x[i] * window * cos(angle);
        im -= x[i] * window * sin(angle);
        window_sum += window;
    }
    return 2.0 * sqrt(re * re + im * im) / window_sum;
}

static double fold(double hz)
{
    hz = fmod(hz, OUTPUT_RATE);
    return hz > OUTPUT_RATE / 2.0 ? OUTPUT_RATE - hz : hz;
}

int main(int argc, char **argv)
{
    double frequency = 1000.0;
    unsigned packets = 40, packet_frames = 4410;
    const char *wav = NULL;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--frequency")) frequency = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--packets")) packets = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--packet-frames")) packet_frames = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--wav")) wav = argv[i + 1];
        else { fprintf(stderr, "usage: --frequency HZ --packets N --packet-frames N --wav PATH\n"); return 2; }
    }
    const double amplitude = 16000.0;
    dsound_mixer *mixer = dsound_mixer_create(OUTPUT_RATE, TICKS);
    int16_t *pcm = malloc((size_t)packet_frames * 4u);
    if (mixer == NULL || pcm == NULL) return 1;
    for (unsigned p = 0; p < packets; p++) {
        for (unsigned i = 0; i < packet_frames; i++) {
            const double t = (double)((uint64_t)p * packet_frames + i) / SOURCE_RATE;
            const int16_t v = (int16_t)lrint(amplitude * sin(2.0 * M_PI * frequency * t));
            pcm[2u * i] = v;
            pcm[2u * i + 1u] = v;
        }
        if (!dsound_mixer_submit_pcm16_stereo(mixer, 1u, 1000u, SOURCE_RATE, pcm, packet_frames)) return 1;
    }
    const double seconds = (double)packets * packet_frames / SOURCE_RATE;
    const size_t capacity = (size_t)(seconds * OUTPUT_RATE) + 4096u;
    int16_t *out = calloc(capacity, 4u);
    size_t written = 0;
    if (out == NULL || !dsound_mixer_render_until(mixer, 1000u + (uint64_t)(seconds * TICKS), out, capacity, &written))
        return 1;
    if (wav != NULL) {
        present_audio_sink *sink = present_audio_sink_open(PRESENT_AUDIO_WAV_FILE, wav, OUTPUT_RATE, 2u);
        if (sink == NULL || !present_audio_sink_write(sink, out, written)) return 1;
        present_audio_sink_close(sink);
    }
    /* Skip the first and last 50 ms (start and end of the whole signal are not joins). */
    const size_t first = OUTPUT_RATE / 20u, last = written - OUTPUT_RATE / 20u, count = last - first;
    double *x = malloc(count * sizeof(double));
    double *e = malloc(count * sizeof(double));
    for (size_t i = 0; i < count; i++) x[i] = out[(first + i) * 2u];
    /* Least squares sine and cosine at the tone, then residual. */
    double ss = 0, cc = 0, sc = 0, xs = 0, xc = 0, tone = 0;
    for (size_t i = 0; i < count; i++) {
        const double a = 2.0 * M_PI * frequency * (double)(first + i) / OUTPUT_RATE;
        ss += sin(a) * sin(a); cc += cos(a) * cos(a); sc += sin(a) * cos(a);
        xs += x[i] * sin(a); xc += x[i] * cos(a);
    }
    const double det = ss * cc - sc * sc, bs = (xs * cc - xc * sc) / det, bc = (xc * ss - xs * sc) / det;
    double residual = 0;
    for (size_t i = 0; i < count; i++) {
        const double a = 2.0 * M_PI * frequency * (double)(first + i) / OUTPUT_RATE;
        e[i] = x[i] - bs * sin(a) - bc * cos(a);
        residual += e[i] * e[i];
    }
    tone = hypot(bs, bc);
    printf("tone %.0f Hz packets %u x %u frames, %zu output frames\n", frequency, packets, packet_frames, written);
    printf("fundamental %.2f dB re input (linear interpolation droop)\n", 20.0 * log10(tone / amplitude));
    printf("residual (images, aliasing, clicks) %.2f dB re tone\n", 10.0 * log10(residual / (double)count / (tone * tone / 2.0)));
    for (int k = 1; k <= 3; k++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            const double hz = fold((double)k * SOURCE_RATE + sign * frequency);
            if (fabs(hz - frequency) < 1.0) continue;
            printf("image %2d * 44100 %c f -> %8.1f Hz: %.2f dB re tone\n", k, sign < 0 ? '-' : '+', hz, 20.0 * log10(level_at(x, count, hz) / tone + 1e-12));
        }
    }
    double join_peak = 0, body_peak = 0;
    for (size_t i = 0; i < count; i++) {
        const double output_time = (double)(first + i) / OUTPUT_RATE;
        const double join_position = output_time * SOURCE_RATE / packet_frames;
        const double nearest = floor(join_position + 0.5);
        const double distance = fabs(join_position - nearest) * packet_frames / SOURCE_RATE * OUTPUT_RATE;
        const double a = fabs(e[i]);
        if (nearest >= 1.0 && distance < 2.0) { if (a > join_peak) join_peak = a; }
        else if (a > body_peak) body_peak = a;
    }
    printf("residual peak at joins %.1f, elsewhere %.1f (full scale 32768), ratio %.2f\n", join_peak, body_peak, join_peak / (body_peak + 1e-9));
    dsound_mixer_destroy(mixer);
    return 0;
}
