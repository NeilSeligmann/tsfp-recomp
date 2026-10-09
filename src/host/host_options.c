/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See host_options.h for why this is not in main.c: everything compiled only into
 * tsfp_host is unreachable by every test suite and by every mutation, and these are
 * the defaults that decide what the title sees.
 *
 * MOVED VERBATIM. Not one branch changed in the extraction, deliberately, so that a
 * diff of the host run before and after is the proof -- and it is byte-identical.
 */

#include "host_options.h"
#include "../gpu/gpu_standin_pattern.h"
#include "xinput_hotkey.h"
#include "xinput_route_nav_spec.h"
#include "xinput_route_spec.h"
#include "xmv_seed_patch.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* WxH:RRGGBBAA or WxH:checker for --gpu-replay-standin-texture. Plain decimal edges of 1..GPU_REPLAY_MAX_EDGE and exactly eight hex
 * digits, nothing before, between or after (strtol would take a sign, a space or a 0x prefix, so the digits are read by hand). */
static bool parse_standin(const char *text, options *out)
{
    unsigned edges[2];
    for (unsigned edge = 0u; edge < 2u; edge++) {
        unsigned long value = 0u;
        size_t digits = 0u;
        while (text[digits] >= '0' && text[digits] <= '9') {
            value = value * 10u + (unsigned long)(text[digits] - '0');
            if (++digits > 5u) {
                return false;
            }
        }
        if (digits == 0u || value < 1u || value > (unsigned long)GPU_REPLAY_MAX_EDGE ||
            text[digits] != (edge == 0u ? 'x' : ':')) {
            return false;
        }
        edges[edge] = (unsigned)value;
        text += digits + 1u;
    }
    uint8_t rgba[4] = {0};
    const bool checker = strcmp(text, "checker") == 0;
    for (size_t byte = 0u; byte < 4u && !checker; byte++) {
        unsigned value = 0u;
        for (size_t nibble = 0u; nibble < 2u; nibble++) {
            const char digit = text[byte * 2u + nibble];
            const unsigned part = digit >= '0' && digit <= '9' ? (unsigned)(digit - '0')
                                  : digit >= 'a' && digit <= 'f' ? (unsigned)(digit - 'a') + 10u
                                  : digit >= 'A' && digit <= 'F' ? (unsigned)(digit - 'A') + 10u
                                                                 : 16u;
            if (part == 16u) {
                return false;
            }
            value = value * 16u + part;
        }
        rgba[byte] = (uint8_t)value;
    }
    if (!checker && text[8] != '\0') {
        return false;
    }
    out->gpu_replay_standin = true;
    out->gpu_replay_standin_pattern = checker ? GPU_STANDIN_CHECKER : GPU_STANDIN_SOLID;
    out->gpu_replay_standin_width = edges[0];
    out->gpu_replay_standin_height = edges[1];
    memcpy(out->gpu_replay_standin_rgba, rgba, sizeof rgba);
    return true;
}

/* A positive whole number up to the policy cap, nothing after it (T460 owner waits, T592 worker blanks). */
/* T751: --pad-source script|keyboard|gamepad. script needs --pad-script and no feed, keyboard and gamepad need
 * the synthetic pad, exclude --pad-script and take --pad-feed or --present window as the one provider (checked when the source opens). */
/* T1629: a plain decimal (or, with base 0, C integer) number in [min, max], digits only, no sign or blanks. */
static bool parse_bounded(const char *text, int base, unsigned long long min, unsigned long long max, unsigned long long *value)
{
    if (text == NULL || text[0] < '0' || text[0] > '9') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long long parsed = strtoull(text, &end, base);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }
    *value = parsed;
    return true;
}

/* T1629: --dump-after-frames 2,30: 1 to 4 strictly increasing frame counts, each 1..HOST_BUTTON_DUMP_MAX_AFTER_FRAME. */
static bool parse_after_frames(const char *text, options *out)
{
    uint32_t frames[4];
    unsigned count = 0u;
    const char *cursor = text;
    while (true) {
        if (count >= 4u || cursor[0] < '0' || cursor[0] > '9') {
            return false;
        }
        char *end = NULL;
        errno = 0;
        const unsigned long long value = strtoull(cursor, &end, 10);
        if (errno != 0 || value == 0u || value > HOST_BUTTON_DUMP_MAX_AFTER_FRAME ||
            (count != 0u && value <= frames[count - 1u])) {
            return false;
        }
        frames[count++] = (uint32_t)value;
        if (*end == '\0') {
            break;
        }
        if (*end != ',') {
            return false;
        }
        cursor = end + 1;
    }
    memset(out->dump_after, 0, sizeof out->dump_after);
    memcpy(out->dump_after, frames, count * sizeof frames[0]);
    out->dump_after_count = count;
    return true;
}

static bool pad_source_options_valid(const options *out)
{
    if (out->pad_source == NULL) return out->pad_feed == NULL;
    if (!out->synthetic_pad && !out->controllers) return false;
    if (strcmp(out->pad_source, "script") == 0) return out->pad_script != NULL && out->pad_feed == NULL;
    if (strcmp(out->pad_source, "keyboard") != 0 && strcmp(out->pad_source, "gamepad") != 0) return false;
    /* One provider: the fake-feed file or the SDL window, never both. */
    if (out->pad_feed != NULL && out->present != NULL && strcmp(out->present, "window") == 0) return false;
    return out->pad_script == NULL;
}

static bool parse_blank_budget(const char *text, uint32_t *out)
{
    char *end = NULL;
    const unsigned long long budget = strtoull(text, &end, 10);
    if (end == text || *end != '\0' || budget == 0u || budget > 1000000u ||
        text[0] < '0' || text[0] > '9') {
        return false;
    }
    *out = (uint32_t)budget;
    return true;
}

/* One decimal number below 10 million, then the first character after it. False for no digit. */
static bool parse_entry_number(const char *text, uint32_t *value, const char **rest)
{
    char *end = NULL;
    if (text[0] < '0' || text[0] > '9') {
        return false;
    }
    const unsigned long long number = strtoull(text, &end, 10);
    if (end == text || number >= 10000000u) {
        return false;
    }
    *value = (uint32_t)number;
    *rest = end;
    return true;
}

/* T611: a patch spec that the seeded wrapper run can apply. */
static bool xmv_seed_patch_valid(const char *text)
{
    xmv_seed_patch patch;
    return xmv_seed_patch_parse(text, &patch);
}

/* T538: "2,5-9" is the entries 2 and 5 to 9, at most 32 ranges, ascending and not overlapping. */
static bool parse_entry_ranges(const char *text, options *out)
{
    out->capture_xmv_range_count = 0u;
    for (;;) {
        uint32_t first = 0u, last = 0u;
        const char *rest = NULL;
        if (!parse_entry_number(text, &first, &rest)) {
            return false;
        }
        last = first;
        if (*rest == '-' && !parse_entry_number(rest + 1, &last, &rest)) {
            return false;
        }
        const uint32_t count = out->capture_xmv_range_count;
        if (last < first || count == 32u ||
            (count != 0u && first <= out->capture_xmv_range_last[count - 1u])) {
            return false;
        }
        out->capture_xmv_range_first[count] = first;
        out->capture_xmv_range_last[count] = last;
        out->capture_xmv_range_count = count + 1u;
        if (*rest == '\0') {
            return true;
        }
        if (*rest != ',') {
            return false;
        }
        text = rest + 1;
    }
}

bool parse_options(int argc, char **argv, options *out)
{
    out->xbe_path = NULL;
    out->continue_on_missing = false;
    out->trace_report = DEFAULT_TRACE_REPORT;
    out->stub_status = 0u;
    out->stub_status_set = false;
    out->thread_timeout_ms = DEFAULT_THREAD_TIMEOUT_MS;
    out->thread_timeout_given = false;
    out->open_missing_as_empty = false;
    out->mount_count = 0u;
    out->ac97_ready = true; /* T375 owner decision: a retail console has the codec ready */
    out->ac97_ready_flag_given = false;
    /* Default ON since T72a (see host_options.h): measured acceptance, not fabrication.
     * The binding itself still refuses everything unproven, loudly. */
    out->headless_effects = true;
    out->headless_streams = false;
    out->headless_buffers = false;
    out->headless_listener = false;
    out->headless_first_vblank = false;
    out->headless_second_vblank = false;
    out->trace_vblank_schedule = false;
    out->couple_vblank_effects = false;
    out->model_flips = false;
    out->vblank_dispatch_level = false;
    out->trace_vblank_readers = false;
    out->check_vblank_quiescence = false;
    out->interactive = false;
    out->vblank_owner_waits = 0u;
    out->vblank_worker_blanks = 0u;
    out->vblank_poll_blank = false;
    out->native_shader_assembler = false;
    out->xonline_offline = false;
    out->xnet_local_entropy = false;
    out->native_xmv = false;
    out->native_xmv_explicit = false;
    out->native_xmv_off = false;
    out->trace_xmv = false;
    out->skip_intro = false;
    out->dump_xmv_frames = NULL;
    out->dump_xmv_frames_max = 0u;
    out->capture_xmv_entries = NULL;
    out->capture_xmv_range_count = 0u;
    memset(out->capture_xmv_range_first, 0, sizeof out->capture_xmv_range_first);
    memset(out->capture_xmv_range_last, 0, sizeof out->capture_xmv_range_last);
    out->seed_xmv_entry_set = false;
    out->seed_xmv_entry = 0u;
    out->seed_xmv_result = NULL;
    out->seed_xmv_patch_count = 0u;
    memset(out->seed_xmv_patch, 0, sizeof out->seed_xmv_patch);
    out->xmv_substitute = NULL;
    out->eeprom_language = 0u;
    out->eeprom_path = NULL;
    out->eeprom_key_path = NULL;
    out->xnet_scheduler = false;
    out->overlay_consume = false;
    out->dump_overlay = NULL;
    out->dump_overlay_max = 0u;
    out->overlay_xemu_image = false;
    out->overlay_xemu_key = false;
    out->present = NULL;
    out->audio_sink = NULL;
    out->audio_output = NULL;
    out->audio_mute = false;
    out->present_hold_ms = 0u;
    out->audio_latency_ms = 80u; /* T1487: was 150 */
    out->live_pipeline = 0u;
    out->live_pipeline_set = false;
    out->live_present_sync = false;
    out->live_frame_hash = NULL;
    out->live_pipeline_cache = NULL;
    out->live_pipeline_cache_off = false;
    out->audio_pump = true;
    out->audio_hold_ms = 400u; /* T1250 gaps */
    out->audio_min_rate_permille = 250u; /* T1250: on by default, the pitch preserving stretch keeps the slowdown natural */
    out->audio_stretch_resample = false;
    out->audio_stretch_legacy = false; /* T1487 */
    out->av_sync_offset_ms = 0;
    out->audio_latency_set = false;
    out->present_capture = NULL;
    out->present_no_pace = false;
    out->present_timeline = NULL;
    out->cpu_profile = NULL;
    out->cpu_profile_wall = false;
    out->guest_frame_trace_off = false;
    out->headless_movie_audio = false;
    out->passive_audio_completion = false;
    out->passive_audio_completion_off = false;
    out->passive_audio_completion_at_dowork = false;
    out->passive_audio_completion_by_default = false;
    out->passive_audio_completion_at_dowork_off = false;
    out->passive_audio_completion_at_dowork_by_default = false;
    out->async_file_io = false;
    out->async_file_io_file_object = false;
    out->async_file_io_off = false;
    out->async_file_io_by_default = false;
    out->async_file_io_spin = 0u;
    out->vblank_poll_blank_off = false;
    out->vblank_poll_blank_by_default = false;
    out->synthetic_pad = false;
    out->synthetic_pad_remove_after_polls = 0u;
    out->pad_script = NULL;
    out->pad_script_live = false;
    out->record_input = NULL;
    out->replay_input = NULL;
    out->replay_handover = false;
    out->route_wait_count = 0u;
    out->poke_at_count = 0u;
    out->route_event_wait_count = 0u;
    out->route_event_log = NULL;
    out->route_log_mem_count = 0u;
    out->route_nav = NULL;
    out->route_nav_menus = NULL;
    out->route_nav_timing = NULL;
    out->route_nav_record = NULL;
    out->hotkey_count = 0u;
    out->hotkey_dir = NULL;
    out->shot_dir = NULL;
    out->shot_max = HOST_OPTIONS_SHOT_MAX_DEFAULT;
    out->shot_max_bytes = HOST_OPTIONS_SHOT_MAX_BYTES_DEFAULT;
    out->snapshot_at_poll = 0u;
    out->stop_at_poll = 0u;
    out->pad_source = NULL;
    out->pad_feed = NULL;
    out->mu_image_count = 0u;
    memset(out->mu_images, 0, sizeof(out->mu_images));
    out->controllers = false;
    out->controller_config = NULL;
    out->controller_mappings = NULL;
    out->controller_save = NULL;
    out->disc_path = NULL;
    out->disc_device = DEFAULT_DISC_DEVICE;
    out->hdd_path = NULL;
    out->disk_identity_xemu = false;
    out->hdd_device = DEFAULT_HDD_DEVICE;
    out->cache_partitions = DEFAULT_CACHE_PARTITIONS;
    out->av_pack = NULL;
    out->profile_calls = false;
    out->profile_calls_top = DEFAULT_PROFILE_TOP;
    out->profile_watch_count = 0u;
    memset(&out->dump_guest_set, 0, sizeof out->dump_guest_set);
    out->dump_guest_dir = NULL;
    memset(&out->watch_write_set, 0, sizeof out->watch_write_set);
    out->watch_write_log = NULL;
    out->watch_write_max = GUEST_WATCH_DEFAULT_RECORDS;
    out->forced_state = false;
    out->dump_guest_max_bytes = GUEST_DUMP_DEFAULT_MAX_BYTES;
    out->dump_on_button = false;
    out->dump_button_tuned = false;
    out->dump_after_count = 2u;
    out->dump_after[0] = HOST_BUTTON_DUMP_DEFAULT_AFTER_1;
    out->dump_after[1] = HOST_BUTTON_DUMP_DEFAULT_AFTER_2;
    out->dump_after[2] = 0u;
    out->dump_after[3] = 0u;
    out->dump_button_threshold = HOST_BUTTON_DUMP_DEFAULT_THRESHOLD;
    out->dump_button_coalesce = HOST_BUTTON_DUMP_DEFAULT_COALESCE;
    out->dump_button_max_pending = HOST_BUTTON_DUMP_DEFAULT_MAX_PENDING;
    out->dump_button_max_dumps = HOST_BUTTON_DUMP_DEFAULT_MAX_DUMPS;
    out->dump_button_max_bytes = HOST_BUTTON_DUMP_DEFAULT_MAX_BYTES;
    out->dump_button_start_poll = 0u;
    out->dump_button_after_replay = false;
    out->dump_button_idle_every = 0u;
    out->dump_button_max_idle = HOST_BUTTON_DUMP_DEFAULT_MAX_IDLE;
    out->census_icalls = false;
    out->census_phases = NULL;
    out->voice_log = NULL;
    out->census_window_set = false;
    out->census_window_first = 0u;
    out->census_window_last = 0u;
    out->stop_after_set = false;
    out->stop_after_ordinal = false;
    out->stop_after_id = 0u;
    out->stop_after_count = 0u;
    out->gpu_replay_dir = NULL;
    out->gpu_replay_dump = NULL;
    out->gpu_replay_width = 0u;
    out->gpu_replay_height = 0u;
    out->gpu_replay_lenient = false;
    out->gpu_replay_flip_y = false;
    out->gpu_replay_undo_viewport = false;
    out->gpu_replay_window_to_clip = false;
    out->gpu_replay_assume_program_mode = false;
    out->gpu_replay_viewport_from_target = false;
    out->gpu_replay_output_state = false;
    out->gpu_replay_combiner = false;
    out->gpu_replay_standin = false;
    out->gpu_replay_standin_texel_units = false;
    out->gpu_replay_standin_unit_rule_count = 0u;
    out->gpu_replay_draw_dump = NULL;
    out->gpu_replay_rt_texture = false;
    out->gpu_replay_rt_texture_census = false;
    out->gpu_live = false;
    out->gpu_live_inferred = false;
    out->gpu_live_blit = false;
    out->live_readback = false;
    out->gp_effects = false; /* T1267: was never defaulted, stack garbage failed the explicit opt-in test */
    out->live_blit_verify = 0u;
    out->gpu_live_translate = false;
    out->gpu_replay_surface_source = false;
    out->gpu_replay_target_persist = false;
    out->gpu_replay_standin_pattern = GPU_STANDIN_SOLID;
    out->gpu_replay_standin_width = 0u;
    out->gpu_replay_standin_height = 0u;
    memset(out->gpu_replay_standin_rgba, 0, sizeof out->gpu_replay_standin_rgba);
    out->gpu_replay_dump_every = 0u;
    out->gpu_replay_dump_last = false;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--continue-on-missing") == 0) {
            out->continue_on_missing = true;
        } else if (strcmp(arg, "--ac97-ready") == 0) {
            out->ac97_ready = true;
            out->ac97_ready_flag_given = true; /* deprecated alias, announced by main */
        } else if (strcmp(arg, "--no-ac97-ready") == 0) {
            out->ac97_ready = false;
        } else if (strcmp(arg, "--headless-audio") == 0) {
            /* T156 convenience only: exactly the four audio policies below, spelled
             * individually. Effects is already the default, vblank policies are timing
             * and stay separate. Sets no field of its own. */
            out->ac97_ready = true;
            out->headless_effects = true;
            out->headless_streams = true;
            out->headless_buffers = true;
            out->headless_listener = true;
        } else if (strcmp(arg, "--headless-effects") == 0) {
            out->headless_effects = true;
        } else if (strcmp(arg, "--gp-effects") == 0) {
            out->gp_effects = true;
        } else if (strcmp(arg, "--headless-streams") == 0) {
            out->headless_streams = true;
        } else if (strcmp(arg, "--headless-buffers") == 0) {
            out->headless_buffers = true;
        } else if (strcmp(arg, "--headless-second-vblank") == 0) {
            out->headless_second_vblank = true;
        } else if (strcmp(arg, "--vblank-dispatch-level") == 0) {
            out->vblank_dispatch_level = true;
        } else if (strcmp(arg, "--trace-vblank-readers") == 0) {
            out->trace_vblank_readers = true;
        } else if (strcmp(arg, "--check-vblank-quiescence") == 0) {
            out->check_vblank_quiescence = true;
        } else if (strcmp(arg, "--interactive") == 0) {
            out->interactive = true;
        } else if (strcmp(arg, "--vblank-owner-waits") == 0) {
            if (++i >= argc || !parse_blank_budget(argv[i], &out->vblank_owner_waits)) {
                return false;
            }
        } else if (strcmp(arg, "--vblank-worker-blanks") == 0) {
            if (++i >= argc || !parse_blank_budget(argv[i], &out->vblank_worker_blanks)) {
                return false;
            }
        } else if (strcmp(arg, "--couple-vblank-effects") == 0) {
            out->couple_vblank_effects = true;
        } else if (strcmp(arg, "--model-flips") == 0) {
            out->model_flips = true;
        } else if (strcmp(arg, "--trace-vblank-schedule") == 0) {
            out->trace_vblank_schedule = true;
        } else if (strcmp(arg, "--headless-first-vblank") == 0) {
            out->headless_first_vblank = true;
        } else if (strcmp(arg, "--native-shader-assembler") == 0) {
            out->native_shader_assembler = true;
        } else if (strcmp(arg, "--native-xmv") == 0) {
            out->native_xmv = true;
            out->native_xmv_explicit = true;
        } else if (strcmp(arg, "--no-native-xmv") == 0) {
            out->native_xmv_off = true;
        } else if (strcmp(arg, "--xonline-offline") == 0) {
            out->xonline_offline = true;
        } else if (strcmp(arg, "--xnet-scheduler") == 0) {
            out->xnet_scheduler = true;
        } else if (strcmp(arg, "--xnet-local-entropy") == 0) {
            out->xnet_local_entropy = true;
        } else if (strcmp(arg, "--skip-intro") == 0) {
            out->skip_intro = true;
        } else if (strcmp(arg, "--trace-xmv") == 0) {
            out->trace_xmv = true;
        } else if (strcmp(arg, "--dump-xmv-frames") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->dump_xmv_frames = argv[i];
        } else if (strcmp(arg, "--dump-xmv-frames-max") == 0) {
            if (++i >= argc) {
                return false;
            }
            char *end = NULL;
            const unsigned long long limit = strtoull(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || limit > 1000000u || argv[i][0] < '0' || argv[i][0] > '9') {
                return false;
            }
            out->dump_xmv_frames_max = (uint32_t)limit;
        } else if (strcmp(arg, "--capture-xmv-entries") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->capture_xmv_entries = argv[i];
        } else if (strcmp(arg, "--capture-xmv-entry-at") == 0) {
            if (++i >= argc || !parse_entry_ranges(argv[i], out)) {
                return false;
            }
        } else if (strcmp(arg, "--seed-xmv-entry") == 0) {
            const char *rest = NULL;
            if (++i >= argc || !parse_entry_number(argv[i], &out->seed_xmv_entry, &rest) || *rest != '\0') {
                return false;
            }
            out->seed_xmv_entry_set = true;
        } else if (strcmp(arg, "--seed-xmv-result") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->seed_xmv_result = argv[i];
        } else if (strcmp(arg, "--seed-xmv-patch") == 0) {
            if (++i >= argc || out->seed_xmv_patch_count == 32u || !xmv_seed_patch_valid(argv[i])) {
                return false;
            }
            out->seed_xmv_patch[out->seed_xmv_patch_count++] = argv[i];
        } else if (strcmp(arg, "--xmv-substitute") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->xmv_substitute = argv[i];
        } else if (strcmp(arg, "--eeprom") == 0) {
            if (++i >= argc || out->eeprom_path || !argv[i][0] || argv[i][0] == '-') return false;
            out->eeprom_path = argv[i];
        } else if (strcmp(arg, "--eeprom-key") == 0) {
            if (++i >= argc || out->eeprom_key_path || !argv[i][0] || argv[i][0] == '-') return false;
            out->eeprom_key_path = argv[i];
        } else if (strcmp(arg, "--eeprom-language") == 0) {
            if (++i >= argc) {
                return false;
            }
            const long value = strtol(argv[i], NULL, 10);
            if (value < 1 || value > 9) {
                return false;
            }
            out->eeprom_language = (unsigned)value;
        } else if (strcmp(arg, "--overlay-consume") == 0) {
            out->overlay_consume = true;
        } else if (strcmp(arg, "--dump-overlay") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->dump_overlay = argv[i];
        } else if (strcmp(arg, "--dump-overlay-max") == 0) {
            if (++i >= argc) {
                return false;
            }
            char *end = NULL;
            const unsigned long long limit = strtoull(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || argv[i][0] < '0' || argv[i][0] > '9' || limit > 1000000u) {
                return false;
            }
            out->dump_overlay_max = (uint32_t)limit;
        } else if (strcmp(arg, "--overlay-xemu-image") == 0) {
            out->overlay_xemu_image = true;
        } else if (strcmp(arg, "--overlay-xemu-key") == 0) {
            out->overlay_xemu_key = true;
        } else if (strcmp(arg, "--present") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->present = argv[i];
        } else if (strcmp(arg, "--present-capture") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->present_capture = argv[i];
        } else if (strcmp(arg, "--present-timeline") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->present_timeline = argv[i];
        } else if (strcmp(arg, "--cpu-profile") == 0 || strcmp(arg, "--cpu-profile-wall") == 0) {
            out->cpu_profile_wall = strcmp(arg, "--cpu-profile-wall") == 0;
            if (++i >= argc || argv[i][0] == '\0') {
                return false;
            }
            out->cpu_profile = argv[i];
        } else if (strcmp(arg, "--present-no-pace") == 0) {
            out->present_no_pace = true;
        } else if (strcmp(arg, "--present-hold-ms") == 0) {
            if (++i >= argc) {
                return false;
            }
            char *end = NULL;
            const unsigned long long hold = strtoull(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || argv[i][0] < '0' || argv[i][0] > '9' || hold > 86400000u) {
                return false;
            }
            out->present_hold_ms = (uint32_t)hold;
        } else if (strcmp(arg, "--live-pipeline-cache") == 0) {
            if (++i >= argc) {
                return false;
            }
            if (strcmp(argv[i], "none") == 0) {
                out->live_pipeline_cache_off = true;
            } else {
                out->live_pipeline_cache = argv[i];
            }
        } else if (strcmp(arg, "--live-frame-hash") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->live_frame_hash = argv[i];
        } else if (strcmp(arg, "--live-readback") == 0) {
            out->live_readback = true;
        } else if (strcmp(arg, "--live-blit-verify") == 0) {
            if (++i >= argc) {
                return false;
            }
            char *end = NULL;
            const unsigned long long every = strtoull(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || argv[i][0] < '1' || argv[i][0] > '9' || every > 1000000u) {
                return false;
            }
            out->live_blit_verify = (uint32_t)every;
        } else if (strcmp(arg, "--live-present-sync") == 0) {
            out->live_present_sync = true;
        } else if (strcmp(arg, "--live-pipeline") == 0) {
            if (++i >= argc) {
                return false;
            }
            char *end = NULL;
            const unsigned long long depth = strtoull(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || argv[i][0] < '0' || argv[i][0] > '9' || depth > 1u) {
                return false;
            }
            out->live_pipeline = (uint32_t)depth;
            out->live_pipeline_set = true;
        } else if (strcmp(arg, "--audio-latency-ms") == 0) {
            if (++i >= argc) {
                return false;
            }
            char *end = NULL;
            const unsigned long long latency = strtoull(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || argv[i][0] < '0' || argv[i][0] > '9' || latency > 2000u) {
                return false;
            }
            out->audio_latency_ms = (uint32_t)latency;
            out->audio_latency_set = true;
        } else if (strcmp(arg, "--audio-min-rate") == 0) {
            if (++i >= argc) {
                return false;
            }
            char *end = NULL;
            const unsigned long long permille = strtoull(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || argv[i][0] < '0' || argv[i][0] > '9' || permille > 1000u) {
                return false;
            }
            out->audio_min_rate_permille = (uint32_t)permille;
        } else if (strcmp(arg, "--av-sync-offset-ms") == 0) {
            if (++i >= argc) {
                return false;
            }
            char *end = NULL;
            const long offset = strtol(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || offset < -500 || offset > 500) {
                return false;
            }
            out->av_sync_offset_ms = (int32_t)offset;
        } else if (strcmp(arg, "--audio-stretch-legacy") == 0) {
            out->audio_stretch_legacy = true;
        } else if (strcmp(arg, "--no-audio-pump") == 0) {
            out->audio_pump = false;
        } else if (strcmp(arg, "--audio-hold-ms") == 0) {
            if (++i >= argc) {
                return false;
            }
            char *end = NULL;
            const unsigned long long hold = strtoull(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || argv[i][0] < '0' || argv[i][0] > '9' || hold > 2000u) {
                return false;
            }
            out->audio_hold_ms = (uint32_t)hold;
        } else if (strcmp(arg, "--audio-stretch") == 0) {
            if (++i >= argc) {
                return false;
            }
            if (strcmp(argv[i], "wsola") == 0) {
                out->audio_stretch_resample = false;
            } else if (strcmp(argv[i], "resample") == 0) {
                out->audio_stretch_resample = true;
            } else {
                return false;
            }
        } else if (strcmp(arg, "--audio-sink") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->audio_sink = argv[i];
        } else if (strcmp(arg, "--audio-output") == 0) {
            if (++i >= argc || argv[i][0] == '\0') {
                return false;
            }
            out->audio_output = argv[i];
        } else if (strcmp(arg, "--audio-mute") == 0) {
            out->audio_mute = true;
        } else if (strcmp(arg, "--headless-movie-audio") == 0) {
            out->headless_movie_audio = true;
        } else if (strcmp(arg, "--vblank-poll-blank") == 0) {
            out->vblank_poll_blank = true;
        } else if (strcmp(arg, "--passive-audio-completion") == 0) {
            out->passive_audio_completion = true;
        } else if (strcmp(arg, "--passive-audio-completion-at-dowork") == 0) {
            out->passive_audio_completion_at_dowork = true;
        } else if (strcmp(arg, "--async-file-io") == 0) {
            out->async_file_io = true;
        } else if (strcmp(arg, "--async-file-io-file-object") == 0) {
            out->async_file_io_file_object = true;
        } else if (strcmp(arg, "--async-file-io-spin-complete") == 0) {
            if (++i >= argc) {
                return false;
            }
            char *end = NULL;
            const unsigned long threshold = strtoul(argv[i], &end, 0);
            if (end == argv[i] || *end != '\0' || threshold == 0ul || threshold > 100000000ul) {
                return false;
            }
            out->async_file_io_spin = (unsigned)threshold;
        } else if (strcmp(arg, "--no-guest-frame-trace") == 0) {
            out->guest_frame_trace_off = true;
        } else if (strcmp(arg, "--no-vblank-poll-blank") == 0) {
            out->vblank_poll_blank_off = true;
        } else if (strcmp(arg, "--no-passive-audio-completion-at-dowork") == 0) {
            out->passive_audio_completion_at_dowork_off = true;
        } else if (strcmp(arg, "--no-passive-audio-completion") == 0) {
            out->passive_audio_completion_off = true;
        } else if (strcmp(arg, "--no-async-file-io") == 0) {
            out->async_file_io_off = true;
        } else if (strcmp(arg, "--synthetic-pad") == 0) {
            out->synthetic_pad = true;
        } else if (strcmp(arg, "--synthetic-pad-remove-after-polls") == 0) {
            if (++i >= argc) {
                return false;
            }
            char *end = NULL;
            const unsigned long long polls = strtoull(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || polls == 0u || polls > UINT32_MAX ||
                argv[i][0] < '0' || argv[i][0] > '9') {
                return false;
            }
            out->synthetic_pad_remove_after_polls = (uint32_t)polls;
        } else if (strcmp(arg, "--pad-script") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->pad_script = argv[i];
            out->pad_script_live = false;
        } else if (strcmp(arg, "--pad-script-live") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->pad_script = argv[i];
            out->pad_script_live = true;
        } else if (strcmp(arg, "--record-input") == 0) {
            if (++i >= argc) return false;
            out->record_input = argv[i];
        } else if (strcmp(arg, "--replay-input") == 0) {
            if (++i >= argc) return false;
            out->replay_input = argv[i];
        } else if (strcmp(arg, "--replay-handover") == 0) {
            out->replay_handover = true;
        } else if (strcmp(arg, "--route-wait") == 0) {
            xinput_route_wait wait;
            char spec_error[160];
            if (++i >= argc || out->route_wait_count >= 8u || !xinput_route_wait_parse(argv[i], &wait, spec_error, sizeof spec_error)) return false;
            out->route_waits[out->route_wait_count++] = argv[i];
        } else if (strcmp(arg, "--route-wait-event") == 0) {
            xinput_route_wait wait;
            char spec_error[200];
            if (++i >= argc || out->route_event_wait_count >= 8u || !xinput_route_event_wait_parse(argv[i], &wait, spec_error, sizeof spec_error)) return false;
            out->route_event_waits[out->route_event_wait_count++] = argv[i];
        } else if (strcmp(arg, "--route-event-log") == 0) {
            if (++i >= argc || argv[i][0] == '\0') return false;
            out->route_event_log = argv[i];
        } else if (strcmp(arg, "--route-log-mem") == 0) {
            xinput_route_wait wait;
            char spec_error[200], probe_spec[96];
            if (++i >= argc || out->route_log_mem_count >= 8u || strlen(argv[i]) > 40u) return false;
            snprintf(probe_spec, sizeof probe_spec, "mark1:mem=%s==0", argv[i]);
            if (!xinput_route_event_wait_parse(probe_spec, &wait, spec_error, sizeof spec_error) || wait.cond_count != 1u) return false;
            out->route_log_mem[out->route_log_mem_count++] = argv[i];
        } else if (strcmp(arg, "--route-nav") == 0) {
            route_nav_mode mode;
            if (++i >= argc || !route_nav_mode_parse(argv[i], &mode)) return false;
            out->route_nav = argv[i];
        } else if (strcmp(arg, "--route-nav-menus") == 0) {
            if (++i >= argc || argv[i][0] == '\0') return false;
            out->route_nav_menus = argv[i];
        } else if (strcmp(arg, "--route-nav-record") == 0) {
            if (++i >= argc || (strcmp(argv[i], "on") != 0 && strcmp(argv[i], "off") != 0)) return false;
            out->route_nav_record = argv[i];
        } else if (strcmp(arg, "--route-nav-timing") == 0) {
            route_nav_timing timing;
            char spec_error[160];
            if (++i >= argc || !route_nav_timing_parse(argv[i], &timing, spec_error, sizeof spec_error)) return false;
            out->route_nav_timing = argv[i];
        } else if (strcmp(arg, "--poke-at-poll") == 0) {
            poke_trigger_entry entry;
            char spec_error[160];
            if (++i >= argc || out->poke_at_count >= POKE_TRIGGER_MAX || !poke_trigger_parse(argv[i], &entry, spec_error, sizeof spec_error)) return false;
            out->poke_at[out->poke_at_count++] = argv[i];
        } else if (strcmp(arg, "--hotkey") == 0) {
            xinput_hotkey_spec hotkey;
            char spec_error[160];
            if (++i >= argc || out->hotkey_count >= XINPUT_HOTKEY_MAX || !xinput_hotkey_parse(argv[i], &hotkey, spec_error, sizeof spec_error)) return false;
            out->hotkeys[out->hotkey_count++] = argv[i];
        } else if (strcmp(arg, "--hotkey-dir") == 0) {
            if (++i >= argc || argv[i][0] == '\0') return false;
            out->hotkey_dir = argv[i];
        } else if (strcmp(arg, "--shot-dir") == 0) {
            if (++i >= argc || argv[i][0] == '\0') return false;
            out->shot_dir = argv[i];
        } else if (strcmp(arg, "--shot-max") == 0) {
            if (++i >= argc) return false;
            char *end = NULL;
            const unsigned long long value = strtoull(argv[i], &end, 10);
            if (argv[i][0] == '-' || argv[i][0] == '\0' || end == NULL || *end != '\0' || value == 0ull || value > HOST_OPTIONS_SHOT_MAX_CAP) return false;
            out->shot_max = (unsigned)value;
        } else if (strcmp(arg, "--shot-max-bytes") == 0) {
            if (++i >= argc) return false;
            char *end = NULL;
            const unsigned long long value = strtoull(argv[i], &end, 10);
            if (argv[i][0] == '-' || argv[i][0] == '\0' || end == NULL || *end != '\0' || value == 0ull) return false;
            out->shot_max_bytes = (uint64_t)value;
        } else if (strcmp(arg, "--snapshot-at-poll") == 0) {
            if (++i >= argc) return false;
            char *end = NULL;
            const unsigned long long value = strtoull(argv[i], &end, 10);
            if (argv[i][0] == '\0' || end == NULL || *end != '\0' || value == 0ull) return false;
            out->snapshot_at_poll = (uint64_t)value;
        } else if (strcmp(arg, "--stop-at-poll") == 0) {
            if (++i >= argc) return false;
            char *end = NULL;
            const unsigned long long value = strtoull(argv[i], &end, 10);
            if (argv[i][0] == '\0' || end == NULL || *end != '\0' || value == 0ull) return false;
            out->stop_at_poll = (uint64_t)value;
        } else if (strcmp(arg, "--mu-image") == 0) {
            if (i + 3 >= argc || out->mu_image_count >= MU_STARTUP_MAX) return false;
            const char *port_text = argv[++i];
            const char *slot_text = argv[++i];
            const char *path = argv[++i];
            if (port_text[0] < '0' || port_text[0] > '3' || port_text[1] != '\0' ||
                slot_text[0] < '0' || slot_text[0] > '1' || slot_text[1] != '\0' || path[0] == '\0') return false;
            const unsigned port = (unsigned)(port_text[0] - '0'), slot = (unsigned)(slot_text[0] - '0');
            for (unsigned j = 0u; j < out->mu_image_count; j++)
                if (out->mu_images[j].port == port && out->mu_images[j].slot == slot) return false;
            out->mu_images[out->mu_image_count++] = (mu_startup_image){port, slot, path};
        } else if (strcmp(arg, "--controllers") == 0) {
            out->controllers = true;
        } else if (strcmp(arg, "--controller-config") == 0) {
            if (++i >= argc) return false;
            out->controller_config = argv[i];
        } else if (strcmp(arg, "--controller-mappings") == 0) {
            if (++i >= argc) return false;
            out->controller_mappings = argv[i];
        } else if (strcmp(arg, "--controller-save") == 0) {
            if (++i >= argc) return false;
            out->controller_save = argv[i];
        } else if (strcmp(arg, "--pad-source") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->pad_source = argv[i];
        } else if (strcmp(arg, "--pad-feed") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->pad_feed = argv[i];
        } else if (strcmp(arg, "--headless-listener") == 0) {
            out->headless_listener = true;
        } else if (strcmp(arg, "--disc") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->disc_path = argv[i];
        } else if (strcmp(arg, "--disc-device") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->disc_device = argv[i];
        } else if (strcmp(arg, "--hdd") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->hdd_path = argv[i];
        } else if (strcmp(arg, "--disk-identity-xemu") == 0) {
            out->disk_identity_xemu = true;
        } else if (strcmp(arg, "--cache-partitions") == 0) {
            if (++i >= argc) {
                return false;
            }
            long value = strtol(argv[i], NULL, 10);
            if (value < 0 || value > (long)MAX_CACHE_PARTITIONS) {
                return false;
            }
            out->cache_partitions = (unsigned)value;
        } else if (strcmp(arg, "--av-pack") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->av_pack = argv[i];
        } else if (strcmp(arg, "--gpu-replay") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->gpu_replay_dir = argv[i];
        } else if (strcmp(arg, "--gpu-replay-dump") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->gpu_replay_dump = argv[i];
        } else if (strcmp(arg, "--gpu-replay-size") == 0) {
            if (++i >= argc) {
                return false;
            }
            char *end = NULL;
            const long width = strtol(argv[i], &end, 10);
            if (end == argv[i] || *end != 'x' || width <= 0 || width > (long)GPU_REPLAY_MAX_EDGE) {
                return false;
            }
            char *end_height = NULL;
            const long height = strtol(end + 1, &end_height, 10);
            if (end_height == end + 1 || *end_height != '\0' || height <= 0 ||
                height > (long)GPU_REPLAY_MAX_EDGE) {
                return false;
            }
            out->gpu_replay_width = (unsigned)width;
            out->gpu_replay_height = (unsigned)height;
        } else if (strcmp(arg, "--gpu-replay-lenient") == 0) {
            out->gpu_replay_lenient = true;
        } else if (strcmp(arg, "--gpu-replay-flip-y") == 0) {
            out->gpu_replay_flip_y = true;
        } else if (strcmp(arg, "--gpu-replay-undo-viewport") == 0) {
            out->gpu_replay_undo_viewport = true;
        } else if (strcmp(arg, "--gpu-replay-window-to-clip") == 0) {
            out->gpu_replay_window_to_clip = true;
        } else if (strcmp(arg, "--gpu-replay-assume-program-mode") == 0) {
            out->gpu_replay_assume_program_mode = true;
        } else if (strcmp(arg, "--gpu-replay-viewport-from-target") == 0) {
            out->gpu_replay_viewport_from_target = true;
        } else if (strcmp(arg, "--gpu-replay-output-state") == 0) {
            out->gpu_replay_output_state = true;
        } else if (strcmp(arg, "--gpu-replay-combiner") == 0) {
            out->gpu_replay_combiner = true;
        } else if (strcmp(arg, "--gpu-live") == 0) {
            out->gpu_live = true;
            out->gpu_replay_output_state = true;
            out->gpu_replay_combiner = true;
        } else if (strcmp(arg, "--gpu-live-inferred") == 0) {
            out->gpu_live_inferred = true;
        } else if (strcmp(arg, "--gpu-live-blit") == 0) {
            out->gpu_live_blit = true;
        } else if (strcmp(arg, "--gpu-live-translate") == 0) {
            out->gpu_live_translate = true;
        } else if (strcmp(arg, "--gpu-replay-rt-texture") == 0) {
            out->gpu_replay_rt_texture = true;
        } else if (strcmp(arg, "--gpu-replay-rt-texture-census") == 0) {
            out->gpu_replay_rt_texture = true;
            out->gpu_replay_rt_texture_census = true;
        } else if (strcmp(arg, "--gpu-replay-surface-source") == 0) {
            out->gpu_replay_surface_source = true;
        } else if (strcmp(arg, "--gpu-replay-target-persist") == 0) {
            out->gpu_replay_target_persist = true;
        } else if (strcmp(arg, "--gpu-replay-standin-texel-units") == 0) {
            out->gpu_replay_standin_texel_units = true;
        } else if (strcmp(arg, "--gpu-replay-standin-texel-program") == 0 ||
                   strcmp(arg, "--gpu-replay-standin-normalised-program") == 0) {
            const bool texel = strcmp(arg, "--gpu-replay-standin-texel-program") == 0;
            if (++i >= argc ||
                !gpu_standin_unit_rule_add(out->gpu_replay_standin_unit_rules, &out->gpu_replay_standin_unit_rule_count, argv[i],
                                           texel)) {
                return false;
            }
        } else if (strcmp(arg, "--gpu-replay-draw-dump") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->gpu_replay_draw_dump = argv[i];
        } else if (strcmp(arg, "--gpu-replay-standin-texture") == 0) {
            if (++i >= argc || !parse_standin(argv[i], out)) {
                return false;
            }
        } else if (strcmp(arg, "--gpu-replay-dump-last") == 0) {
            out->gpu_replay_dump_last = true;
        } else if (strcmp(arg, "--gpu-replay-dump-every") == 0) {
            if (++i >= argc) {
                return false;
            }
            /* a positive whole number that fits, nothing after it (strtoull takes a sign and spaces, so check) */
            char *end = NULL;
            const unsigned long long every = strtoull(argv[i], &end, 10);
            if (end == argv[i] || *end != '\0' || every == 0u || every > UINT32_MAX ||
                argv[i][0] < '0' || argv[i][0] > '9') {
                return false;
            }
            out->gpu_replay_dump_every = (unsigned)every;
        } else if (strcmp(arg, "--hdd-device") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->hdd_device = argv[i];
        } else if (strcmp(arg, "--open-missing-as-empty") == 0) {
            out->open_missing_as_empty = true;
        } else if (strcmp(arg, "--mount") == 0) {
            if (++i >= argc || out->mount_count >= OPTION_MOUNT_MAX) {
                return false;
            }
            out->mounts[out->mount_count++] = argv[i];
        } else if (strcmp(arg, "--stub-status") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->stub_status = (uint32_t)strtoul(argv[i], NULL, 0);
            out->stub_status_set = true;
        } else if (strcmp(arg, "--thread-timeout") == 0) {
            if (++i >= argc) {
                return false;
            }
            long value = strtol(argv[i], NULL, 10);
            if (value < 0) {
                return false;
            }
            out->thread_timeout_ms = (unsigned)value;
            out->thread_timeout_given = true;
        } else if (strcmp(arg, "--profile-calls") == 0) {
            out->profile_calls = true;
        } else if (strcmp(arg, "--profile-calls-top") == 0) {
            if (++i >= argc) {
                return false;
            }
            const long value = strtol(argv[i], NULL, 10);
            if (value <= 0) {
                return false;
            }
            out->profile_calls_top = (unsigned)value;
        } else if (strcmp(arg, "--profile-watch") == 0) {
            if (++i >= argc || out->profile_watch_count >= 8u) {
                return false;
            }
            char *end = NULL;
            const unsigned long address = strtoul(argv[i], &end, 0);
            if (end == argv[i] || *end != '\0' || address > UINT32_MAX) {
                return false;
            }
            out->profile_watch[out->profile_watch_count++] = (uint32_t)address;
        } else if (strcmp(arg, "--dump-guest-range") == 0) {
            if (++i >= argc || !guest_dump_parse_append(&out->dump_guest_set, argv[i], NULL, 0u)) {
                return false;
            }
        } else if (strcmp(arg, "--voice-log") == 0) {
            if (++i >= argc || argv[i][0] == '\0') return false;
            out->voice_log = argv[i];
        } else if (strcmp(arg, "--watch-write") == 0) {
            if (++i >= argc || !guest_dump_parse_append(&out->watch_write_set, argv[i], NULL, 0u) ||
                !guest_watch_check(&out->watch_write_set, NULL, 0u)) {
                return false;
            }
        } else if (strcmp(arg, "--watch-write-log") == 0) {
            if (++i >= argc || argv[i][0] == '\0') {
                return false;
            }
            out->watch_write_log = argv[i];
        } else if (strcmp(arg, "--watch-write-max") == 0) {
            unsigned long long value = 0u;
            if (++i >= argc || !parse_bounded(argv[i], 10, 1u, GUEST_WATCH_HARD_RECORDS, &value)) {
                return false;
            }
            out->watch_write_max = (unsigned)value;
        } else if (strcmp(arg, "--dump-guest-dir") == 0) {
            if (++i >= argc || argv[i][0] == '\0') {
                return false;
            }
            out->dump_guest_dir = argv[i];
        } else if (strcmp(arg, "--forced-state") == 0) {
            out->forced_state = true;
        } else if (strcmp(arg, "--dump-guest-max-bytes") == 0) {
            if (++i >= argc || argv[i][0] < '0' || argv[i][0] > '9') {
                return false;
            }
            char *end = NULL;
            const unsigned long long limit = strtoull(argv[i], &end, 0);
            if (*end != '\0' || limit == 0u || limit > GUEST_DUMP_HARD_MAX_BYTES) {
                return false;
            }
            out->dump_guest_max_bytes = limit;
        } else if (strcmp(arg, "--dump-on-button") == 0) {
            out->dump_on_button = true;
        } else if (strcmp(arg, "--dump-after-frames") == 0) {
            if (++i >= argc || !parse_after_frames(argv[i], out)) {
                return false;
            }
            out->dump_button_tuned = true;
        } else if (strcmp(arg, "--dump-button-threshold") == 0) {
            unsigned long long value = 0u;
            if (++i >= argc || !parse_bounded(argv[i], 10, 1u, 254u, &value)) {
                return false;
            }
            out->dump_button_threshold = (unsigned)value;
            out->dump_button_tuned = true;
        } else if (strcmp(arg, "--dump-button-coalesce") == 0) {
            unsigned long long value = 0u;
            if (++i >= argc || !parse_bounded(argv[i], 10, 0u, HOST_BUTTON_DUMP_MAX_COALESCE, &value)) {
                return false;
            }
            out->dump_button_coalesce = (unsigned)value;
            out->dump_button_tuned = true;
        } else if (strcmp(arg, "--dump-button-max-pending") == 0) {
            unsigned long long value = 0u;
            if (++i >= argc || !parse_bounded(argv[i], 10, 1u, HOST_BUTTON_DUMP_MAX_PENDING, &value)) {
                return false;
            }
            out->dump_button_max_pending = (unsigned)value;
            out->dump_button_tuned = true;
        } else if (strcmp(arg, "--dump-button-max-dumps") == 0) {
            unsigned long long value = 0u;
            if (++i >= argc || !parse_bounded(argv[i], 10, 1u, HOST_BUTTON_DUMP_MAX_DUMPS, &value)) {
                return false;
            }
            out->dump_button_max_dumps = (unsigned)value;
            out->dump_button_tuned = true;
        } else if (strcmp(arg, "--dump-button-max-bytes") == 0) {
            unsigned long long value = 0u;
            if (++i >= argc || !parse_bounded(argv[i], 0, 1u, UINT64_MAX, &value)) {
                return false;
            }
            out->dump_button_max_bytes = value;
            out->dump_button_tuned = true;
        } else if (strcmp(arg, "--dump-button-start-poll") == 0) {
            unsigned long long value = 0u;
            if (++i >= argc || !parse_bounded(argv[i], 10, 0u, UINT32_MAX, &value)) {
                return false;
            }
            out->dump_button_start_poll = value;
            out->dump_button_tuned = true;
        } else if (strcmp(arg, "--dump-button-after-replay") == 0) {
            out->dump_button_after_replay = true;
            out->dump_button_tuned = true;
        } else if (strcmp(arg, "--dump-button-idle-every") == 0) {
            unsigned long long value = 0u;
            if (++i >= argc || !parse_bounded(argv[i], 10, 0u, HOST_BUTTON_DUMP_MAX_IDLE_EVERY, &value)) {
                return false;
            }
            out->dump_button_idle_every = (unsigned)value;
            out->dump_button_tuned = true;
        } else if (strcmp(arg, "--dump-button-max-idle") == 0) {
            unsigned long long value = 0u;
            if (++i >= argc || !parse_bounded(argv[i], 10, 0u, HOST_BUTTON_DUMP_MAX_IDLE, &value)) {
                return false;
            }
            out->dump_button_max_idle = (unsigned)value;
            out->dump_button_tuned = true;
        } else if (strcmp(arg, "--census-icalls") == 0) {
            out->census_icalls = true;
        } else if (strcmp(arg, "--census-phases") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->census_phases = argv[i];
        } else if (strcmp(arg, "--census-window") == 0) {
            /* FIRST:LAST present counts, both inclusive. */
            if (++i >= argc) {
                return false;
            }
            char *middle = NULL;
            const unsigned long long first = strtoull(argv[i], &middle, 0);
            if (middle == argv[i] || *middle != ':') {
                return false;
            }
            char *end = NULL;
            const unsigned long long last = strtoull(middle + 1, &end, 0);
            if (end == middle + 1 || *end != '\0' || last < first) {
                return false;
            }
            out->census_window_set = true;
            out->census_window_first = first;
            out->census_window_last = last;
        } else if (strcmp(arg, "--stop-after-calls") == 0) {
            /* ADDRESS:COUNT for an XDK address, ord:ORDINAL:COUNT for a kernel ordinal. */
            if (++i >= argc) {
                return false;
            }
            const char *text = argv[i];
            bool ordinal = false;
            if (strncmp(text, "ord:", 4) == 0) {
                ordinal = true;
                text += 4;
            }
            char *end = NULL;
            const unsigned long id = strtoul(text, &end, 0);
            if (end == text || *end != ':' || id > UINT32_MAX) {
                return false;
            }
            char *end_count = NULL;
            const unsigned long long count = strtoull(end + 1, &end_count, 10);
            if (end_count == end + 1 || *end_count != '\0' || count == 0u) {
                return false;
            }
            out->stop_after_set = true;
            out->stop_after_ordinal = ordinal;
            out->stop_after_id = (uint32_t)id;
            out->stop_after_count = (uint64_t)count;
        } else if (strcmp(arg, "--trace") == 0) {
            if (++i >= argc) {
                return false;
            }
            long value = strtol(argv[i], NULL, 10);
            if (value <= 0) {
                return false;
            }
            out->trace_report = (unsigned)value;
        } else if (arg[0] == '-') {
            return false;
        } else if (!out->xbe_path) {
            out->xbe_path = arg;
        } else {
            return false;
        }
    }
    /* T762: announced defaults. An explicit --no-... wins over the flag and the default, a default only switches on
     * when the flag's own prerequisites already hold, so no existing command line becomes invalid. */
    if (out->passive_audio_completion_off) {
        out->passive_audio_completion = false;
    } else if (!out->passive_audio_completion && out->headless_streams && out->headless_buffers &&
               out->headless_listener) {
        out->passive_audio_completion = true;
        out->passive_audio_completion_by_default = true;
    }
    /* T871: the DoWork delivery is the default of the completion model (xemu-level evidence), --no-... opts out. */
    if (out->passive_audio_completion_at_dowork_off) {
        out->passive_audio_completion_at_dowork = false;
    } else if (out->passive_audio_completion && !out->passive_audio_completion_at_dowork) {
        out->passive_audio_completion_at_dowork = true;
        out->passive_audio_completion_at_dowork_by_default = true;
    }
    if (out->async_file_io_off) {
        out->async_file_io = false;
    } else if (!out->async_file_io) {
        out->async_file_io = true;
        out->async_file_io_by_default = true;
    }
    if (out->vblank_poll_blank_off) {
        out->vblank_poll_blank = false;
    } else if (!out->vblank_poll_blank && out->vblank_owner_waits != 0u) {
        out->vblank_poll_blank = true;
        out->vblank_poll_blank_by_default = true;
    }
    /* T1093: the retained original XMV exports are the default whenever a disc is mounted (the movie
     * is read from d:\xmv). --no-native-xmv restores the unserved exports. */
    if (out->native_xmv_off) {
        out->native_xmv = false;
    } else if (!out->native_xmv && out->disc_path != NULL) {
        out->native_xmv = true;
    }
    return out->xbe_path != NULL &&
        /* the stand-in only feeds the combiner, which only exists inside the replay (and needs --gpu-replay itself) */
        (!out->gpu_replay_standin || out->gpu_replay_combiner) &&
        (!out->gpu_replay_standin_texel_units || (out->gpu_replay_standin && out->gpu_replay_combiner)) &&
        /* T719: per-draw units need the stand-in and combiner and replace the per-boot switch, never combine with it */
        (out->gpu_replay_standin_unit_rule_count == 0u ||
         (out->gpu_replay_standin && out->gpu_replay_combiner && !out->gpu_replay_standin_texel_units)) &&
        /* T510: the render target texture feeds the combiner, checks the TEXTURE group words and cannot share stage 0 with a stand-in */
        (!out->gpu_replay_rt_texture ||
         (out->gpu_replay_combiner && out->gpu_replay_output_state && !out->gpu_replay_standin && !out->gpu_replay_flip_y &&
          out->gpu_replay_dir != NULL)) &&
        /* T838: the live renderer needs the window sink and replaces the replay's own image paths. Its need of the replay's module
         * directory is the clause below (it implies the output state and the combiner, which refuse without --gpu-replay) */
        (!out->gpu_live ||
         (out->present != NULL && strcmp(out->present, "window") == 0 && !out->gpu_replay_rt_texture &&
          !out->gpu_replay_standin && !out->gpu_replay_flip_y && out->gpu_replay_dump == NULL)) &&
        (!out->gpu_live_inferred || out->gpu_live) &&
        (!out->live_pipeline_set || out->gpu_live) &&
        (!out->live_present_sync || out->gpu_live) &&
        (out->live_frame_hash == NULL || out->gpu_live) &&
        ((out->live_pipeline_cache == NULL && !out->live_pipeline_cache_off) || out->gpu_live) &&
        (!out->gpu_live_blit || out->gpu_live) &&
        (!out->live_readback || out->gpu_live) &&
        (out->live_blit_verify == 0u || (out->gpu_live && !out->live_readback && out->live_frame_hash == NULL)) &&
        (!out->gpu_live_translate || out->gpu_live) &&
        /* T633: the surface source feeds the render target texture bridge, persistence cannot mirror a kept image twice */
        (!out->gpu_replay_surface_source || out->gpu_replay_rt_texture) &&
        (!out->gpu_replay_target_persist || (out->gpu_replay_dir != NULL && !out->gpu_replay_flip_y)) &&
        (out->gpu_replay_dir != NULL ||
         (out->gpu_replay_dump == NULL && out->gpu_replay_draw_dump == NULL && out->gpu_replay_width == 0u && !out->gpu_replay_lenient &&
          !out->gpu_replay_flip_y && !out->gpu_replay_undo_viewport && !out->gpu_replay_window_to_clip &&
          !out->gpu_replay_assume_program_mode && !out->gpu_replay_output_state &&
          !out->gpu_replay_combiner && !out->gpu_replay_viewport_from_target)) &&
        /* the cadence only chooses among the frames a dump directory would get (and a dump directory needs
         * --gpu-replay above, so the cadence needs it too) */
        (out->gpu_replay_dump != NULL ||
         (out->gpu_replay_dump_every == 0u && !out->gpu_replay_dump_last)) &&
        !(out->headless_first_vblank && out->headless_second_vblank) &&
        /* The cut counts through the profile, so it is refused without it. */
        (!out->stop_after_set || out->profile_calls) &&
        (out->profile_watch_count == 0u || out->profile_calls) &&
        /* T821: a window of ordered calls is part of the census. */
        (!out->census_window_set || out->census_icalls) &&
        (out->census_phases == NULL || out->census_icalls) &&
        /* T1599: a dump needs somewhere to go and must fit the byte bound (a directory alone dumps nothing). */
        (out->dump_guest_set.count == 0u
             ? out->dump_guest_dir == NULL
             : (out->dump_guest_dir != NULL && guest_dump_check_total(&out->dump_guest_set, out->dump_guest_max_bytes))) &&
        /* T1613: pokes ride on the phase dumps, so --forced-state needs them. */
        (!out->forced_state || out->dump_guest_set.count != 0u) &&
        /* T1629: the button dump rides on the same ranges and directory and needs a pad whose port 0 it can observe; its
         * tuning flags need it; after-replay needs the route handover; every admitted event must fit the dump cap. */
        (!out->dump_on_button ||
         (out->dump_guest_set.count != 0u && (out->synthetic_pad || out->controllers) &&
          out->dump_button_max_dumps >= 1u + out->dump_after_count &&
          (!out->dump_button_after_replay || (out->replay_input != NULL && out->replay_handover)))) &&
        (out->dump_on_button || !out->dump_button_tuned) &&
        (!out->model_flips || out->couple_vblank_effects) &&
        (!out->trace_vblank_schedule || out->headless_first_vblank || out->headless_second_vblank) &&
        (!out->vblank_dispatch_level || out->headless_first_vblank || out->headless_second_vblank) &&
        (!out->trace_vblank_readers || out->headless_first_vblank || out->headless_second_vblank) &&
        (!out->check_vblank_quiescence || out->headless_first_vblank || out->headless_second_vblank) &&
        /* T460: the owner-wait callbacks are built on the coupled count and the quiescence check. */
        (out->vblank_owner_waits == 0u ||
         (out->headless_second_vblank && out->couple_vblank_effects && out->check_vblank_quiescence)) &&
        /* T592: the worker's blanks are the owner-wait delivery with another deliverer. */
        (out->vblank_worker_blanks == 0u || out->vblank_owner_waits != 0u) &&
        (!out->interactive || (out->present != NULL && strcmp(out->present, "window") == 0 &&
                              !out->present_no_pace && out->vblank_owner_waits != 0u &&
                              out->vblank_worker_blanks != 0u && out->vblank_poll_blank)) &&
        /* T696: the poll's blank is the owner-wait delivery. */
        (!out->vblank_poll_blank || out->vblank_owner_waits != 0u) &&
        /* The retained decoder reads the title's own d:\xmv file, so a disc must be mounted. */
        (!out->native_xmv || out->disc_path != NULL) && !(out->native_xmv_off && out->native_xmv_explicit) && (!out->trace_xmv || out->native_xmv) &&
        (out->dump_xmv_frames == NULL || out->trace_xmv) &&
        /* T538: the entry capture needs the trace, the cooperative seam and both halves of the selection. */
        ((out->capture_xmv_entries == NULL) == (out->capture_xmv_range_count == 0u)) &&
        (out->capture_xmv_entries == NULL ||
         (out->trace_xmv && (out->headless_first_vblank || out->headless_second_vblank))) &&
        /* T611: the seeded run needs its entry and its result file together, plus the trace and the seam. */
        (out->seed_xmv_entry_set == (out->seed_xmv_result != NULL)) &&
        (out->seed_xmv_patch_count == 0u || out->seed_xmv_entry_set) &&
        (!out->seed_xmv_entry_set ||
         (out->trace_xmv && (out->headless_first_vblank || out->headless_second_vblank))) &&
        (!out->disk_identity_xemu || out->hdd_path != NULL) &&
        (out->eeprom_key_path == NULL || out->eeprom_path != NULL) &&
        (out->xmv_substitute == NULL || out->native_xmv) &&
        (!out->overlay_consume || (out->native_xmv && out->couple_vblank_effects)) &&
        (out->dump_overlay_max == 0u || out->dump_overlay != NULL) &&
        (out->present_hold_ms == 0u || out->present != NULL) &&
        (out->present_capture == NULL || out->present != NULL) &&
        (!out->present_no_pace || out->present != NULL) &&
        (out->audio_output == NULL ||
         (out->audio_sink != NULL && strcmp(out->audio_sink, "wav-file") == 0)) &&
        (!out->audio_mute || (out->audio_sink != NULL && strcmp(out->audio_sink, "sdl") == 0)) &&
        (!out->audio_latency_set || (out->audio_sink != NULL && strcmp(out->audio_sink, "sdl") == 0)) &&
        (!out->headless_movie_audio || (out->native_xmv && out->headless_streams)) &&
        (out->synthetic_pad_remove_after_polls == 0u || out->synthetic_pad) &&
        (out->pad_script == NULL || out->synthetic_pad || out->controllers) &&
        pad_source_options_valid(out) &&
        (out->record_input == NULL || (out->synthetic_pad && !out->controllers && !out->forced_state)) &&
        (out->replay_input == NULL ||
         (out->synthetic_pad && !out->controllers && out->pad_script == NULL && out->record_input == NULL &&
          (out->replay_handover ? (out->pad_source != NULL && strcmp(out->pad_source, "script") != 0)
                                : (out->pad_source == NULL && out->pad_feed == NULL)))) &&
        /* T1616: the route flags act on a replay; a poke trigger needs the guarded poke (forced state + dump). */
        (out->replay_input != NULL || (!out->replay_handover && out->route_wait_count == 0u && out->poke_at_count == 0u &&
                                       out->route_event_wait_count == 0u)) &&
        /* T1640: the nav flags act on a replay too. */
        (out->replay_input != NULL || (out->route_nav == NULL && out->route_nav_timing == NULL &&
                                       (out->route_nav_menus == NULL || out->record_input != NULL))) &&
        (out->route_nav_record == NULL || out->record_input != NULL) &&
        /* T1633: one wait per mark across --route-wait and --route-wait-event (<= 8 together), the sampled memory needs its log. */
        (out->route_wait_count + out->route_event_wait_count <= 8u) &&
        (out->route_log_mem_count == 0u || out->route_event_log != NULL) &&
        (out->poke_at_count == 0u || out->forced_state) &&
        /* T1627: hotkeys ride the window pad feed (keyboard or gamepad source, no fake-feed file) and need their directory. */
        (out->hotkey_count == 0u
             ? out->hotkey_dir == NULL
             : (out->hotkey_dir != NULL && out->pad_source != NULL &&
                (strcmp(out->pad_source, "keyboard") == 0 || strcmp(out->pad_source, "gamepad") == 0) &&
                out->pad_feed == NULL)) &&
        /* T1720b: the shot bounds and directory belong to the hotkey `shot` label, so they need --hotkey. */
        (out->hotkey_count != 0u ||
         (out->shot_dir == NULL && out->shot_max == HOST_OPTIONS_SHOT_MAX_DEFAULT &&
          out->shot_max_bytes == HOST_OPTIONS_SHOT_MAX_BYTES_DEFAULT)) &&
        ((out->controllers && out->present && strcmp(out->present, "window") == 0 &&
          out->pad_feed == NULL && (out->pad_source == NULL || strcmp(out->pad_source, "gamepad") != 0) &&
          out->synthetic_pad_remove_after_polls == 0) ||
         (!out->controllers && out->controller_config == NULL && out->controller_mappings == NULL && out->controller_save == NULL)) &&
        (!out->passive_audio_completion ||
         (out->headless_streams && out->headless_buffers && out->headless_listener)) &&
        /* T855: the DoWork delivery is a mode of the completion model, which must be on (a flag or the T762 default). */
        (!out->passive_audio_completion_at_dowork || out->passive_audio_completion) &&
        (!out->async_file_io_file_object || out->async_file_io) &&
        /* T821: the spin rule completes reads of the async model and runs from the cooperative provider. */
        (out->async_file_io_spin == 0u ||
         (out->async_file_io && (out->headless_first_vblank || out->headless_second_vblank))) &&
        (!out->headless_second_vblank ||
         (out->ac97_ready && out->headless_effects && out->headless_streams &&
          out->headless_buffers && out->headless_listener));
}

const char *host_options_route_replay_help(void)
{
    return "  Route replay (T1616, FABRICATED input and FABRICATED-STATE pokes, results INFERRED at most). Record once with\n"
           "  --record-input FILE (needs --synthetic-pad, refused with --forced-state): SIGUSR2 marks the record, `kill -USR2\n"
           "  $(cat FILE.pid)` appends the comment line `# mark: at=N` (N = polls recorded so far) at the next poll, up to 16\n"
           "  marks, to say a screen finished. Replay with --synthetic-pad --replay-input FILE (a plain replay refuses\n"
           "  --pad-source). The three flags below are refused without --replay-input:\n"
           "  --route-wait SPEC      T1616: hold the replay at a recorded mark until a condition holds. SPEC =\n"
           "                         markK:mem=ADDR[:W]==VALUE[&MASK],min=N,max=N, K = 1..16. mem= is a W byte guest memory\n"
           "                         value (W = 1, 2 or 4, default 4, aligned, ADDR 0..0x03FFFFFF), min= the polls to stall at\n"
           "                         least, max= the polls before the route FAILS and the host stops (default 36000). Give\n"
           "                         mem= or min=. Repeatable, at most 8, one per mark.\n"
           "                         E.g. --route-wait mark3:mem=0x52D3FC==6,max=36000\n"
           "  --poke-at-poll SPEC    T1616: serve the guarded poke request DIR/guestpoke.LABEL once at a route position, no\n"
           "                         signal needed. SPEC = WHERE:LABEL, WHERE = N (host poll N or the first later one), markK\n"
           "                         (after its --route-wait) or replay-end (record exhausted), LABEL = 1..63 of [A-Za-z0-9_-].\n"
           "                         Repeatable, at most 4. Needs --forced-state, --dump-guest-range and --dump-guest-dir\n"
           "                         (same guards and logs as the T1613 poke). N counts host polls, which shift when a wait\n"
           "                         stalls: use markK after a wait. E.g. --poke-at-poll mark3:batch1\n"
           "  --replay-handover      T1616: after the record ends the pad is the live keyboard or gamepad instead of rest.\n"
           "                         Needs --pad-source keyboard|gamepad (with --present window or --pad-feed FILE), which\n"
           "                         is then not refused. The live source is polled during the replay so no key is stuck at\n"
           "                         the handover.\n"
           "  Route flag identity (T1618): a replay is refused unless the record was made with the same game affecting flags.\n"
           "  The read-only observers --census-icalls, --census-phases and --census-window (and --cpu-profile) are not part of\n"
           "  that identity, --skip-intro is. The refusal prints only the flags that differ.\n";
}

const char *host_options_route_event_help(void)
{
    return "  Event driven route (T1633, observers only, they never change what the title sees). A recorded mark can wait for host\n"
           "  observed events instead of a poll count, so a slow profile or map load neither desyncs nor wastes time:\n"
           "  --route-wait-event SPEC  SPEC = markK:field,field,... (fields AND-ed, `any` makes them alternatives). Refused without\n"
           "                         --replay-input. One wait per mark across this flag, --route-wait and the record's `# wait:` lines\n"
           "                         (a command line wait replaces the record's). Fields:\n"
           "                           mem=[*]ADDR[+OFF][:W]OP VALUE[&MASK]  guest memory, OP == != < <= > >=, * = pointer at ADDR\n"
           "                           file-open=SUBSTR  file-read=SUBSTR[@BYTES]  file-idle=MS[@SUBSTR]  guest file I/O since the\n"
           "                             previous mark (SUBSTR of the guest path, case blind, `!SUBSTR` = files WITHOUT it, e.g.\n"
           "                             file-idle=500@u:\\ = quiet on the HDD saves although the disc streams; idle needs I/O first)\n"
           "                           call=VA[@COUNT]   the lifted code reached VA (needs --headless-second-vblank)\n"
           "                           frame-change  frame-stable=MS   the presented frame signature changed / stopped changing\n"
           "                           min=N min-ms=MS   stall at least that long;   max=N timeout=MS   FAIL after that (default\n"
           "                             timeout 600000 ms). A failure stops the host and prints what each condition saw.\n"
           "                         E.g. --route-wait-event mark2:file-open=anicemap.mkr,file-idle=800,timeout=180000\n"
           "  --route-event-log FILE  write a timestamped log of the file opens/reads, watched calls, memory samples and the route's\n"
           "                         own events (tools.route_events reads it). --route-log-mem SPEC  [*]ADDR[+OFF][:W], repeatable, at\n"
           "                         most 8, sampled every 20 ms into that log (changes only). Both work in any session.\n"
           "  A record may carry `# wait: markK:fields` lines (same fields) and `# mark-info: markK ...` facts the host wrote when\n"
           "  the mark was recorded; python -m tools.route_events inserts and proposes them.\n";
}

const char *host_options_route_nav_help(void)
{
    return "  Closed loop menu navigation (T1640, FABRICATED input, observers of the guest only: it reads menu variables, it writes none).\n"
           "  A record line `# nav: at=P to=Q menu=ID select=index:N|name:TEXT [activate] [expect=FIELDS] [timeout=MS] [retry=N]`\n"
           "  replaces the recorded presses P..Q (record poll positions, a `# mark:` may sit at P or Q, not between): when the replay\n"
           "  reaches P the pad is driven from the menu's cursor in guest memory (d-pad edges, 2 polls down, 3 up, lost presses are\n"
           "  retried, then A when `activate`) and the replay continues at Q. TEXT is percent encoded (%20 space), matched case blind.\n"
           "  The flags below are refused without --replay-input and are not part of the flag identity:\n"
           "  --route-nav MODE       on (default when --route-nav-menus is given): a step that cannot run (unknown menu, unreadable\n"
           "                         cursor) before its first press falls back to the recorded presses; a timeout, a lost press\n"
           "                         that does not come back, an ambiguous name or a failed expect= FAILS the route. off: the nav\n"
           "                         lines are ignored, the recorded presses replay open loop. strict: a step that cannot run fails.\n"
           "  --route-nav-menus FILE the menu table (tools/data/menu_nav.txt): `menu ID active=COND[,COND] cursor=MEM count=MEM\n"
           "                         axis=v|h wrap=0|1 [ready=COND[,COND]] [names=MEM/STRIDE/ascii|utf16|ptr-ascii|ptr-utf16/MAXLEN]\n"
           "                         [select=A|B|X|Y|BLACK|WHITE|START|BACK] [conf=TEXT]`, COND = mem= syntax of --route-wait-event.\n"
           "  --route-nav-timing H:G[:E] d-pad hold and release polls, E extra polls for the cursor to change (default 2:3:2).\n"
           "                         E.g. --route-nav on --route-nav-menus tools/data/menu_nav.txt\n"
           "  Recording (T1640): with --record-input FILE --route-nav-menus TABLE the host samples the menu table at every pad poll\n"
           "  (--route-nav-record on|off, default on with a table) and writes `# nav: at=P to=Q menu=ID select=id:I|index:C activate`\n"
           "  lines into the record for every select press (A) on a ready menu, on a row the cursor does not skip and that is not\n"
           "  greyed (at = the first d-pad press in this page, to = the first poll with A up). A press while not ready, on a greyed\n"
           "  row, or a mark strictly inside (P,Q) writes no line. B (back) is never emitted. With --route-event-log the changes of\n"
           "  (menu, cursor, count, item id, ready) are logged as `nav menu=ID cursor=C count=K id=I ready=R` (`nav menu=none`),\n"
           "  and `nav-line` / `nav-skip` lines say what was written or why not. Table keys: dpad=0 (the menu does not take the d-pad:\n"
           "  the navigator only waits for the cursor), selectby=id|name|index (what the recorder writes as select=).\n";
}

const char *host_options_hotkey_help(void)
{
    return "  Host hotkeys (T1627, T1632, T1720, tooling only, FABRICATED: nothing is sent to the game except that a chord is kept away from it):\n"
           "  --hotkey SPEC          a chord on the live gamepad or keyboard that writes DIR/hotkey.<n> ('<n> LABEL poll=P ms=M')\n"
           "                         so tools.action_profile --advance-file can continue a step without a window focus change.\n"
           "                         SPEC = DEVICE=KEY+KEY[+KEY..][@HOLDMS]:LABEL, DEVICE pad or kb, 2..5 keys, HOLDMS 0..10000\n"
           "                         (default 500 for pad, 0 for kb), LABEL 1..31 of [A-Za-z0-9_-]. pad keys: A B X Y LB RB START\n"
           "                         BACK GUIDE LSTICK RSTICK DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT. kb keys: A-Z 0-9 UP DOWN\n"
           "                         LEFT RIGHT ENTER BACKSPACE SPACE TAB F1 and CTRL SHIFT ALT (either side). Repeatable, at\n"
           "                         most 8. E.g. --hotkey pad=BACK+START+LB+RB@500:advance --hotkey kb=CTRL+SHIFT+N:advance\n"
           "                         The chord fires once per press. WHAT THE GAME SEES: the FIRST key of the spec is the\n"
           "                         LEAD, press it first (BACK, CTRL): it is forwarded, and while it is held the other chord keys\n"
           "                         (and their releases) are swallowed. Chord keys pressed with no lead held are ordinary game\n"
           "                         input (LB+RB or LB+X played normally still work). With --record-input a chord key that did\n"
           "                         reach the game is taken out of the recorded route. The keyboard chord only works while the\n"
           "                         game window has focus. Needs --pad-source keyboard|gamepad on the window (no --pad-feed).\n"
           "                         The pad and keyboard events are polled with the game's pad, so nothing fires while the game\n"
           "                         does not poll (a load, a stop).\n"
           "  Host actions by LABEL (case insensitive, the file is written as well): 'mark' places a record mark like\n"
           "                         SIGUSR2 ('# mark: at=N', needs --record-input, at most 16 marks), 'stop' ends the run cleanly\n"
           "                         like closing the window (the recording gets its trailer), 'shot' (T1720, needs --present window) writes\n"
           "                         DIR/shot-NNN.png of the presented frame and a line in DIR/shots.manifest ('NAME poll= present=\n"
           "                         phase= label= hotkey= size=', phase = first line of DIR/phase). Other labels only write the file.\n"
           "                         Default chords (pad, press BACK first, hold): BACK+START+LB+RB 0.5 s advance, +X 0.5 s mark,\n"
           "                         +B 0.5 s dump, +Y 1.5 s stop, +A 0.5 s shot. Keyboard: CTRL+SHIFT+N advance, M mark, D dump, O stop (1 s), S shot.\n"
           "  --hotkey-dir DIR       T1627: where hotkey.<n> is written (created if missing). Required with --hotkey.\n"
           "                         Not part of the route flag identity (T1618).\n"
           "  --shot-dir DIR         T1720b: where the 'shot' label writes shot-NNN.png (atomic: a temp file then rename, never overwrites\n"
           "                         a name that exists) and shots.manifest (default: --hotkey-dir, created if missing).\n"
           "  --shot-max N           T1720b: most pictures per run, 1..100000 (default 200). A press beyond it is refused with one stderr line.\n"
           "  --shot-max-bytes N     T1720b: most PNG bytes per run (default 536870912). A press whose estimated PNG would not fit is refused.\n"
           "                         The three shot flags need --hotkey. Manifest lines end 'size=WxH unix=EPOCH bytes=PNGBYTES'.\n";
}

const char *host_options_watch_write_help(void)
{
    return
          "  --watch-write SPEC     T1741, PASSIVE write watch: log every guest store into a range, with the host PC (named as the lifted\n"
          "                         sub_XXXXXXXX), the bytes before and after, the present and pad poll counts and the lifted callers\n"
          "                         found on the host stack. SPEC is the --dump-guest-range grammar (ADDR:LEN, *ADDR+OFF:LEN,\n"
          "                         **ADDR+OFF1+OFF2:LEN), repeatable, at most 4 ranges of at most 64 bytes. Pointer forms are re-resolved\n"
          "                         every 20 ms and re-armed (a null chain is unarmed). Works by page protection and a trap-flag\n"
          "                         single step: zero cost without the flag, with it every write to the watched 4 KiB PAGE costs two\n"
          "                         signals. Writes by another thread inside the one-instruction window are missed.\n"
          "  --watch-write-log FILE append the watch log to FILE (default stderr)\n"
          "  --watch-write-max N    keep at most N records, 1..65536 (default 4096), more are only counted in the summary line\n";
}

const char *host_options_button_dump_help(void)
{
    return
          "  --dump-on-button       T1629, READ-ONLY, passive: while the title runs, write a guest memory dump just BEFORE and just\n"
          "                         AFTER each digital button press or release of pad port 0 (UP DOWN LEFT RIGHT START BACK LTHUMB\n"
          "                         RTHUMB, and A B X Y BLACK WHITE LT RT with the title's own rule: pressure > 0x3C; sticks never).\n"
          "                         Same ranges and text format as --dump-guest-range, files DIR/buttons/guestdump.<NNNN>_<edge>_\n"
          "                         <buttons>_<before|afterK>, manifest DIR/buttons.jsonl. Needs --dump-guest-range, --dump-guest-dir\n"
          "                         and a pad source (--synthetic-pad or --controllers). The before dump is taken at the pad poll that\n"
          "                         first reports the input, after dump k at the k-th later poll (the title polls once per frame).\n"
          "                         The guest thread only captures bytes; one writer thread formats and writes atomically.\n"
          "                         Evidence: xemu-level MEASURED observation of the unmodified game. With --forced-state the manifest\n"
          "                         start line says forced_state:true (FABRICATED-STATE pokes ran). The tuning flags below are\n"
          "                         refused without it:\n"
          "  --dump-after-frames L  1 to 4 strictly increasing frame counts 1..3600 (default 2,30)\n"
          "  --dump-button-threshold N      analog button down when pressure > N, 1..254 (default 60 = the title's 0x3C)\n"
          "  --dump-button-coalesce N       an edge within N polls (0..60, default 3) of the first edge of the newest event joins it\n"
          "  --dump-button-max-pending N    events waiting for their after dumps, 1..64 (default 8), more edges are dropped\n"
          "  --dump-button-max-dumps N      total dump files 1..100000 (default 2000, at least 1 + the after count), then dropped\n"
          "  --dump-button-max-bytes N      total dump bytes (default 0x40000000), estimated per dump, then dropped\n"
          "  --dump-button-start-poll N     no events before port 0 poll N (default 0)\n"
          "  --dump-button-after-replay     arm only once the --replay-input route handed over (needs --replay-input and\n"
          "                         --replay-handover); the idle clock starts then too\n"
          "  --dump-button-idle-every N     every N polls with no event pending and no edge lately, take an idle control\n"
          "                         event (default 0 = off), --dump-button-max-idle N of them at most (default 40)\n";
}

const char *host_options_voice_help(void)
{
    return "  --voice-log FILE      T1793 read-only NPC voice/subtitle call entries, guest ticks and audio frames; resident banks in FILE.resident. Requires a headless vblank provider. Return values unavailable.\n";
}
