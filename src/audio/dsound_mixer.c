/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dsound_mixer.h"

#include "dsound_adpcm.h"
#include "dsound_stream_frequency.h"

#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define MIXER_MAX_STREAMS 64u
#define MIXER_MAX_PACKETS 256u
#define MIXER_MAX_PACKET_BYTES (4u * 1024u * 1024u)
#define MIXER_MAX_QUEUED_PCM_BYTES (16u * 1024u * 1024u)
#define MIXER_MAX_PAUSE_SEGMENTS 64u
#define MIXER_MAX_BUFFER_RUNS 128u
#define MIXER_MAX_BUFFER_EVENTS 64u
#define MIXER_MAX_ROUTE_EVENTS 512u

typedef struct mixer_route_event {
    uint64_t ticks;
    uint32_t gain[4];
    uint32_t bin_gain[6][4];
} mixer_route_event;

typedef struct mixbin_headroom_event {
    uint64_t ticks;
    uint8_t amount[32];
} mixbin_headroom_event;

typedef struct buffer_event {
    uint64_t ticks, position_q32;
    uint32_t frequency, gain_q16;
    size_t loop_start, loop_end;
    bool paused, looping, looped;
} buffer_event;

typedef struct buffer_run {
    bool used;
    uint32_t key;
    uint64_t serial, end_ticks;
    int16_t *pcm;
    size_t frames, pcm_bytes, event_count;
    buffer_event events[MIXER_MAX_BUFFER_EVENTS];
} buffer_run;

typedef struct mixer_pause_segment {
    uint64_t start_ticks;
    uint64_t end_ticks;
} mixer_pause_segment;

typedef struct mixer_packet {
    bool used;
    uint32_t stream_key;
    uint32_t source_rate_hz;
    uint64_t start_ticks;
    uint64_t end_ticks;
    uint32_t start_fraction, end_fraction;
    size_t frames;
    size_t pcm_bytes;
    int16_t *pcm;
} mixer_packet;

static unsigned __int128 source_q32(dsound_frequency_phase phase)
{
    return ((unsigned __int128)phase.ticks << 32u) | phase.fraction;
}

static unsigned __int128 packet_start_q32(const mixer_packet *packet)
{
    return source_q32((dsound_frequency_phase){packet->start_ticks, packet->start_fraction});
}

static unsigned __int128 packet_end_q32(const mixer_packet *packet)
{
    return source_q32((dsound_frequency_phase){packet->end_ticks, packet->end_fraction});
}

typedef struct mixer_stream {
    bool used;
    uint32_t key;
    bool paused;
    bool attenuated;      /* a SetVolume below 0 is in force, gain_q16 applies */
    uint32_t gain_q16;    /* linear gain, 65536 = 1.0 */
    uint32_t source_rate_hz;
    dsound_frequency_clock frequency_clock;
    uint64_t frequency_last_wall;
    uint64_t pause_started;
    uint64_t tail_active_ticks;
    uint32_t tail_fraction;
    size_t pause_count;
    mixer_pause_segment pauses[MIXER_MAX_PAUSE_SEGMENTS];
    uint64_t route_serial;
    size_t route_count;
    mixer_route_event routes[MIXER_MAX_ROUTE_EVENTS];
} mixer_stream;

struct dsound_mixer {
    uint32_t output_rate_hz;
    uint64_t ticks_per_second;
    bool started;
    uint64_t origin_ticks;
    uint64_t rendered_frames;
    size_t queued_pcm_bytes;
    bool mixbin_headroom_enabled;
    uint64_t mixbin_headroom_identity;
    uint64_t mixbin_headroom_last_ticks;
    size_t mixbin_headroom_count;
    mixbin_headroom_event mixbin_headroom[512];
    mixer_stream streams[MIXER_MAX_STREAMS];
    mixer_packet packets[MIXER_MAX_PACKETS];
    buffer_run buffers[MIXER_MAX_BUFFER_RUNS];
    dsound_mixer_counts counts;
    const char *last_buffer_refusal; /* T1216: why the last buffer command was refused, for the guest-stop text */
};

static uint64_t mul_div_floor(uint64_t value, uint64_t multiplier, uint64_t divisor)
{
    return (uint64_t)(((unsigned __int128)value * multiplier) / divisor);
}

static uint64_t mul_div_ceil(uint64_t value, uint64_t multiplier, uint64_t divisor)
{
    const unsigned __int128 numerator = (unsigned __int128)value * multiplier;
    return (uint64_t)((numerator + divisor - 1u) / divisor);
}

/* DirectSound volume is hundredths of a decibel of AMPLITUDE: 0 is full, DSBVOLUME_MIN -10000 is
 * silence. The title builds it as round(2000 * log10(gain)) with a floor at -10000 for gain below
 * 1e-5 (sub_00028180, measured from the retail bytes, docs/presentation.md), so the inverse is
 * gain = 10^(mb / 2000), written once here and kept in Q16. Computed with a range reduced Taylor
 * series, no libm: exp(x) = exp(x / 256)^256. The floor is mute, not 10^-5. */
bool dsound_mixer_volume_to_gain_q16(int32_t millibels, uint32_t *gain_q16)
{
    if (gain_q16 == NULL || millibels > 0 || millibels < DSOUND_MIXER_VOLUME_MIN) return false;
    if (millibels == 0) { *gain_q16 = 65536u; return true; }
    if (millibels == DSOUND_MIXER_VOLUME_MIN) { *gain_q16 = 0u; return true; }
    const double x = (double)millibels * 2.302585092994046 / 2000.0 / 256.0;
    double term = 1.0, sum = 1.0;
    for (int i = 1; i < 14; i++) { term *= x / i; sum += term; }
    for (int i = 0; i < 8; i++) sum *= sum;
    *gain_q16 = (uint32_t)(sum * 65536.0 + 0.5);
    return true;
}

static mixer_stream *stream_for(dsound_mixer *mixer, uint32_t key);
static mixer_stream *stream_find(dsound_mixer *mixer, uint32_t key);
bool dsound_mixer_set_stream_volume(dsound_mixer *mixer, uint32_t stream_key, int32_t millibels)
{
    uint32_t gain = 0u;
    if (mixer == NULL || stream_key == 0u || !dsound_mixer_volume_to_gain_q16(millibels, &gain)) return false;
    mixer_stream *stream = stream_for(mixer, stream_key);
    if (stream == NULL) return false;
    stream->attenuated = millibels != 0;
    stream->gain_q16 = gain;
    return true;
}

static uint64_t stream_active_ticks(const mixer_stream *stream, uint64_t wall_ticks,
                                    bool *paused);
/* Packet boundaries live in source time; wall-time frequency events preserve
 * their contiguous decoded samples rather than retiming already-rendered PCM. */
static bool stream_source_phase(const mixer_stream *stream,uint64_t active,
                                dsound_frequency_phase *phase)
{
    if(stream->frequency_clock.count==0u) {
        *phase=(dsound_frequency_phase){active,0u};return true;
    }
    return dsound_stream_frequency_clock_phase(&stream->frequency_clock,active,phase);
}
bool dsound_mixer_set_stream_frequency(dsound_mixer *mixer, uint32_t stream_key,
                                       uint64_t ticks, uint32_t source_rate_hz)
{
    if (mixer == NULL || stream_key == 0u || source_rate_hz < 8000u || source_rate_hz > 96000u)
        return false;
    mixer_stream *stream = stream_find(mixer, stream_key);
    if(stream!=NULL && ticks<stream->frequency_last_wall)return false;
    uint64_t edge=0u;
    if (mixer->started) {
        /* rendered_frames names the next, still-unrendered sample. A rate
         * event between sample points must affect that future sample; only
         * events at or before the last emitted point are retroactive. */
        const uint64_t committed=mixer->rendered_frames!=0u?mixer->rendered_frames-1u:0u;
        const uint64_t offset=mul_div_floor(committed,mixer->ticks_per_second,mixer->output_rate_hz);
        if(offset>UINT64_MAX-mixer->origin_ticks)return false;
        edge=mixer->origin_ticks+offset;
        if(mixer->rendered_frames!=0u?ticks<=edge:ticks<edge)return false;
    }
    bool paused=false;
    const uint64_t active=stream!=NULL?stream_active_ticks(stream,ticks,&paused):ticks;
    const uint64_t retired=stream!=NULL?stream_active_ticks(stream,edge,&paused):edge;
    dsound_frequency_clock candidate=stream!=NULL?stream->frequency_clock:(dsound_frequency_clock){0};
    if(candidate.count==0u && !dsound_stream_frequency_clock_init(&candidate,
        stream!=NULL && stream->source_rate_hz!=0u?stream->source_rate_hz:source_rate_hz))return false;
    if(!dsound_stream_frequency_clock_change(&candidate,active,(uint64_t)source_rate_hz<<32u,retired))return false;
    if (stream == NULL) stream = stream_for(mixer, stream_key);
    if (stream == NULL) return false;
    stream->frequency_clock=candidate;
    stream->frequency_last_wall=ticks;
    stream->source_rate_hz=source_rate_hz;
    return true;
}

bool dsound_mixer_frequency_stream(dsound_mixer *mixer,uint32_t key,uint64_t serial,
    uint64_t ticks,uint32_t base_hz,uint32_t old_hz,uint32_t new_hz)
{
    if(mixer==NULL || serial==0u || base_hz<8000u || base_hz>96000u)return false;
    mixer_stream *stream=stream_find(mixer,key);
    if(stream==NULL || stream->route_serial!=serial ||
       (stream->source_rate_hz!=0u && stream->source_rate_hz!=old_hz) ||
       (stream->frequency_clock.count!=0u && stream->frequency_clock.base_hertz!=base_hz))return false;
    const mixer_stream previous=*stream;
    if(stream->frequency_clock.count==0u &&
       !dsound_stream_frequency_clock_init(&stream->frequency_clock,base_hz))return false;
    if(!dsound_mixer_set_stream_frequency(mixer,key,ticks,new_hz)){*stream=previous;return false;}
    return true;
}

static void cut_stream_phase(dsound_mixer *mixer,mixer_stream *stream,uint32_t stream_key,
    dsound_frequency_phase phase);
bool dsound_mixer_format_stream(dsound_mixer *mixer,uint32_t key,uint64_t serial,
    uint64_t ticks,uint32_t old_hz,uint32_t new_hz)
{
    if(mixer==NULL || old_hz<8000u || old_hz>96000u || new_hz<8000u || new_hz>48000u)return false;
    mixer_stream *stream=stream_find(mixer,key);
    if(stream==NULL || serial==0u || stream->route_serial!=serial ||
       (stream->source_rate_hz!=0u && stream->source_rate_hz!=old_hz))return false;
    bool paused=false;
    dsound_frequency_phase phase;
    const uint64_t active=stream_active_ticks(stream,ticks,&paused);
    if(!stream_source_phase(stream,active,&phase))return false;
    const mixer_stream previous=*stream;
    if(stream->frequency_clock.count==0u &&
       !dsound_stream_frequency_clock_init(&stream->frequency_clock,old_hz))return false;
    if(!dsound_mixer_set_stream_frequency(mixer,key,ticks,new_hz)) {
        const uint64_t edge=mixer->origin_ticks+mul_div_floor(mixer->rendered_frames,mixer->ticks_per_second,mixer->output_rate_hz);
        fprintf(stderr,"dsound: PCM format event rejected ticks=%llu rendered-edge=%llu prior-event=%llu packets-clock-base=%u\n",
            (unsigned long long)ticks,(unsigned long long)edge,(unsigned long long)stream->frequency_last_wall,stream->frequency_clock.base_hertz);
        *stream=previous;return false;
    }
    cut_stream_phase(mixer,stream,key,phase);
    return true;
}

bool dsound_mixer_enable_mixbin_headroom(dsound_mixer *mixer, bool enabled)
{
    if (mixer == NULL || mixer->rendered_frames != 0u) return false;
    mixer->mixbin_headroom_enabled = enabled;
    mixer->mixbin_headroom_count = 0u;
    mixer->mixbin_headroom_identity = 0u;
    return true;
}

bool dsound_mixer_bind_mixbin_headroom(dsound_mixer *mixer,uint64_t identity)
{
    if(mixer==NULL || !mixer->mixbin_headroom_enabled || identity==0u)return false;
    if(mixer->mixbin_headroom_identity==identity)return true;
    for(size_t i=0u;i<MIXER_MAX_PACKETS;i++)if(mixer->packets[i].used)return false;
    for(size_t i=0u;i<MIXER_MAX_BUFFER_RUNS;i++)if(mixer->buffers[i].used)return false;
    mixer->mixbin_headroom_identity=identity;
    mixer->mixbin_headroom_count=0u;
    mixer->mixbin_headroom_last_ticks=0u;
    return true;
}

bool dsound_mixer_set_mixbin_headroom(dsound_mixer *mixer, uint64_t identity, uint64_t ticks,
                                      uint32_t bin, uint32_t headroom)
{
    if (mixer == NULL || !mixer->mixbin_headroom_enabled || identity == 0u || bin >= 32u) return false;
    const bool replacement = mixer->mixbin_headroom_identity != 0u &&
                             mixer->mixbin_headroom_identity != identity;
    if (replacement) {
        for (size_t i=0u;i<MIXER_MAX_PACKETS;i++) if (mixer->packets[i].used) return false;
        for (size_t i=0u;i<MIXER_MAX_BUFFER_RUNS;i++) if (mixer->buffers[i].used) return false;
    }
    if (mixer->started && mixer->rendered_frames != 0u) {
        const uint64_t last = mixer->origin_ticks +
            mul_div_floor(mixer->rendered_frames-1u,mixer->ticks_per_second,mixer->output_rate_hz);
        if (ticks <= last) return false;
    }
    size_t count = replacement ? 0u : mixer->mixbin_headroom_count, keep = 0u;
    mixbin_headroom_event event = {.ticks=ticks};
    if (count != 0u) {
        if (ticks < mixer->mixbin_headroom_last_ticks) return false;
        memcpy(event.amount,mixer->mixbin_headroom[count-1u].amount,sizeof(event.amount));
    } else {
        memset(event.amount,1u,31u);
    }
    event.amount[bin] = (uint8_t)headroom;
    if (count != 0u && memcmp(event.amount,mixer->mixbin_headroom[count-1u].amount,
                               sizeof(event.amount)) == 0) {
        mixer->mixbin_headroom_last_ticks=ticks;
        return true;
    }
    if (count != 0u && ticks == mixer->mixbin_headroom[count-1u].ticks) {
        mixer->mixbin_headroom[count-1u] = event;
        mixer->mixbin_headroom_last_ticks=ticks;
        return true;
    }
    if (mixer->started) {
        const uint64_t edge = mixer->origin_ticks +
            mul_div_floor(mixer->rendered_frames,mixer->ticks_per_second,mixer->output_rate_hz);
        while (keep+1u < count && mixer->mixbin_headroom[keep+1u].ticks <= edge) keep++;
    }
    if (count-keep == 512u) return false;
    if (keep != 0u) memmove(mixer->mixbin_headroom,mixer->mixbin_headroom+keep,
                             (count-keep)*sizeof(event));
    mixer->mixbin_headroom_count = count-keep;
    mixer->mixbin_headroom_identity = identity;
    mixer->mixbin_headroom_last_ticks=ticks;
    mixer->mixbin_headroom[mixer->mixbin_headroom_count++] = event;
    return true;
}

static uint32_t route_gain(uint16_t attenuation)
{
    if (attenuation == 4095u) return 0u;
    if (attenuation == 0u) return 65536u;
    const double x = -(double)attenuation * 2.302585092994046 / 1280.0 / 256.0;
    double term = 1.0, sum = 1.0;
    for (int i = 1; i < 14; i++) { term *= x / i; sum += term; }
    for (int i = 0; i < 8; i++) sum *= sum;
    return (uint32_t)(sum * 65536.0 + 0.5);
}

bool dsound_mixer_route_stream(dsound_mixer *mixer, uint32_t key, uint64_t serial,
                               uint64_t ticks, const dsound_stream_routing *routing)
{
    if (mixer == NULL || key == 0u || serial == 0u || routing == NULL ||
        routing->count > DSOUND_STREAM_ROUTE_SLOTS) return false;
    /* Host stereo projection: FL/FR unity, C/back -3dB, LFE -6dB to both.
     * This projection is explicitly INFERRED, not original DSP execution. */
    static const uint32_t speaker[6][2] = {
        {65536u,0u}, {0u,65536u}, {46341u,46341u},
        {32768u,32768u}, {46341u,0u}, {0u,46341u}};
    mixer_route_event event = {.ticks=ticks};
    const uint32_t per_channel = routing->count / 2u;
    for (uint32_t slot = 0u; slot < routing->count; slot++) {
        if (routing->bins[slot] > 5u) return false;
        if (per_channel == 0u || slot >= per_channel * 2u) continue;
        const uint32_t source = slot / per_channel;
        const uint32_t gain = route_gain(dsound_stream_routing_attenuation(routing,slot));
        for (uint32_t output = 0u; output < 2u; output++) {
            const uint32_t coefficient =
                (uint32_t)(((uint64_t)gain * speaker[routing->bins[slot]][output]+32768u)>>16u);
            event.gain[output*2u+source] += coefficient;
            event.bin_gain[routing->bins[slot]][output*2u+source] += coefficient;
        }
    }
    if (mixer->started && mixer->rendered_frames != 0u) {
        const uint64_t last = mixer->origin_ticks +
            mul_div_floor(mixer->rendered_frames-1u,mixer->ticks_per_second,mixer->output_rate_hz);
        if (ticks <= last) return false;
    }
    mixer_stream *stream = stream_find(mixer,key);
    bool replacement = stream != NULL && stream->route_count != 0u && stream->route_serial != serial;
    if (replacement) {
        for (size_t i = 0u; i < MIXER_MAX_PACKETS; i++)
            if (mixer->packets[i].used && mixer->packets[i].stream_key == key) return false;
    }
    size_t keep = 0u, count = stream != NULL && !replacement ? stream->route_count : 0u;
    if (count != 0u) {
        if (ticks < stream->routes[count-1u].ticks) return false;
        if (memcmp(event.bin_gain,stream->routes[count-1u].bin_gain,sizeof(event.bin_gain)) == 0) return true;
        /* Commands at one timestamp affect the same first unrendered sample.
         * Replace that event rather than spending bounded history on states
         * which no sample can observe. The emitted-sample guard already ran. */
        if (ticks == stream->routes[count-1u].ticks) {
            stream->routes[count-1u] = event;
            return true;
        }
        if (mixer->started) {
            const uint64_t edge = mixer->origin_ticks +
                mul_div_floor(mixer->rendered_frames,mixer->ticks_per_second,mixer->output_rate_hz);
            while (keep+1u < count && stream->routes[keep+1u].ticks <= edge) keep++;
        }
    }
    if (count-keep == MIXER_MAX_ROUTE_EVENTS) return false;
    if (stream == NULL) stream = stream_for(mixer,key);
    if (stream == NULL) return false;
    if (count != 0u && keep != 0u)
        memmove(stream->routes,stream->routes+keep,(count-keep)*sizeof(stream->routes[0]));
    stream->route_count = count-keep;
    stream->route_serial = serial;
    stream->routes[stream->route_count++] = event;
    return true;
}

bool dsound_mixer_reset_stream_volume(dsound_mixer *mixer, uint32_t stream_key)
{
    if (mixer == NULL || stream_key == 0u) return false;
    mixer_stream *stream = stream_find(mixer, stream_key);
    if (stream == NULL) return true;
    stream->attenuated = false;
    stream->gain_q16 = 65536u;
    return true;
}

static mixer_stream *stream_find(dsound_mixer *mixer, uint32_t key)
{
    for (size_t i = 0u; i < MIXER_MAX_STREAMS; i++) {
        if (mixer->streams[i].used && mixer->streams[i].key == key) return &mixer->streams[i];
    }
    return NULL;
}

static mixer_stream *stream_for(dsound_mixer *mixer, uint32_t key)
{
    mixer_stream *free_stream = NULL;
    for (size_t i = 0u; i < MIXER_MAX_STREAMS; i++) {
        mixer_stream *stream = &mixer->streams[i];
        if (stream->used && stream->key == key) return stream;
        if (!stream->used && free_stream == NULL) free_stream = stream;
    }
    if (free_stream != NULL) {
        free_stream->used = true;
        free_stream->key = key;
        free_stream->tail_active_ticks = 0u;
    }
    return free_stream;
}

static uint64_t stream_active_ticks(const mixer_stream *stream, uint64_t wall_ticks,
                                    bool *paused)
{
    uint64_t paused_total = 0u;
    *paused = false;
    for (size_t i = 0u; i < stream->pause_count; i++) {
        const mixer_pause_segment *segment = &stream->pauses[i];
        if (wall_ticks < segment->start_ticks) break;
        if (wall_ticks < segment->end_ticks) {
            *paused = true;
            return segment->start_ticks - paused_total;
        }
        paused_total += segment->end_ticks - segment->start_ticks;
    }
    if (stream->paused && wall_ticks >= stream->pause_started) {
        *paused = true;
        return stream->pause_started - paused_total;
    }
    return wall_ticks - paused_total;
}

static mixer_packet *free_packet(dsound_mixer *mixer)
{
    for (size_t i = 0u; i < MIXER_MAX_PACKETS; i++) {
        if (!mixer->packets[i].used) return &mixer->packets[i];
    }
    return NULL;
}

dsound_mixer *dsound_mixer_create(uint32_t output_rate_hz, uint64_t ticks_per_second)
{
    if (output_rate_hz == 0u || ticks_per_second == 0u) return NULL;
    dsound_mixer *mixer = calloc(1u, sizeof(*mixer));
    if (mixer == NULL) return NULL;
    mixer->output_rate_hz = output_rate_hz;
    mixer->ticks_per_second = ticks_per_second;
    return mixer;
}

void dsound_mixer_destroy(dsound_mixer *mixer)
{
    if (mixer == NULL) return;
    for (size_t i = 0u; i < MIXER_MAX_PACKETS; i++) free(mixer->packets[i].pcm);
    for (size_t i = 0u; i < MIXER_MAX_BUFFER_RUNS; i++) free(mixer->buffers[i].pcm);
    free(mixer);
}

void dsound_mixer_reset_buffers(dsound_mixer *mixer)
{
    if (mixer == NULL) return;
    for (size_t i = 0u; i < MIXER_MAX_BUFFER_RUNS; i++) {
        buffer_run *run = &mixer->buffers[i];
        mixer->queued_pcm_bytes -= run->pcm_bytes;
        free(run->pcm);
        memset(run, 0, sizeof(*run));
    }
}

/* Takes ownership of `decoded` (interleaved stereo s16, `frames` frames) on every path. */
static bool submit_decoded(dsound_mixer *mixer, uint32_t stream_key, uint64_t now_ticks,
                           uint32_t source_rate_hz, int16_t *decoded, size_t frames)
{
    const size_t decoded_bytes = frames * 2u * sizeof(int16_t);
    mixer_stream *stream = NULL;
    for (size_t i = 0u; i < MIXER_MAX_STREAMS; i++) {
        if (mixer->streams[i].used && mixer->streams[i].key == stream_key) {
            stream = &mixer->streams[i];
            break;
        }
    }
    bool currently_paused = false;
    const uint64_t active_wall = stream != NULL
                                    ? stream_active_ticks(stream, now_ticks, &currently_paused)
                                    : now_ticks;
    (void)currently_paused;
    dsound_frequency_phase source_now={active_wall,0u};
    if(stream!=NULL && !stream_source_phase(stream,active_wall,&source_now)){free(decoded);return false;}
    const dsound_frequency_phase tail = stream != NULL
        ? (dsound_frequency_phase){stream->tail_active_ticks, stream->tail_fraction}
        : (dsound_frequency_phase){0u, 0u};
    const dsound_frequency_phase start = source_q32(tail) > source_q32(source_now) ? tail : source_now;
    const uint64_t rendered_offset =
        mixer->started ? mul_div_floor(mixer->rendered_frames!=0u?mixer->rendered_frames-1u:0u,
                                      mixer->ticks_per_second,mixer->output_rate_hz)
                       : 0u;
    if (mixer->started && rendered_offset > UINT64_MAX - mixer->origin_ticks) {
        free(decoded);
        return false;
    }
    const uint64_t rendered_tick = mixer->started ? mixer->origin_ticks + rendered_offset : 0u;
    bool rendered_paused = false;
    const uint64_t rendered_active = stream != NULL
                                        ? stream_active_ticks(stream, rendered_tick,
                                                              &rendered_paused)
                                        : rendered_tick;
    (void)rendered_paused;
    dsound_frequency_phase rendered_source={rendered_active,0u};
    if(stream!=NULL && !stream_source_phase(stream,rendered_active,&rendered_source)){free(decoded);return false;}
    /* A packet beginning between sample points cannot change an emitted
     * sample. Equality at an emitted, running source point is retroactive;
     * a paused point is silent and may share the pending packet's origin. */
    if (mixer->started && (source_q32(start) < source_q32(rendered_source) ||
        (mixer->rendered_frames!=0u && !rendered_paused &&
         source_q32(start)==source_q32(rendered_source)))) {
        mixer->counts.late_packets_refused++;
        free(decoded);
        return false;
    }
    mixer_packet *packet = free_packet(mixer);
    if (stream == NULL) stream = stream_for(mixer, stream_key);
    if (stream == NULL || packet == NULL) {
        free(decoded);
        return false;
    }
    if(stream->source_rate_hz==0u)stream->source_rate_hz=source_rate_hz;
    const uint32_t effective_rate=stream->frequency_clock.count!=0u?stream->frequency_clock.base_hertz:source_rate_hz;
    const uint64_t duration = mul_div_ceil(frames, mixer->ticks_per_second, effective_rate);
    if (duration == 0u || start.ticks > UINT64_MAX - duration) {
        free(decoded);
        return false;
    }

    *packet = (mixer_packet){
        .used = true,
        .stream_key = stream_key,
        .source_rate_hz = effective_rate,
        .start_ticks = start.ticks,
        .end_ticks = start.ticks + duration,
        .start_fraction = start.fraction,
        .end_fraction = start.fraction,
        .frames = frames,
        .pcm_bytes = decoded_bytes,
        .pcm = decoded,
    };
    mixer->queued_pcm_bytes += decoded_bytes;
    stream->tail_active_ticks = packet->end_ticks;
    stream->tail_fraction = packet->end_fraction;
    if (!mixer->started) {
        mixer->started = true;
        mixer->origin_ticks = now_ticks;
    }
    mixer->counts.packets_queued++;
    return true;
}

bool dsound_mixer_submit_xbox_adpcm(dsound_mixer *mixer, uint32_t stream_key,
                                    uint64_t now_ticks, uint32_t source_rate_hz,
                                    const uint8_t *encoded, size_t encoded_bytes)
{
    if (mixer == NULL || stream_key == 0u || source_rate_hz == 0u || encoded == NULL ||
        encoded_bytes == 0u || encoded_bytes > MIXER_MAX_PACKET_BYTES ||
        encoded_bytes % DSOUND_XBOX_ADPCM_BLOCK_BYTES != 0u)
        return false;
    const size_t blocks = encoded_bytes / DSOUND_XBOX_ADPCM_BLOCK_BYTES;
    if (blocks > SIZE_MAX / DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK) return false;
    const size_t frames = blocks * DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK;
    if (frames > SIZE_MAX / (2u * sizeof(int16_t))) return false;
    const size_t decoded_bytes = frames * 2u * sizeof(int16_t);
    if (decoded_bytes > MIXER_MAX_QUEUED_PCM_BYTES - mixer->queued_pcm_bytes) return false;

    int16_t *decoded = malloc(decoded_bytes);
    if (decoded == NULL) return false;
    size_t decoded_frames = 0u;
    if (!dsound_adpcm_xbox_decode_stereo(encoded, encoded_bytes, decoded, frames, &decoded_frames) ||
        decoded_frames != frames) {
        free(decoded);
        return false;
    }

    return submit_decoded(mixer, stream_key, now_ticks, source_rate_hz, decoded, frames);
}

bool dsound_mixer_submit_xbox_adpcm_mono(dsound_mixer *mixer, uint32_t stream_key,
                                         uint64_t now_ticks, uint32_t source_rate_hz,
                                         const uint8_t *encoded, size_t encoded_bytes)
{
    if (mixer == NULL || stream_key == 0u || source_rate_hz == 0u || encoded == NULL ||
        encoded_bytes == 0u || encoded_bytes > MIXER_MAX_PACKET_BYTES ||
        encoded_bytes % DSOUND_XBOX_ADPCM_MONO_BLOCK_BYTES != 0u)
        return false;
    const size_t blocks = encoded_bytes / DSOUND_XBOX_ADPCM_MONO_BLOCK_BYTES;
    if (blocks > SIZE_MAX / DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK) return false;
    const size_t frames = blocks * DSOUND_XBOX_ADPCM_FRAMES_PER_BLOCK;
    if (frames > SIZE_MAX / (2u * sizeof(int16_t))) return false;
    const size_t decoded_bytes = frames * 2u * sizeof(int16_t);
    if (decoded_bytes > MIXER_MAX_QUEUED_PCM_BYTES - mixer->queued_pcm_bytes) return false;
    int16_t *mono = malloc(frames * sizeof(*mono));
    int16_t *decoded = malloc(decoded_bytes);
    size_t decoded_frames = 0u;
    if (mono == NULL || decoded == NULL ||
        !dsound_adpcm_xbox_decode_mono(encoded, encoded_bytes, mono, frames, &decoded_frames) ||
        decoded_frames != frames) {
        free(mono);
        free(decoded);
        return false;
    }
    /* A mono voice feeds both speakers at full gain, exactly like the mono buffer voices (INFERRED, no 3D DSP). */
    for (size_t i = 0u; i < frames; i++) {
        decoded[2u * i] = mono[i];
        decoded[2u * i + 1u] = mono[i];
    }
    free(mono);
    return submit_decoded(mixer, stream_key, now_ticks, source_rate_hz, decoded, frames);
}

bool dsound_mixer_submit_pcm16_stereo(dsound_mixer *mixer, uint32_t stream_key, uint64_t now_ticks,
                                      uint32_t source_rate_hz, const int16_t *pcm, size_t frames)
{
    if (mixer == NULL || stream_key == 0u || source_rate_hz == 0u || pcm == NULL || frames == 0u ||
        frames > MIXER_MAX_PACKET_BYTES / (2u * sizeof(int16_t)))
        return false;
    const size_t bytes = frames * 2u * sizeof(int16_t);
    if (bytes > MIXER_MAX_QUEUED_PCM_BYTES - mixer->queued_pcm_bytes) return false;
    int16_t *copy = malloc(bytes);
    if (copy == NULL) return false;
    memcpy(copy, pcm, bytes);
    return submit_decoded(mixer, stream_key, now_ticks, source_rate_hz, copy, frames);
}

static void cut_stream_phase(dsound_mixer *mixer,mixer_stream *stream,uint32_t stream_key,
    dsound_frequency_phase phase)
{
    const unsigned __int128 active_now=source_q32(phase);
    for (size_t i = 0u; i < MIXER_MAX_PACKETS; i++) {
        mixer_packet *packet = &mixer->packets[i];
        if (!packet->used || packet->stream_key != stream_key || packet_end_q32(packet) <= active_now) continue;
        if (packet_start_q32(packet) >= active_now) {
            mixer->queued_pcm_bytes -= packet->pcm_bytes;
            free(packet->pcm);
            memset(packet, 0, sizeof(*packet));
        } else {
            packet->end_ticks = phase.ticks;
            packet->end_fraction = phase.fraction;
        }
    }
    if (source_q32((dsound_frequency_phase){stream->tail_active_ticks, stream->tail_fraction}) > active_now) {
        stream->tail_active_ticks = phase.ticks;
        stream->tail_fraction = phase.fraction;
    }
}


bool dsound_mixer_cut_stream(dsound_mixer *mixer,uint32_t stream_key,uint64_t now_ticks)
{
    if(mixer==NULL || stream_key==0u)return false;
    mixer_stream *stream=stream_find(mixer,stream_key);
    if(stream==NULL)return true;
    bool paused=false;
    dsound_frequency_phase phase;
    if(!stream_source_phase(stream,stream_active_ticks(stream,now_ticks,&paused),&phase))return false;
    cut_stream_phase(mixer,stream,stream_key,phase);
    return true;
}

bool dsound_mixer_set_stream_running(dsound_mixer *mixer, uint32_t stream_key,
                                     uint64_t now_ticks, bool running)
{
    if (mixer == NULL || stream_key == 0u) return false;
    mixer_stream *stream = stream_for(mixer, stream_key);
    if (stream == NULL) return false;
    if (running == !stream->paused) return true;
    if (!running) {
        stream->paused = true;
        stream->pause_started = now_ticks;
        return true;
    }
    if (stream->pause_count == MIXER_MAX_PAUSE_SEGMENTS || now_ticks < stream->pause_started)
        return false;
    stream->pauses[stream->pause_count++] =
        (mixer_pause_segment){.start_ticks = stream->pause_started, .end_ticks = now_ticks};
    stream->paused = false;
    stream->pause_started = 0u;
    return true;
}

/* T818: band limited resampling and packet joins.
 * Measured with tools/diagnostics/movie_resample_measure.c (docs/presentation.md): the old per packet
 * linear interpolation held the last frame of a packet instead of interpolating into the next one (a
 * click 88 times the body error for a 1 kHz tone) and left a 15 kHz tone with its image at -11.5 dB.
 * Now every tap is read through frame_at(), which walks into the neighbouring packets of the same
 * stream (back to back packets share a tick, finished packets are kept for the filter history), and an
 * upsampling stream (source rate below the output rate) uses a Kaiser windowed sinc, normalised per
 * phase so a constant stays exact. Equal or higher source rates keep the 2 tap linear path, its
 * output is unchanged away from a join. */
#define RESAMPLE_HALF_TAPS 24
#define RESAMPLE_TAPS (2 * RESAMPLE_HALF_TAPS)
#define RESAMPLE_PHASES 256
#define RESAMPLE_KAISER_BETA 8.0

static double resample_sin(double x)
{
    const double pi = 3.14159265358979323846;
    x -= 2.0 * pi * (double)(int64_t)(x / (2.0 * pi));
    if (x > pi) x -= 2.0 * pi;
    if (x < -pi) x += 2.0 * pi;
    double term = x, sum = x;
    for (int i = 1; i < 24; i++) {
        term *= -x * x / (double)((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}

static double resample_sqrt(double x)
{
    if (x <= 0.0) return 0.0;
    double guess = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 60; i++) guess = 0.5 * (guess + x / guess);
    return guess;
}

static double resample_bessel_i0(double x)
{
    double term = 1.0, sum = 1.0;
    for (int i = 1; i < 40; i++) {
        const double half = x / (2.0 * i);
        term *= half * half;
        sum += term;
    }
    return sum;
}

static double resample_table[RESAMPLE_PHASES + 1][RESAMPLE_TAPS];
static bool resample_table_ready;

/* Row p holds h(f + HALF - 1 - m) for tap m (source frame s - HALF + 1 + m) at f = p / PHASES. */
static void resample_table_build(void)
{
    const double pi = 3.14159265358979323846;
    const double denominator = resample_bessel_i0(RESAMPLE_KAISER_BETA);
    for (int phase = 0; phase <= RESAMPLE_PHASES; phase++) {
        double sum = 0.0;
        for (int tap = 0; tap < RESAMPLE_TAPS; tap++) {
            const double u = (double)phase / RESAMPLE_PHASES + (RESAMPLE_HALF_TAPS - 1 - tap);
            const double ratio = u / RESAMPLE_HALF_TAPS;
            const double window = (ratio < 0.0 ? -ratio : ratio) >= 1.0 ? 0.0
                : resample_bessel_i0(RESAMPLE_KAISER_BETA * resample_sqrt(1.0 - ratio * ratio)) / denominator;
            const double sinc = u == 0.0 ? 1.0 : resample_sin(pi * u) / (pi * u);
            resample_table[phase][tap] = sinc * window;
            sum += resample_table[phase][tap];
        }
        for (int tap = 0; tap < RESAMPLE_TAPS; tap++) resample_table[phase][tap] /= sum;
    }
}

static const mixer_packet *packet_neighbour(const dsound_mixer *mixer, const mixer_packet *packet, bool later)
{
    for (size_t i = 0u; i < MIXER_MAX_PACKETS; i++) {
        const mixer_packet *other = &mixer->packets[i];
        if (!other->used || other == packet || other->stream_key != packet->stream_key ||
            other->source_rate_hz != packet->source_rate_hz)
            continue;
        if (later ? packet_start_q32(other) == packet_end_q32(packet) : packet_end_q32(other) == packet_start_q32(packet)) return other;
    }
    return NULL;
}

/* One source sample at `index` relative to the packet start, walking into back to back packets of the
 * same stream. Past the first or last frame of the run it holds that edge frame. */
static int32_t frame_at(const dsound_mixer *mixer, const mixer_packet *packet, int64_t index, unsigned channel)
{
    for (;;) {
        if (index < 0) {
            const mixer_packet *previous = packet_neighbour(mixer, packet, false);
            if (previous == NULL) { index = 0; break; }
            packet = previous;
            index += (int64_t)packet->frames;
        } else if (index >= (int64_t)packet->frames) {
            const mixer_packet *next = packet_neighbour(mixer, packet, true);
            if (next == NULL) { index = (int64_t)packet->frames - 1; break; }
            index -= (int64_t)packet->frames;
            packet = next;
        } else {
            break;
        }
    }
    return packet->pcm[(size_t)index * 2u + channel];
}

static int16_t packet_sample(const dsound_mixer *mixer, const mixer_packet *packet, dsound_frequency_phase at,
                             uint64_t playback_rate_q32,unsigned channel)
{
    const uint64_t ticks_per_second = mixer->ticks_per_second;
    const unsigned __int128 elapsed = source_q32(at) - packet_start_q32(packet);
    const unsigned __int128 position = (elapsed * packet->source_rate_hz) >> 32u;
    const uint64_t source_frame = (uint64_t)(position / ticks_per_second);
    if (source_frame >= packet->frames) return 0;
    const uint64_t remainder = (uint64_t)(position % ticks_per_second);
    if (playback_rate_q32 < ((uint64_t)mixer->output_rate_hz<<32u)) {
        if (!resample_table_ready) { resample_table_build(); resample_table_ready = true; }
        const uint64_t scaled = remainder * RESAMPLE_PHASES;
        const size_t phase = (size_t)(scaled / ticks_per_second);
        const double weight = (double)(scaled % ticks_per_second) / (double)ticks_per_second;
        double total = 0.0;
        for (int tap = 0; tap < RESAMPLE_TAPS; tap++) {
            const double coefficient = resample_table[phase][tap] * (1.0 - weight) +
                                       resample_table[phase + 1u][tap] * weight;
            total += coefficient * (double)frame_at(mixer, packet,
                                                    (int64_t)source_frame - RESAMPLE_HALF_TAPS + 1 + tap, channel);
        }
        const double rounded = total < 0.0 ? total - 0.5 : total + 0.5;
        if (rounded >= 32767.0) return INT16_MAX;
        if (rounded <= -32768.0) return INT16_MIN;
        return (int16_t)(int64_t)rounded;
    }
    const int64_t first = frame_at(mixer, packet, (int64_t)source_frame, channel);
    const int64_t second = frame_at(mixer, packet, (int64_t)source_frame + 1, channel);
    const int64_t weighted = first * (int64_t)(ticks_per_second - remainder) +
                             second * (int64_t)remainder;
    return (int16_t)(weighted / (int64_t)ticks_per_second);
}

static uint64_t buffer_position(const dsound_mixer *mixer, const buffer_run *run,
                                 const buffer_event *event, uint64_t ticks, bool *looped)
{
    unsigned __int128 position = event->position_q32;
    if (!event->paused) {
        position += (((unsigned __int128)(ticks - event->ticks) * event->frequency) << 32u) /
                    mixer->ticks_per_second;
    }
    const uint64_t start = (uint64_t)event->loop_start << 32u;
    const uint64_t end = (uint64_t)event->loop_end << 32u;
    if (looped != NULL) *looped = event->looped;
    if (event->looping && position >= end) {
        if (looped != NULL) *looped = true;
        position = start + (position - start) % (end - start);
    }
    const uint64_t limit = (uint64_t)run->frames << 32u;
    return position < limit ? (uint64_t)position : limit;
}

static buffer_run *buffer_latest(dsound_mixer *mixer, uint32_t key, uint64_t serial)
{
    for (size_t i = 0u; i < MIXER_MAX_BUFFER_RUNS; i++) {
        buffer_run *run = &mixer->buffers[i];
        if (run->used && run->key == key && run->serial == serial && run->end_ticks == UINT64_MAX)
            return run;
    }
    return NULL;
}

/* T1216: the sink renders ahead of the guest's virtual clock (its refill target grows after an underrun, up to
 * seconds), so a guest command stamped before the audio already emitted can no longer change that audio. The
 * command takes effect at the first tick not rendered yet (the horizon), never retroactively, instead of
 * refusing the guest (the Story control run stopped on a Stop after 120 s). INFERRED: the real hardware applies
 * a command when the guest issues it, the host's buffering latency is what makes the audio late. */
static bool buffer_command_time(dsound_mixer *mixer, uint64_t *ticks)
{
    if (!mixer->started) return true;
    const uint64_t offset = mul_div_floor(mixer->rendered_frames, mixer->ticks_per_second,
                                         mixer->output_rate_hz);
    if (offset > UINT64_MAX - mixer->origin_ticks) return false;
    const uint64_t horizon = mixer->origin_ticks + offset;
    if (*ticks < horizon) { *ticks = horizon; mixer->counts.buffer_commands_late++; }
    return true;
}

bool dsound_mixer_play_mono_buffer(dsound_mixer *mixer, uint32_t key, uint64_t serial,
                                  uint64_t ticks, uint32_t frequency,
                                  const int16_t *pcm, size_t frames,
                                  bool paused, bool looping, size_t loop_start, size_t loop_end,
                                  int32_t volume)
{
    uint32_t gain = 0u;
    if (mixer == NULL || key == 0u || serial == 0u || frequency == 0u || pcm == NULL ||
        frames == 0u || frames > MIXER_MAX_PACKET_BYTES / sizeof(*pcm) ||
        (looping && (loop_start >= loop_end || loop_end > frames)) ||
        !dsound_mixer_volume_to_gain_q16(volume, &gain) || !buffer_command_time(mixer, &ticks))
        return false;
    buffer_run *slot = NULL;
    for (size_t i = 0u; i < MIXER_MAX_BUFFER_RUNS; i++) {
        buffer_run *run = &mixer->buffers[i];
        if (!run->used && slot == NULL) slot = run;
        if (run->used && run->key == key && run->events[run->event_count - 1u].ticks > ticks)
            return false;
    }
    const size_t bytes = frames * sizeof(*pcm);
    if (slot == NULL || bytes > MIXER_MAX_QUEUED_PCM_BYTES - mixer->queued_pcm_bytes) return false;
    int16_t *copy = malloc(bytes);
    if (copy == NULL) return false;
    memcpy(copy, pcm, bytes);
    /* Allocation and every refusal precede retirement of the previous play. */
    for (size_t i = 0u; i < MIXER_MAX_BUFFER_RUNS; i++) {
        buffer_run *run = &mixer->buffers[i];
        if (run->used && run->key == key && run->end_ticks == UINT64_MAX) run->end_ticks = ticks;
    }
    *slot = (buffer_run){.used = true, .key = key, .serial = serial, .end_ticks = UINT64_MAX,
        .pcm = copy, .frames = frames, .pcm_bytes = bytes, .event_count = 1u,
        .events = {{.ticks = ticks, .frequency = frequency, .gain_q16 = gain,
                    .loop_start = loop_start, .loop_end = loop_end,
                    .paused = paused, .looping = looping}}};
    mixer->queued_pcm_bytes += bytes;
    if (!mixer->started) { mixer->started = true; mixer->origin_ticks = ticks; }
    return true;
}

typedef enum buffer_command { BUFFER_PAUSE, BUFFER_STOP, BUFFER_FREQUENCY, BUFFER_VOLUME, BUFFER_LOOP } buffer_command;
static bool change_buffer(dsound_mixer *mixer, uint32_t key, uint64_t serial, uint64_t ticks,
                           buffer_command command, uint32_t value, size_t loop_start, size_t loop_end)
{
    if (mixer == NULL) return false;
    mixer->last_buffer_refusal = "the render horizon overflows the tick range";
    if (!buffer_command_time(mixer, &ticks)) return false;
    mixer->last_buffer_refusal = "no live run for this buffer lease (its Play was retired by the end of its data or never reached the mixer)";
    buffer_run *run = buffer_latest(mixer, key, serial);
    if (run == NULL) return false;
    const buffer_event *previous = &run->events[run->event_count - 1u];
    mixer->last_buffer_refusal = "the command timestamp is before the buffer's latest recorded event";
    if (ticks < previous->ticks) return false;
    mixer->last_buffer_refusal = "the frequency or volume value is invalid";
    buffer_event event = *previous;
    event.position_q32 = buffer_position(mixer, run, previous, ticks, &event.looped);
    event.ticks = ticks;
    if (command == BUFFER_PAUSE) event.paused = value != 0u;
    else if (command == BUFFER_STOP) { event.looping = false; event.paused = false; }
    else if (command == BUFFER_FREQUENCY) {
        if (value == 0u) return false;
        event.frequency = value;
    } else if (command == BUFFER_LOOP) {
        /* T1524: a live SetLoopRegion. The cursor so far was wrapped by the OLD region (event.position_q32 above), the NEW
         * region applies from this tick on: a cursor past the new end wraps into it (buffer_position), a cursor before the
         * new start plays forward to the new end first (looped is cleared so the sample taps do not wrap backwards). */
        mixer->last_buffer_refusal = "the live loop region is empty or past the buffer's data";
        if (loop_start >= loop_end || loop_end > run->frames) return false;
        event.loop_start = loop_start;
        event.loop_end = loop_end;
        if (event.position_q32 < ((uint64_t)loop_start << 32u)) event.looped = false;
    } else if (!dsound_mixer_volume_to_gain_q16((int32_t)value, &event.gain_q16)) return false;
    if (event.loop_start != previous->loop_start || event.loop_end != previous->loop_end) {
        /* a region change is an event of its own, the checks below only cover the other commands */
    } else if (event.paused == previous->paused && event.looping == previous->looping &&
        event.frequency == previous->frequency && event.gain_q16 == previous->gain_q16) return true;
    mixer->last_buffer_refusal = "the 64 event history of the run is full (events not yet rendered)";
    if (ticks != previous->ticks && run->event_count == MIXER_MAX_BUFFER_EVENTS) return false;
    const size_t index = ticks == previous->ticks ? run->event_count - 1u : run->event_count++;
    run->events[index] = event;
    return true;
}

const char *dsound_mixer_last_buffer_refusal(const dsound_mixer *mixer)
{ return mixer != NULL && mixer->last_buffer_refusal != NULL ? mixer->last_buffer_refusal : "unknown"; }
bool dsound_mixer_pause_buffer(dsound_mixer *mixer, uint32_t key, uint64_t serial,
                               uint64_t ticks, bool paused)
{ return change_buffer(mixer, key, serial, ticks, BUFFER_PAUSE, paused ? 1u : 0u, 0u, 0u); }
bool dsound_mixer_stop_buffer(dsound_mixer *mixer, uint32_t key, uint64_t serial, uint64_t ticks)
{ return change_buffer(mixer, key, serial, ticks, BUFFER_STOP, 0u, 0u, 0u); }
bool dsound_mixer_frequency_buffer(dsound_mixer *mixer, uint32_t key, uint64_t serial,
                                   uint64_t ticks, uint32_t frequency)
{ return change_buffer(mixer, key, serial, ticks, BUFFER_FREQUENCY, frequency, 0u, 0u); }
bool dsound_mixer_volume_buffer(dsound_mixer *mixer, uint32_t key, uint64_t serial,
                                uint64_t ticks, int32_t volume)
{ return change_buffer(mixer, key, serial, ticks, BUFFER_VOLUME, (uint32_t)volume, 0u, 0u); }
bool dsound_mixer_loop_buffer(dsound_mixer *mixer, uint32_t key, uint64_t serial,
                              uint64_t ticks, size_t loop_start, size_t loop_end)
{ return change_buffer(mixer, key, serial, ticks, BUFFER_LOOP, 0u, loop_start, loop_end); }

static int16_t buffer_frame_at(const buffer_run *run, const buffer_event *event, int64_t frame)
{
    if (event->looping && (frame >= (int64_t)event->loop_end ||
                           (event->looped && frame < (int64_t)event->loop_start))) {
        const int64_t length = (int64_t)(event->loop_end - event->loop_start);
        int64_t relative = (frame - (int64_t)event->loop_start) % length;
        if (relative < 0) relative += length;
        frame = (int64_t)event->loop_start + relative;
    }
    if (frame < 0) frame = 0;
    if (frame >= (int64_t)run->frames) frame = (int64_t)run->frames - 1;
    return run->pcm[(size_t)frame];
}

static int32_t submix_sample(int32_t sample,uint8_t headroom)
{
    const uint32_t gain=65536u >> (headroom&7u);
    return (int32_t)(((int64_t)sample*gain+32768)>>16u);
}

static int32_t buffer_sample(const dsound_mixer *mixer, const buffer_run *run, uint64_t ticks)
{
    if (!run->used || ticks < run->events[0].ticks || ticks >= run->end_ticks) return 0;
    const buffer_event *event = &run->events[0];
    for (size_t i = 1u; i < run->event_count && run->events[i].ticks <= ticks; i++) event = &run->events[i];
    if (event->paused) return 0;
    buffer_event sampling = *event;
    const uint64_t position = buffer_position(mixer, run, event, ticks, &sampling.looped);
    const uint64_t frame = position >> 32u;
    if (frame >= run->frames) return 0;
    const uint64_t fraction = position & UINT32_MAX;
    int64_t sample;
    if (event->frequency < mixer->output_rate_hz) {
        if (!resample_table_ready) { resample_table_build(); resample_table_ready = true; }
        const uint64_t scaled = fraction * RESAMPLE_PHASES;
        const size_t phase = (size_t)(scaled >> 32u);
        const double weight = (double)(scaled & UINT32_MAX) / 4294967296.0;
        double total = 0.0;
        for (int tap = 0; tap < RESAMPLE_TAPS; tap++) {
            const double coefficient = resample_table[phase][tap] * (1.0 - weight) +
                                       resample_table[phase + 1u][tap] * weight;
            total += coefficient * buffer_frame_at(run, &sampling, (int64_t)frame - RESAMPLE_HALF_TAPS + 1 + tap);
        }
        sample = (int64_t)(total < 0.0 ? total - 0.5 : total + 0.5);
        if (sample > INT16_MAX) sample = INT16_MAX;
        if (sample < INT16_MIN) sample = INT16_MIN;
    } else {
        const int64_t first = buffer_frame_at(run, &sampling, (int64_t)frame);
        const int64_t second = buffer_frame_at(run, &sampling, (int64_t)frame + 1);
        sample = (first * (int64_t)(UINT64_C(1) << 32u) + (second - first) * (int64_t)fraction) /
                 (int64_t)(UINT64_C(1) << 32u);
    }
    return (int32_t)((sample * event->gain_q16 + 32768) >> 16u);
}

bool dsound_mixer_render_until(dsound_mixer *mixer, uint64_t now_ticks, int16_t *output,
                               size_t frame_capacity, size_t *frames_written)
{
    if (frames_written == NULL || mixer == NULL ||
        (frame_capacity != 0u && output == NULL))
        return false;
    *frames_written = 0u;
    if (!mixer->started || now_ticks <= mixer->origin_ticks || frame_capacity == 0u) return true;

    const uint64_t elapsed = now_ticks - mixer->origin_ticks;
    const uint64_t target_frame =
        mul_div_ceil(elapsed, mixer->output_rate_hz, mixer->ticks_per_second);
    if (target_frame <= mixer->rendered_frames) return true;
    uint64_t available = target_frame - mixer->rendered_frames;
    if (available > frame_capacity) available = frame_capacity;
    if (available > SIZE_MAX) return false;
    const size_t count = (size_t)available;
    memset(output, 0, count * 2u * sizeof(*output));

    for (size_t frame_index = 0u; frame_index < count; frame_index++) {
        const uint64_t timeline_frame = mixer->rendered_frames + frame_index;
        const uint64_t offset_ticks =
            mul_div_floor(timeline_frame, mixer->ticks_per_second, mixer->output_rate_hz);
        if (offset_ticks > UINT64_MAX - mixer->origin_ticks) return false;
        const uint64_t at_ticks = mixer->origin_ticks + offset_ticks;
        uint8_t headroom[32] = {0};
        if (mixer->mixbin_headroom_enabled) {
            memset(headroom,1u,31u);
            for (size_t i=0u;i<mixer->mixbin_headroom_count;i++) {
                if (mixer->mixbin_headroom[i].ticks > at_ticks) break;
                memcpy(headroom,mixer->mixbin_headroom[i].amount,sizeof(headroom));
            }
        }
        int32_t mix_left = 0;
        int32_t mix_right = 0;
        for (size_t i = 0u; i < MIXER_MAX_BUFFER_RUNS; i++) {
            const int32_t sample = buffer_sample(mixer, &mixer->buffers[i], at_ticks);
            mix_left += submix_sample(sample,headroom[0]);
            mix_right += submix_sample(sample,headroom[1]);
        }
        for (size_t packet_index = 0u; packet_index < MIXER_MAX_PACKETS; packet_index++) {
            const mixer_packet *packet = &mixer->packets[packet_index];
            if (!packet->used) continue;
            const mixer_stream *stream = NULL;
            for (size_t stream_index = 0u; stream_index < MIXER_MAX_STREAMS; stream_index++) {
                if (mixer->streams[stream_index].used &&
                    mixer->streams[stream_index].key == packet->stream_key) {
                    stream = &mixer->streams[stream_index];
                    break;
                }
            }
            if (stream == NULL) continue;
            bool paused = false;
            const uint64_t active_ticks = stream_active_ticks(stream, at_ticks, &paused);
            dsound_frequency_phase phase;
            if(!stream_source_phase(stream,active_ticks,&phase))return false;
            if (paused || source_q32(phase) < packet_start_q32(packet) || source_q32(phase) >= packet_end_q32(packet))
                continue;
            uint64_t playback_rate_q32=(uint64_t)packet->source_rate_hz<<32u;
            for(uint32_t event=0u;event<stream->frequency_clock.count;event++) {
                if(stream->frequency_clock.events[event].active_ticks>active_ticks)break;
                playback_rate_q32=stream->frequency_clock.events[event].frequency_q32;
            }
            int32_t left = packet_sample(mixer, packet, phase, playback_rate_q32,0u);
            int32_t right = packet_sample(mixer, packet, phase, playback_rate_q32,1u);
            const mixer_route_event *route = NULL;
            for (size_t r = 0u; r < stream->route_count; r++) {
                if (stream->routes[r].ticks > at_ticks) break;
                route = &stream->routes[r];
            }
            if (route != NULL && mixer->mixbin_headroom_enabled) {
                uint32_t gain[4] = {0};
                for (size_t bin=0u;bin<6u;bin++)
                    for (size_t channel=0u;channel<4u;channel++)
                        gain[channel] += route->bin_gain[bin][channel] >> (headroom[bin]&7u);
                const int32_t source_left = left, source_right = right;
                left = (int32_t)(((int64_t)source_left*gain[0] +
                                 (int64_t)source_right*gain[1]+32768)>>16);
                right = (int32_t)(((int64_t)source_left*gain[2] +
                                  (int64_t)source_right*gain[3]+32768)>>16);
            } else if (route != NULL) {
                const int32_t source_left = left, source_right = right;
                left = (int32_t)(((int64_t)source_left*route->gain[0] +
                                  (int64_t)source_right*route->gain[1]+32768)>>16);
                right = (int32_t)(((int64_t)source_left*route->gain[2] +
                                   (int64_t)source_right*route->gain[3]+32768)>>16);
            }
            if (route == NULL) {
                if (stream->attenuated) {
                    left = (int32_t)(((int64_t)left * stream->gain_q16 + 32768) >> 16);
                    right = (int32_t)(((int64_t)right * stream->gain_q16 + 32768) >> 16);
                }
            }
            if (route == NULL) {
                left = submix_sample(left,headroom[0]);
                right = submix_sample(right,headroom[1]);
            }
            mix_left += left;
            mix_right += right;
        }
        if (mix_left > INT16_MAX) mix_left = INT16_MAX;
        if (mix_left < INT16_MIN) mix_left = INT16_MIN;
        if (mix_right > INT16_MAX) mix_right = INT16_MAX;
        if (mix_right < INT16_MIN) mix_right = INT16_MIN;
        output[frame_index * 2u] = (int16_t)mix_left;
        output[frame_index * 2u + 1u] = (int16_t)mix_right;
    }

    mixer->rendered_frames += count;
    mixer->counts.frames_emitted += count;
    *frames_written = count;
    /* Retire only audio whose sample points were actually emitted. A bounded
     * render capacity must not discard the unrendered tail at now_ticks. */
    const unsigned __int128 next_value = mixer->origin_ticks +
        (unsigned __int128)mixer->rendered_frames * mixer->ticks_per_second / mixer->output_rate_hz;
    const uint64_t next_tick = next_value > UINT64_MAX ? UINT64_MAX : (uint64_t)next_value;
    for (size_t i = 0u; i < MIXER_MAX_BUFFER_RUNS; i++) {
        buffer_run *run = &mixer->buffers[i];
        if (!run->used) continue;
        size_t consumed = 0u;
        while (consumed + 1u < run->event_count && run->events[consumed + 1u].ticks <= next_tick)
            consumed++;
        const buffer_event *last = &run->events[run->event_count - 1u];
        const bool finished = last->ticks <= next_tick && !last->paused && !last->looping &&
            buffer_position(mixer, run, last, next_tick, NULL) >= ((uint64_t)run->frames << 32u);
        if (run->end_ticks <= next_tick || finished) {
            mixer->queued_pcm_bytes -= run->pcm_bytes;
            free(run->pcm);
            memset(run, 0, sizeof(*run));
        } else if (consumed != 0u) {
            run->event_count -= consumed;
            memmove(run->events, run->events + consumed, run->event_count * sizeof(run->events[0]));
        }
    }
    for (size_t i = 0u; i < MIXER_MAX_PACKETS; i++) {
        mixer_packet *packet = &mixer->packets[i];
        mixer_stream *stream = packet->used ? stream_find(mixer, packet->stream_key) : NULL;
        bool paused = false;
        const uint64_t active_now = stream != NULL
                                        ? stream_active_ticks(stream, now_ticks, &paused)
                                        : now_ticks;
        dsound_frequency_phase source_now={active_now,0u};
        if(stream!=NULL && !stream_source_phase(stream,active_now,&source_now))return false;
        /* A finished packet is kept for the filter history of the next one (T818). */
        if (packet->used && source_q32(source_now) >= packet_end_q32(packet) +
            ((unsigned __int128)mul_div_ceil(RESAMPLE_HALF_TAPS, mixer->ticks_per_second,
                                           packet->source_rate_hz) << 32u)) {
            mixer->queued_pcm_bytes -= packet->pcm_bytes;
            free(packet->pcm);
            memset(packet, 0, sizeof(*packet));
        }
    }
    return true;
}

dsound_mixer_counts dsound_mixer_get_counts(const dsound_mixer *mixer)
{
    return mixer != NULL ? mixer->counts : (dsound_mixer_counts){0};
}
