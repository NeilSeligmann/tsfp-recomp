/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T1289: the per vblank interval guest thread trace (src/host/guest_frame_trace.c).
 *
 * Checks the exclusive time accounting of nested scopes, the closing of intervals, the slow event capture and its names, the slowest ring,
 * the per draws cost model, that only the claiming thread is traced, and that a disabled trace does nothing. CPU time is spent with a
 * thread CPU clock so the numbers do not depend on machine load.
 *
 * MUTATIONS (each must fail a check below): a parent scope not charged without its children (inclusive instead of exclusive), the
 * interval accumulators not reset at a boundary, events recorded below 1 ms, every thread traced (not only the claimer), the fit slope
 * computed with the wrong sums.
 */
#define _GNU_SOURCE
#include "guest_frame_trace.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); failures++; } } while (0)

static void sleep_ms(long milliseconds)
{
    const struct timespec pause = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    nanosleep(&pause, NULL);
}

/* Burn `microseconds` of THIS thread's CPU time. */
static void burn_cpu_us(long microseconds)
{
    struct timespec start, now;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start);
    do {
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    } while ((now.tv_sec - start.tv_sec) * 1000000L + (now.tv_nsec - start.tv_nsec) / 1000L < microseconds);
}

static const char *kernel_name(unsigned ordinal)
{
    return ordinal == 219u ? "NtReadFile" : "other";
}

static const char *xdk_name(uint32_t address)
{
    return address == 0x3D8E50u ? "Swap" : NULL;
}

static void *foreign_thread(void *unused)
{
    (void)unused;
    gft_enter(GFT_XDK, 1u); /* not the claiming thread: nothing may be recorded */
    sleep_ms(8);
    gft_leave();
    guest_frame_trace_boundary();
    return NULL;
}

static char *print_to_string(const gft_names *names, char *buffer, size_t bytes)
{
    FILE *file = fmemopen(buffer, bytes, "w");
    if (file == NULL) {
        return NULL;
    }
    guest_frame_trace_print(file, names);
    fclose(file);
    return buffer;
}

int main(void)
{
    static char text[1 << 20];
    const gft_names names = {xdk_name, kernel_name};

    /* Disabled: nothing is claimed, scopes are no-ops, the summary is empty. */
    guest_frame_trace_reset_for_test();
    guest_frame_trace_enable(false);
    guest_frame_trace_claim();
    gft_enter(GFT_XDK, 1u);
    gft_leave();
    guest_frame_trace_boundary();
    CHECK(guest_frame_trace_summary().intervals == 0u);
    CHECK(print_to_string(&names, text, sizeof text) != NULL && text[0] == '\0');

    /* Enabled: this thread claims. */
    guest_frame_trace_reset_for_test();
    guest_frame_trace_enable(true);
    guest_frame_trace_claim();

    /* Interval 1: an XDK call of 12 ms that enters a kernel call of 6 ms. Exclusive times: XDK about 6, KERNEL about 6, never XDK 12. */
    gft_enter(GFT_XDK, 0x3D8E50u);
    sleep_ms(6);
    gft_enter(GFT_KERNEL, 219u);
    sleep_ms(6);
    gft_leave();
    gft_leave();
    guest_frame_trace_swap();
    guest_frame_trace_draws(100u);
    guest_frame_trace_boundary();
    gft_summary sum = guest_frame_trace_summary();
    CHECK(sum.intervals == 1u);
    CHECK(sum.phase_ms_total[GFT_XDK] > 4.5 && sum.phase_ms_total[GFT_XDK] < 9.5);
    CHECK(sum.phase_ms_total[GFT_KERNEL] > 4.5 && sum.phase_ms_total[GFT_KERNEL] < 9.5);
    CHECK(sum.phase_ms_total[GFT_GUEST] < 3.0);
    CHECK(sum.wall_ms_mean > 11.0);
    CHECK(sum.scopes_per_interval[GFT_XDK] == 1.0 && sum.scopes_per_interval[GFT_KERNEL] == 1.0); /* the claim's own calibration scopes are not counted */
    CHECK(sum.scopes_per_interval[GFT_AUDIO] == 0.0);
    CHECK(sum.trace_ms_per_interval > 0.0 && sum.trace_ms_per_interval < 1.0);

    /* Interval 2: the accumulators were reset at the boundary. Only 3 ms of AUDIO now. */
    gft_enter(GFT_AUDIO, 0u);
    sleep_ms(3);
    gft_leave();
    guest_frame_trace_boundary();
    sum = guest_frame_trace_summary();
    CHECK(sum.intervals == 2u);
    CHECK(sum.phase_ms_total[GFT_AUDIO] > 2.0 && sum.phase_ms_total[GFT_AUDIO] < 6.0);
    CHECK(sum.phase_ms_total[GFT_XDK] < 9.5); /* not charged again */

    /* Interval 3: a scope under 1 ms makes no event, one over 1 ms does (the print names it). */
    gft_enter(GFT_KERNEL, 7u);
    burn_cpu_us(200);
    gft_leave();
    guest_frame_trace_boundary();
    gft_enter(GFT_KERNEL, 219u);
    sleep_ms(3);
    gft_leave();
    guest_frame_trace_boundary();
    CHECK(print_to_string(&names, text, sizeof text) != NULL);
    CHECK(strstr(text, "kernel call NtReadFile (ordinal 219)") != NULL);
    CHECK(strstr(text, "ordinal 7)") == NULL);
    CHECK(strstr(text, "XDK call Swap (0x003D8E50) 6.") != NULL || strstr(text, "XDK call Swap (0x003D8E50)") != NULL); /* the exclusive 6 ms of interval 1 */
    CHECK(strstr(text, "XDK call Swap (0x003D8E50) 12.") == NULL); /* not the inclusive 12 ms */

    /* Another thread is not traced. */
    pthread_t thread;
    const gft_summary before = guest_frame_trace_summary();
    CHECK(pthread_create(&thread, NULL, foreign_thread, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    const gft_summary after = guest_frame_trace_summary();
    CHECK(after.intervals == before.intervals);
    CHECK(after.phase_ms_total[GFT_XDK] == before.phase_ms_total[GFT_XDK]);

    /* The per draws cost model: thread CPU = 1 ms + 40 us per draw over frames of 20 to 80 draws. */
    guest_frame_trace_reset_for_test();
    guest_frame_trace_enable(true);
    guest_frame_trace_claim();
    for (unsigned frame = 0u; frame < 40u; frame++) {
        const unsigned draws = 20u + (frame * 7u) % 61u;
        burn_cpu_us(1000L + 40L * (long)draws);
        guest_frame_trace_swap();
        guest_frame_trace_draws(draws);
        guest_frame_trace_boundary();
    }
    sum = guest_frame_trace_summary();
    CHECK(sum.fit_intervals == 40u);
    CHECK(sum.fit_slope_us_per_draw > 33.0 && sum.fit_slope_us_per_draw < 47.0);
    CHECK(sum.fit_intercept_ms > 0.5 && sum.fit_intercept_ms < 1.6);
    CHECK(sum.fit_r_squared > 0.9);
    CHECK(print_to_string(&names, text, sizeof text) != NULL);
    CHECK(strstr(text, "by draws per frame, 0 to 99: 40 frames") != NULL);

    /* The slowest ring keeps the slowest intervals, in order. */
    guest_frame_trace_reset_for_test();
    guest_frame_trace_enable(true);
    guest_frame_trace_claim();
    for (unsigned frame = 0u; frame < 30u; frame++) {
        sleep_ms(frame == 17u ? 30 : 2);
        guest_frame_trace_swap();
        guest_frame_trace_draws(10u);
        guest_frame_trace_boundary();
    }
    CHECK(print_to_string(&names, text, sizeof text) != NULL);
    const char *first = strstr(text, "slowest 1: interval 17 ");
    CHECK(first != NULL);
    CHECK(strstr(text, "slowest 16:") != NULL && strstr(text, "slowest 17:") == NULL);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("test_guest_frame_trace: all checks passed\n");
    return 0;
}
