/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Command-line options for the host, and the defaults that are policy decisions.
 *
 * WHY THIS IS ITS OWN TRANSLATION UNIT. It used to live in `src/host/main.c`, which is
 * compiled into `tsfp_host` ALONE. `tsfp_host` is not a ctest binary, so nothing in it
 * can be covered by a suite or reached by `tools/mutate/c_suites.py`, which drives ctest
 * binaries. That is not a theoretical gap: the task that built `--hdd` wanted to mutate
 * "the no-`--hdd` default flipped to writable", found it unreachable, and mutated a
 * kernel-side guard instead -- detection by NULL dereference rather than by assertion.
 *
 * EVERY DEFAULT BELOW IS A SAFETY ARGUMENT, not a convenience, which is exactly why they
 * need pinning. `hdd_path` defaults to NULL because nothing should be writable unless an
 * operator said where. `disc_device` defaults to a DEVICE rather than `D:` because the
 * title creates that alias itself. `open_missing_as_empty` defaults to false because
 * fabricating a file is a diagnostic, not a behaviour. A silent change to any of them
 * would not fail a build or a test; it would change what the title sees and surface far
 * from its cause.
 *
 * Parsing is deliberately plain `strcmp` over argv rather than `getopt`: the flags are
 * long-form only, there is no clustering, no optional arguments, and the ordering of a
 * repeated `--mount` matters. `getopt_long` would add a dependency on global state
 * (`optind`) that a test has to reset between cases, which is the kind of hidden coupling
 * that makes a suite pass in one order and fail in another.
 */

#ifndef TSFP_HOST_OPTIONS_H
#define TSFP_HOST_OPTIONS_H

#include <stdbool.h>
#include <stdint.h>
#include "guest_dump.h"
#include "guest_watch.h"
#include "../input/mu_startup.h"

#include "../gpu/gpu_standin_units.h"

/* Enough of the trace to be useful without burying the stop reason. */
/* T1720b: --shot-max / --shot-max-bytes defaults and the --shot-max cap */
#define HOST_OPTIONS_SHOT_MAX_DEFAULT 200u
#define HOST_OPTIONS_SHOT_MAX_CAP 100000u
#define HOST_OPTIONS_SHOT_MAX_BYTES_DEFAULT 536870912ull
#define DEFAULT_TRACE_REPORT 64

/* How many addresses --profile-calls lists. */
#define DEFAULT_PROFILE_TOP 40

/* How long to let guest threads run before calling it a hang. Ten seconds is far
 * more than the boot path needs when it is making progress, and short enough that
 * a hang is reported rather than waited on. */
#define DEFAULT_THREAD_TIMEOUT_MS 10000u

/* How many --mount names one invocation can carry. The image has 13 NtOpenFile call
 * sites, so a handful of distinct volumes is the most a run can want. */
#define OPTION_MOUNT_MAX 8u

/*
 * Which guest device the disc mounts behind, when --disc-device is not given.
 *
 * `\Device\CdRom0`, NOT `D:`. MEASURED: the guest creates the `D:` alias itself at
 * 0x00381301, calling IoCreateSymbolicLink("\??\D:", "\Device\CdRom0"). Mounting the
 * device is therefore what makes the title's own symbolic link resolve, and it leaves
 * exactly one source of truth for what `D:` means -- the title's.
 */
#define DEFAULT_DISC_DEVICE "\\Device\\CdRom0"

/*
 * Which guest device --hdd backs, when --hdd-device is not given.
 *
 * `\Device\Harddisk0\partition1`, MEASURED as the partition the title's storage startup
 * actually uses: at 0x00380D43 NtOpenFile opens `\Device\Harddisk0\partition1\`, and at
 * 0x00381321 NtCreateFile creates `TDATA\45410066` under it. On a real console partition1
 * is the data partition behind `T:` and `U:`, which the title also symlinks itself.
 *
 * NO TRAILING SEPARATOR, and that is not cosmetic. The mount table requires the character
 * after a matched prefix to be a separator or the end of the name, so a prefix that
 * already ends in one would match the bare device and nothing under it --
 * `kernel_file_mount_host_dir` refuses such a prefix outright rather than let a
 * half-working volume be mounted.
 */
/* Retail HDDs carry three cache partitions (X, Y, Z). */
#define DEFAULT_CACHE_PARTITIONS 3u

/* Largest --cache-partitions accepted: a corrupt value must not become a plausible one. */
#define MAX_CACHE_PARTITIONS 8u

#define DEFAULT_HDD_DEVICE "\\Device\\Harddisk0\\partition1"

#define LIVE_PIPELINE_DEFAULT_DEPTH 1u /* T1246: validated on the Story and Map Maker replays (docs/t982-timing-audio.md) */

/* T1629 defaults and bounds of --dump-on-button (the title's own analog button threshold is 0x3C). */
#define HOST_BUTTON_DUMP_DEFAULT_AFTER_1 2u
#define HOST_BUTTON_DUMP_DEFAULT_AFTER_2 30u
#define HOST_BUTTON_DUMP_MAX_AFTER_FRAME 3600u
#define HOST_BUTTON_DUMP_DEFAULT_THRESHOLD 60u
#define HOST_BUTTON_DUMP_DEFAULT_COALESCE 3u
#define HOST_BUTTON_DUMP_MAX_COALESCE 60u
#define HOST_BUTTON_DUMP_DEFAULT_MAX_PENDING 8u
#define HOST_BUTTON_DUMP_MAX_PENDING 64u
#define HOST_BUTTON_DUMP_DEFAULT_MAX_DUMPS 2000u
#define HOST_BUTTON_DUMP_MAX_DUMPS 100000u
#define HOST_BUTTON_DUMP_DEFAULT_MAX_BYTES 0x40000000ull
#define HOST_BUTTON_DUMP_DEFAULT_MAX_IDLE 40u
#define HOST_BUTTON_DUMP_MAX_IDLE 100000u
#define HOST_BUTTON_DUMP_MAX_IDLE_EVERY 1000000u

typedef struct {
    const char *xbe_path;
    bool continue_on_missing;
    unsigned trace_report;
    /* What an unimplemented ordinal hands back under --continue-on-missing. */
    uint32_t stub_status;
    bool stub_status_set;
    unsigned thread_timeout_ms;
    bool thread_timeout_given; /* T1126: --thread-timeout on the command line, so a replay does not override it */
    /* Whether a file name no volume is mounted behind opens anyway, as an empty
     * file. See kernel_file.h: the pair exists so the two paths can be compared. */
    bool open_missing_as_empty;
    /* Names declared openable on the command line. A bounded array because this is
     * an operator convenience, not a mount table: the real one, when there is
     * content behind it, belongs in src/xbox. */
    const char *mounts[OPTION_MOUNT_MAX];
    unsigned mount_count;
    /* Whether the AC97 codec reports itself ready (global status bit 8). Default TRUE
     * since T375 (owner decision: a retail console always has it ready), still FABRICATED
     * until a xemu-level reference exists. `--no-ac97-ready` restores the NOT_READY path. */
    bool ac97_ready;
    /* `--ac97-ready` was spelled: a deprecated alias that now only announces itself. */
    bool ac97_ready_flag_given;
    /* Passive effects CPU views, without DSP execution or acknowledgement. Default ON
     * since T72a: the acceptance contract is fully measured (caller 0x000270AF, exact
     * dsstdfx image, descriptor layout from T151/T152), so refusing it by policy was the
     * one artificial stop left at the measured call. The flag is still accepted as an
     * explicit no-op. Everything the binding cannot prove still refuses loudly. */
    bool headless_effects;
    /* INFERRED opt-in owned GP bridge; requires pinned DSP build. No host PCM. */
    bool gp_effects;
    /* Startup-only passive stream objects/CPU caches; default off, no playback. */
    bool headless_streams;
    /* Startup-only passive buffer objects/CPU caches; default off, no playback. */
    bool headless_buffers;
    /* Passive startup listener scalar caches; default off, no derived3D or DSP. */
    bool headless_listener;
    /* One cooperative startup callback; default off, no timing/IRQ policy. */
    bool headless_first_vblank;
    /* Separate bounded two-event policy; requires all passive startup audio flags. */
    bool headless_second_vblank;
    /* T183: print the callback delivery schedule to stderr; default off, requires a
     * vblank policy, changes no guest or stdout behavior. */
    bool trace_vblank_schedule;
    /* T372: each completed wait-vblank also runs the derivable device effects of the original
     * vblank helper (count, timestamps, threshold). Default off, no other flag required. */
    bool couple_vblank_effects;
    /* T407: the Swap's flip is queued by the model and completed by the coupled vblank helper
     * (the display start, gamma ramp and PGRAPH increment go to a record, not a register file).
     * Default off. Needs couple_vblank_effects, since only the helper completes a flip. */
    bool model_flips;
    /* T370: run the vblank CPU callback at DISPATCH_LEVEL (IRQL 2) as the original DPC path
     * does. Default off, refines the headless vblank policies so it requires one. */
    bool vblank_dispatch_level;
    /* T371: print every counter-getter poll and the guest thread census at each frame entry to
     * stderr (observation only). Default off, needs a headless vblank policy. */
    bool trace_vblank_readers;
    /* T371: refuse a vblank delivery with a named stop unless the two-consumer quiescence
     * predicate holds, and refuse an unregistered counter reader. Default off, needs a headless
     * vblank policy. */
    bool check_vblank_quiescence;
    /* T460: after the second event the owner's own completed vblank waits each deliver one
     * callback from inside the wait, at most this many (then a named stop). 0 is off. Needs
     * --headless-second-vblank, --couple-vblank-effects and --check-vblank-quiescence. */
    bool interactive; /* T908: live FABRICATED wall-clock continuation, window only. */
    uint32_t vblank_owner_waits;
    /* T592: the loading bar worker's own completed vblank waits each deliver one callback, at most
     * this many (then a named stop), while the owner is proven parked in the loading bar gate. 0 is
     * off. Needs --vblank-owner-waits (so every option it needs). */
    uint32_t vblank_worker_blanks;
    /* T696: a frame wait poll whose exit test fails by one blank delivers it (owner decision, option a, FABRICATED
     * timing, same budget as the owner waits). Default off, needs --vblank-owner-waits. */
    bool vblank_poll_blank;
    /* Execute retained original CPU shader compiler code; default off. */
    bool native_shader_assembler;
    /* T904: opt-in INFERRED no-live-account XONLINE Startup/empty-user policy. */
    bool xonline_offline;
    /* T1111: INFERRED local OS entropy, preserves original initialization guard. */
    bool xnet_local_entropy;
    /* Execute the retained original XMV exports on the user's disc data (T92); default on with a --disc (T1093). */
    bool native_xmv;
    /* T1093: --native-xmv given explicitly (a missing profile is then an error, not a quiet fallback). */
    bool native_xmv_explicit;
    /* T1093: --no-native-xmv, opts out of the disc default. */
    bool native_xmv_off;
    /* Log each retained XMV export call and return to stderr (T234); default off. */
    bool trace_xmv;
    bool skip_intro; /* FABRICATED startup-only original movie termination, default off. */
    /* T394: write each decoded movie frame (the decoder's Y, U and V planes) as one file into this
     * directory, for comparison against an independent decoder. Needs --trace-xmv. Default NULL.
     * The frames are the user's disc data: keep the directory out of the repository. */
    const char *dump_xmv_frames;
    /* T394: stop writing frame files after this many (the hash lines go on). 0 means no limit. */
    uint32_t dump_xmv_frames_max;
    /* T538: write the guest state at the entry of the XMV codec wrapper 0x447E5E into this directory
     * (one private "entry_NNNNN.bin" per selected entry), so a Python tool can replay the ORIGINAL
     * instructions over the same packet. Needs --trace-xmv and a headless vblank policy (the capture
     * rides the cooperative call seam). Default NULL. The files hold disc data: keep them out of Git. */
    const char *capture_xmv_entries;
    /* T538: which wrapper entries (0 based, counted over the whole run) are captured: up to 32 inclusive
     * ranges from "--capture-xmv-entry-at 2,5-9". Required with --capture-xmv-entries. */
    uint32_t capture_xmv_range_first[32];
    uint32_t capture_xmv_range_last[32];
    uint32_t capture_xmv_range_count;
    /* T611: seeded native run of the codec wrapper. At wrapper entry number --seed-xmv-entry the host applies
     * the --seed-xmv-patch edits ("dec+0xA0=01", "esp+4=...", "dec@0x8+0=..."), runs the lifted wrapper itself,
     * writes the pages it changed to --seed-xmv-result and exits. A Python tool replays the same edits on the
     * original instructions and compares. Needs --trace-xmv and a headless vblank policy. Default off. */
    bool seed_xmv_entry_set;
    uint32_t seed_xmv_entry;
    const char *seed_xmv_result;
    const char *seed_xmv_patch[32];
    uint32_t seed_xmv_patch_count;
    /* T394: "FROM=TO", when the title asks CreateDecoderForFile for the movie FROM (the base name, no
     * directory or extension) the host opens TO from the same directory instead. FABRICATED, a test aid
     * that runs the retained codec over the other movies on the disc. Needs --trace-xmv. Default NULL. */
    const char *xmv_substitute;
    /* T630: the console language setting (ExQueryNonVolatileSetting index 7, XDK XC_LANGUAGE 1 to 9)
     * the title reads through XGetLanguage. Without it the setting is the fabricated zero. FABRICATED
     * input that stands for a console configured in that language, 0 means not given. */
    unsigned eeprom_language;
    /* Actual256-byte EEPROM image for raw FFFF and factoryMAC101; defaultNULL. */
    const char *eeprom_path;
    /* Actual16-byte console EEPROM key, authenticated with --eeprom; defaultNULL. */
    const char *eeprom_key_path;
    bool xnet_scheduler;
    /* T392: bounded CPU model of the XMV movie PCM audio stream (no audio is produced). Default
     * off, requires --native-xmv and --headless-streams. */
    bool headless_movie_audio;
    /* T681: the opt-in PASSIVE completion model of the title's own stream and sound state machines (owner decision
     * T608, option (a)): Process packets and Play complete on the kernel virtual clock, statuses follow the measured
     * original words, no audio is produced. Default off, requires --headless-streams, --headless-buffers and
     * --headless-listener. */
    bool passive_audio_completion;
    /* T762: DEFAULT ON when its prerequisites (the three headless audio policies) are present, --no-passive-audio-completion
     * opts out and reproduces the old row 15 recipe. *_by_default is true when the default (not the flag) switched it on,
     * main announces that. */
    bool passive_audio_completion_off;
    bool passive_audio_completion_by_default;
    /* T855 + T871: a packet past its deadline completes (words, list slot) at the next DirectSoundDoWork the model sees, like the
     * original's completion function 0x40B244 (XEMU-LEVEL T871: the retail title's size and status 0 are written back to back
     * only inside DoWork). DEFAULT ON with the completion model, --no-passive-audio-completion-at-dowork opts out and gives the
     * T681 delivery (first observation at Process, GetStatus, Pause). *_by_default is true when the default switched it on. */
    bool passive_audio_completion_at_dowork;
    bool passive_audio_completion_at_dowork_off;
    bool passive_audio_completion_at_dowork_by_default;
    /* T743: asynchronous (overlapped) NtReadFile completes on the virtual clock, XEMU-LEVEL timing (T763). T762: DEFAULT ON,
     * --no-async-file-io opts out. */
    bool async_file_io;
    /* T764: also queue an NtReadFile with NO Event on an asynchronous handle (the file object is
     * signalled at completion). INFERRED, default off, requires --async-file-io. */
    bool async_file_io_file_object;
    bool async_file_io_off;
    bool async_file_io_by_default;
    /* T821: --async-file-io-spin-complete N (INFERRED, async_io_spin.h): a thread that passes N cooperative safepoints
     * with no HLE dispatch of its own while a read is pending completes the earliest read. 0 is off. */
    unsigned async_file_io_spin;
    /* T762: --vblank-poll-blank (FABRICATED timing) is DEFAULT ON when --vblank-owner-waits is given, --no-vblank-poll-blank
     * opts out. */
    bool vblank_poll_blank_off;
    bool vblank_poll_blank_by_default;
    /* T717: FABRICATED. Port 0 reports one synthetic gamepad inserted (once) and the five XPP open-path
     * functions (XInputOpen, Close, GetCapabilities, GetState, SetState) are answered with contracts measured
     * against the original bytes. Buttons and sticks stay at rest. Default off, sets no other policy. */
    bool synthetic_pad;
    /* T731: FABRICATED. With --synthetic-pad, unplug the pad after this many XInputGetState calls (0 = never,
     * the default). Needs --synthetic-pad. */
    uint32_t synthetic_pad_remove_after_polls;
    /* T707: FABRICATED scripted input, a per-frame pad state file (src/input/xinput_source.h). Needs --synthetic-pad. */
    const char *pad_script;
    /* T1222: FABRICATED live pad, pad_script names a one line token file re-read at every poll (--pad-script-live). */
    bool pad_script_live;
    /* T1074: FABRICATED record/replay of port 0 pad states by poll index (src/input/xinput_record.h). */
    const char *record_input;
    const char *replay_input;
    /* T1616: FABRICATED route replay. replay_handover: after the record the live --pad-source has the pad. route_waits:
     * --route-wait specs, poke_at: --poke-at-poll specs (xinput_route_spec.h), validated at parse. */
    bool replay_handover;
    const char *route_waits[8];
    unsigned route_wait_count;
    const char *poke_at[4];
    unsigned poke_at_count;
    /* T1633: event driven route. route_event_waits: --route-wait-event specs (validated at parse), route_event_log: the event
     * log file (--route-event-log), route_log_mem: --route-log-mem specs sampled into that log. Observers only. */
    const char *route_event_waits[8];
    unsigned route_event_wait_count;
    const char *route_event_log;
    const char *route_log_mem[8];
    unsigned route_log_mem_count;
    /* T1640: closed loop menu navigation of a route replay (needs --replay-input). route_nav: on|off|strict (NULL = on when a table
     * is given, else off), route_nav_menus: the menu table file, route_nav_timing: HOLD:GAP polls. Tooling, not part of the flag identity. */
    const char *route_nav;
    const char *route_nav_menus;
    const char *route_nav_timing;
    /* T1640 recorder side: --route-nav-record on|off (needs --record-input; NULL = on when --route-nav-menus is given). */
    const char *route_nav_record;
    /* T1627: host hotkeys (src/input/xinput_hotkey.h), --hotkey SPEC (validated at parse, at most 8, T1632) writes
     * hotkey_dir/hotkey.<n> when its chord is held. Needs --pad-source keyboard|gamepad on the window. */
    const char *hotkeys[8];
    unsigned hotkey_count;
    const char *hotkey_dir;
    /* T1720b: the `shot` label writes shot-NNN.png and shots.manifest to shot_dir (NULL = hotkey_dir), refuses a press beyond
     * shot_max pictures (default 200, 1..100000) or beyond shot_max_bytes of PNG (default 512 MiB). Need --hotkey. */
    const char *shot_dir;
    unsigned shot_max;
    uint64_t shot_max_bytes;
    /* T1153: take a whole-process DMTCP snapshot when port 0 has been polled this many times (0 = off), host_snapshot.h. */
    uint64_t snapshot_at_poll;
    /* T1153: end the run when port 0 has been polled this many times (0 = off), a deterministic cut, host_snapshot.h. */
    uint64_t stop_at_poll;
    /* T751: FABRICATED host source for the pad, `script` `keyboard` or `gamepad` (NULL = script when --pad-script is
     * given). keyboard and gamepad need --pad-feed FILE (the fake-feed provider, no window or device library yet). */
    const char *pad_source;
    const char *pad_feed;
    /* Explicit writable existing MU images, modeled presence; no implicit format/create. */
    unsigned mu_image_count;
    mu_startup_image mu_images[MU_STARTUP_MAX];
    bool controllers; /* T942: optional four-port SDL gamepad backend. */
    const char *controller_config;
    const char *controller_mappings;
    const char *controller_save;

    /* T540: a completed coupled/modelled vblank retires a pending overlay buffer (inferred one-vblank
     * latency). Default off; needs --native-xmv and --couple-vblank-effects. */
    bool overlay_consume;
    /* T537: write the picture of each UpdateOverlay (RGB png scaled to the destination rectangle, the raw
     * Y U V planes, an index line) into this existing directory. A pure observer, default NULL. The
     * pictures are the user's disc data: keep the directory out of the repository. The colour matrix is
     * UNMEASURED for the overlay hardware (src/gpu/d3d8_overlay_image.h). */
    const char *dump_overlay;
    /* T537: stop writing overlay files after this many updates. 0 means no limit. Needs --dump-overlay. */
    uint32_t dump_overlay_max;
    /* T831, both default off and XEMU-LEVEL (T770, xemu v0.8.136, never NV2A silicon): the overlay picture is built
     * with the xemu matrix and bilinear scale instead of the XMV library matrix and nearest scale
     * (d3d8_overlay_image.h), and the overlay is composed over the swap replay's frame by the key rules xemu
     * showed (d3d8_overlay_key.h). The first changes the dump and present pictures, the second the frame a
     * replay frame hook receives through d3d8_overlay_key_displayed. */
    bool overlay_xemu_image;
    bool overlay_xemu_key;
    /* T744: opt-in presentation sinks (present_sink.h), NULL means absent. Names are validated by main
     * through present_video_select / present_audio_select, which refuse with a reason. */
    const char *present;
    const char *audio_sink;
    /* T759: optional WAV destination. Defaults to tsfp-audio.wav for wav-file. */
    const char *audio_output;
    /* --audio-mute: the sdl sink keeps its clock and pulls its ring but plays silence. */
    bool audio_mute;
    /* T1246: --live-pipeline N, frames the live renderer may run behind the guest thread (0 serial, 1 one frame). Needs --gpu-live. The depth
     * with --gpu-live and no flag is LIVE_PIPELINE_DEFAULT_DEPTH. */
    uint32_t live_pipeline;
    bool live_present_sync; /* T1262: --live-present-sync, wait for the queue after every present (the old behaviour). Needs --gpu-live. */
    bool live_pipeline_set; /* --live-pipeline was given, else the default depth applies with --gpu-live */
    /* T1247: --live-pipeline-cache DIR keeps the VkPipelineCache file in DIR, `none` turns it off. Default with --gpu-live: the --gpu-replay directory. */
    const char *live_pipeline_cache;
    bool live_pipeline_cache_off;
    const char *live_frame_hash; /* T1246: --live-frame-hash FILE, a sha256 per frame handed to the window (equivalence evidence) */
    /* T1235: --audio-latency-ms N holds the sdl sink's ring fill (the audio latency the host adds) near N ms in the
     * interactive window play, 0 turns the governor off (the old, unbounded behaviour). Default 150. */
    uint32_t audio_latency_ms;
    uint32_t audio_min_rate_permille; /* T1248: slowest continuous playback rate in permille of real time (T1250 default 250 = slow to 25 percent, 0 = off: pause and refill) */
    bool audio_pump; /* T1250 gaps: the 5 ms mixer pump outside DirectSoundDoWork (default on, --no-audio-pump) */
    uint32_t audio_hold_ms; /* T1250 gaps: --audio-hold-ms, how long the stretch bridges a guest production gap by looping (full gain 100 ms, then a decay), default 400, 0 = 100 ms plain loop */
    int32_t av_sync_offset_ms; /* T1487: --av-sync-offset-ms N, the video follows the audio clock N ms later (negative: earlier), default 0 */
    bool audio_stretch_legacy; /* T1487: --audio-stretch-legacy restores the pre T1487 stretch (step ratio, 6.7 ms block) for A/B runs */
    bool audio_stretch_resample; /* T1250: --audio-stretch resample: the slowed playback lowers the pitch (T1248 resampler) instead of the pitch preserving stretch (default) */
    bool audio_latency_set;
    /* T760: --present-hold-ms N keeps the window open that long after the stop report (0 closes at once). */
    uint32_t present_hold_ms;
    /* T760: --present-capture PATH writes the window's last frame as a BMP at the stop (window only). */
    const char *present_capture;
    /* --present-no-pace: with --present window, do not hold the modelled vblanks to wall-clock 59.94 Hz. */
    bool present_no_pace;
    /* T827: --present-timeline FILE writes a CSV of wall time, modelled time, ring fill and thread CPU every 25 ms. */
    const char *present_timeline;
    /* T1235: --cpu-profile FILE writes an in-process CPU sample file (see cpu_sampler.h). */
    const char *cpu_profile;
    bool cpu_profile_wall; /* --cpu-profile-wall FILE: the wall clock call chain mode (200 Hz) */
    bool guest_frame_trace_off; /* T1289: --no-guest-frame-trace turns the per vblank interval guest thread trace off (on by default) */
    /* The user's own Xbox disc image, mounted behind --disc-device. NULL means no
     * disc, which is the default: the image is the user's property and is never
     * assumed to be present. */
    const char *disc_path;
    const char *disc_device;
    /* A real, WRITABLE host directory standing in for the console's hard disk. NULL
     * means no hard disk, which is the default and is the honest answer: nothing is
     * writable unless an operator said where. See --hdd's help text. */
    const char *hdd_path;
    const char *hdd_device;
    /* Explicit INFERRED xemu-qualified primary-master device identity; requires --hdd. */
    bool disk_identity_xemu;
    /* What HalDiskCachePartitionCount (ordinal 40, a DATA export) reads, applied ONLY with
     * --hdd. A retail console has 3 cache partitions; the default here is that, and 0 is
     * not neutral: the title then calls _memmove with length 0xFFFFFFF4 and runs off the
     * stack. It is a FABRICATION and the host announces it. */
    unsigned cache_partitions;
    /* The fabricated AV pack, by name. NULL means the default derived from the binary
     * (kernel_av_default_pack). The name is validated by the host with kernel_av_parse_pack,
     * because this library is dependency-free and must not carry a second copy of the set. */
    const char *av_pack;
    /* T422: whole-run call and frame profile, reported after the run. Default off, observation only.
     * `profile_calls_top` is how many addresses the report lists. `stop_after_*` cuts the run by
     * guest progress (HOST_STOP_BUDGET) once one address has been dispatched `stop_after_count`
     * times, which makes two boots comparable where the watchdog's time based cut does not. */
    bool profile_calls;
    unsigned profile_calls_top;
    /* --profile-watch ADDR, repeatable: a guest dword sampled at every present. */
    uint32_t profile_watch[8];
    unsigned profile_watch_count;
    /* T821: the read-only census of the lifted dispatcher's indirect call targets (function_census.h), default
     * off. `census_window_*` also keeps every indirect call, in order, between two present counts. */
    bool census_icalls;
    const char *census_phases;
    const char *voice_log; /* T1793: read-only call entries, NULL disables the provider. */ /* T1502: --census-phases FILE, SIGUSR1 phase dumps FILE.<phase> (needs --census-icalls) */
    /* T1599: READ-ONLY dump of guest memory ranges (guest_dump.h): --dump-guest-range ADDR:LEN[,..] (repeatable),
     * --dump-guest-dir DIR (required with ranges), --dump-guest-max-bytes N (total per dump, default 1 MiB). */
    guest_dump_set dump_guest_set;
    const char *dump_guest_dir;
    /* T1613: --forced-state, FABRICATED-STATE opt-in for the guarded pokes (guest_poke.h), needs --dump-guest-range. */
    bool forced_state;
    uint64_t dump_guest_max_bytes;
    /* T1741: WRITE WATCH (guest_watch.h): --watch-write SPEC (repeatable, dump-range grammar, 4 ranges of at most 64 bytes),
     * --watch-write-log FILE (default stderr), --watch-write-max N (records, default 4096). Passive, no guest change. */
    guest_dump_set watch_write_set;
    const char *watch_write_log;
    unsigned watch_write_max;
    /* T1629: --dump-on-button, a guest dump just before and after each digital button edge of pad port 0 (button_dump.h).
     * Needs --dump-guest-range and --dump-guest-dir (same ranges, files under DIR/buttons) and a pad source. The other
     * --dump-button-* / --dump-after-frames flags tune it and are refused without it. */
    bool dump_on_button;
    bool dump_button_tuned;
    unsigned dump_after_count;
    uint32_t dump_after[4];
    unsigned dump_button_threshold;
    unsigned dump_button_coalesce;
    unsigned dump_button_max_pending;
    unsigned dump_button_max_dumps;
    uint64_t dump_button_max_bytes;
    uint64_t dump_button_start_poll;
    bool dump_button_after_replay;
    unsigned dump_button_idle_every;
    unsigned dump_button_max_idle;
    bool census_window_set;
    uint64_t census_window_first;
    uint64_t census_window_last;
    bool stop_after_set;
    bool stop_after_ordinal;
    uint32_t stop_after_id;
    uint64_t stop_after_count;
    /* The Swap replay (T84a, d3d8_swap_replay.h): decode the recorded stream at each present and
     * replay it on a Vulkan device. OFF unless --gpu-replay names the directory of `<module>.spv`.
     * The rest only refine it and are refused without it. A size of 0x0 is measured from the bound
     * render target. Strict (unhandled methods refuse the frame) unless --gpu-replay-lenient. */
    const char *gpu_replay_dir;
    const char *gpu_replay_dump;
    unsigned gpu_replay_width;
    unsigned gpu_replay_height;
    bool gpu_replay_lenient;
    /* T84a3. --gpu-replay-flip-y reverses the rows of each replayed frame (T100f, UNDECIDED which
     * orientation the NV2A's is, so default off). --gpu-replay-undo-viewport STATES that DIR holds
     * modules generated with `vsh_modules.py --undo-viewport` (T96): the module selection is the
     * directory, the flag only tells the replay which kind it was given. Both refused without
     * --gpu-replay. */
    bool gpu_replay_flip_y;
    bool gpu_replay_undo_viewport;
    /* T714. --gpu-replay-window-to-clip STATES that DIR holds modules generated with `vsh_modules.py
     * --window-to-clip` (x and y of none-class window coordinates converted with c58/c59, xemu-level, HQ57;
     * z-only xy remains open, HQ21). Like
     * --gpu-replay-undo-viewport it selects nothing and is refused without --gpu-replay. */
    bool gpu_replay_window_to_clip;
    /* T441. --gpu-replay-assume-program-mode allows GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN (a stream that
     * never wrote the transform execution mode but started a vertex program is replayed as the program mode,
     * the real title's case). --gpu-replay-output-state turns on every measured output-state group (T267: scissor,
     * cull, blend and colour mask, alpha test, depth and stencil, clear) and allows their named inferences.
     * Both OFF by default and refused without --gpu-replay. */
    bool gpu_replay_assume_program_mode;
    /* T477. Infer a whole-target D3D viewport into c58/c59 from the bound target dimensions.
     * The dimensions and ported emitter values are measured; the NV2A register feed is not. */
    bool gpu_replay_viewport_from_target;
    bool gpu_replay_output_state;
    /* T478. --gpu-replay-combiner replaces the fixed fragment stage with the translated register combiner the stream
     * programmed (T75) and allows the combiner inferences the title's texture-free configurations need. The
     * `combiner_<sha256>.spv` modules are read from the --gpu-replay directory. OFF by default, refused without
     * --gpu-replay. */
    bool gpu_replay_combiner;
    /* T497. --gpu-replay-standin-texture WxH:RRGGBBAA gives the combiner ONE stand-in texture for texture stage 0, a
     * WxH texture of the single colour RRGGBBAA (R first), so a draw whose combiner reads t0 is not refused for want of
     * the title's texture, which is not decoded (T480). IT IS NOT THE TITLE'S TEXTURE and the replay says so. Allows the
     * TEXTURE_SAMPLING inference (the stage program is the stream's own, MEASURED, so STAGE_PROGRAM stays refused). OFF by default, refused without --gpu-replay-combiner. W and H are
     * 1..GPU_REPLAY_MAX_EDGE, the value is eight hex digits or the literal checker. */
    bool gpu_replay_standin;
    unsigned gpu_replay_standin_pattern; /* GPU_STANDIN_SOLID (default) or GPU_STANDIN_CHECKER */
    unsigned gpu_replay_standin_width;
    unsigned gpu_replay_standin_height;
    uint8_t gpu_replay_standin_rgba[4];
    /* T713. --gpu-replay-standin-texel-units: the stand-in is sampled in TEXEL units (the vertex program's oT0 is divided by the
     * texture size, the T510 unnormalised path), so a program that writes texels 0..640 x 0..480 maps a 640x480 checker one to one.
     * INFERRED (the title's own texture state is not decoded). OFF by default, refused without --gpu-replay-standin-texture. */
    bool gpu_replay_standin_texel_units;
    /* T719. --gpu-replay-standin-texel-program HEX and --gpu-replay-standin-normalised-program HEX (repeatable): the stand-in's unit
     * is chosen PER DRAW by the vertex program digest prefix (8..64 lowercase hex). A draw whose combiner samples the stand-in and whose
     * program is in neither list refuses the frame. INFERRED, OFF by default, refused with --gpu-replay-standin-texel-units, without
     * the stand-in and combiner, on an overlapping or malformed prefix. */
    gpu_standin_unit_rule gpu_replay_standin_unit_rules[GPU_STANDIN_UNIT_RULES];
    /* T724. --gpu-replay-draw-dump FILE: append one JSON line per replayed draw (vertex program digest, whether the combiner samples t0,
     * the assembled attributes and constants) so tools.nv2a.standin_units can MEASURE the oT0 range. Observation only, needs --gpu-replay. */
    const char *gpu_replay_draw_dump;
    uint32_t gpu_replay_standin_unit_rule_count;
    /* T510. --gpu-replay-rt-texture lets a texture stage bound to a render target header an EARLIER pass of the same frame drew be
     * sampled from that pass's image, instead of the combiner refusing the draw. Every other binding is still refused by name. INFERRED
     * sampling model, announced. OFF by default, refused without --gpu-replay-combiner and --gpu-replay-output-state (the TEXTURE group),
     * and refused with --gpu-replay-standin-texture or --gpu-replay-flip-y. */
    bool gpu_replay_rt_texture;
    /* T838 (M9 host hook-up). --gpu-live: the live Vulkan renderer draws the title's frames in the --present window, which then is a
     * Vulkan window (the movie overlay pictures go through the live compositor). OPT-IN, INFERRED until validated against xemu. Needs
     * --gpu-replay DIR (the shader modules and the model), --present window; it turns on --gpu-replay-output-state and
     * --gpu-replay-combiner and skips the CPU replay. Refused with --gpu-replay-rt-texture, the stand-in and --gpu-replay-flip-y.
     * --gpu-live-inferred (needs --gpu-live) also admits the INFERRED texture formats and sampler states and the INFERRED byte
     * format CopyRects. */
    bool gpu_live;
    bool gpu_live_inferred;
    /* T849. --gpu-live-blit (needs --gpu-live): present with the swapchain pre-pass blit (the front target image and the overlay
     * texture are blitted straight into the acquired image, no front buffer readback or CPU compose) instead of the readback and
     * upload route, which stays the default and the fallback for fronts the blit cannot read. INFERRED sampling rounding, announced. */
    bool gpu_live_blit;
    /* T1267: the swapchain blit is the DEFAULT present route of --gpu-live (--gpu-live-blit is accepted and implied).
     * --live-readback forces the old GPU to CPU readback route (also implied by --live-frame-hash, which hashes the readback frames).
     * --live-blit-verify N: every Nth vblank also present the readback route and compare it with the blit (needs the blit route). */
    bool live_readback;
    uint32_t live_blit_verify;
    /* T847. --gpu-live-translate (needs --gpu-live): a vertex program or register combiner configuration that is in no module table is
     * translated ON DEMAND by the repository's own translators (a subprocess, tools/nv2a/live_modules.py, run from the repository root)
     * into the --gpu-replay directory, and the draw is retried. INFERRED translators. Needs python3 and glslangValidator. */
    bool gpu_live_translate;
    /* T510. --gpu-replay-rt-texture-census (implies --gpu-replay-rt-texture): CENSUS ONLY, replay no pixel, classify every draw. */
    bool gpu_replay_rt_texture_census;
    /* T633 (1) and T596. --gpu-replay-surface-source (announced, the rule MEASURED in xemu T736, the timing INFERRED): a surface no pass of the frame drew is its kept
     * image from an earlier frame, else the guest memory under it (MEASURED all zero on the retail boots), for a texture stage and for the CopyRects blits (and their
     * byte path). It also turns on target persistence (measured: a pass starts from the previous image). OFF by default, refused without --gpu-replay-rt-texture
     * (and so everything that needs). */
    bool gpu_replay_surface_source;
    /* T633 (2). --gpu-replay-target-persist (announced, MEASURED in xemu T736): a render target pass starts from the kept image an earlier frame left under its Data
     * word (implied by --gpu-replay-surface-source). OFF by default, refused without --gpu-replay and with --gpu-replay-flip-y. */
    bool gpu_replay_target_persist;
    /* T484. The dump cadence: --gpu-replay-dump-every N writes the frames whose number is a multiple of N,
     * --gpu-replay-dump-last writes the last replayed frame when the run ends (alone: nothing else, with an N: both).
     * 0 and false are the unchanged default, every replayed frame. Both are refused without --gpu-replay-dump. */
    unsigned gpu_replay_dump_every;
    bool gpu_replay_dump_last;
} options;

/* Largest --gpu-replay-size edge accepted (a linear surface's Size word holds 12 bits per edge). */
#define GPU_REPLAY_MAX_EDGE 4096u

/**
 * Parse argv into `out`. False on any malformed invocation.
 *
 * Every field of `out` is set before any argument is read, so a caller never sees a
 * partially-initialised struct even on the false path. False means: an unknown flag, a
 * flag whose argument is missing because it was last in argv, a second positional
 * argument, more than OPTION_MOUNT_MAX mounts, a negative `--thread-timeout`, a
 * non-positive `--trace`, or no XBE path at all.
 *
 * REFUSING IS THE POINT. A flag given no argument used to be able to walk off the end of
 * argv; returning false instead means the operator is told, rather than the run
 * proceeding with a default they did not ask for and a trace that looks plausible.
 */
bool parse_options(int argc, char **argv, options *out);

/**
 * T1617. The --help text of the T1616 route replay flags (--replay-handover, --route-wait,
 * --poke-at-poll) and the --record-input SIGUSR2 marks. It lives here, not in main.c, so a unit test
 * can check it. It carries the literal markers "T1616" and "--route-wait": the owner scripts grep
 * `--help` for them to tell a host that has route replay from one that has not.
 */
const char *host_options_route_replay_help(void);

/**
 * T1633. The --help text of the event driven route flags (--route-wait-event, --route-event-log, --route-log-mem) and the
 * `# wait:` / `# mark-info:` record lines. Carries the literal markers "T1633" and "--route-wait-event" so the owner scripts can
 * tell a host that has event waits from one that has not.
 */
const char *host_options_route_event_help(void);

/**
 * T1640. The --help text of the closed loop menu navigation (--route-nav, --route-nav-menus, --route-nav-timing) and the
 * `# nav:` record line. Carries the literal markers "T1640" and "--route-nav" so the owner scripts can tell a host that has
 * it from one that has not.
 */
const char *host_options_route_nav_help(void);

/**
 * T1627. The --help text of the host hotkeys (--hotkey, --hotkey-dir). Carries the literal markers "T1627" and
 * "--hotkey" so the owner scripts can tell a host that has them from one that has not.
 */
const char *host_options_hotkey_help(void);
/* T1629: the --help text of --dump-on-button and its tuning flags. Carries the literal marker "T1629" (the owner scripts grep
 * `--help` for it to tell a host with the button dump from one without) and names every flag. */
const char *host_options_button_dump_help(void);
/** T1741: the help block of --watch-write and its two tuning flags. */
const char *host_options_watch_write_help(void);

const char *host_options_voice_help(void);

#endif /* TSFP_HOST_OPTIONS_H */
