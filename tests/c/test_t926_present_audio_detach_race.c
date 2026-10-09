/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T926: deterministically overlap a presenter media-time query with audio detach.
 * The T926-only library hooks hold the real query open while another thread proves
 * the playback setter sees the busy video mutex. No sleeps or real audio device.
 */
#define _POSIX_C_SOURCE 200809L
#include "present_sink.h"

#include <pthread.h>
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

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    bool armed;
    bool release_query;
    bool query_entered;
    bool query_active;
    bool query_left;
    bool playback_lock_attempted;
    bool playback_lock_busy;
    bool playback_lock_acquired;
    bool query_active_when_lock_acquired;
    bool detached;
    unsigned media_query_calls;
    unsigned sequence;
    unsigned query_enter_sequence;
    unsigned lock_attempt_sequence;
    unsigned query_leave_sequence;
    unsigned lock_acquired_sequence;
} race_state;

static void record_event(race_state *state, unsigned *at)
{
    *at = ++state->sequence;
    pthread_cond_broadcast(&state->changed);
}

static void race_hook(present_sink_t926_event event, bool lock_busy, void *context)
{
    race_state *state = context;
    pthread_mutex_lock(&state->lock);
    if (!state->armed) {
        pthread_mutex_unlock(&state->lock);
        return;
    }
    switch (event) {
    case PRESENT_SINK_T926_AUDIO_QUERY_ENTER:
        state->media_query_calls++;
        state->query_entered = true;
        state->query_active = true;
        record_event(state, &state->query_enter_sequence);
        while (!state->release_query) {
            pthread_cond_wait(&state->changed, &state->lock);
        }
        break;
    case PRESENT_SINK_T926_AUDIO_QUERY_LEAVE:
        state->query_active = false;
        state->query_left = true;
        record_event(state, &state->query_leave_sequence);
        break;
    case PRESENT_SINK_T926_PLAYBACK_LOCK_ATTEMPT:
        state->playback_lock_attempted = true;
        state->playback_lock_busy = lock_busy;
        record_event(state, &state->lock_attempt_sequence);
        break;
    case PRESENT_SINK_T926_PLAYBACK_LOCK_ACQUIRED:
        state->playback_lock_acquired = true;
        state->query_active_when_lock_acquired = state->query_active;
        record_event(state, &state->lock_acquired_sequence);
        break;
    }
    pthread_mutex_unlock(&state->lock);
}

typedef struct {
    present_video_sink *video;
    race_state *state;
    bool run_ok;
    bool media_time_known;
    uint64_t media_time;
} query_request;

static void query_from_presenter(void *context)
{
    query_request *request = context;
    request->media_time_known = present_video_sink_job_media_time(request->video, &request->media_time);
}

static void *run_presenter_query(void *context)
{
    query_request *request = context;
    request->run_ok = present_video_sink_run(request->video, query_from_presenter, request);
    return NULL;
}

static void *detach_playback(void *context)
{
    query_request *request = context;
    present_video_sink_set_playback(request->video, NULL);
    pthread_mutex_lock(&request->state->lock);
    request->state->detached = true;
    pthread_cond_broadcast(&request->state->changed);
    pthread_mutex_unlock(&request->state->lock);
    return NULL;
}

static void wait_for_bool(race_state *state, const bool *value)
{
    pthread_mutex_lock(&state->lock);
    while (!*value) {
        pthread_cond_wait(&state->changed, &state->lock);
    }
    pthread_mutex_unlock(&state->lock);
}

int main(void)
{
    race_state state = {0};
    CHECK(pthread_mutex_init(&state.lock, NULL) == 0);
    CHECK(pthread_cond_init(&state.changed, NULL) == 0);
    present_sink_t926_set_hook(race_hook, &state);
    if (!present_window_available()) {
        puts("SKIPPED T926: this build has no SDL3 window sink");
        present_sink_t926_set_hook(NULL, NULL);
        pthread_cond_destroy(&state.changed);
        pthread_mutex_destroy(&state.lock);
        return 77;
    }

    const char *error = NULL;
    present_video_sink *video = present_video_sink_open(PRESENT_VIDEO_WINDOW, "T926 detach race", &error);
    if (video == NULL) {
        printf("SKIPPED T926: SDL window unavailable: %s\n", error != NULL ? error : "unknown reason");
        present_sink_t926_set_hook(NULL, NULL);
        pthread_cond_destroy(&state.changed);
        pthread_mutex_destroy(&state.lock);
        return 77;
    }
    present_audio_sink *audio = present_audio_sink_open(PRESENT_AUDIO_NULL, NULL, 48000u, 2u);
    CHECK(audio != NULL);
    CHECK(audio != NULL && present_video_sink_playback_enabled(video) == false);
    present_video_sink_set_playback(video, audio);
    CHECK(present_video_sink_playback_enabled(video));

    pthread_mutex_lock(&state.lock);
    state.armed = true;
    pthread_mutex_unlock(&state.lock);

    query_request query = {.video = video, .state = &state};
    pthread_t query_thread;
    CHECK(pthread_create(&query_thread, NULL, run_presenter_query, &query) == 0);
    wait_for_bool(&state, &state.query_entered);

    pthread_t detach_thread;
    CHECK(pthread_create(&detach_thread, NULL, detach_playback, &query) == 0);
    wait_for_bool(&state, &state.playback_lock_attempted);

    pthread_mutex_lock(&state.lock);
    CHECK(state.query_active);
    CHECK(state.playback_lock_busy);
    CHECK(state.query_entered && !state.release_query);
    state.release_query = true;
    pthread_cond_broadcast(&state.changed);
    pthread_mutex_unlock(&state.lock);

    CHECK(pthread_join(query_thread, NULL) == 0);
    CHECK(pthread_join(detach_thread, NULL) == 0);
    CHECK(query.run_ok);
    CHECK(state.query_left && state.playback_lock_acquired && state.detached);
    CHECK(!state.query_active_when_lock_acquired);
    CHECK(state.query_enter_sequence < state.lock_attempt_sequence);
    CHECK(state.lock_attempt_sequence < state.query_leave_sequence);
    CHECK(state.query_leave_sequence < state.lock_acquired_sequence);
    CHECK(!present_video_sink_playback_enabled(video));

    /* Audio destruction occurs only after the blocked query has returned and playback
     * is detached. A subsequent presenter-side query must short-circuit on playback=off. */
    CHECK(present_audio_sink_close(audio));
    query_request after_close = {.video = video};
    CHECK(present_video_sink_run(video, query_from_presenter, &after_close));
    CHECK(!after_close.media_time_known);
    pthread_mutex_lock(&state.lock);
    CHECK(state.media_query_calls == 1u);
    pthread_mutex_unlock(&state.lock);

    present_video_sink_close(video);
    present_sink_t926_set_hook(NULL, NULL);
    pthread_cond_destroy(&state.changed);
    pthread_mutex_destroy(&state.lock);
    printf("T926 presenter/audio detach race: %d checks, %d failures; query blocked, setter observed EBUSY, detach followed query exit, post-close query skipped audio\n",
           checks, failures);
    return failures == 0 ? 0 : 1;
}
