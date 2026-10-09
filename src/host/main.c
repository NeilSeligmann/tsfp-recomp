/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Run lifted guest code to the point it stops.
 *
 * Phase 1.4 of `docs/superpowers/plans/2026-10-01-runnable-game.md`: parse and
 * map the user's XBE, stand up the kernel HLE, and call the lifted entry point.
 *
 * WHAT SUCCESS LOOKS LIKE HERE, AND WHAT IT DOES NOT. Stopping at a kernel
 * ordinal nobody has written yet IS the intended outcome. 10 of 151 imported
 * ordinals have implementations, so the guest cannot get far, and the valuable
 * output is not "it ran" -- it is the ORDERED LIST of ordinals the guest actually
 * reached. That list turns "implement 141 remaining ordinals" into a measured
 * work queue, which is what `kernel_hle_report_missing` was built to consume.
 *
 * NOTHING HERE IS ALLOWED TO LOOK LIKE SUCCESS WHEN IT IS NOT. Every exit path
 * names where it stopped and why; a fault reports the faulting address; and a
 * kernel ordinal whose argument count we cannot account for stops the run rather
 * than being assumed to take none. A linked binary that produces a plausible
 * trace on a desynced stack is the specific failure this project has been bitten
 * by, so the trace is only ever extended through calls we can unwind exactly.
 *
 * THE ENTRY POINT IS A THREAD-SPAWNING STUB, so running it is not running the
 * game. Measured: it calls `PsCreateSystemThreadEx`, closes the handle and
 * returns. Every piece of real work is in that thread, so this program's job is
 * now to start it, WAIT for it, and stop with a diagnosis if it does not finish.
 *
 * THE WATCHDOG IS NOT OPTIONAL. A guest thread entering 2.56 M lines of lifted
 * code with 141 of 151 kernel ordinals missing will hang, repeatedly, and a hang
 * with no diagnosis is the worst outcome available here -- strictly worse than a
 * fault, which at least names an address. So the join is bounded and a timeout
 * prints the trace and the backlog exactly as any other stop does.
 */

#include <errno.h>
#include "guest_frame_trace.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "guest_mem.h"
#include "host_runtime.h"
#include "host_symbols.h"
#include "kernel_call.h"
#include "kernel_async_io.h"
#include "kernel_clock.h"
#include "kernel_hle.h"
#include "kernel_identity.h"
#include "kernel_disk_identity.h"
#include "disk_identity_linux.h"
#include "disk_identity_xemu.h"
#include "kernel_memory.h"
#include "kernel_ordinals.h"
#include "kernel_sync.h"
#include "kernel_object.h"
#include "kernel_thread.h"
#include "kernel_rtl.h"
#include "kernel_pool.h"
#include "kernel_event.h"
#include "kernel_critsec.h"
#include "kernel_crypto.h"
#include "kernel_hal.h"
#include "kernel_config.h"
#include "kernel_av.h"
#include "dsound_hle.h"
#include "dsound_device.h"
#include "dsound_stream.h"
#include "dsound_movie_stream.h"
#include "dsound_completion.h"
#include "dsound_mixbin_headroom.h"
#include "recomp_guest_call.h"
#include "dsound_buffer.h"
#include "dsound_listener.h"
#include "dsound_audio_runtime.h"
#include "dsound_effects_binding.h"
#include "dsound_hrtf.h"
#include "xinput_hle.h"
#include "xvoice_media.h"
#include "xinput_devices.h"
#include "mu_device.h"
#include "mu_startup.h"
#include "xinput_host_source.h"
#include "xinput_source.h"
#include "host_snapshot.h"
#include "xinput_record.h"
#include "host_route.h"
#include "route_probe.h"
#include "gpu_sha256.h"
#include "xgrph_hle.h"
#include "xgrph_texture.h"
#include "xgrph_object_lifetime.h"
#include "xgrph_shader_query.h"
#include "xgrph_swizzle.h"
#include "d3d8_cube_surface.h"
#include "d3d8_target_query.h"
#include "kernel_file.h"
#include "kernel_io.h"
#include "kernel_register_all.h"
#include "kernel_xe.h"
#include "nt_status.h"
#include "recomp_abi.h"
#include "recomp_callback.h"
#include "recomp_cooperative.h"
#include "d3d8_first_vblank.h"
#include "d3d8_present.h"
#include "d3d8_flip.h"
#include "recomp_second_vblank.h"
#include "cpu_sampler.h"
#include "recomp_vblank_quiescence.h"
#include "d3d8_callbacks.h"
#include "d3d8_gpu.h"
#include "d3d8_frame_profile.h"
#include "call_profile.h"
#include "function_census.h"
#include "voice_observer.h"
#include "guest_dump.h"
#include "guest_watch.h"
#include "button_dump_host.h"
#include "async_io_spin.h"
#include "d3d8_vblank_effects.h"
#include "d3d8_overlay.h"
#include "d3d8_overlay_image.h"
#include "d3d8_overlay_key.h"
#include "d3d8_overlay_present.h"
#include "d3d8_resource.h"
#include "d3d8_swap_replay.h"
#include "live_render.h"
#include "live_module_maker.h"
#include "xbe.h"

#include "kernel_thunk.h"
#include "monitor_thunk.h"
#include "thunk_trace.h"
#include "xdk_thunk.h"
#include "xdk_original.h"
#include "xmv_original.h"
#include "xonline_hle.h"
#include "xonline_offline.h"
#include "xnet_hle.h"
#include "xnet_offline.h"
#include "xnet_eeprom.h"
#include "xnet_dpc.h"
#include "xnet_random.h"

/* Not behind TSFP_HAVE_XDK_SURFACE: neither header needs a generated file, and main() below calls
 * into them whether or not the surface was adopted (an unadopted table registers nothing). */
#include "d3d8_device.h"
#include "d3d8_hle.h"

/* The XDK call surface is derived from the user's own executable and is gitignored,
 * so this whole seam is conditional in exactly the way the CMake target is. Without it
 * the kernel boundary still works and the XDK one is simply not adopted, which the
 * report says in as many words rather than looking like a boundary with no traffic. */
#if defined(TSFP_HAVE_XDK_SURFACE)
#include "d3d8_hle.h"
#include "d3d8_surface_adapter.h"
#include "xdk_surface.h"
#endif

/* One megabyte of guest stack. The real console gives a title's main thread the
 * same, and a bring-up run that overflows a smaller one would look like a fault
 * in guest code rather than a host sizing mistake. */
#define GUEST_STACK_BYTES (1u << 20)

/* Pushed as the entry point's return address. Deliberately not a real guest VA:
 * `_mainCRTStartup` is not expected to return, and if it does we want the trace
 * to show a value that cannot be mistaken for guest code. */
#define GUEST_SENTINEL_RETURN 0xDEADBEEFu

/* The options struct, every default, and the parser live in host_options.h and
 * host_options.c, so a suite can link them without tsfp_host. See that header:
 * nothing compiled only into this binary can be tested or mutated. */
#include "host_options.h"
#include "audio_sink_sdl.h"
#include "pad_sdl_feed.h"
#include "hotkey_actions.h"
#include "shot_hotkey.h"
#include "controller_sdl.h"
#include "present_sink.h"

/* The run report's formatting, the stop-record bookkeeping and the verdict live in
 * host_report.{c,h} for the same reason. main.c gathers the numbers under its own
 * locks and passes them in; the lookups the lines need arrive through this table. */
#include "host_report.h"

/* T505: the fault handler reads the guest ebp through this, a plain thread local read. */
static uint32_t read_guest_ebp(void)
{
    return g_ebp;
}

static const host_report_names report_names = {
    .stop_reason_str = host_stop_reason_str,
    .ordinal_name = xbox_kernel_ordinal_name,
    .xdk_name_of = xdk_thunk_name_of,
    .thunk_is_va = kernel_thunk_is_va,
    .measured_as_data = kernel_thunk_measured_as_data,
    .host_symbol = host_symbols_describe,
    .guest_function = host_symbols_guest_function,
};

static void usage(const char *program)
{
    fprintf(stderr,
            "usage: %s <default.xbe> [--continue-on-missing] [--trace N]\n"
            "\n"
            "  Maps the XBE, initialises the kernel HLE and calls the lifted\n"
            "  entry point. Reports where it stopped and, in call order, the\n"
            "  kernel ordinals the guest reached.\n"
            "\n"
            "  --mount PATH           declare one guest path openable, as an empty\n"
            "                         file. Repeatable. Narrower and more honest\n"
            "                         than --open-missing-as-empty: the operator\n"
            "                         names the one volume being stood in for, and\n"
            "                         every other name still refuses. The title's\n"
            "                         own first open is\n"
            "                         \"\\Device\\Harddisk0\\partition1\\\".\n"
            "  --open-missing-as-empty  let NtOpenFile succeed for a name no\n"
            "                         volume is mounted behind, handing back a\n"
            "                         handle to an EMPTY file. Off by default: the\n"
            "                         honest answer is that the name does not\n"
            "                         resolve, and every measured call site has an\n"
            "                         error arm. On, the title proceeds on content\n"
            "                         that is ours rather than a disc's -- which is\n"
            "                         fabricated, announced, and useful only for\n"
            "                         comparing the two paths.\n"
            "  --disc PATH            mount YOUR OWN Xbox disc image, so the title\n"
            "                         reads its real assets from real sectors. The\n"
            "                         image is validated when it is mounted, not at\n"
            "                         the first read, so a wrong path is diagnosed\n"
            "                         here rather than looking like missing content.\n"
            "                         Nothing is fabricated behind this: a file the\n"
            "                         disc does not have is reported absent.\n"
            "  --disc-device NAME     which guest device --disc mounts behind\n"
            "                         (default %s).\n"
            "                         Deliberately a DEVICE and not \"D:\": the title\n"
            "                         creates the D: alias itself with\n"
            "                         IoCreateSymbolicLink at 0x00381301, so mounting\n"
            "                         the device lets its own link resolve instead of\n"
            "                         competing with it.\n",
            program, DEFAULT_DISC_DEVICE);
    /*
     * SPLIT ACROSS CALLS, because one string literal here passed 4095 bytes and
     * -Woverlength-strings is an error in this build. Split rather than shortened: the
     * help text is where an operator learns that `--hdd` is what stops the title
     * rebooting itself, and trimming that to fit a compiler limit would trade a
     * diagnosis for a line count.
     */
    fprintf(stderr,
            "  --hdd PATH             back the console's hard disk with a real,\n"
            "                         WRITABLE host directory. The directory must\n"
            "                         already exist and be a directory; nothing is\n"
            "                         created for you, because a typo would otherwise\n"
            "                         produce an empty hard disk somewhere you did not\n"
            "                         mean. With this, NtCreateFile with a creating\n"
            "                         disposition GENUINELY CREATES -- the title's\n"
            "                         TDATA\\<TitleID> and UDATA\\<TitleID> become real\n"
            "                         directories that are still there next run.\n"
            "                         WITHOUT it nothing is writable, which is the\n"
            "                         default and the honest answer. That matters: the\n"
            "                         title tests a failed create against\n"
            "                         STATUS_DISK_FULL and treats anything else as\n"
            "                         fatal, so it REBOOTS ITSELF via XLaunchNewImage\n"
            "                         (measured at 0x00381321). Your disc is never\n"
            "                         touched by this -- a writable volume is a\n"
            "                         DIFFERENT volume, the image is opened read-only,\n"
            "                         and a path containing \"..\" or crossing a host\n"
            "                         symbolic link is refused rather than followed.\n"
            "  --disk-identity-xemu   opt-in INFERRED virtual ATA identity from the\n"
            "                         mounted nonempty partition0 image under --hdd.\n"
            "  --hdd-device NAME      which guest device --hdd backs (default %s).\n"
            "                         MEASURED: that is the partition the title's\n"
            "                         storage startup opens and creates under. Must NOT\n"
            "                         end in a separator or nothing under it resolves.\n"
            "  --cache-partitions N   what HalDiskCachePartitionCount reads, with --hdd\n"
            "                         only (default 3, a retail console). FABRICATED\n"
            "                         and announced. 0 is not neutral: the title then\n"
            "                         calls memmove with length 0xFFFFFFF4.\n"
            "  --ac97-ready           DEPRECATED alias, still accepted: the AC97 codec is\n"
            "                         now modelled as present by default (T375). Prints\n"
            "                         a notice and changes nothing.\n"
            "  --no-ac97-ready        opt out of the default: the codec global status\n"
            "                         bit stays clear, DirectSoundCreate fails, the NULL\n"
            "                         device is carried forward and the title faults at\n"
            "                         0x00406BCA (453 calls). The default sets the bit\n"
            "                         (FABRICATED and announced: there is no AC97 codec\n"
            "                         and no xemu-level reference yet). Named after\n"
            "                         upstream's RECOMP_AC97_READY.\n"
            "  --gp-effects           INFERRED owned CPU-view/GP bridge, pinned DSP build;\n"
            "                         real firmware updates; no APU or host PCM scheduling.\n"
            "  --headless-effects     accepted as an explicit no-op: passive effects CPU\n"
            "                         views are the DEFAULT since the acceptance contract\n"
            "                         was fully measured (T72a). Copies measured initial\n"
            "                         state; zeroes workspace. No DSP execution or\n"
            "                         acknowledgement.\n",
            DEFAULT_HDD_DEVICE);
    fprintf(stderr,
            "  --headless-audio       shorthand for exactly --headless-effects\n"
            "                         --headless-streams --headless-buffers\n"
            "                         --headless-listener, plus the (default) FABRICATED\n"
            "                         codec readiness, and every policy still announces itself.\n"
            "                         Not the vblank policies. Default off.\n"
            "  --headless-streams     enable startup stream objects and CPU caches\n"
            "                         (default off); no playback or packet completion.\n"
            "                         Requires a lift with all stream safety stops.\n"
            "  --headless-buffers     enable startup buffer objects and CPU caches\n"
            "                         (default off); no playback or completion.\n"
            "                         Requires a lift with all buffer lifetime stops.\n"
            "  --headless-listener    enable startup listener scalar CPU caches\n"
            "                         (default off); no derived3D or DSP commit.\n"
            "  --headless-first-vblank deliver one startup CPU callback (default off).\n"
            "  --headless-second-vblank deliver startup plus one credited CPU callback.\n"
            "                         Requires compiled cooperative call safe points.\n"
            "                         No clock/MMIO/event/GPU stats advance or general IRQ.\n"
            "  --couple-vblank-effects each completed vblank wait also applies the original\n"
            "                         helper's device count, timestamps and threshold\n"
            "                         (default off, T372); refuses field-status state.\n"
            "  --model-flips          queue the Swap's flip for that helper (default off,\n"
            "                         T407); requires --couple-vblank-effects.\n"
            "  --vblank-dispatch-level run the vblank CPU callback at DISPATCH_LEVEL (IRQL 2)\n"
            "                         through the kernel IRQL model (default off, T370);\n"
            "                         requires a headless vblank policy.\n"
            "  --trace-vblank-schedule print the callback delivery schedule to stderr\n"
            "                         (default off); requires a headless vblank policy.\n"
            "  --native-shader-assembler run the retained original CPU shader compiler\n"
            "                         (default off); requires a complete compiled profile.\n"
            "  --native-xmv           run the retained original XMV library exports on the\n"
            "                         disc's own movie data (default on with --disc, T1093);\n"
            "                         an explicit flag requires the compiled profile.\n"
            "  --no-native-xmv        leave the XMV exports unserved even with --disc.\n"
            "  --skip-intro           FABRICATED startup-logo termination through original cleanup; default off.\n"
            "  --trace-xmv            log each retained XMV export call and return to stderr, and the\n"
            "                         title movie routine's caller at each open (T636)\n");
    fprintf(stderr, "  --eeprom FILE          read an actual256-byte EEPROM image for raw NV and factoryMAC queries\n"
                    "  --eeprom-key FILE      with --eeprom: authenticate using actual16-byte console EEPROM key\n");
    fprintf(stderr, "  --cpu-profile FILE     T1235: sample the process CPU time (SIGPROF, 997 Hz) into FILE, resolve with tools/cpu_profile_report.py\n");
    fprintf(stderr, "  --no-guest-frame-trace  T1289: turn off the per vblank interval guest thread trace (slowest intervals, phases, events) of the stop report\n");
    fprintf(stderr, "  --cpu-profile-wall FILE  T1235: as --cpu-profile but WALL time at 200 Hz with 8 frame call chains (shows what blocked threads wait in)\n");
    fprintf(stderr, "  --live-pipeline N     with --gpu-live: frames the live renderer runs behind the guest thread (T1246, 0 = serial, 1 = one frame)\n");
    fprintf(stderr, "  --live-readback       with --gpu-live: force the GPU to CPU readback present route (T1267, the swapchain blit is the default)\n");
    fprintf(stderr, "  --live-blit-verify N  with --gpu-live: every Nth vblank also present the readback route, compare both swapchain images (T1267)\n");
    fprintf(stderr, "  --live-present-sync   with --gpu-live: wait for the GPU queue after every present (T1262, the pre T1262 behaviour, for bisecting a stutter)\n");
    fprintf(stderr, "  --live-pipeline-cache DIR  with --gpu-live: keep the VkPipelineCache file in DIR (T1247, default the --gpu-replay directory, 'none' = off)\n");
    fprintf(stderr, "  --live-frame-hash FILE  with --gpu-live: write a sha256 line per frame handed to the window (T1246 equivalence evidence)\n");
    fprintf(stderr, "  --no-audio-pump        render the HLE mixer only inside the guest's DirectSoundDoWork (default: also from a 5 ms host pump, T1250 gaps)\n");
    fprintf(stderr, "  --audio-hold-ms N      with the WSOLA stretch: bridge a guest audio production gap by looping the last 13 ms for up to N ms (full gain 100 ms, then a linear decay), default 400, 0 = 100 ms plain loop (T1250 gaps)\n");
    fprintf(stderr, "  --audio-min-rate N     with --audio-sink sdl and --interactive: slowest playback rate in per mille of real time when the guest produces audio too slowly (T1250 default 250, 0 = off: pause and refill, every voice is cut for a refill)\n");
    fprintf(stderr, "  --audio-stretch MODE   how the audio slows down below real time: wsola (default, pitch preserving time stretch) or resample (T1248, the pitch follows the speed)\n");
    fprintf(stderr, "  --av-sync-offset-ms N  show the video N ms later than the audio clock (negative: earlier), compensates the audio device's own delay (default 0)\n");
    fprintf(stderr, "  --audio-stretch-legacy A/B: the pre T1487 stretch control (step ratio, 6.7 ms blocks)\n");
    fprintf(stderr, "  --audio-latency-ms N   with --audio-sink sdl and --interactive: hold the audio latency the host adds near N ms (default 80, 0 = off)\n");
    fprintf(stderr,
            "  --overlay-consume      opt in to T540: a completed coupled vblank retires one pending overlay buffer (inferred timing, not hardware captured)\n"
            "  --dump-overlay DIR     write each UpdateOverlay picture (png, raw Y U V planes, index) into the existing DIR, a pure observer; colour matrix UNMEASURED for the hardware\n"
            "  --present SINK         opt-in video sink: null, png-dir (needs --dump-overlay), window (SDL3 builds only, T760)\n"
            "  --present-hold-ms N    with --present: keep the window open N ms after the stop report (window only)\n"
            "  --present-timeline FILE  with --audio-sink sdl: write a CSV row every 25 ms (wall, modelled time, ring fill, thread CPU), T827\n"
            "  --present-no-pace      with --present window: free-run the modelled vblanks instead of holding them to wall-clock 59.94 Hz (movies then flash past)\n"
            "  --present-capture PATH with --present window: write the last window frame as a BMP at the stop\n"
            "  --audio-sink SINK      opt-in audio sink: null, wav-file (stereo 48 kHz PCM), sdl (SDL3 audio device, T761, needs SDL3)\n"
            "  --audio-mute           with --audio-sink sdl: play silence, the ring and the audio clock still run\n"
            "  --audio-output PATH    with wav-file, write PCM to PATH (default tsfp-audio.wav)\n"
            "  --dump-overlay-max N   stop writing overlay files after N updates, 0 is no limit\n"
            "  --overlay-xemu-image   opt in to T831: overlay pictures use the xemu matrix and bilinear scale (xemu-level, T770, not NV2A silicon; the XMV library matrix stays the default)\n"
            "  --overlay-xemu-key     opt in to T831: compose the overlay over the swap replay frame by the xemu key rules (xemu-level, T770, not NV2A silicon)\n"
            "  --eeprom-language N    answer ExQueryNonVolatileSetting index 7 (XGetLanguage) with N, 1 English 2 Japanese 3 German 4 French 5 Spanish 6 Italian (FABRICATED console setting)\n"
            "  --xmv-substitute A=B   when the title opens movie A open movie B from the same directory instead (FABRICATED test aid)\n"
            "  --dump-xmv-frames DIR  with --trace-xmv, write each decoded movie frame's Y, U, V planes into DIR\n"
            "  --dump-xmv-frames-max N  stop writing frame files after N (hash lines go on), 0 is no limit\n"
            "  --capture-xmv-entries DIR  T538: write the guest state at the codec wrapper 0x447E5E entry into DIR (needs --trace-xmv and a headless vblank policy, disc data)\n"
            "  --capture-xmv-entry-at LIST  which wrapper entries to capture, 0 based, e.g. 2,5-9 (up to 32 ranges)\n"
            "  --seed-xmv-entry N  T611: at wrapper entry N apply the --seed-xmv-patch edits, run the lifted wrapper, write the changed pages to --seed-xmv-result and exit (needs --trace-xmv and a headless vblank policy)\n"
            "  --seed-xmv-patch SPEC  one edit, dec+OFF=HEX, esp+OFF=HEX or dec@PTR+OFF=HEX (repeatable, up to 32)\n"
            "  --seed-xmv-result FILE  where the seeded run writes the pages it changed (disc data)\n"
            "  --headless-movie-audio bounded CPU model of the XMV movie PCM stream (default\n"
            "                         off); requires --native-xmv and --headless-streams.\n"
            "                         No audio is produced. Packets complete in DirectSoundDoWork\n"
            "                         on the virtual clock at the format byte rate.\n"
            "  --passive-audio-completion  T681: passive completion model of the title's\n"
            "                         stream and sound state machines (DEFAULT ON since T762 when\n"
            "                         the three headless audio policies are given, --no-passive-audio-completion\n"
            "                         opts out); requires\n"
            "                         --headless-streams --headless-buffers --headless-listener.\n"
            "                         Process packets and Play complete on the virtual clock,\n"
            "                         statuses are the measured original words, no audio is produced.\n"
            "  --synthetic-pad        T717 FABRICATED: port 0 reports one synthetic gamepad inserted\n"
            "                         (once) and XInputOpen/Close/GetCapabilities/GetState/SetState\n"
            "                         are answered (default off, empty ports). Pad at rest.\n"
            "  --synthetic-pad-remove-after-polls N  T731 FABRICATED: unplug the synthetic pad after\n"
            "                         the N-th XInputGetState (default off); needs --synthetic-pad.\n");
    fprintf(stderr,
            "  --pad-script FILE      T707 FABRICATED: per-frame pad states from FILE (lines of\n"
            "                         `<frames> [BUTTONS] [LX=n ...]`, see src/input/xinput_source.h),\n"
            "                         one entry per XInputGetState; needs --synthetic-pad.\n"
            "  --mu-image PORT SLOT FILE         Attach existing writable MU image (ports 0..3, slots 0..1);\n"
            "                                   explicit modeled presence, never creates/formats; writes on unmount\n"
            "  --controllers                    Four independent SDL gamepads (--present window); opt-in guest adapter\n"
            "  --controller-config FILE         Load persisted port assignment/remapping/calibration\n"
            "  --controller-mappings FILE       Load local SDL mappings for compatible unknown devices\n"
            "  --controller-save FILE           Atomically save effective controller configuration\n"
            "  --record-input FILE    T1074 FABRICATED: record port 0 pad states by poll index to FILE (needs\n"
            "                         --synthetic-pad; combine with --pad-source keyboard to record a session)\n"
            "  --stop-at-poll N       T1153 end the run when port 0 has been polled N times (deterministic cut)\n"
            "  --snapshot-at-poll N   T1153 take a whole-process DMTCP snapshot at the Nth port 0 poll (tools.snapshot)\n"
            "  --replay-input FILE    T1074 FABRICATED: replay a record, refused if the XBE or flag set differs\n"
            "  --pad-source script|keyboard|gamepad  T751 FABRICATED: pad source (needs --synthetic-pad);\n"
            "                         keyboard and gamepad need ONE event provider: --present window\n"
            "                         (SDL3 builds) or --pad-feed FILE (fake feed)\n"
            "                         (fake-feed events, see src/input/xinput_host_source.h).\n"
            "  --continue-on-missing  do not stop at the first unimplemented\n"
            "                         ordinal. Collects a longer trace, but every\n"
            "                         observation after the first stub is made on\n"
            "                         a guest that was handed a fabricated return\n"
            "                         value, so treat the tail as a hint only.\n"
            "  --xnet-scheduler      T1091 opt-in genuine queued DPC execution; elapsed coalescing INFERRED\n"
            "  --xnet-local-entropy  T1111 INFERRED opt-in local RNG/key generation;\n"
            "                         real OS entropy, original initialization guard;\n"
            "                         no XNET startup, connection or packets.\n"
            "  --xonline-offline     T904 INFERRED, opt-in no-live-account XONLINE policy:\n"
            "                         Startup succeeds; initialized GetUsers returns an\n"
            "                         empty list. No network service or account is faked.\n"
            "  --stub-status V        what an unimplemented ordinal returns under\n"
            "                         --continue-on-missing. The default, 0, reads as\n"
            "                         STATUS_SUCCESS to every ordinal returning an\n"
            "                         NTSTATUS, so the guest proceeds as though an\n"
            "                         out-parameter it never received were valid.\n"
            "                         0xC0000001 makes them report failure instead.\n"
            "                         Neither is true; the pair exists so the two\n"
            "                         paths can be compared rather than one assumed.\n"
            "  --trace N              report the first N ordinal calls (default %d)\n"
            "  --thread-timeout MS    how long to let guest threads run before\n"
            "                         declaring a hang (default %u, 0 means do not\n"
            "                         wait at all). A timeout is a REPORTED stop\n"
            "                         with a full trace, not a silent hang.\n",
            DEFAULT_TRACE_REPORT, DEFAULT_THREAD_TIMEOUT_MS);
    /* T371 options in their own call, same limit as above. */
    fputs("  --trace-vblank-readers print each counter getter poll and the guest thread census at\n"
          "                         each frame entry to stderr (default off, T371); requires a\n"
          "                         headless vblank policy.\n"
          "  --check-vblank-quiescence refuse a vblank delivery unless every other guest thread is\n"
          "                         terminated or parked, and refuse an unregistered counter reader\n"
          "                         (default off, T371); requires a headless vblank policy.\n"
          "  --vblank-owner-waits N after the second event deliver one callback inside each of the\n"
          "                         owner thread's next N completed vblank waits (default off,\n"
          "                         T460); requires --headless-second-vblank,\n"
          "                         --couple-vblank-effects and --check-vblank-quiescence.\n",
          stderr);
    fputs("  --vblank-worker-blanks N after --vblank-owner-waits, deliver one callback inside each of\n"
          "                         the loading bar worker's next N completed vblank waits while the\n"
          "                         owner is proven parked in the loading bar gate (default off, T592);\n"
          "                         requires --vblank-owner-waits. FABRICATED timing.\n"
          "  --vblank-poll-blank    with --vblank-owner-waits, a frame wait (0x1538C0) whose exit test\n"
          "                         fails by one blank delivers it from the poll (DEFAULT ON with\n"
          "                         --vblank-owner-waits since T762, --no-vblank-poll-blank opts out; T696,\n"
          "                         owner decision). Counts in the owner budget. FABRICATED timing.\n",
          stderr);
    fputs("  --gpu-replay-standin-texel-program HEX / --gpu-replay-standin-normalised-program HEX\n"
          "                         (repeatable, T719, INFERRED) choose the stand-in's unit PER\n"
          "                         DRAW by the vertex program digest prefix (8..64 lowercase\n"
          "                         hex). A stand-in draw whose program is in neither list\n"
          "                         refuses. Excludes --gpu-replay-standin-texel-units\n",
          stderr);
    fputs("  --gpu-replay-draw-dump FILE\n"
          "                         (T724) append one JSON line per replayed draw (program digest,\n"
          "                         t0 sampled, vertex attributes, constants) for\n"
          "                         tools.nv2a.standin_units. Observation only\n",
          stderr);
    /* A fourth call: the third literal is already at the 4095-byte -Woverlength-strings limit. */
    fputs("  --gpu-live-translate   needs --gpu-live: translate a vertex program or combiner\n"
          "                         configuration that is in no module table on demand into the\n"
          "                         --gpu-replay directory (T847, INFERRED translators; run from\n"
          "                         the repository root, needs python3 and glslangValidator)\n", stdout);
    fputs("  --gpu-replay DIR       OFF by default. At each present, decode the recorded\n"
          "                         pushbuffer commands and replay them on a Vulkan device\n"
          "                         (T84a). DIR holds the <module>.spv files\n"
          "                         (generated/shaders/vsh/spv). Strict: a method the\n"
          "                         decoder has not measured refuses the frame, loudly.\n"
          "                         Allows every named inference and announces them.\n"
          "  --gpu-replay-dump DIR  write frame_<n>.png of each replayed frame\n"
          "  --gpu-replay-size WxH  the frame size (default: the bound render target's)\n"
          "  --gpu-replay-lenient   log unhandled methods instead of refusing the frame\n"
          "  --gpu-replay-flip-y    reverse the rows of each replayed frame (T100f: which\n"
          "                         orientation the NV2A has is UNDECIDED, default off)\n"
          "  --gpu-replay-undo-viewport  DIR holds modules made with vsh_modules.py\n"
          "                         --undo-viewport (T96); selects nothing, states it\n"
          "  --gpu-replay-window-to-clip  DIR holds modules made with vsh_modules.py\n"
          "                         --window-to-clip (T714, xemu-level for none-class xy; z-only xy\n"
          "                         remains open, HQ21); selects\n"
          "                         nothing, states it. A draw without c58 is refused\n"
          "  --gpu-replay-viewport-from-target  infer whole-target c58/c59 from measured\n"
          "                         target dimensions (T477; opt-in, not NV2A evidence)\n"
          "  --gpu-replay-assume-program-mode  a stream that never wrote the execution mode\n"
          "                         (0x1E94) but started a vertex program is replayed as\n"
          "                         the program mode (T441, INFERRED, the title's case)\n"
          "  --gpu-replay-output-state  decode and apply every measured output-state group\n"
          "                         (T267: scissor, cull, blend, alpha test, depth, stencil,\n"
          "                         clear) and allow their named inferences (T441)\n"
          "  --gpu-replay-combiner  replace the fixed fragment stage with the register\n"
          "                         combiner the stream programmed (T478): reads the\n"
          "                         combiner_<sha256>.spv modules from the --gpu-replay\n"
          "                         DIR and allows the CONSTANT_BYTES and COLOUR_RANGE\n"
          "                         inferences. Textures, fog and a missing module refuse\n"
          "  --gpu-replay-standin-texture WxH:RRGGBBAA|WxH:checker  needs --gpu-replay-combiner: give\n"
          "                         stage 0 a solid colour or UV-visible coloured checker\n"
          "                         so a draw reading t0 is not refused (T497/T520).\n"
          "                         NOT the title's texture and announced as such\n"
          "                         (infers TEXTURE_SAMPLING)\n"
          "  --gpu-replay-standin-texel-units  needs --gpu-replay-standin-texture: sample the\n"
          "                         stand-in with oT0 in TEXELS (divided by its size, T713,\n"
          "                         INFERRED) so a 640x480 checker maps one to one\n"
          "  --gpu-replay-rt-texture  needs --gpu-replay-combiner and --gpu-replay-output-state:\n"
          "                         a texture stage bound to a render target an earlier pass\n"
          "                         of the same frame drew samples that pass's image (T510).\n"
          "                         Any other binding refuses by name. INFERRED sampling,\n"
          "                         announced. Not with the stand-in texture or --gpu-replay-flip-y\n"
          "  --gpu-replay-rt-texture-census  as --gpu-replay-rt-texture but CENSUS ONLY: replay no\n"
          "                         pixel, classify every draw (resolved source or refusal)\n"
          , stderr);
    fputs("  --gpu-replay-surface-source  needs --gpu-replay-rt-texture: a surface no pass of the\n"
          "                         frame drew is its kept image of an earlier frame, else the\n"
          "                         guest memory under it (MEASURED all zero, T633/T596,\n"
          "                         INFERRED), for textures and the CopyRects blits and byte path\n"
          "  --gpu-replay-target-persist  a render target pass starts from the image an earlier\n"
          "                         frame left under its Data word (T633, INFERRED); not with\n"
          "                         --gpu-replay-flip-y\n"
          "  --gpu-replay-dump-every N  with --gpu-replay-dump, write only the frames whose\n"
          "                         number is a multiple of N (T484, default every frame)\n"
          "  --gpu-replay-dump-last with --gpu-replay-dump, write the last replayed frame\n"
          "                         when the run ends (alone: only that one, T484)\n"
          "                         The device comes from $VKRUN_DEVICE (default: the first).\n",
          stderr);
    fputs("  --async-file-io        T743: DEFAULT ON since T762 (--no-async-file-io opts out). An overlapped NtReadFile (an Event on a\n"
          "                         handle opened without FILE_SYNCHRONOUS_IO) returns STATUS_PENDING and\n"
          "                         completes on the virtual clock (buffer, IoStatusBlock, Event), one\n"
          "                         request at a time. XEMU-LEVEL timing (T763), announced at startup. A blocking\n"
          "                         wait on a pending read (T764) advances the clock to its due time.\n"
          "  --async-file-io-spin-complete N  T821, OPT-IN, INFERRED: a guest thread that passes N cooperative\n"
          "                         safepoints (lifted calls) with no HLE dispatch of its own while an\n"
          "                         overlapped read is pending completes the earliest pending read, the virtual\n"
          "                         clock advancing to its due time and never past it. For a loader that polls\n"
          "                         the OVERLAPPED in guest memory with no vblank and no NtReadFile in the loop\n"
          "                         (the pack loader sub_00060050). Needs the async model and a headless\n"
          "                         vblank policy (the cooperative provider). 2048 is about 0.3 to 0.6 ms.\n"
          "  --async-file-io-file-object  T764: with --async-file-io, also queue an NtReadFile with NO\n"
          "                         Event (the title's loader helper and XMV reads) and signal the\n"
          "                         FILE OBJECT at completion. INFERRED, default off.\n",
          stderr);
    fputs("  --av-pack NAME         the AV pack the title is TOLD it has: composite,\n"
          "                         svideo or hdtv (default hdtv). FABRICATED and\n"
          "                         announced: there is no encoder. The video standard\n"
          "                         is derived from the XBE certificate region.\n",
          stderr);
    fputs("  --profile-calls        OFF by default (T422). Count every HLE dispatch per address\n"
          "                         and caller over the whole run, keep each guest thread's last\n"
          "                         dispatches and the call it is inside, and summarise every\n"
          "                         present (swap counter, vblank, command stream digest).\n"
          "                         Observation only, printed after the trace.\n"
          "  --profile-calls-top N  addresses listed (default 40)\n"
          "  --profile-watch ADDR   sample the guest dword at ADDR at every present (up to 8), and log\n"
          "                         the first 32 changes (present index -> value, T636)\n"
          "  --census-icalls        OFF by default (T821). READ-ONLY census of the lifted dispatcher's\n"
          "                         indirect call targets (every function pointer call and indirect\n"
          "                         tail jump): per target the total, the first present, thread and\n"
          "                         pushed return address, the last present and the calls per present.\n"
          "                         Observation only, not a model. Needs no --profile-calls.\n"
          "  --census-window F:L    with --census-icalls, also list every indirect call in order while\n"
          "                         the completed present count is in F..L (first 4096 kept)\n"
          "  --census-phases FILE   with --census-icalls (T1502): on SIGUSR1 write the census since the\n"
          "                         last mark to FILE.<phase> and reset it (FILE.pid, FILE.ack, control\n"
          "                         file FILE.phase), like --cpu-profile. tools.action_profile drives it.\n"
          "  --dump-guest-range A:N[,A:N..]  T1599, READ-ONLY (a poke needs --forced-state, T1613): dump guest memory ranges\n"
          "                         or, pointer-indirect (T1607), *A[+OFF]:N = N bytes at [A]+OFF (the\n"
          "                         dword at A is read at dump time, header lines 'pointer' and 'final',\n"
          "                         pointee anywhere in 32 bits, unreadable or wrapping is written, others continue)\n"
          "                         or, two level (T1759), **A+OFF1+OFF2:N = N bytes at [[A]+OFF1]+OFF2 (both offsets\n"
          "                         required, a null or unreadable pointer at either level is written, never followed)\n"
          "                         (start and end inside 0..0x03FFFFFF, no wrap, at most 16) as hex\n"
          "                         text to DIR/guestdump.<phase> on every SIGUSR1 (phase = first word\n"
          "                         of DIR/guestdump.phase, else 1, 2, 3...) and DIR/guestdump.exit at\n"
          "                         exit. Needs --dump-guest-dir DIR (gitignored place, e.g. under tmp/)\n"
          "                         and keeps one dump under --dump-guest-max-bytes N (default 1 MiB).\n"
          "  --forced-state         T1613, FABRICATED-STATE opt-in, needs --dump-guest-range: serve guarded pokes.\n"
          "                         A request file DIR/guestpoke.<label> (lines ADDR[+OFF]=VALUE[:W] or\n"
          "                         *ADDR[+OFF]=VALUE[:W], W = 1, 2 or 4, at most 16) is applied when the SIGUSR1\n"
          "                         phase <label> is served, just before that phase dump, which then shows the\n"
          "                         result. Refused (logged, nothing written) without this flag, outside 0..0x03FFFFFF,\n"
          "                         below 0x10000, in the XBE header/.text/.rdata/read-only sections, unaligned or\n"
          "                         if any poke of the request is refused. Read back after each write. Logged to\n"
          "                         DIR/guestpoke.log and the dump header, DIR/FORCED_STATE marks the run.\n"
          "                         Results from a poked run: INFERRED at most, never MEASURED, never proof.\n"
          "  --stop-after-calls A:N stop (HOST_STOP_BUDGET, exit 1) once XDK address A has been\n"
          "                         dispatched N times, or ord:K:N for kernel ordinal K. A cut by\n"
          "                         guest progress, so two boots compare. Needs --profile-calls.\n"
          "                         With --gpu-replay the per present frame summary and\n"
          "                         --profile-watch are off (T441): the replay owns the\n"
          "                         recorded commands, the call counting and the cut still run.\n",
          stderr);
    fputs("  (--forced-state, T1614) poke trigger SIGUSR2 (the SIGUSR1 phase also marks census phases, so a poke\n"
          "                         signal there would be a census boundary): kill -USR2 PID serves the request\n"
          "                         DIR/guestpoke.<label>, label = first word of DIR/guestpoke.phase, writes the dump\n"
          "                         DIR/guestdump.<label> and DIR/guestpoke.ack ('<n> <label>'). SIGUSR1 still serves\n"
          "                         guestpoke.<label> as before. No census phase is marked by SIGUSR2.\n",
          stderr);
    fputs(host_options_route_replay_help(), stderr);
    fputs(host_options_route_event_help(), stderr);
    fputs(host_options_route_nav_help(), stderr);
    fputs(host_options_hotkey_help(), stderr);
    fputs(host_options_button_dump_help(), stderr);
    fputs(host_options_watch_write_help(), stderr);
    fputs(host_options_voice_help(), stderr);
    fputs("  --passive-audio-completion-at-dowork  T855/T871: a packet past its deadline completes at the\n"
          "                         next DirectSoundDoWork (xemu-level), DEFAULT with the completion\n"
          "                         model. --no-passive-audio-completion-at-dowork gives the T681\n"
          "                         delivery at first observation.\n",
          stderr);
}

static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size <= 0) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    uint8_t *data = malloc((size_t)size);
    if (!data) {
        fclose(file);
        return NULL;
    }
    if (fread(data, 1, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "short read on %s\n", path);
        free(data);
        fclose(file);
        return NULL;
    }
    fclose(file);
    *out_len = (size_t)size;
    return data;
}

/*
 * Widest span of executable sections.
 *
 * The lifted code uses this to drop an indirect target that is not plausibly
 * code. Taking the min and max over every executable section pulls `.rdata` and
 * `.data` into the span, because DOLBY and XON_RD sit above them -- which
 * weakens the filter. That is the right trade here: a target this check wrongly
 * ADMITS is looked up, found missing, and stops the run with an exact address,
 * whereas one it wrongly REJECTS is also a stop but blames the wrong thing. Both
 * outcomes are now a hard stop, so the only thing left to optimise for is the
 * quality of the diagnosis.
 */
static void set_code_bounds(const xbe_image *image)
{
    uint32_t low = 0xFFFFFFFFu;
    uint32_t high = 0u;
    for (uint32_t i = 0; i < image->section_count; i++) {
        const xbe_section *section = &image->sections[i];
        if ((section->flags & XBE_SECTION_FLAG_EXECUTABLE) == 0u) {
            continue;
        }
        if (section->virtual_addr < low) {
            low = section->virtual_addr;
        }
        uint32_t end = section->virtual_addr + section->virtual_size;
        if (end > high) {
            high = end;
        }
    }
    if (high == 0u) {
        return;
    }
    g_xbox_code_lo = low;
    g_xbox_code_hi = high;
}

/* Allocate the guest stack and point esp at it, with the sentinel return address
 * already pushed. Returns false with a message on failure. */
static bool prepare_guest_stack(void)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.bytes = GUEST_STACK_BYTES;
    request.alignment = 0x10000u;
    request.protect = 0x04u; /* PAGE_READWRITE */
    request.state = 0x1000u; /* MEM_COMMIT */

    nt_status status = STATUS_SUCCESS;
    kernel_guest_ptr base = guest_region_alloc(&request, &status);
    if (base == 0u) {
        fprintf(stderr, "could not allocate a %u-byte guest stack (status 0x%08X)\n",
                GUEST_STACK_BYTES, (unsigned)status);
        return false;
    }

    /* Grows down, so start at the top. Leave a slot for the sentinel and keep the
     * result 16-byte aligned the way a real entry would be. */
    uint32_t top = base + GUEST_STACK_BYTES - 16u;
    g_esp = top;
    g_ebp = 0u;
    g_esp -= 4u;
    if (!kernel_guest_write_u32(g_esp, GUEST_SENTINEL_RETURN)) {
        fprintf(stderr, "could not write the sentinel return address at 0x%08X\n", g_esp);
        return false;
    }
    return true;
}

/* Only the opt-in startup callback uses this exclusive private guest region. */
#define FIRST_VBLANK_STACK_BYTES 0x10000u
static kernel_guest_ptr first_vblank_stack;
static bool second_vblank_policy;
static TSFP_RECOMP_TLS uint32_t callback_thread_handle;
static bool second_vblank_producer_confirmed(uint32_t handle,uint32_t fs)
{
    kernel_thread_record record;
    return kernel_thread_get(handle,&record) && record.in_use && record.started &&
           record.finished && record.terminated && record.termination_requested &&
           record.control_base==fs && record.system_routine==0x37FE1Du &&
           record.start_routine==0x156CB0u && record.start_context==0u;
}
/* T183: opt-in schedule record. Every field is guest progress or the virtual clock,
 * never wall time or an ASLR address, so two runs compare line by line. */
static bool vblank_schedule_trace;
static TSFP_RECOMP_TLS uint32_t schedule_safepoints;
/* T372: with --couple-vblank-effects every schedule line also carries the device's own vblank
 * count (device+0x1DE8), so the coupling is visible per event. Absent otherwise, which keeps the
 * T183 lines byte-identical with the flag off. */
static void schedule_device_count(char out[32])
{
    uint32_t count = 0u;
    out[0] = '\0';
    if (d3d8_vblank_effects_enabled() && kernel_guest_read_u32(0x3E3F60u + 0x1DE8u, &count))
        snprintf(out, 32, " devcount=%u", count);
}
static bool vblank_readers_trace;
static void trace_reader_census(const char *label, uint32_t caller);
static void second_vblank_wait_observer(bool success)
{
    if (success && vblank_readers_trace) {
        /* T460: the thread census at each owner wait that is about to deliver, observation only and
         * only with the owner-wait budget on, so the T371 records stay byte identical otherwise. */
        recomp_second_vblank_snapshot before;
        recomp_second_vblank_get_snapshot(&before);
        if (before.owner_budget != 0u && before.second_delivered) trace_reader_census("owner-wait", 0u);
    }
    if (vblank_schedule_trace) {
        char count[32], flip[512];
        schedule_device_count(count);
        d3d8_vblank_effects_trace(flip,sizeof flip);
        fprintf(stderr,"vblank-schedule: wait success=%d thread=0x%X clock=%llu%s%s\n",(int)success,
                callback_thread_handle,(unsigned long long)kernel_clock_peek(),count,flip);
    }
    recomp_second_vblank_note_wait_completed(success,callback_thread_handle,g_fs_base);
    if (vblank_schedule_trace) {
        /* T460: only with the owner-wait budget on, so the T183 lines stay byte identical otherwise. */
        recomp_second_vblank_snapshot snapshot;
        recomp_second_vblank_get_snapshot(&snapshot);
        /* T592: with the worker blanks on the line is the owner's, a worker wait has its own line below. */
        if (snapshot.owner_budget != 0u && snapshot.owner_delivered != 0u &&
            (snapshot.worker_budget == 0u || callback_thread_handle == snapshot.owner_handle)) {
            uint32_t counter = 0u, count = 0u, flip = 0u, delta = 0u, last = 0u;
            (void)kernel_guest_read_u32(0x563918u, &counter);
            (void)kernel_guest_read_u32(0x3E3F60u + 0x1DE8u, &count);
            (void)kernel_guest_read_u32(0x3E3F60u + 0x1DE4u, &flip);
            (void)kernel_guest_read_u32(0x3E3F60u + 0x1DFCu, &delta);
            (void)kernel_guest_read_u32(0x3E3F60u + 0x1E00u, &last);
            fprintf(stderr,"vblank-schedule: owner-wait delivered=%u of %u thread=0x%X counter=%u "
                    "record=%u,%u delta=0x%X last=0x%X\n",
                    (unsigned)snapshot.owner_delivered,(unsigned)snapshot.owner_budget,
                    callback_thread_handle,counter,count,flip,delta,last);
        }
        /* T592: one line per worker blank, only with the worker budget on. */
        static uint32_t worker_printed;
        if (snapshot.worker_budget != 0u && snapshot.worker_delivered != worker_printed) {
            uint32_t counter = 0u, count = 0u, flip = 0u, delta = 0u, last = 0u;
            worker_printed = snapshot.worker_delivered;
            (void)kernel_guest_read_u32(0x563918u, &counter);
            (void)kernel_guest_read_u32(0x3E3F60u + 0x1DE8u, &count);
            (void)kernel_guest_read_u32(0x3E3F60u + 0x1DE4u, &flip);
            (void)kernel_guest_read_u32(0x3E3F60u + 0x1DFCu, &delta);
            (void)kernel_guest_read_u32(0x3E3F60u + 0x1E00u, &last);
            fprintf(stderr,"vblank-schedule: worker-wait delivered=%u of %u thread=0x%X counter=%u "
                    "record=%u,%u delta=0x%X last=0x%X\n",
                    (unsigned)snapshot.worker_delivered,(unsigned)snapshot.worker_budget,
                    callback_thread_handle,counter,count,flip,delta,last);
        }
    }
}
static void second_vblank_registration_observer(uint32_t callback)
{
    if (vblank_schedule_trace) {
        char count[32];
        schedule_device_count(count);
        fprintf(stderr,"vblank-schedule: register callback=0x%08X thread=0x%X clock=%llu%s\n",callback,
                callback_thread_handle,(unsigned long long)kernel_clock_peek(),count);
    }
    recomp_second_vblank_note_registration(callback,callback_thread_handle,g_fs_base);
}
static void trace_schedule_point(const char *label, uint32_t callee)
{
    uint32_t counter = 0u, caller = 0u;
    (void)kernel_guest_read_u32(0x563918u, &counter);
    (void)kernel_guest_read_u32(g_esp, &caller);
    char devcount[32];
    schedule_device_count(devcount);
    fprintf(stderr,"vblank-schedule: %s callee=0x%08X thread=0x%X caller=0x%08X counter=%u safepoints=%u clock=%llu%s\n",
            label,callee,callback_thread_handle,caller,counter,schedule_safepoints,
            (unsigned long long)kernel_clock_peek(),devcount);
}
/* T371: counter-reader census and the quiescence check. Both are observation only: they read the
 * thread table and the guest counter and write nothing in the guest. */
static bool vblank_readers_trace;
static TSFP_RECOMP_TLS uint32_t reader_polls, reader_repeat, reader_last_counter;
static void trace_reader_census(const char *label, uint32_t caller)
{
    kernel_thread_record records[KERNEL_THREAD_MAX];
    const unsigned count = kernel_thread_snapshot(records, KERNEL_THREAD_MAX);
    uint32_t counter = 0u;
    (void)kernel_guest_read_u32(0x563918u, &counter);
    fprintf(stderr,"vblank-readers: %s thread=0x%X caller=0x%08X counter=%u threads=%u\n",label,
            callback_thread_handle,caller,counter,count);
    for (unsigned i = 0u; i < count; i++) {
        char text[96];
        recomp_vblank_quiescence_describe(&records[i],text,sizeof text);
        fprintf(stderr,"vblank-readers:   %s%s\n",text,
                records[i].handle == callback_thread_handle ? " (this thread)" : "");
    }
}
/* Sampled at every call boundary of the calling thread: a line each time the set of guest thread
 * states it sees changes. Which safepoint sees a change first is host timing, so these lines carry
 * their own prefix and are measurement only, never part of the compared record. */
static TSFP_RECOMP_TLS char sample_last[256];
static TSFP_RECOMP_TLS uint32_t sample_safepoints;
static void trace_reader_sample(void)
{
    kernel_thread_record records[KERNEL_THREAD_MAX];
    const unsigned count = kernel_thread_snapshot(records, KERNEL_THREAD_MAX);
    char now[256] = "";
    for (unsigned i = 0u; i < count; i++) {
        const size_t used = strlen(now);
        snprintf(now + used, sizeof now - used, " 0x%X:%s", (unsigned)records[i].handle,
                 recomp_vblank_quiescence_state_name(recomp_vblank_quiescence_classify(&records[i])));
    }
    if (strcmp(now, sample_last) != 0) {
        fprintf(stderr,"vblank-readers-sample: thread=0x%X safepoint=%u threads=[%s ]\n",
                callback_thread_handle,sample_safepoints,now);
        memcpy(sample_last, now, sizeof now);
    }
}
static void vblank_reader_hook(uint32_t callee)
{
    uint32_t caller = 0u;
    if (vblank_readers_trace) { sample_safepoints++; trace_reader_sample(); }
    (void)kernel_guest_read_u32(g_esp, &caller);
    if (callee == RVQ_GETTER) {
        kernel_thread_record record;
        const bool known = kernel_thread_get(callback_thread_handle,&record);
        recomp_vblank_quiescence_check_getter(callback_thread_handle,caller);
        if (vblank_readers_trace) {
            uint32_t counter = 0u;
            (void)kernel_guest_read_u32(0x563918u, &counter);
            reader_repeat = (reader_polls != 0u && counter == reader_last_counter) ? reader_repeat + 1u : 0u;
            reader_last_counter = counter;
            reader_polls++;
            /* The other threads' states at this poll, in creation order. */
            kernel_thread_record records[KERNEL_THREAD_MAX];
            const unsigned count = kernel_thread_snapshot(records, KERNEL_THREAD_MAX);
            char others[256] = "";
            for (unsigned i = 0u; i < count; i++) {
                if (records[i].handle == callback_thread_handle) continue;
                const size_t used = strlen(others);
                snprintf(others + used, sizeof others - used, " 0x%X:%s", (unsigned)records[i].handle,
                         recomp_vblank_quiescence_state_name(recomp_vblank_quiescence_classify(&records[i])));
            }
            fprintf(stderr,"vblank-readers: getter thread=0x%X start=0x%X caller=0x%08X counter=%u poll=%u repeat=%u registered=%d checks=%u others=[%s ]\n",
                    callback_thread_handle,known ? record.start_routine : 0u,caller,counter,reader_polls,
                    reader_repeat,(int)(known && recomp_vblank_quiescence_is_registered_reader(caller,record.start_routine)),
                    recomp_vblank_quiescence_getter_checks(),others);
        }
    } else if (callee == 0x1538C0u) {
        if (vblank_readers_trace) trace_reader_census("frame-entry",caller);
        const rvq_verdict verdict = recomp_vblank_quiescence_check(callback_thread_handle,0x1538C0u);
        if (vblank_readers_trace && recomp_vblank_quiescence_enabled())
            fprintf(stderr,"vblank-readers: quiescence holds=%d examined=%u thread=0x%X\n",(int)verdict.holds,
                    verdict.examined,callback_thread_handle);
    }
}
static char g_poll_limit_detail[64];
static void stop_at_poll_limit(uint64_t polls)
{
    snprintf(g_poll_limit_detail, sizeof g_poll_limit_detail, "poll limit %llu of port 0 reached (--stop-at-poll)",
             (unsigned long long)polls);
    host_run_stop(HOST_STOP_HOST_SHUTDOWN, 0u, 0u, g_poll_limit_detail);
}
/* T1633: the route probe's guest memory reads (the sampler thread and --route-log-mem) use the same path as the dumps. */
static bool route_probe_read_guest(uint32_t address, unsigned width, uint32_t *value, void *user)
{
    (void)user;
    uint8_t raw[4] = {0u, 0u, 0u, 0u};
    if (width == 0u || width > 4u || !kernel_guest_read_bytes(address, raw, width)) return false;
    *value = (uint32_t)raw[0] | ((uint32_t)raw[1] << 8) | ((uint32_t)raw[2] << 16) | ((uint32_t)raw[3] << 24);
    return true;
}
/* T1633: a cheap signature of the presented frame, the GPU command count recorded for it (a static screen repeats it, an
 * animated one varies). EXPERIMENTAL and weak: see docs/input-replay.md "Event driven route". */
static void route_frame_observer(const d3d8_frame_record *record)
{
    static uint64_t previous_commands;
    const uint64_t delta = record->stream_commands - previous_commands;
    previous_commands = record->stream_commands;
    route_probe_note_frame(delta * 1000003u + record->width * 4099u + record->height);
}
/* T1616: a route wait that never held ends the run with the reason (FABRICATED route replay, host_route.c). */
static char g_route_failure_detail[800];
static void route_failure_stop(const char *reason)
{
    snprintf(g_route_failure_detail, sizeof g_route_failure_detail, "%s", reason);
    host_run_stop(HOST_STOP_HOST_SHUTDOWN, 0u, 0u, g_route_failure_detail);
}
static bool interactive_play;
static bool interactive_async_io; /* Immutable after startup; evidence/default runs remain virtual-only. */
static atomic_bool interactive_shutdown;
static atomic_bool interactive_user_close;
static uint64_t interactive_wall_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}
static bool interactive_closed(void);
static const char *interactive_close_detail(void);
static void first_vblank_provider_body(uint32_t callee);
static FILE *g_voice_log;
static FILE *g_voice_resident;
static pthread_mutex_t g_voice_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_voice_descriptors[12];
static uint64_t voice_audio_frames(void);
static void voice_vblank_provider(uint32_t callee, void *userdata);

/* T1289: the guest frame trace times the cooperative safepoint (async io service, vblank poll, second observer). */
static void first_vblank_provider(uint32_t callee, void *userdata)
{
    (void)userdata;
    gft_count(GFT_SAFEPOINT); /* about 30 thousand per frame: counted, and only the parts that can be slow are timed below */
    route_probe_note_call(callee); /* T1633: one atomic load unless a route wait names a guest function */
    first_vblank_provider_body(callee);
}

static void voice_vblank_provider(uint32_t callee, void *userdata)
{
    if (voice_observer_target(callee)) {
        pthread_mutex_lock(&g_voice_lock);
        voice_observer_dump(g_voice_resident, g_voice_descriptors, kernel_guest_read_bytes);
        voice_observer_note(g_voice_log, callee, g_esp, g_ecx, g_eax, kernel_clock_peek(),
                            kernel_clock_frequency(), voice_audio_frames(), kernel_guest_read_bytes);
        pthread_mutex_unlock(&g_voice_lock);
    }
    first_vblank_provider(callee, userdata);
}

static void first_vblank_provider_body(uint32_t callee)
{
    if (interactive_play && interactive_closed())
        host_run_stop(HOST_STOP_HOST_SHUTDOWN, callee, 0u,
                      atomic_load(&interactive_user_close) ? interactive_close_detail() :
                      "interactive cancellation after another guest thread stopped");
    if (interactive_async_io && !kernel_async_io_idle()) { /* T1289: nothing queued, nothing to service (and no clock read) */
        gft_enter(GFT_SAFEPOINT, callee);
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            host_run_stop(HOST_STOP_HOST_SHUTDOWN, callee, 0u, "interactive monotonic clock unavailable");
        const uint64_t nanoseconds = (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
        (void)kernel_async_io_service_elapsed(nanoseconds);
        gft_leave();
    }
    async_io_spin_note();
    xmv_original_note_call(callee);
    if (vblank_readers_trace || recomp_vblank_quiescence_enabled()) vblank_reader_hook(callee);
    if (vblank_schedule_trace) {
        schedule_safepoints++;
        /* 0x1538C0 is the title frame wait, 0x22030 its counter getter. */
        if (callee == 0x1538C0u || callee == 0x22030u) trace_schedule_point("safepoint",callee);
    }
    if (callee == 0x1538C0u) gft_enter(GFT_SAFEPOINT, callee); /* the title frame wait: where a blank may be delivered */
    if (second_vblank_policy) {
        recomp_second_vblank_note_call(callee,callback_thread_handle);
        recomp_second_vblank_poll(callee,callback_thread_handle,g_fs_base,g_esp,
                                  kernel_sync_current_irql());
    }
    else
        d3d8_first_vblank_poll(callee, g_fs_base, g_esp, kernel_sync_current_irql());
    if (callee == 0x1538C0u) gft_leave();
    if (vblank_schedule_trace && callee == 0x1538C0u) trace_schedule_point("after-poll",callee);
    if (vblank_readers_trace && callee == 0x1538C0u) {
        uint32_t caller = 0u;
        (void)kernel_guest_read_u32(g_esp, &caller);
        trace_reader_census("after-poll",caller);
    }
}

static void host_xnet_frame_service(void)
{
    kernel_async_io_service_hook();
    host_xnet_dpc_service_hook();
}

static bool prepare_first_vblank(bool second)
{
    guest_region_request request = {0};
    request.bytes = FIRST_VBLANK_STACK_BYTES;
    request.alignment = 0x1000u;
    request.protect = 0x04u; /* PAGE_READWRITE */
    request.state = 0x1000u; /* MEM_COMMIT */
    nt_status status = STATUS_SUCCESS;
    first_vblank_stack = guest_region_alloc(&request, &status);
    if (first_vblank_stack == 0u ||
        (uint64_t)first_vblank_stack + FIRST_VBLANK_STACK_BYTES > UINT32_MAX) {
        if (first_vblank_stack != 0u) {
            (void)guest_region_free(first_vblank_stack);
        }
        first_vblank_stack = 0u;
        fprintf(stderr, "could not allocate private startup callback stack (status 0x%08X)\n",
                (unsigned)status);
        return false;
    }
    second_vblank_policy=second;
    if (second) {
        if (!recomp_second_vblank_configure(true,recomp_callback_run,first_vblank_stack,
                first_vblank_stack+FIRST_VBLANK_STACK_BYTES,second_vblank_producer_confirmed))
            return false;
        d3d8_gpu_set_vblank_observer(second_vblank_wait_observer);
        d3d8_callbacks_set_vblank_observer(second_vblank_registration_observer);
    } else {
        d3d8_first_vblank_configure(true, recomp_callback_run, first_vblank_stack,
                                    first_vblank_stack + FIRST_VBLANK_STACK_BYTES);
    }
    if (!recomp_cooperative_configure(g_voice_log != NULL ? voice_vblank_provider : first_vblank_provider, NULL)) {
        abort();
    }
    return true;
}

static bool cleanup_first_vblank(void)
{
    /* Only after every guest thread joined; watchdog exits before reaching here. */
    if (!recomp_cooperative_configure(NULL, NULL)) {
        abort();
    }
    d3d8_gpu_set_vblank_observer(NULL);
    d3d8_callbacks_set_vblank_observer(NULL);
    if (!recomp_second_vblank_configure(false,NULL,0u,0u,NULL) ||
        !recomp_second_vblank_reset()) abort();
    second_vblank_policy=false;
    d3d8_first_vblank_configure(false, NULL, 0u, 0u);
    d3d8_first_vblank_reset();
    if (first_vblank_stack == 0u) {
        return true;
    }
    bool okay = guest_region_free(first_vblank_stack);
    if (okay) {
        first_vblank_stack = 0u;
    } else {
        fprintf(stderr, "startup callback stack cleanup refused; region preserved\n");
    }
    return okay;
}

/*
 * Allocate and initialise one thread's KPCR, and point `g_fs_base` at it.
 *
 * The guest reads `fs:[0x20]`, `fs:[0x24]`, `fs:[0x28]` and `fs:[0x58]`, and with a
 * zero `fs` base those are dereferences of 0x20..0x58 -- which is exactly where an
 * earlier bring-up run was measured to fault. `kernel_thread.c` owns the layout and
 * the evidence for it; this just gives the MAIN host thread the same thing a guest
 * thread gets at creation, so the two are not subtly different environments.
 */
static bool prepare_main_thread_control(void)
{
    guest_region_request request;
    memset(&request, 0, sizeof(request));
    request.protect = PAGE_READWRITE;
    request.state = MEM_COMMIT;

    nt_status status = STATUS_SUCCESS;
    request.bytes = KERNEL_THREAD_CONTROL_BYTES;
    kernel_guest_ptr control = guest_region_alloc(&request, &status);
    if (control == 0u) {
        return false;
    }
    /* The main thread's TLS size is not known here -- the guest computes it inside
     * the entry point -- so one page, which is 200x the 20 bytes this title's TLS
     * directory works out to. */
    request.bytes = GUEST_PAGE_SIZE;
    kernel_guest_ptr tls = guest_region_alloc(&request, &status);
    if (tls == 0u) {
        (void)guest_region_free(control);
        return false;
    }
    /* The Prcb monitor block, in its own region exactly as a guest thread's is. The
     * main thread gets one for the same reason it gets a KPCR at all: the two
     * environments must not differ, or a bug reproduces on one thread and not the
     * other and the difference is invisible. */
    request.bytes = KERNEL_THREAD_MONITOR_BYTES;
    kernel_guest_ptr monitor = guest_region_alloc(&request, &status);
    if (monitor == 0u) {
        (void)guest_region_free(control);
        (void)guest_region_free(tls);
        return false;
    }
    if (!kernel_thread_control_init(control, tls, monitor,
                                    KERNEL_THREAD_MONITOR_BYTES)) {
        (void)guest_region_free(control);
        (void)guest_region_free(tls);
        (void)guest_region_free(monitor);
        return false;
    }
    g_fs_base = control;
    return true;
}

/*
 * --- publishing IRQL into the guest's own copy ----------------------------
 *
 * `kernel_sync.c` tracks the level per thread and calls this on every change. It
 * cannot write the guest's copy itself: the copy lives at `KPCR.Irql`, the KPCR is
 * found through the `fs` base, and the `fs` base is `g_fs_base` in the LIFTED
 * runtime -- which `src/xbox/` deliberately does not depend on. So the sink is
 * installed from here.
 *
 * WHY IT MUST READ `g_fs_base` EVERY TIME rather than capture it at installation.
 * `g_fs_base` is `RECOMP_TLS`, and `host_thread_enter` gives each guest thread its
 * own. A captured base would send every thread's raises into the KPCR of whichever
 * thread happened to install the hook, which is worse than publishing nothing: the
 * other thread would then read a level it never set.
 */
static unsigned long g_irql_publish_count;
static uint32_t g_irql_publish_peak;
static unsigned long g_irql_publish_failures;
static pthread_mutex_t g_irql_publish_lock = PTHREAD_MUTEX_INITIALIZER;

static void publish_guest_irql(uint32_t level)
{
    /* Read-back verification, not decoration. A write that lands somewhere the guest
     * does not read is indistinguishable from no wiring at all, and this project has
     * been bitten before by a handler that was correct and ignored. */
    bool ok = false;
    const uint32_t fs_base = g_fs_base;
    if (fs_base != 0u && kernel_thread_set_irql(fs_base, level)) {
        uint8_t written = 0u;
        ok = kernel_guest_read_u8(fs_base + KERNEL_PCR_IRQL, &written) &&
             written == (uint8_t)(level & 0xFFu);
    }

    pthread_mutex_lock(&g_irql_publish_lock);
    if (ok) {
        g_irql_publish_count++;
        if (level > g_irql_publish_peak) {
            g_irql_publish_peak = level;
        }
    } else {
        g_irql_publish_failures++;
    }
    pthread_mutex_unlock(&g_irql_publish_lock);
}

static void report_irql_publishing(void)
{
    pthread_mutex_lock(&g_irql_publish_lock);
    const unsigned long count = g_irql_publish_count;
    const uint32_t peak = g_irql_publish_peak;
    const unsigned long failures = g_irql_publish_failures;
    pthread_mutex_unlock(&g_irql_publish_lock);

    host_report_irql_publishing(stdout, count, peak, failures);
}

/* The EEPROM block: gather what kernel_config counted, format in host_report.c. */
static void report_non_volatile_settings(void)
{
    uint32_t indices[HOST_REPORT_CONFIG_INDEX_MAX];
    const unsigned total =
        kernel_config_queried_indices(indices, HOST_REPORT_CONFIG_INDEX_MAX);
    host_report_non_volatile_settings(stdout, indices, total,
                                      kernel_config_served_count(),
                                      kernel_config_refused_count(),
                                      kernel_config_fabricated_count());
}

/* The file-I/O block: every counter is read here, in one place, and handed over. */
static void report_file_attempts(void)
{
    const host_report_file_counts counts = {
        .total = kernel_file_attempt_count(),
        .refused = kernel_file_refused_count(),
        .relative_refused = kernel_file_relative_refused_count(),
        .fabricated = kernel_file_fabricated_count(),
        .escape_refused = kernel_file_escape_refused_count(),
        .disc_opened = kernel_file_disc_opened_count(),
        .disc_bytes_read = (unsigned long long)kernel_file_disc_bytes_read(),
        .host_opened = kernel_file_host_opened_count(),
        .host_bytes_read = (unsigned long long)kernel_file_host_bytes_read(),
        .created = kernel_file_created_count(),
        .bytes_written = (unsigned long long)kernel_io_bytes_written(),
        .write_count = kernel_io_write_count(),
        .write_refused = kernel_io_write_refused_count(),
    };
    host_report_file_attempts(stdout, &counts, kernel_file_attempt_at);
}

static void report_symbolic_links(void)
{
    host_report_symbolic_links(stdout, kernel_file_symlink_count(),
                               kernel_file_symlink_target);
}

/* An XDK handler reached a state it will not guess at: end the run AT the guest address, with the
 * handler's own explanation as the detail, exactly as an unimplemented address does. */
static void host_d3d8_fatal(uint32_t guest_address, const char *message)
{
    host_run_stop(HOST_STOP_XDK_UNIMPLEMENTED, guest_address, 0u, message);
}

/* A kernel handler reached a guest-fatal state (KeBugCheck, or a raise nothing
 * dispatches). End the run AT the ordinal with the handler's explanation: on
 * hardware neither call continues, so neither may this host. */
static void host_kernel_fatal(unsigned ordinal, const char *detail)
{
    host_run_stop(HOST_STOP_KERNEL_FATAL, 0u, ordinal, detail);
}

/* Audio wrappers read the calling thread's published guest IRQL byte.
 * Sample the thread-local FS base on every call. */
static bool host_audio_irql(uint8_t *level)
{
    return g_fs_base != 0u &&
        kernel_guest_read_u8(kernel_guest_add(g_fs_base, KERNEL_PCR_IRQL), level);
}

static bool host_audio_control_word(uint16_t *word)
{*word=g_fp_control_word;return true;}

/* The movie stream completion callback (T392): the guest function the XMV library registered,
 * run on the calling guest thread with (stream context, packet context, status). */
static present_audio_sink *g_audio_sink;
/* T819: the virtual clock in nanoseconds (INFERRED timeline), the stamp the audio clock and the pictures share. */
static uint64_t host_virtual_ns(void)
{
    const uint64_t ticks = kernel_clock_peek();
    return (ticks / KERNEL_CLOCK_FREQUENCY_HZ) * 1000000000ull +
           (ticks % KERNEL_CLOCK_FREQUENCY_HZ) * 1000000000ull / KERNEL_CLOCK_FREQUENCY_HZ;
}
static present_audio_kind g_audio_kind = PRESENT_AUDIO_NONE;
static pthread_mutex_t g_audio_work_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t voice_audio_frames(void)
{
    pthread_mutex_lock(&g_audio_work_lock);
    const uint64_t frames = present_audio_sink_frames(g_audio_sink);
    pthread_mutex_unlock(&g_audio_work_lock);
    return frames;
}
/* T681: DirectSoundDoWork observes the passive completion model first (a no-op while it is off), then the movie
 * stream route decides exactly as before. */
/* Renders the mixer up to the virtual clock into the sink. Caller holds g_audio_work_lock. */
static bool host_audio_render_locked(uint64_t *frames_total)
{
    int16_t samples[2048u * 2u];
    size_t frames = 0u;
    do {
        if (!dsound_audio_runtime_render(kernel_clock_peek(), samples, 2048u, &frames)) {
            fprintf(stderr, "audio mixer failed while rendering virtual-clock PCM\n");
            return false;
        }
        if (frames != 0u && !present_audio_sink_write_at(g_audio_sink, samples, frames, host_virtual_ns())) {
            fprintf(stderr, "audio sink failed while writing %s PCM\n", present_audio_name(g_audio_kind));
            return false;
        }
        if (frames_total != NULL) {
            *frames_total += frames;
        }
    } while (frames == 2048u);
    return true;
}

static bool host_audio_work_route_body(uint32_t return_address);

/* T1289: the guest frame trace times the audio work route (the mixer renders on the guest thread). */
static bool host_audio_work_route(uint32_t return_address)
{
    gft_enter(GFT_AUDIO, 0u);
    const bool good = host_audio_work_route_body(return_address);
    gft_leave();
    return good;
}

static bool host_audio_work_route_body(uint32_t return_address)
{
    dsound_completion_work();
    if (dsound_audio_runtime_active()) {
        pthread_mutex_lock(&g_audio_work_lock);
        const bool rendered = host_audio_render_locked(NULL);
        pthread_mutex_unlock(&g_audio_work_lock);
        if (!rendered) {
            return false;
        }
    }
    return dsound_movie_stream_route_work(return_address);
}

/* T1250 gaps: the mixer is otherwise rendered ONLY inside the guest's DirectSoundDoWork. Story's loading screen spins the
 * main thread (sub_00156840, 0.35 to 0.95 s) without calling it, so the already submitted audio of the gap was rendered
 * in one burst after the spin and the sink ran dry (a cut of every voice) although the stream data existed. On the console
 * the APU mixes the queued packets by itself, so a host pump renders up to the virtual clock every 5 ms of wall time. */
static pthread_t g_audio_pump_thread;
static atomic_bool g_audio_pump_run;
static bool g_audio_pump_started;
static atomic_ullong g_audio_pump_frames;
static atomic_ullong g_audio_pump_renders;

static void *host_audio_pump_main(void *argument)
{
    (void)argument;
    while (atomic_load(&g_audio_pump_run)) {
        const struct timespec nap = {0, 5000000L};
        nanosleep(&nap, NULL);
        if (!dsound_audio_runtime_active() || pthread_mutex_trylock(&g_audio_work_lock) != 0) {
            continue;
        }
        uint64_t frames = 0u;
        (void)host_audio_render_locked(&frames);
        pthread_mutex_unlock(&g_audio_work_lock);
        if (frames != 0u) {
            atomic_fetch_add(&g_audio_pump_frames, frames);
            atomic_fetch_add(&g_audio_pump_renders, 1u);
        }
    }
    return NULL;
}

static void host_audio_pump_stop(void)
{
    if (g_audio_pump_started) {
        atomic_store(&g_audio_pump_run, false);
        pthread_join(g_audio_pump_thread, NULL);
        g_audio_pump_started = false;
    }
}
static bool host_movie_stream_callback(uint32_t callback, uint32_t stream_context,
                                       uint32_t packet_context, uint32_t status)
{
    const uint32_t arguments[3] = {stream_context, packet_context, status};
    return recomp_guest_call_stdcall(callback, arguments, 3u);
}

/*
 * Adopt the measured XDK call surface, so the four address-keyed HLE modules become
 * reachable from lifted code. Returns how many addresses were adopted.
 *
 * ONE PLACE NAMES THE GENERATED SYMBOL. `xdk_surface` is produced by
 * tools/gen_d3d8_surface.py from the user's own executable and is gitignored, so
 * everything downstream of it takes its table by injection. This is the one function
 * that has the generated array in scope, which is why the translation from section
 * string to module happens here -- through `xdk_module_for_section`, so the mapping
 * itself still lives in exactly one place and cannot drift between the two.
 */
/* Compiled manual wrappers must cover the adopted audio surface before any
 * passive opt-in can expose omitted original child state. No guest state exists
 * yet when main queries this; extra vtable stops are checked independently. */
static bool passive_audio_dispatch_ready(void)
{
#if defined(TSFP_HAVE_XDK_SURFACE)
    uint32_t addresses[XDK_SURFACE_COUNT];
    size_t count = 0u;
    for (size_t i = 0u; i < (size_t)XDK_SURFACE_COUNT; i++)
        if (xdk_module_for_section(xdk_surface[i].section) == XDK_MODULE_DSOUND)
            addresses[count++] = xdk_surface[i].address;
    return xdk_thunk_dispatch_boundaries_ready(addresses, count);
#else
    return false;
#endif
}

/* WSAGetLastError (0x431D53) is a bare jump to GetLastError 0x37E9A7: [[fs:4] + [0x771368]*4] + 4.
 * The fs base is this thread's g_fs_base, so the XNET module reads the slot through this reader. */
static bool host_xnet_last_error(uint32_t *last_error)
{
    uint32_t index = 0u, array = 0u, block = 0u;
    if (g_fs_base == 0u || !kernel_guest_read_u32(0x00771368u, &index) ||
        !kernel_guest_read_u32(g_fs_base + 4u, &array) ||
        !kernel_guest_read_u32(array + index * 4u, &block))
        return false;
    return kernel_guest_read_u32(block + 4u, last_error);
}

static size_t adopt_xdk_surface(void)
{
#if defined(TSFP_HAVE_XDK_SURFACE)
    /* Each module owns its own table; init puts every row back to an unreported stub
     * so a run's report is about this run. */
    dsound_hle_init();
    dsound_listener_reset();
    dsound_buffer_reset();
    dsound_stream_reset();
    dsound_effects_binding_reset();
    dsound_device_reset();
    xinput_hle_init();
    xinput_devices_reset();
    if (!xgrph_hle_adopt((const xgrph_xdk_row *)xdk_surface, XDK_SURFACE_COUNT)) {
        fprintf(stderr, "warning: XGRPH measured surface could not be adopted\n");
    }
    if (!d3d8_surface_adopt((const d3d8_xdk_row *)xdk_surface, XDK_SURFACE_COUNT,
                            D3D8_SECTION_D3D)) {
        fprintf(stderr, "warning: the D3D rows of the measured surface could not be\n"
                        "         adopted, so no D3D address is reachable\n");
    }

    xdk_dispatch_entry *rows = calloc(XDK_SURFACE_COUNT, sizeof(*rows));
    if (!rows) {
        fprintf(stderr, "warning: out of memory adopting the XDK surface\n");
        return 0;
    }
    for (size_t i = 0; i < (size_t)XDK_SURFACE_COUNT; i++) {
        rows[i].address = xdk_surface[i].address;
        rows[i].name = xdk_surface[i].name;
        rows[i].module = xdk_module_for_section(xdk_surface[i].section);
    }
    const bool adopted = xdk_thunk_init(rows, (size_t)XDK_SURFACE_COUNT);
    /* The rows are copied, and the `name` pointers are borrowed from the generated
     * table, which is static storage and outlives everything. */
    free(rows);
    return adopted ? xdk_thunk_count() : 0u;
#else
    fprintf(stderr, "note: src/xbox/xdk_surface.c was not generated, so the XDK address\n"
                    "      boundary is not adopted and no D3D/DSOUND/XAPI address is\n"
                    "      reachable. Generate it with:\n"
                    "      uv run python -m tools.gen_d3d8_surface <your.xbe> "
                    "--xtlid xtlid.xml\n");
    return 0u;
#endif
}

/*
 * Declare the measured XDK ABIs (tools/xdk_abi.py, src/xbox/xdk_abi.inc). Must follow
 * `adopt_xdk_surface`, because adopting a surface drops every declared ABI.
 *
 * A refusal is a table that has drifted from the surface it is keyed by (or a row the
 * quorum rejected), so it is printed beside the count rather than folded into it. With no
 * generated table the line reads "0 of N", and every address keeps stopping for want of
 * an ABI, which is the pre-existing behaviour.
 */
static void declare_xdk_abis(void)
{
    if (xdk_thunk_count() == 0u) {
        return;
    }
    size_t refused = 0;
    const size_t declared = xdk_thunk_declare_generated_abis(&refused);
    printf("xdk abi        %zu of %zu declared (%zu rows in the generated table, %zu "
           "refused)\n",
           declared, xdk_thunk_count(), xdk_thunk_generated_abi_count(), refused);
    /* T1163/T904: exact 111-byte retail Cleanup proof in tools/xdk_abi.py
     * establishes a bare RET with no incoming arguments. Two callers cannot
     * satisfy the generated ABI quorum; declare only this proven entry. */
    const bool cleanup_abi =
        xdk_thunk_declare_abi(XONLINE_CLEANUP_ENTRY, XDK_CC_CDECL, 0u, 0u);
    printf("xdk XOnlineCleanup original-established ABI: %s (no arguments)\n",
           cleanup_abi ? "declared" : "refused");
    /* T998/T1008: Begin has a bare RET and only one caller, so the generated
     * caller-vote quorum intentionally withholds it. Original-byte execution and
     * complete body inspection establish zero incoming stack/register arguments;
     * this is the explicit hand-established ABI path, not a lowered quorum. */
    const bool visibility_abi = xdk_thunk_declare_abi(0x003D4250u, XDK_CC_STDCALL, 0u, 0u);
    printf("xdk visibility original-established Begin ABI: %s (T1008, no stack/register arguments)\n",
           visibility_abi ? "declared" : "refused");
    /* T1067: this vtable-only XGRPH member has no direct-call sites for the
     * generated ABI quorum. The retail body is exactly `mov [ecx], 0x004A1BD0; ret`,
     * so this explicit declaration carries one ECX `this` and no stack arguments. */
    const bool compmask_abi = xdk_thunk_declare_abi(0x003EF236u, XDK_CC_THISCALL, 0u, 1u);
    printf("xdk XGRPH 0x003EF236 original-established ABI: %s (one ECX object, no stack arguments)\n",
           compmask_abi ? "declared" : "refused");
    /* The two additional original vtable resets at 0x003EF3B7 and 0x004029BD have
     * the same register-only ABI, established by their exact store-and-return bodies. */
    const bool secondary_vtable_abi =
        xdk_thunk_declare_abi(0x003EF3B7u, XDK_CC_THISCALL, 0u, 1u);
    const bool third_vtable_abi = xdk_thunk_declare_abi(0x004029BDu, XDK_CC_THISCALL, 0u, 1u);
    printf("xdk XGRPH 0x003EF3B7 original-established ABI: %s (one ECX object, no stack arguments)\n",
           secondary_vtable_abi ? "declared" : "refused");
    printf("xdk XGRPH 0x004029BD original-established ABI: %s (one ECX object, no stack arguments)\n",
           third_vtable_abi ? "declared" : "refused");
}

/* --- the host side of the thread model ----------------------------------- */

/*
 * Where each guest thread stopped.
 *
 * Published by the thread itself and read by main after the join, so it needs its
 * own lock: `host_run_result()` is thread-local by design, and the main thread
 * cannot see another thread's copy.
 */
#define GUEST_THREAD_STOPS 32 /* T1492: was 8, and 15 threads dropped the main game thread's final stop */

static host_thread_stop_record g_thread_stops[GUEST_THREAD_STOPS];
static pthread_mutex_t g_thread_stop_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_thread_orderly_exits; /* T1492: PsTerminateSystemThread ends, itemised or not */
static unsigned g_thread_unlisted_stops; /* T1492: stops that did not fit the table */

static void publish_thread_stop(const kernel_thread_launch *launch)
{
    thunk_trace_entry tail[THUNK_TAIL_MAX];
    const unsigned tail_count = thunk_trace_thread_tail(tail, THUNK_TAIL_MAX);
    const host_stop *stop = host_run_result();
    pthread_mutex_lock(&g_thread_stop_lock);
    if (stop->reason == HOST_STOP_THREAD_EXITED) {
        g_thread_orderly_exits++;
    }
    if (!host_thread_stop_publish(g_thread_stops, GUEST_THREAD_STOPS, launch->handle,
                                  launch->entry_va, g_esp, g_eax, stop, tail, tail_count)) {
        g_thread_unlisted_stops++;
    }
    pthread_mutex_unlock(&g_thread_stop_lock);
    if (stop->reason != HOST_STOP_THREAD_EXITED) {
        /* T1492: said NOW as well as in the final report, so a run that is killed or crashes later
         * still carries the reason in its log. */
        printf("guest thread %#x (entered 0x%08X) STOPPED: %s%s%s\n", (unsigned)launch->handle,
               launch->entry_va, host_stop_reason_str(stop->reason),
               stop->detail != NULL && stop->detail[0] != '\0' ? " -- " : "",
               stop->detail != NULL ? stop->detail : "");
        fflush(stdout);
    }
    if (interactive_play && host_run_result()->reason != HOST_STOP_THREAD_EXITED &&
        host_run_result()->reason != HOST_STOP_RETURNED)
        atomic_store(&interactive_shutdown, true);
}

static _Thread_local char wait_stop_detail[256];

static void host_thread_wait_refused(uint32_t handle, kernel_thread_wait_refusal reason)
{
    if (reason == KERNEL_THREAD_WAIT_TERMINAL_HOST_FAILURE) {
        host_stop copied = {0};
        bool found = false;
        pthread_mutex_lock(&g_thread_stop_lock);
        for (unsigned i = 0u; i < GUEST_THREAD_STOPS; i++) {
            if (g_thread_stops[i].valid && g_thread_stops[i].handle == handle) {
                copied = g_thread_stops[i].stop;
                if (copied.detail != NULL) {
                    (void)snprintf(wait_stop_detail, sizeof(wait_stop_detail), "%s", copied.detail);
                    copied.detail = wait_stop_detail;
                }
                found = true;
                break;
            }
        }
        pthread_mutex_unlock(&g_thread_stop_lock);
        if (found) {
            host_run_rethrow(&copied);
        }
    }
    uint32_t caller = 0u;
    (void)kernel_guest_read_u32(g_esp, &caller);
    const char *detail = reason == KERNEL_THREAD_WAIT_CONDITION_ERROR ?
        "NtWaitForSingleObjectEx host condition wait failed" :
        reason == KERNEL_THREAD_WAIT_TERMINAL_HOST_FAILURE ?
        "NtWaitForSingleObjectEx target stopped without a retained diagnostic" :
        "NtWaitForSingleObjectEx unsupported thread/object/timeout scope";
    host_run_stop(HOST_STOP_KERNEL_UNIMPLEMENTED, caller, 234u, detail);
}

static bool host_thread_has_code(uint32_t guest_va)
{
    return recomp_lookup(guest_va) != NULL;
}

/* Ends the CALLING guest thread. Does not return: the measured exit path of this
 * title's thread startup shim is PsTerminateSystemThread followed by an int3, so
 * returning would run code the guest considers dead. */
static void host_thread_terminate(uint32_t exit_status)
{
    host_run_stop(HOST_STOP_THREAD_EXITED, 0u, 0u, "PsTerminateSystemThread");
    (void)exit_status;
}

/* Host completion alone is not evidence that the guest thread terminated. */
static bool host_thread_termination_confirmed(const kernel_thread_launch *launch)
{
    (void)launch;
    return host_run_result()->reason == HOST_STOP_THREAD_EXITED;
}

/*
 * Run one guest thread on this host thread.
 *
 * The guest register file is already thread-local, so installing `esp`, `ebp` and
 * the `fs` base here gives this thread its own complete machine state. Nothing is
 * shared with the thread that created it except guest memory, which is the
 * situation on the real console too.
 */
static void host_thread_enter(const kernel_thread_launch *launch)
{
    callback_thread_handle=launch->handle;
    g_esp = launch->esp;
    g_ebp = 0u;
    g_fs_base = launch->fs_base;

    recomp_func_t code = recomp_lookup(launch->entry_va);
    if (!code) {
        /* has_code already screened this on the creating thread, so reaching here
         * means the lift changed under us. Record it as a stop rather than
         * returning quietly, which would look like a thread that ran and finished. */
        if (sigsetjmp(*host_run_jmp(), 1) == 0) {
            host_run_arm();
            host_run_stop(HOST_STOP_THREAD_NO_CODE, launch->entry_va, 0u,
                          "no lifted function at the thread entry");
        }
        host_run_disarm();
        publish_thread_stop(launch);
        return;
    }

    printf("--- guest thread %#x entering 0x%08X  esp=0x%08X  fs=0x%08X ---\n",
           (unsigned)launch->handle, launch->entry_va, launch->esp, launch->fs_base);
    fflush(stdout);

    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        if (second_vblank_policy)
            recomp_second_vblank_bind_owner(launch->handle,launch->fs_base,
                                             launch->start_routine,launch->entry_va);
        else
            d3d8_first_vblank_bind_owner(launch->handle, launch->fs_base,
                                         launch->start_routine, launch->entry_va);
        code();
        /* The measured exit path is an ordinal, never a return, so a return means
         * the entry convention is wrong. Say so instead of treating it as normal. */
        host_run_stop(HOST_STOP_RETURNED, launch->entry_va, 0u,
                      "guest thread entry routine returned, which its measured exit "
                      "path says it should not");
    }
    host_run_disarm();
    xdk_thunk_stream_virtual_cancel_pending();
    publish_thread_stop(launch);
}

/*
 * HalReturnToFirmware: the title asked to reboot, so the run ends here.
 *
 * STOPPING IS THE WHOLE POINT. On hardware this call does not return -- the console
 * reboots -- so letting the guest continue would produce a trace no console could
 * ever produce, which is the one failure this host is built to avoid. src/xbox must not
 * depend on the host runtime, so kernel_hal.c calls out through this sink and the sink
 * is what never comes back.
 *
 * THE DETAIL BUFFER IS THREAD-LOCAL BECAUSE `host_stop.detail` IS BORROWED, not copied.
 * A function-local array would be dangling by the time the stop is reported, and a
 * single shared static would be a race between two guest threads rebooting at once --
 * unlikely, and the kind of unlikely that produces an unreproducible wrong message.
 */
#if defined(__GNUC__) || defined(__clang__)
#define HOST_TLS __thread
#else
#define HOST_TLS
#endif

static HOST_TLS char firmware_detail[96];

static void host_firmware_return(uint32_t routine, unsigned pending_notifications)
{
    /* The routine value is reported RAW. nxdk (CC0) names the enumerators, and
     * src/xbox/kernel_hal.h explains why this project does not: a wrong name attached
     * to a correct number is worse than no name. */
    (void)snprintf(firmware_detail, sizeof(firmware_detail),
                   "routine %u, %u shutdown registration(s) pending", (unsigned)routine,
                   pending_notifications);
    host_run_stop(HOST_STOP_FIRMWARE_RETURN, 0u, 49u, firmware_detail);
}

static void report_thread_stops(void)
{
    pthread_mutex_lock(&g_thread_stop_lock);
    for (unsigned i = 0; i < GUEST_THREAD_STOPS; i++) {
        if (!g_thread_stops[i].valid) {
            continue;
        }
        host_report_thread_stop(stdout, &g_thread_stops[i], &report_names);
    }
    pthread_mutex_unlock(&g_thread_stop_lock);
}

/* The ordered trace: gather the entries and the per-kind totals, format in
 * host_report.c (see the comment there for why both boundaries share one list). */
static void report_trace(unsigned limit)
{
    size_t count = 0;
    const thunk_trace_entry *trace = thunk_trace_entries(&count);
    const host_report_trace_totals totals = {
        .total = (unsigned long long)thunk_trace_total(),
        .total_ordinal = (unsigned long long)thunk_trace_total_of_kind(THUNK_KIND_ORDINAL),
        .total_xdk = (unsigned long long)thunk_trace_total_of_kind(THUNK_KIND_XDK),
        .total_monitor = (unsigned long long)thunk_trace_total_of_kind(THUNK_KIND_MONITOR),
    };
    host_report_trace(stdout, trace, count, &totals, limit, &report_names);
}

static void report_stop(const host_stop *stop)
{
    /* The ring is volatile because the guest writes it while running; the guest has
     * stopped by the time this is printed, so a plain snapshot is the same bytes. */
    uint32_t icall_trace[ICALL_TRACE_RING];
    for (unsigned i = 0; i < ICALL_TRACE_RING; i++) {
        icall_trace[i] = g_icall_trace[i];
    }
    host_report_stop(stdout, stop, g_esp, g_eax, icall_trace, g_icall_trace_idx,
                     ICALL_TRACE_RING, &report_names);
}

/* T760: the --present sink and the two overlay hooks that feed it (d3d8_overlay_set_present_hook). */
static present_video_sink *g_present_sink;
static const char *interactive_close_detail(void)
{
    /* T1492: said which input closed the window (the Escape key quits the host by design, and so
     * does any SDL quit event), so a report of "the game crashed" names what really happened. */
    static char text[160];
    const char *cause = present_video_sink_close_cause(g_present_sink);
    (void)snprintf(text, sizeof(text), "interactive window closed by user (%s)", cause != NULL ? cause : "cause unknown");
    return text;
}
static bool interactive_closed(void)
{
    if (present_video_sink_closed(g_present_sink)) {
        atomic_store(&interactive_user_close, true);
        atomic_store(&interactive_shutdown, true);
    }
    return atomic_load(&interactive_shutdown);
}
/* T1629: close the button dump with the reason the run ended: a user quit of the interactive window (Escape, SDL quit) is
 * "signal", anything else "exit". Bounded (button_dump_host_stop_bounded), so no exit path can hang on it. */
#define BUTTON_DUMP_STOP_BOUND_MS 3000u
static void button_dump_stop_for_exit(bool interactive)
{
    button_dump_host_stop_bounded(interactive && interactive_closed() ? "signal" : "exit", BUTTON_DUMP_STOP_BOUND_MS);
}
static present_video_kind g_present_kind = PRESENT_VIDEO_NONE;
/* T1632: the host side of the `mark` and `stop` hotkey labels (src/host/hotkey_actions.c) */
static hotkey_actions g_hotkey_actions;
static bool hotkey_mark_hook(void *user)
{
    (void)user;
    return xinput_record_request_mark();
}
static void hotkey_stop_hook(void *user, const char *cause)
{
    (void)user;
    present_video_sink_request_close(g_present_sink, cause);
}
/* T1720: the host side of the `shot` hotkey label: PNG of the presented frame in --hotkey-dir (src/host/shot_hotkey.c) */
static shot_hotkey g_shot_hotkey;
static bool g_shot_ready;
static bool g_shot_configured; /* a --hotkey spec carries the `shot` label: the run ends with a summary line */
static bool shot_capture_hook(void *user, const char *bmp_path)
{
    (void)user;
    return present_video_sink_capture(g_present_sink, bmp_path);
}
static uint64_t shot_presented_hook(void *user)
{
    (void)user;
    return present_video_sink_counts(g_present_sink).presented;
}
static bool hotkey_shot_hook(void *user, unsigned number, const char *label, uint64_t poll)
{
    (void)user;
    return g_shot_ready && shot_hotkey_take(&g_shot_hotkey, number, label, poll);
}
static controller_sdl *g_controllers;

static live_module_maker *g_live_maker; /* T847: --gpu-live-translate */
static bool g_live_render; /* T838: --gpu-live, the --present window is the live Vulkan window */

static void present_picture_cb(const d3d8_overlay_picture *picture, void *context)
{
    if (g_live_render) {
        live_render_note_picture();
    }
    present_video_sink_submit_at((present_video_sink *)context, picture->number, picture->width,
                                 picture->height, picture->rgb, host_virtual_ns());
}

static void present_vblank_cb(void *context)
{
    present_video_sink_vblank((present_video_sink *)context);
    controller_sdl_pump(g_controllers);
}

/* T839: --overlay-xemu-key with --gpu-replay and --present, the sink gets the composed frame at each vblank. */
static void present_composed_submit_cb(void *context, uint64_t number, uint32_t width, uint32_t height,
                                       const uint8_t *rgb)
{
    present_video_sink_submit_at((present_video_sink *)context, number, width, height, rgb, host_virtual_ns());
}

static void present_composed_vblank_cb(void *context)
{
    (void)context;
    d3d8_overlay_present_vblank();
}

static uint64_t watch_pad_polls(void)
{
    return xinput_source_port_poll_count(0u);
}

int main(int argc, char **argv)
{
    host_run_set_fault_guest_ebp(read_guest_ebp);
    options opts;
    if (!parse_options(argc, argv, &opts)) {
        usage(argv[0]);
        return 2;
    }
    guest_frame_trace_enable(!opts.guest_frame_trace_off);
    if (opts.cpu_profile != NULL && !cpu_sampler_start(opts.cpu_profile, opts.cpu_profile_wall ? 200u : 997u, opts.cpu_profile_wall)) {
        fprintf(stderr, "--cpu-profile %s could not start\n", opts.cpu_profile);
        return 2;
    }
    if (opts.audio_output != NULL &&
        (opts.audio_sink == NULL || strcmp(opts.audio_sink, "wav-file") != 0)) {
        fprintf(stderr, "--audio-output requires --audio-sink wav-file\n");
        return 2;
    }

    if ((opts.headless_first_vblank || opts.headless_second_vblank) && !recomp_cooperative_ready()) {
        fprintf(stderr, "CPU callback policies require compiled cooperative call safe points; "
                        "re-lift with cooperative calls enabled\n");
        return 2;
    }

    if (opts.headless_second_vblank) {
        /* The observed waits are native Swap-internal calls. The original
         * 3D3550 body is outside the measured surface and remains translated. */
        const uint32_t required[]={0x3D3530u,0x3D8E50u};
        if (!xdk_thunk_dispatch_boundaries_ready(required,sizeof(required)/sizeof(required[0]))) {
            fprintf(stderr,"credited callback policy requires compiled callback-registration and swap dispatch guards\n");
            return 2;
        }
    }

    if (opts.headless_streams && !xdk_thunk_stream_stops_ready()) {
        fprintf(stderr, "--headless-streams requires all compiled stream stop boundaries; "
                        "regenerate the manual inputs with --stop-boundaries and re-lift\n");
        return 2;
    }

    if (opts.headless_buffers && !dsound_buffer_stops_ready()) {
        fprintf(stderr, "--headless-buffers requires all compiled buffer lifetime stops; "
                        "regenerate both audio stop lists and re-lift\n");
        return 2;
    }

    if ((opts.headless_streams || opts.headless_buffers || opts.headless_listener) && !passive_audio_dispatch_ready()) {
        fprintf(stderr, "passive audio requires all compiled DSOUND dispatcher boundaries; "
                        "regenerate manual inputs including DSOUND and both audio stop lists\n");
        return 2;
    }

    if (opts.native_shader_assembler && !xdk_original_ready()) {
        fprintf(stderr, "--native-shader-assembler requires a complete retained original compiler profile; "
                        "generate the original shader profile chunk before building\n");
        return 2;
    }
    if (!xdk_original_configure(opts.native_shader_assembler)) {
        fprintf(stderr, "could not configure original shader compiler routes\n");
        return 2;
    }
    if (opts.native_shader_assembler) {
        printf("shader compiler original CPU profile enabled; no renderer\n");
    }
    if (opts.native_xmv && !opts.native_xmv_explicit && !xmv_original_ready()) {
        opts.native_xmv = false; /* T1093: the disc default needs the compiled profile, quietly absent otherwise */
    }
    if (opts.native_xmv && !xmv_original_ready()) {
        fprintf(stderr, "--native-xmv requires the compiled retained XMV profile; "
                        "generate it with tools.gen_xmv_original_bodies before building\n");
        return 2;
    }
    if (!xmv_original_configure(opts.native_xmv)) {
        fprintf(stderr, "could not configure original XMV routes\n");
        return 2;
    }
    if (!xmv_original_set_skip_intro(opts.skip_intro)) {
        fprintf(stderr, "--skip-intro requires the configured retained original XMV profile\n");
        return 2;
    }
    xmv_original_set_trace(opts.trace_xmv);
    dsound_movie_stream_set_trace(opts.trace_xmv);
    xmv_original_set_data_resolver(d3d8_resource_virtual_of_registered_data);
    xmv_original_set_frame_dump(opts.dump_xmv_frames, opts.dump_xmv_frames_max);
    if (!xmv_original_set_entry_capture(opts.capture_xmv_entries, opts.capture_xmv_range_first,
                                        opts.capture_xmv_range_last, opts.capture_xmv_range_count)) {
        fprintf(stderr, "could not configure the XMV entry capture\n");
        return 2;
    }
    if (!xmv_original_set_seed(opts.seed_xmv_entry_set, opts.seed_xmv_entry, opts.seed_xmv_result,
                               opts.seed_xmv_patch, opts.seed_xmv_patch_count)) {
        fprintf(stderr, "could not configure the seeded XMV run\n");
        return 2;
    }
    /* The overlay dump allocates its picture buffers here, before the guest image is mapped (T537). */
    if (!d3d8_overlay_set_dump(opts.dump_overlay, opts.dump_overlay_max,
                               d3d8_resource_virtual_of_registered_data)) {
        fprintf(stderr, "--dump-overlay could not allocate its picture buffers\n");
        return 2;
    }
    {
        present_video_kind present_video = PRESENT_VIDEO_NONE;
        present_audio_kind present_audio = PRESENT_AUDIO_NONE;
        const char *reason = NULL;
        if (!present_video_select(opts.present, opts.dump_overlay != NULL, &present_video, &reason)) {
            fprintf(stderr, "%s\n", reason);
            return 2;
        }
        if (!present_audio_select(opts.audio_sink, true, &present_audio, &reason)) {
            fprintf(stderr, "%s\n", reason);
            return 2;
        }
        if (opts.gpu_live) {
            const live_render_config live_config = {opts.gpu_live_inferred, opts.live_pipeline_set ? opts.live_pipeline : LIVE_PIPELINE_DEFAULT_DEPTH, !opts.live_readback && opts.live_frame_hash == NULL, opts.live_pipeline_cache_off ? NULL : (opts.live_pipeline_cache != NULL ? opts.live_pipeline_cache : opts.gpu_replay_dir), opts.live_frame_hash, opts.live_blit_verify, opts.live_present_sync};
            const present_live_ops live_ops = live_render_ops(&live_config);
            present_video_set_live_ops(&live_ops);
            g_live_render = true;
            printf("live renderer  ON (opt-in, M9 host hook-up T838, INFERRED until validated against xemu): the --present window is a "
                   "Vulkan window, the title's draws are drawn by the live NV2A renderer, the movie overlay pictures go through "
                   "the live compositor (audio clock still master in playback mode)%s\n",
                   opts.gpu_live_inferred ? "; INFERRED texture formats, sampler states and byte format CopyRects admitted" : "");
            printf("live renderer  frame pipeline (T1246): %s (--live-pipeline %u). The presenter thread draws a frame while the guest thread runs the next one; "
                   "the frame's model, SetTexture history and texture bytes are copied at the Swap, a visibility report slot the guest reads or "
                   "rewrites waits for the frame that owes it, so the guest sees what the serial renderer showed (--live-pipeline 0)\n",
                   (opts.live_pipeline_set ? opts.live_pipeline : LIVE_PIPELINE_DEFAULT_DEPTH) != 0u ? "ON" : "off",
                   opts.live_pipeline_set ? opts.live_pipeline : LIVE_PIPELINE_DEFAULT_DEPTH);
            if (opts.live_readback || opts.live_frame_hash != NULL) {
                printf("live renderer  present route (T1267): READBACK, %s; the swapchain blit is the default otherwise\n",
                       opts.live_readback ? "--live-readback" : "--live-frame-hash needs the readback frames to hash");
            } else {
                printf("live renderer  DEFAULT (T1267, was OPT-IN --gpu-live-blit in T849): frames are presented with the swapchain pre-pass blit (the front "
                       "target image and the overlay texture are blitted straight into the acquired image, no front buffer readback), "
                       "a front the blit cannot read falls back to the readback route; the frame rate is measured on this machine only\n");
            }
        }
        if (present_video != PRESENT_VIDEO_NONE) {
            printf("%s\n", present_video_announce(present_video));
            const char *open_error = NULL;
            g_present_kind = present_video;
            g_present_sink = present_video_sink_open(present_video, "tsfp", &open_error);
            if (g_present_sink != NULL && !opts.present_no_pace) {
                present_video_sink_set_pace(g_present_sink, 59940u); /* NTSC Xbox vblank rate, INFERRED */
            }
            present_video_sink_set_interactive(g_present_sink, opts.interactive);
            interactive_play = opts.interactive;
            interactive_async_io = opts.interactive && opts.async_file_io;
            const bool composed = opts.overlay_xemu_key && opts.gpu_replay_dir != NULL;
            if (composed && g_present_sink != NULL) {
                const d3d8_overlay_present_sink composed_sink = {
                    present_composed_submit_cb, present_vblank_cb, g_present_sink,
                    present_video == PRESENT_VIDEO_PNG_DIR ? opts.dump_overlay : NULL, opts.dump_overlay_max};
                d3d8_overlay_present_configure(&composed_sink, NULL);
            }
            if (g_present_sink == NULL ||
                !d3d8_overlay_set_present_hook(composed ? NULL : present_picture_cb,
                                               composed ? present_composed_vblank_cb : present_vblank_cb,
                                               g_present_sink, d3d8_resource_virtual_of_registered_data)) {
                fprintf(stderr, "%s\n", open_error != NULL ? open_error : "--present could not start its sink");
                return 2;
            }
        }
        if (present_audio != PRESENT_AUDIO_NONE)
            printf("%s\n", present_audio_announce(present_audio));
        g_audio_kind = present_audio;
        if (present_audio == PRESENT_AUDIO_WAV_FILE) {
            const char *path = opts.audio_output != NULL ? opts.audio_output : "tsfp-audio.wav";
            g_audio_sink = present_audio_sink_open(PRESENT_AUDIO_WAV_FILE, path, 48000u, 2u);
            if (g_audio_sink == NULL) {
                fprintf(stderr, "--audio-sink wav-file could not open %s\n", path);
                return 2;
            }
            if (!dsound_audio_runtime_start(48000u, KERNEL_CLOCK_FREQUENCY_HZ)) {
                fprintf(stderr, "--audio-sink wav-file could not start the HLE mixer\n");
                (void)present_audio_sink_close(g_audio_sink);
                g_audio_sink = NULL;
                return 2;
            }
            printf("audio output   %s (stereo S16, 48000 Hz, timing INFERRED)\n", path);
        }
        if (present_audio == PRESENT_AUDIO_SDL) {
            audio_sink_sdl_set_mute(opts.audio_mute);
            g_audio_sink = present_audio_sink_open(PRESENT_AUDIO_SDL, NULL, 48000u, 2u);
            if (g_audio_sink == NULL) {
                const char *why = present_audio_sink_last_error();
                fprintf(stderr, "--audio-sink sdl could not open the audio device: %s\n", why != NULL ? why : "?");
                return 2;
            }
            if (!dsound_audio_runtime_start(48000u, KERNEL_CLOCK_FREQUENCY_HZ)) {
                fprintf(stderr, "--audio-sink sdl could not start the HLE mixer\n");
                present_audio_sink_close(g_audio_sink);
                g_audio_sink = NULL;
                return 2;
            }
            if (g_present_sink != NULL && present_video == PRESENT_VIDEO_WINDOW && !opts.present_no_pace) {
                /* T819 playback: the audio device clock is the master, pictures follow it, the guest runs ahead a
                 * bounded amount (a 4 s ring and a 96 picture queue) and the sound rebuffers instead of stuttering. */
                present_audio_sink_enable_clock(g_audio_sink, 400u, 1500u, 4u);
                if (opts.interactive && opts.audio_latency_ms != 0u) {
                    present_audio_sink_set_latency_governor(g_audio_sink, opts.audio_latency_ms, opts.audio_stretch_legacy ? 100u : 30u, 50u);
                    present_audio_sink_set_av_offset_ms(g_audio_sink, opts.av_sync_offset_ms);
                    printf("audio latency  governor ON (T1235): the sink holds its ring fill near %u ms (starts above +100 ms): digital "
                           "silence is dropped first, then up to 5%% faster consumption; --audio-latency-ms 0 turns it off\n",
                           (unsigned)opts.audio_latency_ms);
                }
                if (opts.interactive && opts.audio_min_rate_permille != 0u) {
                    /* Two thirds of the governor target: a guest at real time holds the ring near the target and is never slowed. */
                    const uint32_t low_water_ms = opts.audio_latency_ms != 0u ? (opts.audio_latency_ms * 2u / 3u > 60u ? opts.audio_latency_ms * 2u / 3u : 60u) : 100u;
                    present_audio_sink_set_continuous_playback(g_audio_sink, low_water_ms, opts.audio_min_rate_permille);
                    if (opts.audio_stretch_legacy) {
                        const present_audio_stretch_tuning legacy = {6667u, 1000u, 1000u, 0u, 1000u, 999u};
                        present_audio_sink_set_stretch_tuning(g_audio_sink, &legacy);
                    }
                    present_audio_sink_set_stretch(g_audio_sink, !opts.audio_stretch_resample);
                    if (opts.audio_hold_ms != 0u) {
                        present_audio_sink_set_stretch_hold(g_audio_sink, 100u, opts.audio_hold_ms);
                    }
                    printf("audio playback continuous (T1248/T1250): below %u ms of ring the device plays slower (down to %u per mille of real time, "
                           "%s) instead of draining the ring and cutting every voice for a refill; --audio-min-rate 0 turns it off, "
                           "--audio-stretch resample|wsola picks the method\n",
                           (unsigned)low_water_ms, (unsigned)opts.audio_min_rate_permille,
                           opts.audio_stretch_resample ? "the pitch follows" : "pitch preserving WSOLA stretch");
                } else {
                    printf("audio playback continuous OFF: %s, so a dry ring pauses every voice until it has refilled "
                           "(--audio-min-rate 250 turns the slowed playback on)\n",
                           opts.interactive ? "--audio-min-rate 0" : "not --interactive");
                }
                if (opts.audio_pump) {
                    atomic_store(&g_audio_pump_run, true);
                    g_audio_pump_started = pthread_create(&g_audio_pump_thread, NULL, host_audio_pump_main, NULL) == 0;
                    printf("audio pump     %s (T1250 gaps): the HLE mixer is rendered up to the virtual clock every 5 ms of wall time, not only inside "
                           "DirectSoundDoWork, so a guest that stops calling it (the Story loading screen spins the main thread for 0.3 to 0.9 s) "
                           "no longer starves the sink of audio it already submitted; --no-audio-pump restores the DoWork only rendering\n",
                           g_audio_pump_started ? "ON" : "FAILED to start");
                } else {
                    printf("audio pump     OFF (--no-audio-pump): the mixer is rendered only inside DirectSoundDoWork\n");
                }
                present_video_sink_set_playback(g_present_sink, g_audio_sink);
                if (opts.present_timeline != NULL &&
                    !present_timeline_start(g_audio_sink, g_present_sink, host_virtual_ns, opts.present_timeline))
                    fprintf(stderr, "--present-timeline %s could not start\n", opts.present_timeline);
                printf("present playback ON (T819, INFERRED timing): audio device clock is master, pictures presented at their "
                       "modelled time, rebuffer 400..1500 ms, ring 4 s\n");
            }
        }
    }
    if (!xmv_original_set_substitute(opts.xmv_substitute)) {
        fprintf(stderr, "--xmv-substitute needs FROM=TO with names of 1 to 32 letters, digits or underscores\n");
        return 2;
    }
    if (opts.xmv_substitute != NULL) {
        printf("xmv substitute ON (opt-in, T394): CreateDecoderForFile of movie '%s' opens the other disc movie instead. "
               "FABRICATED, the title's path is rewritten in guest memory\n", opts.xmv_substitute);
    }
    if (opts.native_xmv) {
        printf("xmv original CPU profile enabled; original decoder over the disc's own movie data\n");
    }

    size_t length = 0;
    uint8_t *data = read_file(opts.xbe_path, &length);
    if (!data) {
        return 1;
    }

    xbe_image image;
    memset(&image, 0, sizeof(image));
    xbe_status status = xbe_parse(data, length, &image);
    if (status != XBE_OK) {
        fprintf(stderr, "parse failed: %s\n", xbe_status_str(status));
        free(data);
        return 1;
    }
    status = xbe_map(&image, data, length);
    if (status != XBE_OK) {
        fprintf(stderr, "map failed: %s\n", xbe_status_str(status));
        free(data);
        return 1;
    }

    printf("xbe            %s\n", opts.xbe_path);
    printf("entry point    0x%08X\n", image.entry_point);
    printf("thunk table    0x%08X (%u imports)\n", image.kernel_thunk_addr,
           image.kernel_import_count);
    printf("lifted funcs   %zu\n", recomp_get_count());

    kernel_hle_init();
    const size_t implemented = kernel_register_all();

    /* ONE LOG SINK FOR EVERY SUBSYSTEM. Each HLE module carries its own printer
     * because none of them depends on src/xbox/; pointing them all at the sink the
     * kernel HLE uses means a capture of one is not missing half the story. */
    dsound_hle_set_log(kernel_hle_log());
    xinput_hle_set_log(kernel_hle_log());
    xgrph_hle_set_log(kernel_hle_log());
    xdk_thunk_set_log(kernel_hle_log());
    xonline_hle_set_log(kernel_hle_log());
    /* Wired here rather than left for later: without it, ordinal 49 reports the reboot
     * and then RETURNS, and the guest runs on past a reboot it cannot honestly survive.
     * kernel_hal.c says so loudly when no sink is installed, but a loud wrong answer is
     * still a wrong answer. */
    kernel_hal_set_firmware_sink(host_firmware_return);
    /* NOT OPTIONAL for the section bounds check: src/xbox does not link src/loader,
     * so the image base has to arrive from here. Forgetting it leaves the reference
     * counting correct and the bounds check DISABLED, which the module says once and
     * loudly rather than silently. */
    kernel_xe_set_image_base(image.base_address);
    printf("hle registered %zu ordinals\n", implemented);

    /* The address-keyed XDK boundary, adopted BEFORE --ac97-ready is applied: the
     * adoption calls dsound_hle_init, which resets the codec state, so doing it the
     * other way round would silently discard the flag. */
    printf("xdk surface    %zu address(es) adopted\n", adopt_xdk_surface());
    /* Explicit MU attachment happens after guest memory/XDK adoption and before XInitDevices. */
    if (opts.mu_image_count != 0u) {
#if !defined(TSFP_HAVE_XDK_SURFACE)
        fprintf(stderr, "--mu-image requires the generated measured XDK surface\n");
        return 2;
#else
        char mu_error[192];
        if (!mu_startup_attach(opts.mu_images, opts.mu_image_count, mu_error, sizeof(mu_error))) {
            fprintf(stderr, "--mu-image: %s\n", mu_error);
            return 2;
        }
        if (!mu_enable_guest_filesystem(true)) {
            fprintf(stderr, "--mu-image: cannot change the filesystem route while mounted\n");
            mu_reset(); return 2;
        }
        printf("MU: %u explicit writable images attached (modeled presence, kernel FATX/xemu unvalidated)\n",
               opts.mu_image_count);
#endif
    }
    declare_xdk_abis();
    xonline_hle_set_fatal(host_d3d8_fatal);
    xnet_hle_set_fatal(host_d3d8_fatal);
    xnet_offline_reset();
    xnet_offline_set_last_error_reader(host_xnet_last_error);
    printf("XNET T1070 MEASURED: %zu of 3 self-contained functions registered; the network stack methods still stop\n",
           xnet_offline_register());
    if (opts.xnet_local_entropy) {
        const size_t registered = xnet_random_register();
        if (registered != 2u) {
            fprintf(stderr, "--xnet-local-entropy needs both measured XNET surface entries\n");
            return 2;
        }
        printf("XNET local RNG INFERRED opt-in: %zu functions; HOST OS entropy replaces opaque RC4 stream, original readiness/key layout; no network startup\n", registered);
    }
    xonline_offline_reset();
    if (opts.xonline_offline) {
        const size_t registered = xonline_offline_register();
        if (registered != 30u) {
            fprintf(stderr,
                    "--xonline-offline requires the generated measured XONLINE surface; "
                    "registered %zu of 30 handlers\n", registered);
            return 2;
        }
        printf("XONLINE policy T904 INFERRED, opt-in: no live service; Startup succeeds, initialized GetUsers returns no accounts, T1071 no-state wrappers return the measured original values and refuse after Startup\n");
    }
    /* AFTER the adoption, which re-initialises the D3D8 table and so drops every handler. */
    d3d8_hle_set_fatal(host_d3d8_fatal);
    xgrph_hle_set_fatal(host_d3d8_fatal);
    dsound_effects_binding_set_fatal(host_d3d8_fatal);
    dsound_effects_binding_set_enabled(opts.headless_effects);
    if (!dsound_effects_binding_set_gp_enabled(opts.gp_effects)) {
        fprintf(stderr,"--gp-effects requires a pinned TSFP_XEMU_DSP_SOURCE build and quiescent binding\n");
        return 2;
    }
    if (opts.gp_effects && !xdk_thunk_declare_abi(0x00407A02u,XDK_CC_STDCALL,6u,0u)) {
        fprintf(stderr,"--gp-effects original SetEffectData ABI declaration failed\n");
        return 2;
    }
    dsound_stream_set_fatal(host_d3d8_fatal);
    dsound_stream_set_irql_provider(host_audio_irql);
    dsound_stream_set_enabled(opts.headless_streams);
    const bool stream_routing_pcm=opts.passive_audio_completion && dsound_audio_runtime_active();
    dsound_stream_set_routing_note(stream_routing_pcm?dsound_completion_stream_routing:NULL);
    dsound_stream_set_frequency_note(stream_routing_pcm?dsound_completion_note_frequency:NULL,
        stream_routing_pcm?host_audio_control_word:NULL);
    dsound_stream_set_format_note(stream_routing_pcm?dsound_completion_note_format:NULL);
    if(opts.headless_streams && stream_routing_pcm &&
       !xdk_thunk_declare_abi(0x004085CFu,XDK_CC_STDCALL,2u,0u)){
        fprintf(stderr,"stream frequency original ABI declaration refused\n");return 2;
    }
    if (opts.headless_streams && stream_routing_pcm) {
        /* T1129: exact original forwarding bodies end RET8; explicit ABI does
         * not weaken measured quorum for any other XDK method. */
        const uint32_t entries[3]={0x00407B19u,0x00407B1Eu,0x004085D4u};
        for (unsigned i=0u;i<3u;i++)
            if (!xdk_thunk_declare_abi(entries[i],XDK_CC_STDCALL,2u,0u)) {
                fprintf(stderr,"stream routing ABI declaration refused %#x\n",entries[i]);return 2;
            }
        printf("stream routing stereo speaker projection/timeline INFERRED; DSP/HRTF routes refused\n");
    }
    dsound_buffer_set_fatal(host_d3d8_fatal);
    dsound_buffer_set_irql_provider(host_audio_irql);
    dsound_buffer_set_enabled(opts.headless_buffers);
    dsound_listener_set_fatal(host_d3d8_fatal);
    dsound_listener_set_irql_provider(host_audio_irql);
    dsound_listener_set_enabled(opts.headless_listener);
    const bool mixbin_pcm=stream_routing_pcm &&
        (opts.headless_streams || opts.headless_buffers || opts.headless_movie_audio);
    if (mixbin_pcm && !dsound_audio_runtime_enable_mixbin_headroom(true)) {
        fprintf(stderr,"global mix-bin headroom PCM initialization failed\n");
        return 2;
    }
    dsound_mixbin_headroom_configure(mixbin_pcm?dsound_completion_mixbin_headroom:NULL,
                                      mixbin_pcm?dsound_completion_bind_mixbin_headroom:NULL,
                                      host_audio_irql,host_d3d8_fatal);
    if (mixbin_pcm && !xdk_thunk_declare_abi(0x00407A2Cu,XDK_CC_STDCALL,3u,0u)) {
        fprintf(stderr,"global mix-bin headroom original ABI declaration failed\n");
        return 2;
    }
    if (mixbin_pcm) printf("global submix headroom enabled; stereo projection/timeline INFERRED, DSP/HRTF unsupported\n");
    dsound_device_set_fatal(host_d3d8_fatal);
    dsound_hrtf_set_fatal(host_d3d8_fatal);
    dsound_hrtf_set_irql_provider(host_audio_irql);
    (void)dsound_device_register();
    (void)dsound_mixbin_headroom_register();
    (void)dsound_hrtf_register();
    (void)dsound_effects_binding_register();
    (void)dsound_stream_register();
    dsound_movie_stream_set_fatal(host_d3d8_fatal);
    dsound_movie_stream_set_irql_provider(host_audio_irql);
    dsound_movie_stream_set_callback_runner(host_movie_stream_callback);
    dsound_movie_stream_set_enabled(opts.headless_movie_audio);
    dsound_stream_set_extension(dsound_movie_stream_route_public, dsound_movie_stream_reset_checked);
    /* T681: the passive completion model, announced, default off. */
    kernel_async_io_set_enabled(opts.async_file_io);
    kernel_async_io_set_file_object_enabled(opts.async_file_io && opts.async_file_io_file_object);
    if (!host_xnet_dpc_configure(opts.xnet_scheduler)) return 2;
    kernel_clock_set_frame_hook(opts.xnet_scheduler ? host_xnet_frame_service :
                                opts.async_file_io ? kernel_async_io_service_hook : NULL);
    if (opts.xnet_scheduler) printf("XNET elapsed scheduler enabled; coalescing INFERRED, genuine queued DPC only\n");
    async_io_spin_configure(opts.async_file_io ? opts.async_file_io_spin : 0u);
    dsound_completion_set_fatal(host_d3d8_fatal);
    dsound_completion_set_enabled(opts.passive_audio_completion);
    dsound_completion_set_dowork_delivery(opts.passive_audio_completion && opts.passive_audio_completion_at_dowork);
    dsound_stream_set_completion(opts.passive_audio_completion, dsound_completion_note_pause);
    dsound_buffer_set_completion(opts.passive_audio_completion, dsound_completion_buffer_started);
    dsound_buffer_set_completion_pause(dsound_completion_buffer_pause);
    dsound_buffer_set_completion_frequency(dsound_completion_buffer_frequency);
    dsound_buffer_set_completion_voice_running(dsound_completion_buffer_voice_running);
    dsound_buffer_set_completion_loop(dsound_completion_buffer_loop);
    (void)dsound_completion_register();
    dsound_listener_set_work_route(host_audio_work_route);
    if (!xdk_thunk_set_completion_method_handler(
            opts.passive_audio_completion ? dsound_completion_method_owned : NULL,
            opts.passive_audio_completion ? dsound_completion_route_method : NULL)) {
        fprintf(stderr, "passive audio completion route requires all compiled stream stop boundaries\n");
        return 2;
    }
    if (!xdk_thunk_set_movie_method_handler(
            opts.headless_movie_audio ? dsound_movie_stream_method_owned : NULL,
            opts.headless_movie_audio ? dsound_movie_stream_route_method : NULL)) {
        fprintf(stderr, "movie stream method route requires all compiled stream stop boundaries\n");
        return 2;
    }
    /* T421: the XMV GetNextFrame device calls (second DirectSoundCreate, SynchPlayback, device Release)
     * need the compiled SynchPlayback stop boundary of the re-lift. A lift without it keeps the movie
     * stream model and the XMV DirectSoundCreate stays refused, as before. */
    const bool movie_device_calls = opts.headless_movie_audio && xdk_thunk_synch_stop_ready();
    dsound_device_set_movie_calls(movie_device_calls);
    if (movie_device_calls) (void)dsound_device_register_movie();
    if (!xdk_thunk_set_synch_handler(movie_device_calls ? dsound_movie_stream_route_synch : NULL)) {
        fprintf(stderr, "SynchPlayback route requires the compiled SynchPlayback stop boundary\n");
        return 2;
    }
    if (!xdk_thunk_set_stream_virtual_handlers(
            opts.headless_streams ? dsound_stream_cache_discontinuity : NULL,
            opts.headless_streams ? dsound_stream_get_startup_status : NULL)) {
        fprintf(stderr, "stream virtual request policy requires the complete compiled DSOUND guards\n");
        return 2;
    }
    (void)dsound_buffer_register();
    (void)dsound_listener_register();
    if (opts.headless_listener)
        printf("dsound policy  passive startup listener scalar caches and commit/work request observations; "
               "guest settings/derived3D/list/APU/DSP/FP/critical-section/notification/timing effects omitted\n");
    if (opts.headless_buffers)
        printf("dsound policy  passive startup buffer handles and spatial CPU caches; "
               "hardware/settings/list objects omitted; no playback or completion\n");
    if (opts.headless_streams)
        printf("dsound policy  passive startup stream objects and spatial CPU caches; "
               "hardware/settings/list objects omitted; no playback or packet completion\n");
    if (opts.passive_audio_completion)
        printf("dsound policy  PASSIVE COMPLETION (T681, owner decision T608 option a%s): stream Process packets complete "
               "after size/average bytes of running virtual clock time, buffer Play after samples/frequency, "
               "statuses are the measured original words, volumes are recorded values; no audio, voice or mixer\n",
               opts.passive_audio_completion_by_default ? ", DEFAULT since T762, --no-passive-audio-completion opts out" : "");
    if (opts.passive_audio_completion && opts.passive_audio_completion_at_dowork)
        printf("dsound policy  PASSIVE COMPLETION AT DOWORK (T855, T871 xemu-level%s): a packet past its deadline keeps its words "
               "and list slot until the next DirectSoundDoWork delivers size and status (the original's completion "
               "function 0x40B244)\n",
               opts.passive_audio_completion_at_dowork_by_default ? ", DEFAULT since T871, --no-passive-audio-completion-at-dowork opts out" : "");
    if (opts.async_file_io)
        printf("file io policy ASYNC READS (T743%s): an NtReadFile with an Event on a handle opened without "
               "FILE_SYNCHRONOUS_IO returns STATUS_PENDING and completes on the virtual clock, one request at a "
               "time, in order (buffer, IoStatusBlock, Event). XEMU-LEVEL timing (T763, not hardware), disc %u us "
               "access plus bytes at %u bytes/s, hdd %u us plus bytes at %u bytes/s, serviced at every virtual "
               "vblank and every NtReadFile. A blocking wait on a pending read advances the clock to its due time, "
               "never past it (T764). An ApcRoutine is REFUSED by name, cancellation is not imported by the title%s\n",
               opts.async_file_io_by_default ? ", DEFAULT since T762, --no-async-file-io opts out" : ", opt-in",
               (unsigned)KERNEL_ASYNC_IO_DISC_ACCESS_US, (unsigned)KERNEL_ASYNC_IO_DISC_BYTES_PER_SECOND,
               (unsigned)KERNEL_ASYNC_IO_HDD_ACCESS_US, (unsigned)KERNEL_ASYNC_IO_HDD_BYTES_PER_SECOND,
               opts.async_file_io_file_object
                   ? ". FILE OBJECT reads ON (T764, --async-file-io-file-object): an NtReadFile with no Event on "
                     "an asynchronous handle is queued and signals the file object at completion, INFERRED"
                   : ". File object reads off: an NtReadFile with no Event completes synchronously");
    if (interactive_async_io)
        printf("file io policy INTERACTIVE ELAPSED TIME (T940/T963, FABRICATED host clock): pending reads advance "
               "the virtual clock from a monotonic wall-time anchor at cooperative calls; only reads due under "
               "the existing XEMU-LEVEL service model complete. No safepoint-count or forced completion is used\n");
    if (opts.async_file_io && opts.async_file_io_spin != 0u)
        printf("file io policy SPIN COMPLETION ON (T821, opt-in, INFERRED, --async-file-io-spin-complete %u): a guest thread that "
               "passes %u cooperative safepoints (lifted calls) without an HLE dispatch of its own while a read is pending completes "
               "the EARLIEST pending read, the virtual clock advancing to its due time and never past it. It stands for the CPU time "
               "a poll of the OVERLAPPED in guest memory takes (the retail pack loader sub_00060050 polls with no vblank and no "
               "NtReadFile). The threshold is FABRICATED (a lifted call is about 100 to 200 cycles, INFERRED), no guest word is "
               "written beyond the completion itself\n",
               opts.async_file_io_spin, opts.async_file_io_spin);
    if (opts.headless_movie_audio && dsound_audio_runtime_active())
        printf("dsound policy  XMV movie stream bounded CPU model; its 44100 Hz stereo S16 PCM is mixed into the audio "
               "sink, packets complete in DirectSoundDoWork on the virtual clock at the format byte rate\n");
    else if (opts.headless_movie_audio)
        printf("dsound policy  XMV movie stream bounded CPU model; no audio is produced, packets complete "
               "in DirectSoundDoWork on the virtual clock at the format byte rate\n");
    if (movie_device_calls)
        printf("dsound policy  XMV device calls: a second DirectSoundCreate and device Release adjust the "
               "device reference, SynchPlayback starts the Pause(2) movie streams; no APU voice exists\n");
    else if (opts.headless_movie_audio)
        printf("dsound policy  XMV device calls NOT enabled: the lift lacks the SynchPlayback stop boundary "
               "(tools/config/movie_synch_boundaries.json), the XMV DirectSoundCreate stays refused\n");
    if (opts.gp_effects)
        printf("dsound policy  INFERRED owned GP bridge; real attested firmware consumer, no APU/PCM scheduling\n");
    else if (opts.headless_effects)
        printf("dsound policy  passive CPU effects views; initial state copied, "
               "workspace zeroed; no DSP execution or acknowledgement\n");
    printf("dsound handlers %zu registered\n", dsound_hle_implemented_count());
    xinput_devices_enable_synthetic_pad(false);
    if (opts.synthetic_pad) {
        (void)xinput_hle_attach_synthetic_pad(0u);
        xinput_devices_enable_synthetic_pad(true);
        printf("xinput policy  --synthetic-pad FABRICATED: port 0 reports one gamepad inserted, the title's "
               "open path is answered, buttons and sticks at rest (T717)\n");
    }
    if (opts.controllers) {
        xinput_devices_enable_synthetic_pad(true);
        xinput_devices_enable_multiport(true);
    }
    xinput_pad_remove_after_polls(opts.synthetic_pad_remove_after_polls);
    if (opts.synthetic_pad_remove_after_polls != 0u)
        printf("xinput policy  --synthetic-pad-remove-after-polls FABRICATED: the pad is unplugged after "
               "XInputGetState call %u (T731)\n", opts.synthetic_pad_remove_after_polls);
    xinput_source_reset();
    if (opts.pad_script != NULL && opts.pad_script_live) {
        char live_error[160];
        xinput_live *live = xinput_live_open(opts.pad_script, live_error, sizeof(live_error));
        if (live == NULL) {
            fprintf(stderr, "--pad-script-live %s: %s\n", opts.pad_script, live_error);
            return 2;
        }
        xinput_live_install(live);
        printf("xinput policy  --pad-script-live FABRICATED: one line of pad tokens re-read at every XInputGetState from %s, "
               "rest while empty (T1222)\n", opts.pad_script);
    } else if (opts.pad_script != NULL) {
        char script_error[160];
        xinput_script *script = xinput_script_load(opts.pad_script, script_error, sizeof(script_error));
        if (script == NULL) {
            fprintf(stderr, "--pad-script %s: %s\n", opts.pad_script, script_error);
            return 2;
        }
        xinput_script_install(script);
        printf("xinput policy  --pad-script FABRICATED: %zu entries, %llu frames of scripted pad state from %s, "
               "one per XInputGetState, rest afterwards (T707)\n", xinput_script_entry_count(script),
               (unsigned long long)xinput_script_total_frames(script), opts.pad_script);
    }
    if (opts.route_event_log != NULL || opts.route_event_wait_count != 0u || opts.record_input != NULL || opts.replay_input != NULL) {
        /* T1633: the observers behind the event driven route (file I/O, watched calls, presented frames, sampled memory). */
        char probe_error[240];
        route_probe_enable();
        d3d8_present_set_route_observer(route_frame_observer);
        if (opts.route_event_log != NULL && !route_probe_log_open(opts.route_event_log, probe_error, sizeof probe_error)) {
            fprintf(stderr, "--route-event-log: %s\n", probe_error);
            return 2;
        }
        for (unsigned mem_index = 0u; mem_index < opts.route_log_mem_count; mem_index++) {
            if (!route_probe_mem_add(opts.route_log_mem[mem_index], probe_error, sizeof probe_error)) {
                fprintf(stderr, "%s\n", probe_error);
                return 2;
            }
        }
        if (opts.route_log_mem_count != 0u && !route_probe_mem_start(route_probe_read_guest, NULL, 20u)) {
            fprintf(stderr, "--route-log-mem: cannot start the sampler thread\n");
            return 2;
        }
        if (opts.route_event_log != NULL)
            printf("route probe    T1633 (observer only): event log %s, %u sampled memory item(s)\n", opts.route_event_log,
                   opts.route_log_mem_count);
    }
    if (opts.record_input != NULL || opts.replay_input != NULL) {
        /* T1074: the identity a record is bound to, the XBE bytes and the title-affecting flag set. */
        uint8_t digest[32];
        char xbe_hex[65], flags_hex[65], flags_text[2048], record_error[2400];
        gpu_sha256(data, length, digest);
        gpu_sha256_hex(digest, xbe_hex);
        if (!xinput_record_identity_flags(argc, argv, flags_text, sizeof(flags_text))) {
            fprintf(stderr, "--record-input/--replay-input: the flag identity is too long\n");
            return 2;
        }
        gpu_sha256(flags_text, strlen(flags_text), digest);
        gpu_sha256_hex(digest, flags_hex);
        if (opts.record_input != NULL) {
            const xinput_record_budgets budgets = {opts.vblank_owner_waits, opts.vblank_worker_blanks,
                                                   opts.thread_timeout_ms, opts.interactive};
            xinput_record_set_budgets(&budgets);
            if (!xinput_record_open(opts.record_input, xbe_hex, flags_hex, flags_text, record_error, sizeof(record_error))) {
                fprintf(stderr, "--record-input %s: %s\n", opts.record_input, record_error);
                return 2;
            }
            xinput_record_set_mark_facts(route_probe_mark_facts, NULL); /* T1633: `# mark-info:` facts per mark */
            if (opts.route_nav_menus != NULL && (opts.route_nav_record == NULL || strcmp(opts.route_nav_record, "on") == 0)) {
                /* T1640: the menu table is sampled while recording: `nav` lines in the event log, `# nav:` lines in the record */
                host_route_config nav_config;
                memset(&nav_config, 0, sizeof nav_config);
                nav_config.nav_menus_file = opts.route_nav_menus;
                if (!host_route_nav_record_start(&nav_config, record_error, sizeof(record_error))) {
                    fprintf(stderr, "--record-input: %s\n", record_error);
                    return 2;
                }
                printf("xinput policy  --record-input nav lines (T1640): the menu table %s is sampled at every pad poll, '# nav:' lines are written for "
                       "select presses on a ready menu\n", opts.route_nav_menus);
            }
            if (!xinput_record_enable_marks(opts.record_input))
                fprintf(stderr, "--record-input: cannot install the SIGUSR2 mark handler, no marks will be recorded\n");
            else
                printf("xinput policy  --record-input marks (T1616): kill -USR2 $(cat %s.pid) writes '# mark: at=N' at the next poll\n",
                       opts.record_input);
            printf("xinput policy  --record-input FABRICATED: port 0 pad states by XInputGetState poll index are "
                   "recorded to %s (xbe sha256 %s, flags sha256 %s, T1074)\n", opts.record_input, xbe_hex, flags_hex);
        } else {
            uint64_t frames = 0u;
            if (!xinput_replay_load(opts.replay_input, xbe_hex, flags_hex, flags_text, record_error, sizeof(record_error), &frames)) {
                fprintf(stderr, "--replay-input %s: refused: %s\n", opts.replay_input, record_error);
                return 2;
            }
            xinput_record_budgets recorded;
            if (!xinput_replay_budgets(&recorded)) {
                printf("xinput replay  the record has no '# budgets:' line (made before T1126): the budgets of this command "
                       "line are used, owner-waits %u worker-blanks %u thread-timeout %u interactive %d\n",
                       opts.vblank_owner_waits, opts.vblank_worker_blanks, opts.thread_timeout_ms, (int)opts.interactive);
            } else {
                /* A budget this command line did not give is the recorded one (T1126). */
                if (opts.vblank_owner_waits == 0u && opts.headless_second_vblank && recorded.owner_waits != 0u)
                    opts.vblank_owner_waits = recorded.owner_waits;
                if (opts.vblank_worker_blanks == 0u && opts.vblank_owner_waits != 0u && recorded.worker_blanks != 0u)
                    opts.vblank_worker_blanks = recorded.worker_blanks;
                if (!opts.thread_timeout_given) opts.thread_timeout_ms = recorded.thread_timeout_ms;
                printf("xinput replay  budgets recorded: owner-waits %u worker-blanks %u thread-timeout %u interactive %d; "
                       "effective: owner-waits %u worker-blanks %u thread-timeout %u interactive %d\n",
                       recorded.owner_waits, recorded.worker_blanks, recorded.thread_timeout_ms, (int)recorded.interactive,
                       opts.vblank_owner_waits, opts.vblank_worker_blanks, opts.thread_timeout_ms, (int)opts.interactive);
                if (recorded.interactive && !opts.interactive)
                    printf("xinput replay  WARNING the recording ran --interactive, which waives the owner-wait and worker "
                           "budget stops; this run is not interactive, so it may stop at a budget the recording never hit "
                           "(run with --interactive and --present window to match)\n");
            }
            printf("xinput policy  --replay-input FABRICATED: %llu polls of recorded pad state from %s, identity "
                   "verified (xbe sha256 %s, flags sha256 %s, T1074)\n", (unsigned long long)frames,
                   opts.replay_input, xbe_hex, flags_hex);
        }
    }
    if (opts.snapshot_at_poll != 0u) {
        char snapshot_error[200];
        if (!host_snapshot_arm(opts.snapshot_at_poll, snapshot_error, sizeof(snapshot_error))) {
            fprintf(stderr, "--snapshot-at-poll: %s\n", snapshot_error);
            return 2;
        }
        printf("snapshot       --snapshot-at-poll %llu armed: a DMTCP process snapshot is taken at that port 0 poll "
               "(T1153, docs/state-snapshot.md)\n", (unsigned long long)opts.snapshot_at_poll);
    }
    if (opts.stop_at_poll != 0u) {
        char stop_error[200];
        if (!host_snapshot_arm_stop(opts.stop_at_poll, stop_at_poll_limit, stop_error, sizeof(stop_error))) {
            fprintf(stderr, "--stop-at-poll: %s\n", stop_error);
            return 2;
        }
        printf("snapshot       --stop-at-poll %llu armed: the run ends when port 0 has been polled that often (T1153)\n",
               (unsigned long long)opts.stop_at_poll);
    }
    xinput_pad_source_kind pad_kind = XINPUT_PAD_SOURCE_SCRIPT;
    xinput_source_fn route_live = NULL; /* T1616: the live pad that takes over after a replay */
    void *route_live_user = NULL;
    if (opts.pad_source != NULL && xinput_pad_source_kind_parse(opts.pad_source, &pad_kind) &&
        pad_kind != XINPUT_PAD_SOURCE_SCRIPT) {
        char source_error[200];
        const bool window_provider = opts.pad_feed == NULL && g_present_kind == PRESENT_VIDEO_WINDOW;
        xinput_host_source *source = NULL;
        if (window_provider) {
            xinput_event_feed feed;
            /* T1627: host hotkeys (tooling, a chord that writes DIR/hotkey.<n> and is kept from the game) */
            xinput_hotkeys *hotkeys = NULL;
            if (opts.hotkey_count != 0u) {
                xinput_hotkey_spec hotkey_specs[XINPUT_HOTKEY_MAX];
                for (unsigned hotkey_index = 0u; hotkey_index < opts.hotkey_count; hotkey_index++) {
                    if (!xinput_hotkey_parse(opts.hotkeys[hotkey_index], &hotkey_specs[hotkey_index], source_error,
                                             sizeof(source_error))) {
                        fprintf(stderr, "--hotkey %s: %s\n", opts.hotkeys[hotkey_index], source_error);
                        return 2;
                    }
                }
                /* T1632: the labels `mark` and `stop` act in the host, a chord key that leaked to the game is cut from the record */
                /* T1720b: pictures go to --shot-dir (default --hotkey-dir), bounded by --shot-max and --shot-max-bytes */
                g_shot_ready = shot_hotkey_init(&g_shot_hotkey, opts.shot_dir != NULL ? opts.shot_dir : opts.hotkey_dir, shot_capture_hook,
                                                shot_presented_hook, NULL);
                if (g_shot_ready) {
                    shot_hotkey_set_bounds(&g_shot_hotkey, opts.shot_max, opts.shot_max_bytes);
                    shot_hotkey_set_phase_dir(&g_shot_hotkey, opts.hotkey_dir); /* the wrapper writes `phase` next to hotkey.<n> */
                }
                for (unsigned hotkey_index = 0u; hotkey_index < opts.hotkey_count; hotkey_index++) {
                    if (xinput_hotkey_action_of(hotkey_specs[hotkey_index].label) == XINPUT_HOTKEY_ACTION_SHOT) g_shot_configured = true;
                }
                const hotkey_action_hooks action_hooks = {hotkey_mark_hook, hotkey_stop_hook, NULL, hotkey_shot_hook};
                hotkey_actions_init(&g_hotkey_actions,
                                    pad_kind == XINPUT_PAD_SOURCE_KEYBOARD ? XINPUT_HOST_KEYBOARD : XINPUT_HOST_GAMEPAD, &action_hooks);
                hotkeys = xinput_hotkeys_create(hotkey_specs, opts.hotkey_count, opts.hotkey_dir, hotkey_actions_fire,
                                                &g_hotkey_actions, source_error, sizeof(source_error));
                if (hotkeys == NULL) {
                    fprintf(stderr, "--hotkey: %s\n", source_error);
                    return 2;
                }
                xinput_hotkeys_set_leak_observer(hotkeys, hotkey_actions_leak, &g_hotkey_actions);
                printf("hotkeys        T1627/T1632 (tooling, FABRICATED): %u chord(s), files %s/hotkey.<n>; the lead key of a chord "
                       "reaches the game, the rest of the chord is swallowed while it is held; labels 'mark' (record mark, %s) and "
                       "'stop' (clean shutdown) and 'shot' (T1720, PNG + shots.manifest) act in the host\n", opts.hotkey_count, opts.hotkey_dir,
                       opts.record_input != NULL ? "recording" : "no --record-input, ignored");
            }
            if (!pad_sdl_feed_open_hotkeys(g_present_sink,
                                           pad_kind == XINPUT_PAD_SOURCE_KEYBOARD ? XINPUT_HOST_KEYBOARD : XINPUT_HOST_GAMEPAD,
                                           hotkeys, &feed, source_error, sizeof(source_error))) {
                fprintf(stderr, "--pad-source %s: %s\n", opts.pad_source, source_error);
                return 2;
            }
            source = xinput_pad_source_open_feed(pad_kind, feed, source_error, sizeof(source_error));
        } else {
            source = xinput_pad_source_open(pad_kind, opts.pad_feed, source_error, sizeof(source_error));
        }
        if (source == NULL) {
            fprintf(stderr, "--pad-source %s: %s\n", opts.pad_source, source_error);
            return 2;
        }
        if (opts.replay_input != NULL && opts.replay_handover) {
            route_live = xinput_host_source_fn;
            route_live_user = source;
        } else {
            xinput_host_source_install(source);
        }
        if (window_provider)
            printf("xinput policy  --pad-source %s FABRICATED: host %s events from the SDL3 window (--present window, "
                   "pumped at each modelled vblank, latched under the sink mutex) mapped by the table in "
                   "src/input/xinput_host_source.c, Xbox button names are the XDK convention, INFERRED (T751)\n",
                   opts.pad_source, opts.pad_source);
        else
            printf("xinput policy  --pad-source %s FABRICATED: host %s events from the fake-feed file %s mapped by the "
                   "table in src/input/xinput_host_source.c (T751)\n", opts.pad_source, opts.pad_source, opts.pad_feed);
    }
    if (opts.replay_input != NULL && (opts.replay_handover || opts.route_wait_count != 0u || opts.poke_at_count != 0u ||
                                      opts.route_event_wait_count != 0u || xinput_replay_wait_count() != 0u ||
                                      xinput_replay_nav_count() != 0u)) {
        host_route_config route_config;
        memset(&route_config, 0, sizeof route_config);
        route_config.waits = opts.route_waits;
        route_config.wait_count = opts.route_wait_count;
        route_config.event_waits = opts.route_event_waits;
        route_config.event_wait_count = opts.route_event_wait_count;
        route_config.nav_mode = opts.route_nav; /* T1640 */
        route_config.nav_menus_file = opts.route_nav_menus;
        route_config.nav_timing = opts.route_nav_timing;
        route_config.pokes = opts.poke_at;
        route_config.poke_count = opts.poke_at_count;
        route_config.live = route_live;
        route_config.live_user = route_live_user;
        route_config.on_failure = route_failure_stop;
        char route_error[240];
        if (!host_route_setup(&route_config, route_error, sizeof route_error)) {
            fprintf(stderr, "--replay-input route: %s\n", route_error);
            return 2;
        }
        if (route_probe_calls_watched() && !(opts.headless_first_vblank || opts.headless_second_vblank)) {
            fprintf(stderr, "--replay-input route: a call= wait needs the cooperative safepoint (add --headless-second-vblank)\n");
            return 2;
        }
        printf("xinput policy  route replay (T1616, FABRICATED): %u wait(s) (%u event wait(s) T1633, %zu from the record), %u poke "
               "trigger(s), handover to the %s pad after the record; FORCED-STATE pokes only through the T1613 guards\n",
               opts.route_wait_count + opts.route_event_wait_count, opts.route_event_wait_count, xinput_replay_wait_count(),
               opts.poke_at_count, route_live != NULL ? "live" : "(no live)");
    }
    if (opts.controllers) {
        controller_config config; controller_config_defaults(&config);
        char controller_error[256];
        if (opts.controller_config && !controller_config_load(opts.controller_config, &config, controller_error, sizeof(controller_error))) {
            fprintf(stderr, "--controller-config: %s\n", controller_error); return 2;
        }
        /* An explicit keyboard/script source reserves guest port 0; SDL devices use the others. */
        if (opts.pad_script || (opts.pad_source && strcmp(opts.pad_source, "keyboard") == 0)) {
            config.selectors[0].enabled = false;
            config.selectors[0].guid[0] = '\0'; config.selectors[0].occurrence = -1;
            (void)xinput_pad_connect(0);
        }
        if (opts.controller_save && !controller_config_save(opts.controller_save, &config, controller_error, sizeof(controller_error))) {
            fprintf(stderr, "--controller-save: %s\n", controller_error); return 2;
        }
        char mapping_path[2048]; const char *mapping = opts.controller_mappings;
        if (!mapping && config.mapping_file[0]) {
            const char *slash = opts.controller_config ? strrchr(opts.controller_config, '/') : NULL;
            if (config.mapping_file[0] != '/' && slash) {
                int n = snprintf(mapping_path, sizeof(mapping_path), "%.*s/%s", (int)(slash - opts.controller_config),
                                 opts.controller_config, config.mapping_file);
                if (n < 0 || (size_t)n >= sizeof(mapping_path)) { fprintf(stderr, "controller mapping path too long\n"); return 2; }
                mapping = mapping_path;
            }
        }
        g_controllers = controller_sdl_open(g_present_sink, &config, mapping, controller_error, sizeof(controller_error));
        if (!g_controllers) { fprintf(stderr, "--controllers: %s\n", controller_error); return 2; }
        controller_sdl_install(g_controllers);
        char controller_status[1024]; controller_sdl_describe(g_controllers, controller_status, sizeof(controller_status));
        printf("controller policy opt-in modeled four-port SDL gamepads (guest USB timing not emulated)\n%s", controller_status);
        printf("controller mappings: local --controller-mappings FILE; profiles: --controller-config FILE / --controller-save FILE\n");
    }
    xinput_devices_set_fatal(host_d3d8_fatal);
    kernel_hle_set_fatal(host_kernel_fatal);
    (void)xinput_devices_register();
    (void)xvoice_media_register(); /* T1086: measured disconnected-device path only. */
    (void)mu_register();
    if (opts.synthetic_pad || opts.controllers) (void)xinput_devices_register_pad();
    printf("xinput handlers %zu registered\n", xinput_hle_implemented_count());
    (void)d3d8_device_register();
    (void)d3d8_target_query_register();
    printf("d3d8 handlers  %zu registered\n", d3d8_hle_implemented_count());
    (void)xgrph_texture_register();
    (void)xgrph_object_lifetime_register();
    (void)xgrph_swizzle_register();
    (void)xgrph_shader_query_register();
    printf("xgrph handlers %zu registered\n", xgrph_hle_implemented_count());
    if (opts.ac97_ready_flag_given) {
        printf("note: --ac97-ready is deprecated, the modelled AC97 codec is now the default "
               "(use --no-ac97-ready for the old NOT_READY boot)\n");
    }
    if (opts.ac97_ready) {
        dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    }

    set_code_bounds(&image);
    printf("code span      0x%08X-0x%08X\n", g_xbox_code_lo, g_xbox_code_hi);

    /* Back the synthetic thunk window with real zeroed memory, so a kernel DATA
     * export read through its slot yields a correct zero instead of faulting.
     * Not fatal if it fails -- function calls still work and a variable read then
     * faults with a diagnosis -- but say so, because it changes what the run can
     * reach. */
    const bool window_mapped = kernel_thunk_map_window();
    if (!window_mapped) {
        fprintf(stderr,
                "warning: could not map the synthetic thunk window at 0x%08X; a kernel\n"
                "         DATA export read through its slot will fault\n",
                KERNEL_THUNK_VA_BASE);
    } else {
        printf("thunk window   0x%08X mapped, readable and zero\n", KERNEL_THUNK_VA_BASE);
        if (!kernel_clock_bind_tick_count(KERNEL_THUNK_VA(156u))) {
            fprintf(stderr, "could not bind the observed KeTickCount data word\n");
            xbe_unmap(&image);
            free(data);
            return 1;
        }
        /* The CRT start reads both console-identity exports on every boot. Their
         * structs live in the window's data annex (see kernel_thunk.h) and the
         * patcher below points slots 322/324 there. Policy in kernel_identity.h. */
        if (!kernel_identity_publish(KERNEL_THUNK_VA_XBOX_HARDWARE_INFO,
                                     KERNEL_THUNK_VA_XBOX_KRNL_VERSION)) {
            fprintf(stderr, "could not publish the console identity exports\n");
            xbe_unmap(&image);
            free(data);
            return 1;
        }
        printf("identity       XboxKrnlVersion %u.%u.%u.%u, XboxHardwareInfo flags "
               "0x%08X mcp 0x%02X gpu 0x%02X (retail policy, kernel_identity.h)\n",
               KERNEL_IDENTITY_KRNL_MAJOR, KERNEL_IDENTITY_KRNL_MINOR,
               KERNEL_IDENTITY_KRNL_BUILD, KERNEL_IDENTITY_KRNL_QFE,
               KERNEL_IDENTITY_HARDWARE_FLAGS, KERNEL_IDENTITY_MCP_REVISION,
               KERNEL_IDENTITY_GPU_REVISION);
    }

    /* Default T1147 route remains actual Linux backing-device acquisition.
     * T1191 explicitly instantiates the xemu-qualified primary-master profile
     * only over the real mounted partition0 image, before import patching. */
    const char *t1191_partition0 = "\\Device\\Harddisk0\\partition0";
    bool t1191_partition0_pre_mounted = false;
    kernel_disk_identity_snapshot disk_identity = {0};
    unsigned disk_status;
    if (opts.disk_identity_xemu) {
        if (!kernel_file_mount_host_device(t1191_partition0, opts.hdd_path,
                                           ".tsfp-partition0.bin", 0x80000u)) {
            fprintf(stderr, "could not mount the selected ATA profile backing\n");
            xbe_unmap(&image);
            free(data);
            return 1;
        }
        t1191_partition0_pre_mounted = true;
        kernel_file_device_backing backing = {.root_fd = -1};
        disk_status = DISK_XEMU_IDENTITY_NO_BACKING;
        if (kernel_file_device_backing_acquire(t1191_partition0, &backing)) {
            disk_status = (unsigned)disk_identity_xemu_acquire_backing(&backing, &disk_identity);
            (void)close(backing.root_fd);
        }
        if (disk_status != DISK_XEMU_IDENTITY_OK) {
            fprintf(stderr, "ATA identity profile refused selected image (status %u); "
                            "requires actual nonempty sector-aligned backing\n", disk_status);
            kernel_file_unmount_all();
            xbe_unmap(&image);
            free(data);
            return 1;
        }
    } else {
        disk_status = (unsigned)disk_identity_linux_acquire(opts.hdd_path, &disk_identity);
    }
    const bool disk_published = window_mapped && disk_status == 0u &&
        kernel_disk_identity_publish(KERNEL_THUNK_VA_DISK_MODEL,
                                     KERNEL_THUNK_VA_DISK_SERIAL,
                                     KERNEL_THUNK_VA_DISK_STORAGE, &disk_identity);
    kernel_thunk_set_disk_identity_available(disk_published);
    if (opts.disk_identity_xemu && !disk_published) {
        fprintf(stderr, "could not publish the configured ATA identity exports\n");
        kernel_file_unmount_all();
        xbe_unmap(&image);
        free(data);
        return 1;
    }
    printf("disk identity  %s (%s status %u; model/serial redacted)\n",
           disk_published ? (opts.disk_identity_xemu
                                 ? "INFERRED xemu-qualified primary-master device"
                                 : "actual backing-device snapshot")
                          : "unavailable",
           opts.disk_identity_xemu ? "source" : "backing-device", disk_status);

    if (opts.eeprom_path) {
        const xnet_eeprom_result loaded = xnet_eeprom_load_keyed(opts.eeprom_path, opts.eeprom_key_path);
        if (loaded != XNET_EEPROM_LOADED) {
            fprintf(stderr, "--eeprom: %s\n", xnet_eeprom_result_name(loaded));
            xbe_unmap(&image);
            free(data);
            return 1;
        }
        if (opts.eeprom_key_path && (!window_mapped || !kernel_thunk_publish_eeprom_keys())) {
            fprintf(stderr, "could not publish authenticated EEPROM/HD source keys\n");
            xbe_unmap(&image);
            free(data);
            return 1;
        }
        printf("eeprom         actual256-byte image loaded; rawFFFF and checksum-qualified factoryMAC101%s\n",
               opts.eeprom_key_path ? "; authenticated source321/323 (INFERRED decode, xemu validated)" : "");
    }
    size_t skipped = 0;
    size_t patched = kernel_thunk_patch_table(image.kernel_thunk_addr,
                                              image.kernel_import_count, &skipped);
    printf("thunks patched %zu (%zu left alone)\n", patched, skipped);
    if (patched == 0) {
        fprintf(stderr,
                "no thunk slot was patched, so every kernel call would be dropped\n");
        xbe_unmap(&image);
        free(data);
        return 1;
    }

    kernel_av_pack av_pack = kernel_av_default_pack();
    if (opts.av_pack != NULL && !kernel_av_parse_pack(opts.av_pack, &av_pack)) {
        fprintf(stderr, "--av-pack \"%s\" is not one of: composite, svideo, hdtv\n",
                opts.av_pack);
        xbe_unmap(&image);
        free(data);
        return 1;
    }
    if (!kernel_av_configure_from_image(av_pack, image.base_address)) {
        fprintf(stderr, "could not derive the AV standard from the XBE certificate\n");
        xbe_unmap(&image);
        free(data);
        return 1;
    }
    (void)kernel_av_install_settings();
    if (opts.eeprom_language != 0u) {
        const uint32_t language_setting = opts.eeprom_language;
        if (!kernel_config_set_setting(7u, &language_setting, (uint32_t)sizeof(language_setting))) {
            fprintf(stderr, "--eeprom-language could not store the language setting\n");
            xbe_unmap(&image);
            free(data);
            return 1;
        }
        printf("eeprom         language setting FABRICATED as %u (--eeprom-language)\n", opts.eeprom_language);
    }
    /* HalBootSMCVideoMode is a DATA export read through its thunk-window slot. */
    if (window_mapped &&
        kernel_guest_write_u32(KERNEL_THUNK_VA(KERNEL_AV_ORD_HAL_BOOT_SMC_VIDEO_MODE),
                               kernel_av_smc_video_mode())) {
        printf("av             HalBootSMCVideoMode FABRICATED as %u (pack \"%s\")\n",
               (unsigned)kernel_av_smc_video_mode(), kernel_av_pack_name(av_pack));
    }

    if (opts.voice_log != NULL && !(opts.headless_first_vblank || opts.headless_second_vblank)) {
        fprintf(stderr, "--voice-log requires --headless-first-vblank or --headless-second-vblank\n"); return 1;
    }
    if (opts.voice_log != NULL) {
        g_voice_log = fopen(opts.voice_log, "a");
        if (g_voice_log == NULL) { perror("--voice-log"); return 1; }
        const size_t path_size = strlen(opts.voice_log) + sizeof ".resident";
        char *resident_path = malloc(path_size);
        if (resident_path == NULL) return 1;
        snprintf(resident_path, path_size, "%s.resident", opts.voice_log);
        g_voice_resident = fopen(resident_path, "ab");
        free(resident_path);
        if (g_voice_resident == NULL) { perror("--voice-log resident"); return 1; }
        fprintf(g_voice_resident, "# T1793 run start resident tables, relocated text banks, cutscene records\n");
        fflush(g_voice_resident);
        fprintf(g_voice_log, "# T1793 read-only entry observer; raw stack[0]=caller; return unavailable; audio_frames=48k sink cumulative writes\n");
    }
    (void)recomp_dispatch_init();
    kernel_thunk_set_stop_on_missing(!opts.continue_on_missing);
    /* The same flag relaxes the same policy at the address boundary. It does NOT
     * relax an unrouted section or an unknown address: see xdk_thunk.h. */
    xdk_thunk_set_stop_on_missing(!opts.continue_on_missing);
    for (unsigned i = 0; i < opts.mount_count; i++) {
        if (!kernel_file_add_openable(opts.mounts[i])) {
            fprintf(stderr, "could not declare \"%s\" openable\n", opts.mounts[i]);
            xbe_unmap(&image);
            free(data);
            return 1;
        }
        printf("mounted        \"%s\" as an EMPTY stand-in (no content behind it)\n",
               opts.mounts[i]);
    }
    if (opts.disc_path) {
        /* MOUNTED BEFORE THE GUEST RUNS, and validated here. A failure is fatal rather
         * than a warning: an operator who asked for a disc and silently got none would
         * read the resulting missing-asset trace as a lifting problem. */
        if (!kernel_file_mount_disc(opts.disc_device, opts.disc_path)) {
            fprintf(stderr,
                    "could not mount \"%s\" on \"%s\" -- see the reason above\n",
                    opts.disc_path, opts.disc_device);
            xbe_unmap(&image);
            free(data);
            return 1;
        }
        printf("disc           \"%s\" mounted on \"%s\" -- REAL bytes from real "
               "sectors\n",
               opts.disc_path, opts.disc_device);
    }
    if (opts.hdd_path) {
        /* Fatal on failure for the same reason --disc is. An operator who asked for
         * writable storage and silently got none would watch the title reboot itself and
         * read that as a lifting problem, which is exactly the misdiagnosis this flag
         * exists to end. */
        if (!kernel_file_mount_host_dir(opts.hdd_device, opts.hdd_path)) {
            fprintf(stderr,
                    "could not back \"%s\" with \"%s\" -- see the reason above\n",
                    opts.hdd_device, opts.hdd_path);
            xbe_unmap(&image);
            free(data);
            return 1;
        }
        printf("hdd            \"%s\" backs \"%s\" -- WRITABLE: NtCreateFile really "
               "creates here,\n               and what it creates is still there next "
               "run\n",
               opts.hdd_path, opts.hdd_device);

        /* The raw config-area device. The title opens partition0 read/write from
         * XapiSelectCachePartition and takes its reboot path if that fails, so refusing it is
         * not neutral. Backed by ONE REGULAR FILE under --hdd, never a host raw device. The
         * capacity is INFERRED from the console's config area, not measured. */
        const char *device_prefix = "\\Device\\Harddisk0\\partition0";
        if (!t1191_partition0_pre_mounted &&
            !kernel_file_mount_host_device(device_prefix, opts.hdd_path,
                                           ".tsfp-partition0.bin", 0x80000u)) {
            fprintf(stderr, "could not back \"%s\" with a file under \"%s\"\n", device_prefix,
                    opts.hdd_path);
            xbe_unmap(&image);
            free(data);
            return 1;
        }
        printf("partition0     \"%s\" is a VIRTUAL device backed by %s/.tsfp-partition0.bin "
               "(0x80000 bytes)\n",
               device_prefix, opts.hdd_path);

        /* HalDiskCachePartitionCount is a DATA export read through its thunk slot. It reads 0
         * from the zeroed window, which is a plausible-looking wrong answer. */
        if (opts.cache_partitions != 0u && window_mapped) {
            *(volatile uint32_t *)(uintptr_t)KERNEL_THUNK_VA(40u) = opts.cache_partitions;
            printf("cache parts    HalDiskCachePartitionCount reads %u -- FABRICATED (no HDD "
                   "geometry exists behind this host)\n",
                   opts.cache_partitions);
        }

        /* One formattable cache partition per counted slot, Partition3 upward (MEASURED:
         * XapiSelectCachePartition stores index + 3). The title formats the one it picks
         * with its own XapiFormatFATVolumeEx, so each is a raw image plus a directory view,
         * both under --hdd. Nothing is created on the host beyond the empty image files
         * until the title formats one. */
        for (unsigned slot = 0u; slot < opts.cache_partitions; slot++) {
            const unsigned number = KERNEL_FILE_CACHE_PARTITION_FIRST + slot;
            char cache_prefix[64];
            char cache_image[64];
            char cache_dir[64];
            (void)snprintf(cache_prefix, sizeof(cache_prefix),
                           "\\Device\\Harddisk0\\Partition%u", number);
            (void)snprintf(cache_image, sizeof(cache_image), ".tsfp-cache%u.bin", number);
            (void)snprintf(cache_dir, sizeof(cache_dir), ".tsfp-cache%u", number);
            if (!kernel_file_mount_cache_partition(cache_prefix, opts.hdd_path, number,
                                                   cache_image, cache_dir,
                                                   KERNEL_FILE_DEVICE_CAPACITY_MAX)) {
                fprintf(stderr, "could not back \"%s\" with files under \"%s\"\n",
                        cache_prefix, opts.hdd_path);
                xbe_unmap(&image);
                free(data);
                return 1;
            }
            printf("cache part %u   \"%s\" is a VIRTUAL cache partition: raw image %s/%s, "
                   "directory view %s/%s\n",
                   number, cache_prefix, opts.hdd_path, cache_image, opts.hdd_path,
                   cache_dir);
        }
    }
    if (opts.open_missing_as_empty) {
        kernel_file_set_missing_policy(KERNEL_FILE_MISSING_EMPTY);
        printf("open policy    a name with no volume behind it opens as an EMPTY, "
               "FABRICATED file\n");
    }
    if (opts.stub_status_set) {
        /* Applies to every ordinal the guest imports, implemented or not; a real
         * implementation never consults its default, so only stubs are affected. */
        for (uint32_t i = 0; i < image.kernel_import_count; i++) {
            (void)kernel_hle_set_default_return(image.kernel_imports[i], opts.stub_status);
        }
        printf("stub status    0x%08X (FABRICATED -- the tail of the trace is a hint)\n",
               opts.stub_status);
    }

    recomp_func_t entry = recomp_lookup(image.entry_point);
    if (!entry) {
        fprintf(stderr, "the lift produced no function at the entry point 0x%08X\n",
                image.entry_point);
        xbe_unmap(&image);
        free(data);
        return 1;
    }

    if (!prepare_guest_stack()) {
        xbe_unmap(&image);
        free(data);
        return 1;
    }
    /* Armed BEFORE any control page is built, because `kernel_thread_control_init`
     * writes `monitor+0x14` from the injected VA at the instant it initialises a
     * block and never revisits it. Arming after `prepare_main_thread_control` would
     * leave the MAIN thread with a NULL notify and every guest thread with a live
     * one, which is the worst of both: it works until the one arm that does not. */
    if (!monitor_thunk_arm()) {
        fprintf(stderr, "could not arm the Prcb debug-monitor notify\n");
        xbe_unmap(&image);
        free(data);
        return 1;
    }
    printf("monitor notify 0x%08X armed (slot %u, %u stack args)\n",
           (unsigned)MONITOR_THUNK_NOTIFY_VA, (unsigned)MONITOR_THUNK_NOTIFY_SLOT,
           (unsigned)MONITOR_THUNK_NOTIFY_STACK_ARGS);
    if (!prepare_main_thread_control()) {
        /* Not fatal: the entry point was measured not to read `fs` on its success
         * path. Say so anyway, because it changes what this run can reach. */
        fprintf(stderr, "warning: could not give the main thread a KPCR; any `fs:`\n"
                        "         read will fault at a small address\n");
    }

    /* Installed AFTER the main KPCR exists, because installing publishes the current
     * level immediately and a publish with no KPCR is counted as a failure. From here
     * on every raise and lower reaches `fs:[0x24]` on the thread that made it. */
    kernel_sync_set_irql_publisher(publish_guest_irql);

    /* Hand the thread model the two things only this program can do: resolve a
     * guest VA to lifted code, and install guest register state before entering it.
     * Registered BEFORE any guest code runs, because the entry point creates its
     * thread on its first kernel call. */
    const kernel_thread_host_ops thread_ops = {
        .has_code = host_thread_has_code,
        .enter = host_thread_enter,
        .terminate = host_thread_terminate,
        .termination_confirmed = host_thread_termination_confirmed,
        .wait_refused = host_thread_wait_refused,
    };
    if (!kernel_thread_set_host_ops(&thread_ops)) {
        fprintf(stderr, "could not install the guest thread operations\n");
        xbe_unmap(&image);
        free(data);
        return 1;
    }

    if (opts.interactive && opts.gpu_live && opts.gpu_live_inferred) {
        if (!d3d8_gpu_set_stream_limit(D3D8_GPU_STREAM_MAX_CAPACITY)) {
            fprintf(stderr,"could not configure bounded interactive GPU recording\n");
            xbe_unmap(&image);
            free(data);
            return 1;
        }
        printf("GPU recording interactive bounded growth: %zu commands maximum (default %u)\n",
               d3d8_gpu_stream_limit(),D3D8_GPU_STREAM_CAPACITY);
    }

    char live_module_profile[1024];
    if (opts.gpu_live && opts.gpu_live_inferred &&
        opts.gpu_replay_dir != NULL) {
      char profile_error[256];
      if (!live_module_maker_profile_directory(
              opts.gpu_replay_dir, live_module_profile,
              sizeof live_module_profile, profile_error,
              sizeof profile_error)) {
        fprintf(stderr, "live raster cache: %s\n", profile_error);
        return 2;
      }
      opts.gpu_replay_dir = live_module_profile;
      fprintf(stdout,
              "live raster mapping INFERRED (pinned xemu source): window "
              "position to positive Vulkan viewport; cache %s\n",
              live_module_profile);
    }
    if (opts.gpu_replay_dir != NULL) {
        d3d8_swap_replay_config replay = d3d8_swap_replay_default_config();
        replay.spv_directory = opts.gpu_replay_dir;
        replay.dump_directory = opts.gpu_replay_dump;
        replay.width = opts.gpu_replay_width;
        replay.height = opts.gpu_replay_height;
        replay.strict = !opts.gpu_replay_lenient;
        replay.visibility = opts.gpu_live && opts.gpu_live_inferred;
        replay.flip_y = opts.gpu_replay_flip_y;
        replay.dump_every = opts.gpu_replay_dump_every;
        replay.dump_last = opts.gpu_replay_dump_last;
        replay.viewport_inverse_modules = opts.gpu_replay_undo_viewport;
        replay.window_clip_modules = opts.gpu_replay_window_to_clip;
        replay.viewport_from_target = opts.gpu_replay_viewport_from_target;
        replay.allowed_inferences = d3d8_swap_replay_host_inferences(
            opts.gpu_replay_assume_program_mode, opts.gpu_replay_output_state, opts.gpu_replay_combiner,
            opts.gpu_replay_viewport_from_target, opts.gpu_replay_standin);
        if (opts.gpu_replay_output_state) {
            replay.output_groups = GPU_PGRAPH_OUTPUT_ALL_MEASURED;
        }
        replay.combiner = opts.gpu_replay_combiner;
        if (opts.gpu_replay_rt_texture) {
            replay.render_target_texture = true;
            replay.render_target_texture_census = opts.gpu_replay_rt_texture_census;
            replay.allowed_inferences |= D3D8_SWAP_REPLAY_INFER_RT_TEXTURE;
        }
        if (opts.gpu_replay_surface_source) {
            replay.surface_source = true;
            replay.allowed_inferences |= D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE;
        }
        if (opts.gpu_replay_target_persist || opts.gpu_replay_surface_source) {
            /* T736 measured that a target persists across frames, so the surface source (whose kept images are the same memory) takes it too */
            replay.target_persist = true;
            replay.allowed_inferences |= D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST;
        }
        replay.standin_texture = opts.gpu_replay_standin;
        replay.standin_pattern = (gpu_standin_pattern)opts.gpu_replay_standin_pattern;
        replay.standin_texel_units = opts.gpu_replay_standin_texel_units;
        memcpy(replay.standin_unit_rules, opts.gpu_replay_standin_unit_rules, sizeof replay.standin_unit_rules);
        replay.standin_unit_rule_count = opts.gpu_replay_standin_unit_rule_count;
        replay.draw_dump_path = opts.gpu_replay_draw_dump;
        replay.live_only = opts.gpu_live; /* T838: the live renderer draws, the CPU replay is skipped */
        replay.standin_width = opts.gpu_replay_standin_width;
        replay.standin_height = opts.gpu_replay_standin_height;
        memcpy(replay.standin_rgba, opts.gpu_replay_standin_rgba, sizeof replay.standin_rgba);
        char replay_error[256];
        if (!d3d8_swap_replay_enable(&replay, replay_error, sizeof replay_error)) {
            fprintf(stderr, "--gpu-replay: %s\n", replay_error);
            xbe_unmap(&image);
            free(data);
            return 1;
        }
        if (opts.gpu_live && opts.gpu_live_translate) {
            char maker_error[256];
            g_live_maker = live_module_maker_create(opts.gpu_replay_dir, NULL, NULL, maker_error, sizeof maker_error);
            if (g_live_maker == NULL) {
                fprintf(stderr, "--gpu-live-translate: %s\n", maker_error);
                xbe_unmap(&image);
                free(data);
                return 1;
            }
            live_module_maker_set_live_raster(g_live_maker,
                                              opts.gpu_live_inferred);
            d3d8_swap_replay_set_live_module_maker(live_render_make_module, g_live_maker);
            printf("live renderer  ON-DEMAND module translation (T847, INFERRED translators): a vertex program or combiner configuration "
                   "in no table is translated by tools/nv2a/live_modules.py into %s and the draw is retried\n",
                   opts.gpu_replay_dir);
        }
        if (opts.gpu_live) {
            char live_error[256];
            if (!live_render_attach(g_present_sink, live_error, sizeof live_error)) {
                fprintf(stderr, "--gpu-live: %s\n", live_error);
                xbe_unmap(&image);
                free(data);
                return 1;
            }
            printf("live renderer  attached: every title frame's model goes to the live renderer at the Swap (CPU replay skipped), the "
                   "front buffer is queued by the second d3d8_present observer, render targets are registered at SetRenderTarget\n");
        }
        printf("gpu replay     ON (opt-in): the recorded stream is replayed at each present on a Vulkan "
               "device; %s; INFERENCES ALLOWED (program header 0x2078, viewport registers as c58/c59, "
               "component defaults, D3DCOLOR byte order); modules from %s (%s viewport programs); "
               "flip_y %s (T100f undecided)\n",
               replay.strict ? "strict" : "lenient", opts.gpu_replay_dir,
               replay.viewport_inverse_modules ? "T96 undo-viewport" : "raw", replay.flip_y ? "ON" : "off");
        if (opts.gpu_replay_assume_program_mode) {
            printf("gpu replay     INFERRED (T441): an execution mode the stream never wrote is the program mode "
                   "when a vertex program was started\n");
        }
        if (replay.window_clip_modules) {
            printf("gpu replay     XEMU-LEVEL (T714/HQ57, none class): x and y of window-coordinate oPos are "
                   "converted to clip space with constants 58 and 59, modules from %s; this is emulator evidence, "
                   "not NV2A silicon. The z-only xy extension remains open (HQ21); a draw with no viewport scale "
                   "is refused\n",
                   opts.gpu_replay_dir);
        }
        if (opts.gpu_replay_viewport_from_target) {
            printf("gpu replay     INFERRED (T477): whole-target viewport c58/c59 from measured target dimensions; "
                   "D3D half-size and negative-y mapping follow the measured viewport emitter, not NV2A hardware evidence\n");
        }
        if (opts.gpu_replay_output_state) {
            printf("gpu replay     OUTPUT STATE ON (T267, T441): scissor, cull, blend, alpha test, depth, stencil "
                   "and clear are decoded and applied, with their named inferences allowed\n");
            printf("gpu replay     INFERRED (T578): the 2D engine blits CopyRects emits (subchannels 2 and 3) are applied "
                   "as SRCCOPY rectangle copies between the replayed A8R8G8B8 surface images (xemu's image blit), a "
                   "surface with no replayed image or any other format refuses the frame\n");
            printf("gpu replay     XEMU-LEVEL (T832/HQ58, emulator evidence, never NV2A silicon): those blits follow the "
                   "live renderer's CopyRects planner, a row wider than the narrower pitch is cut to it (x offsets 0 "
                   "only), rectangles of one surface may overlap (rows ascending, one buffered row, a 3 row overlap "
                   "smears), colour formats 7 and 6 force alpha 0xFF and 0; R5G6B5 and Y8 byte copies stay refused\n");
        }
        if (opts.gpu_replay_combiner) {
            printf("gpu replay     COMBINER ON (T478, T75): the register combiner the stream programmed replaces the "
                   "fixed fragment stage, modules combiner_<sha256>.spv from %s; INFERRED: a factor word is ARGB with "
                   "A in the top byte, oD0 and oD1 reach the combiner unclamped. Textures, fog and a configuration "
                   "with no module refuse the frame\n",
                   opts.gpu_replay_dir);
        }
        if (opts.gpu_replay_standin) {
            char standin_what[64];
            if (opts.gpu_replay_standin_pattern == GPU_STANDIN_CHECKER) {
                (void)snprintf(standin_what, sizeof standin_what, "coloured checker (red/yellow/blue/cyan)");
            } else {
                (void)snprintf(standin_what, sizeof standin_what, "single colour %02X%02X%02X%02X", opts.gpu_replay_standin_rgba[0],
                               opts.gpu_replay_standin_rgba[1], opts.gpu_replay_standin_rgba[2], opts.gpu_replay_standin_rgba[3]);
            }
            printf("gpu replay     STAND-IN TEXTURE ON (T497): a %ux%u texture of the %s is "
                   "given to combiner texture stage 0. IT IS NOT THE TITLE'S TEXTURE (the title's textures are not "
                   "decoded, T480) and a frame that sampled it is NOT a rendering of the title. INFERRED: it is sampled "
                   "nearest, clamped, row 0 on top (TEXTURE_SAMPLING)%s. The stage program is the stream's own and the "
                   "texture coordinate is what the vertex program wrote to oT0: a program that writes none refuses "
                   "the frame\n",
                   opts.gpu_replay_standin_width, opts.gpu_replay_standin_height, standin_what,
                   opts.gpu_replay_standin_texel_units
                       ? ", oT0 in TEXELS divided by the stand-in's size (T713, the T510 unnormalised path, INFERRED)"
                       : opts.gpu_replay_standin_unit_rule_count != 0u
                       ? ", the unit (oT0 in TEXELS or normalised) chosen PER DRAW by the vertex program digest (T719, INFERRED from "
                         "measured programs, an unlisted program refuses)"
                       : ", oT0 normalised");
        }
        if (opts.gpu_replay_rt_texture) {
            printf("gpu replay     RENDER TARGET TEXTURE ON (T510): a texture stage bound to a render target header that an earlier pass "
                   "of the same frame drew samples that pass's image. Every other binding (no earlier image, a later producer, the "
                   "target being drawn, a partial alias, an ambiguous mapping, a Format other than linear A8R8G8B8, an address mode "
                   "other than clamp, a filter other than 0x02062000) refuses the frame by name. INFERRED: the replayed image is the "
                   "surface the hardware samples, texel units for the coordinate, bilinear and clamped\n");
        }
        if (opts.gpu_replay_surface_source) {
            printf("gpu replay     SURFACE SOURCE ON (T633 (1), T596, the rule MEASURED in xemu T736, the timing INFERRED): a surface no pass of the frame drew is its current memory image, the kept image an earlier frame's pass left "
                   "under its Data word, else the guest memory under it read as A8R8G8B8 (the host never writes a GPU result there, MEASURED all zero on the retail "
                   "boots), for texture stages and for the CopyRects blits and their byte path. A size, pitch or Format that differs, a CPU written surface "
                   "under a kept image, an alias and a blit before the draw refuse the frame by name\n");
        }
        if (opts.gpu_replay_target_persist || opts.gpu_replay_surface_source) {
            printf("gpu replay     TARGET PERSISTENCE ON (T633 (2), MEASURED in xemu T736%s): a render target pass starts from the image an earlier frame's pass left under its Data "
                   "word (and the blits since) instead of a fresh image. A surface with no kept image starts fresh, a kept image of another size or over CPU written "
                   "guest memory refuses the frame by name\n",
                   opts.gpu_replay_target_persist ? "" : ", implied by the surface source");
        }
        if (opts.gpu_replay_dump_every != 0u || opts.gpu_replay_dump_last) {
            printf("gpu replay     DUMP CADENCE (T484): %s%s\n",
                   opts.gpu_replay_dump_every != 0u ? "every Nth frame, N from --gpu-replay-dump-every" : "",
                   opts.gpu_replay_dump_last ? (opts.gpu_replay_dump_every != 0u ? " and the last frame at the end"
                                                                                 : "the last frame at the end")
                                             : "");
        }
    }

    vblank_schedule_trace = opts.trace_vblank_schedule;
    if (opts.census_icalls) {
        function_census_set_present_source(d3d8_frame_queue_total);
        if (opts.census_window_set) {
            function_census_set_window(opts.census_window_first, opts.census_window_last);
        }
        function_census_enable(true);
        if (opts.census_phases != NULL && !function_census_phases_start(opts.census_phases)) {
            fprintf(stderr, "--census-phases: cannot start phase marking for %s\n", opts.census_phases);
            return 1;
        }
        printf("function census ON (opt-in, T821, read-only): every indirect call target of the lifted dispatcher counted "
               "per target with its first present, thread and pushed return address%s; direct calls are not seen; "
               "observation only, not a model\n",
               opts.census_window_set ? ", and the calls of the chosen present window kept in order" : "");
    }
    if (opts.watch_write_set.count != 0u) {
        if (!guest_watch_start(&opts.watch_write_set, opts.watch_write_log, opts.watch_write_max, d3d8_frame_queue_total,
                               watch_pad_polls)) {
            fprintf(stderr, "--watch-write: cannot start the write watch\n");
            return 1;
        }
        printf("write watch ON (T1741, passive): %u range(s), page protection + single step, log %s\n",
               opts.watch_write_set.count, opts.watch_write_log != NULL ? opts.watch_write_log : "stderr");
    }
    if (opts.dump_guest_set.count != 0u) {
        guest_dump_set_forced_state(opts.forced_state, d3d8_frame_queue_total);
        if (!guest_dump_start(&opts.dump_guest_set, opts.dump_guest_max_bytes, opts.dump_guest_dir)) {
            fprintf(stderr, "--dump-guest-range: cannot start dumping into %s\n", opts.dump_guest_dir);
            return 1;
        }
        if (opts.forced_state) {
            printf("FORCED-STATE ON (T1613, FABRICATED): guestpoke.<label> requests under %s are applied at their phase\n",
                   opts.dump_guest_dir);
        }
        printf("guest dump ON (T1599, no poke unless --forced-state): %u range(s), %llu byte(s) per dump, files %s/guestdump.<phase> on "
               "SIGUSR1 and guestdump.exit at exit\n",
               opts.dump_guest_set.count, (unsigned long long)opts.dump_guest_set.total_bytes, opts.dump_guest_dir);
    }
    if (opts.dump_on_button) {
        button_dump_host_config button_config;
        memset(&button_config, 0, sizeof button_config);
        button_config.set = &opts.dump_guest_set;
        button_config.dir = opts.dump_guest_dir;
        button_config.after_count = opts.dump_after_count;
        memcpy(button_config.after, opts.dump_after, sizeof button_config.after);
        button_config.threshold = (uint8_t)opts.dump_button_threshold;
        button_config.coalesce = opts.dump_button_coalesce;
        button_config.max_pending = opts.dump_button_max_pending;
        button_config.max_dumps = opts.dump_button_max_dumps;
        button_config.max_bytes = opts.dump_button_max_bytes;
        button_config.start_poll = opts.dump_button_start_poll;
        button_config.after_replay = opts.dump_button_after_replay;
        button_config.forced_state = opts.forced_state;
        button_config.idle_every = opts.dump_button_idle_every;
        button_config.max_idle = opts.dump_button_max_idle;
        button_config.present = d3d8_frame_queue_total;
        button_config.armed = host_route_handed_over;
        char button_error[200];
        if (!button_dump_host_start(&button_config, button_error, sizeof button_error)) {
            fprintf(stderr, "--dump-on-button: %s\n", button_error);
            return 1;
        }
        printf("button dump ON (T1629, read-only, passive): port 0 digital button edges (threshold %u), after %u frame list, "
               "files %s/buttons/guestdump.<label>, manifest %s/buttons.jsonl%s\n",
               opts.dump_button_threshold, opts.dump_after_count, opts.dump_guest_dir, opts.dump_guest_dir,
               opts.dump_button_after_replay ? ", armed at the route handover" : "");
    }
    const bool replay_owns_recording = opts.gpu_replay_dir != NULL;
    if (opts.profile_calls) {
        call_profile_enable(true);
        call_profile_set_code_range(g_xbox_code_lo, g_xbox_code_hi);
        /* T441: the frame profile and the swap replay both want the GPU model's one recorded-commands
         * observer, so with --gpu-replay the call counting (and so --stop-after-calls) runs alone and the
         * per present frame profile is OFF, announced below. The replay decodes the same commands. */
        if (!replay_owns_recording && !d3d8_frame_profile_enable(true)) {
            fprintf(stderr, "--profile-calls could not start the frame profile\n");
            xbe_unmap(&image);
            free(data);
            return 2;
        }
        for (unsigned index = 0u; !replay_owns_recording && index < opts.profile_watch_count; index++) {
            (void)d3d8_frame_profile_add_watch(opts.profile_watch[index]);
        }
        if (opts.stop_after_set &&
            !call_profile_set_limit(opts.stop_after_ordinal ? THUNK_KIND_ORDINAL : THUNK_KIND_XDK,
                                    opts.stop_after_id, opts.stop_after_count)) {
            fprintf(stderr, "--stop-after-calls could not be armed\n");
            xbe_unmap(&image);
            free(data);
            return 2;
        }
        printf("call profile   ON (opt-in, T422): every dispatch counted per address and caller, "
               "%s; observation only%s\n",
               replay_owns_recording ? "NO per present frame profile (--gpu-replay owns the recorded "
                                       "commands, T441)"
                                     : "every present summarised",
               opts.stop_after_set ? ", run cut by guest progress (--stop-after-calls)" : "");
    }
    vblank_readers_trace = opts.trace_vblank_readers;
    recomp_vblank_quiescence_configure(opts.check_vblank_quiescence);
    d3d8_vblank_effects_configure(opts.couple_vblank_effects);
    d3d8_overlay_set_consume_policy(opts.overlay_consume);
    if (opts.overlay_consume) {
        printf("overlay consume ON (opt-in, T540): one completed coupled vblank clears pending 0x8700. "
               "INFERRED one-vblank latency; actual NV2A overlay scanout/latch timing and pixels remain "
               "unmeasured\n");
    }
    if (opts.dump_overlay != NULL) {
        printf("overlay dump ON (opt-in, T537): each UpdateOverlay writes its picture into %s (%s files). A pure "
               "observer, reads guest memory only. The YUY2 to RGB matrix is UNMEASURED for the overlay hardware: "
               "BT.601 studio range as the title's own XMV library converts, and the raw Y U V planes are "
               "written beside each image. Scaling is nearest neighbour, the hardware filter is not modelled\n",
               opts.dump_overlay, opts.dump_overlay_max == 0u ? "all" : "limited");
    }
    d3d8_overlay_set_xemu_image(opts.overlay_xemu_image);
    if (opts.overlay_xemu_image) {
        printf("overlay xemu image ON (opt-in, T831, %s): YUY2 to RGB with the integer matrix 298/409/100/208/516, "
               "bilinear RGB scale with 8 bit weights, repeat edges, a box one pixel wider and taller than the "
               "destination rectangle (the dump file); the present sink gets the declared rectangle. The "
               "register step values are not read\n",
               D3D8_OVERLAY_IMAGE_XEMU_LABEL);
    }
    if (!d3d8_overlay_set_key_composition(opts.overlay_xemu_key, d3d8_resource_virtual_of_registered_data)) {
        fprintf(stderr, "--overlay-xemu-key could not allocate its picture buffers\n");
        return 2;
    }
    if (opts.overlay_xemu_key) {
        char summary[640];
        if (d3d8_overlay_key_summary(summary, sizeof summary) != 0u) {
            printf("%s\n", summary);
        }
        if (d3d8_overlay_present_summary(summary, sizeof summary) != 0u) {
            printf("%s\n", summary);
        }
    }
    d3d8_flip_configure(opts.model_flips);
    if (opts.couple_vblank_effects) {
        printf("vblank effects ON (opt-in, T372): each completed wait-vblank applies the helper's "
               "count, timestamps, threshold and flip processing (T407, display start, gamma ramp "
               "and PGRAPH go to a record, not a register file); the field status port and the "
               "MMIO acknowledge stay unmodelled and field state refuses\n");
    }
    if (opts.model_flips) {
        printf("flip model ON (opt-in, T407): each Swap queues its flip, the coupled wait "
               "completes it; the flip's own commands stay out of the recorded stream\n");
    }
    if (opts.headless_first_vblank || opts.headless_second_vblank) {
        if (!prepare_first_vblank(opts.headless_second_vblank)) {
            xbe_unmap(&image);
            free(data);
            return 1;
        }
        recomp_callback_set_dispatch_level(opts.vblank_dispatch_level);
        if (opts.vblank_owner_waits != 0u && !recomp_second_vblank_set_owner_waits(opts.vblank_owner_waits)) {
            fprintf(stderr, "could not configure the owner-wait callback budget\n");
            return 1;
        }
        printf(opts.headless_second_vblank ?
               "callback policy startup plus one credited CPU callback; no general timing\n" :
               "callback policy one startup CPU vblank callback; "
               "no clock/MMIO/event/GPU stats advance or general timing\n");
        if (opts.vblank_owner_waits != 0u) {
            printf("vblank owner waits ON (opt-in, T460): each of the owner thread's next %u completed "
                   "waits delivers one CPU callback from inside the wait (a stand-in for the DPC the "
                   "original runs while the owner is blocked), then a named stop. FABRICATED timing: the "
                   "blank rate is the title's own wait count, not a display clock. Quiescence is "
                   "checked at every delivery and every unmeasured device state refuses\n",
                   (unsigned)opts.vblank_owner_waits);
        }
        if (opts.vblank_poll_blank) {
            if (!recomp_second_vblank_set_poll_blank(true)) {
                fprintf(stderr, "could not configure the frame wait poll blank\n");
                return 1;
            }
            printf("vblank poll blank ON (%s, T696): a frame wait 0x1538C0 whose exit test fails with the "
                   "counter equal to its last exit delivers ONE CPU callback from inside the poll (a stand-in "
                   "for the 60 Hz interrupt), counted in the owner-wait budget, then a named stop. FABRICATED "
                   "timing: the blank rate is the title's own poll count, not a display clock. The counter and "
                   "[0x78AD2C] are never written\n",
                   opts.vblank_poll_blank_by_default ? "DEFAULT since T762, --no-vblank-poll-blank opts out" : "opt-in");
        }
        if (opts.vblank_worker_blanks != 0u) {
            if (!recomp_second_vblank_set_worker_blanks(opts.vblank_worker_blanks)) {
                fprintf(stderr, "could not configure the worker blank budget\n");
                return 1;
            }
            /* A hold that cannot end must be a named stop, so it ends before the thread watchdog does. */
            recomp_second_vblank_set_worker_hold_ms(opts.interactive ? 30000u : opts.thread_timeout_ms / 10u * 8u);
            printf("vblank worker blanks ON (opt-in, T592): each of the loading bar worker's next %u completed "
                   "waits delivers one CPU callback from inside the wait while the owner is PROVEN not to read "
                   "the counter (parked in the loading bar gate or blocked on the worker), the worker idles "
                   "in its wait until then, then a named stop. FABRICATED timing: the blank rate is the "
                   "worker's own wait count, not a display clock\n",
                   (unsigned)opts.vblank_worker_blanks);
        }
        if (opts.vblank_dispatch_level) {
            printf("vblank callback IRQL ON (opt-in, T370): raised to DISPATCH_LEVEL around the "
                   "callback through the kernel IRQL model and restored on every exit\n");
        }
    }

    printf("guest stack    esp=0x%08X\n", g_esp);
    printf("main kpcr      fs=0x%08X\n", g_fs_base);
    if (!recomp_second_vblank_set_interactive(opts.interactive)) {
        fprintf(stderr, "--interactive callback continuation refused\n"); return 2;
    }
    recomp_second_vblank_set_shutdown_query(opts.interactive ? interactive_closed : NULL);
    if (opts.interactive)
        printf("interactive ON (T908, FABRICATED): presenter wall-clock pacing at 59.94 Hz; callback evidence budgets continue with all proof guards; close window to stop\n");
    printf("thread watchdog %u ms\n", opts.thread_timeout_ms);
    printf("\n--- entering guest code at 0x%08X ---\n", image.entry_point);
    fflush(stdout);

    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        entry();
        /* The entry point IS expected to return: it is a thread-spawning stub.
         * Still recorded as a stop, because this host thread really has stopped
         * running guest code, and the guest thread's own stop is reported
         * separately below. */
        host_run_stop(HOST_STOP_RETURNED, 0u, 0u, "entry point returned");
    }
    host_run_disarm();
    xdk_thunk_stream_virtual_cancel_pending();

    /*
     * Now wait for the thread that holds all the real work.
     *
     * NOTHING IS REPORTED UNTIL THE JOIN COMPLETES. Reporting first would
     * interleave this thread's many small writes with the guest thread's, and
     * `stderr` is unbuffered -- so a single logical line can arrive split in half
     * around another thread's output. For a run whose entire deliverable is a
     * readable ordered trace, that is a real loss, and ordering is a cheaper fix
     * than locking every printf.
     *
     * The main thread runs no more guest code from here on, so this is a pure wait
     * -- which is also why most of the HLE's remaining single-thread assumptions
     * are latent rather than live in this configuration. The only genuinely
     * concurrent window is between `PsCreateSystemThreadEx` returning and the entry
     * point returning, which is the handful of calls the trace shows on thread 1.
     */
    const unsigned started = kernel_thread_started_count();
    unsigned still_running = 0u;
    if (started > 0u) {
        printf("\n--- %u guest thread(s) started; waiting up to %u ms ---\n", started,
               opts.thread_timeout_ms);
        fflush(stdout);
        if (opts.interactive) {
            const uint64_t began = interactive_wall_ms();
            uint64_t shutdown_began = 0u;
            do {
                still_running = kernel_thread_join_all(100u);
                const uint64_t now = interactive_wall_ms();
                if (interactive_closed() && shutdown_began == 0u) shutdown_began = now;
                if (still_running && shutdown_began != 0u && now - shutdown_began >= 5000u) {
                    fprintf(stderr, "interactive shutdown: %u guest threads did not reach a safepoint within 5 s; exiting process without concurrent renderer teardown\n", still_running);
                    report_thread_stops();
                    button_dump_stop_for_exit(true);
                    fflush(NULL);
                    _Exit(1);
                }
            } while (still_running && interactive_wall_ms() - began < opts.thread_timeout_ms);
        } else {
            still_running = kernel_thread_join_all(opts.thread_timeout_ms);
        }
    } else {
        printf("\n--- no guest thread was started; %u requested and not running ---\n",
               kernel_thread_unstarted_count());
    }

    printf("\n--- where each host thread stopped ---\n");
    printf("\nmain thread (entry point 0x%08X) stopped:", image.entry_point);
    report_stop(host_run_result());
    report_thread_stops();
    {
        thunk_trace_entry failures[THUNK_FAILURE_MAX];
        uint64_t failure_total = 0u;
        const unsigned failure_count = thunk_trace_recent_failures(failures, THUNK_FAILURE_MAX, &failure_total);
        pthread_mutex_lock(&g_thread_stop_lock);
        host_report_run_end_evidence(stdout, failures, failure_count, failure_total,
                                     g_thread_orderly_exits, g_thread_unlisted_stops, &report_names);
        pthread_mutex_unlock(&g_thread_stop_lock);
    }
    if (still_running > 0u) {
        printf("\nWATCHDOG: %u guest thread(s) STILL RUNNING after %u ms.\n",
               still_running, opts.thread_timeout_ms);
        printf("  This is a HANG, reported rather than waited on. The ordinal trace\n"
               "  below ends at the last call the thread made, which is where to look.\n");
    }

    report_trace(opts.trace_report);
    button_dump_stop_for_exit(opts.interactive);
    guest_dump_stop();
    guest_watch_stop();
    if (opts.census_icalls) {
        function_census_phases_stop();
        function_census_report(stdout);
    }
    if (opts.profile_calls) {
        const call_profile_names profile_names = {
            .ordinal_name = xbox_kernel_ordinal_name,
            .xdk_name = xdk_thunk_name_of,
        };
        call_profile_report(stdout, opts.profile_calls_top, &profile_names);
        if (!replay_owns_recording) {
            d3d8_frame_profile_report(stdout);
        }
        const uint64_t ticks = kernel_clock_peek();
        printf("virtual clock  %llu ticks = %.3f s at %u Hz (guest time, advances only through modeled "
               "vblanks and clock reads), KeTickCount %u ms\n",
               (unsigned long long)ticks, (double)ticks / (double)KERNEL_CLOCK_FREQUENCY_HZ,
               (unsigned)KERNEL_CLOCK_FREQUENCY_HZ, (unsigned)kernel_clock_tick_count_ms());
        fflush(stdout);
    }
    if (opts.model_flips) {
        const d3d8_flip_hardware flips = d3d8_flip_hardware_get();
        printf("flip model: queued %llu, processed %llu, display start writes %llu (last %#x), "
               "gamma uploads %llu, PGRAPH increments %llu\n",
               (unsigned long long)flips.queued, (unsigned long long)flips.flips,
               (unsigned long long)flips.display_start_writes, (unsigned)flips.display_start,
               (unsigned long long)flips.gamma_uploads,
               (unsigned long long)flips.pgraph_increments);
    }
    /* Through the same sink as everything else, which is stderr by default and is
     * flushed here so the block cannot land inside the next one. */
    fflush(stdout);
    if (opts.headless_second_vblank) {
        recomp_second_vblank_snapshot callbacks;
        recomp_second_vblank_get_snapshot(&callbacks);
        printf("CPU callback policy startup %u, continuation %u, refused %u, outstanding credits %llu\n",
               (unsigned)callbacks.first_delivered,(unsigned)callbacks.second_delivered,
               (unsigned)callbacks.refused,(unsigned long long)callbacks.credits);
        if (callbacks.owner_budget != 0u)
            printf("owner-wait callbacks delivered %u of %u budget\n",
                   (unsigned)callbacks.owner_delivered,(unsigned)callbacks.owner_budget);
        if (opts.vblank_poll_blank)
            printf("poll blanks delivered %u (of the owner-wait callbacks)\n",(unsigned)callbacks.poll_delivered);
        if (callbacks.worker_budget != 0u)
            printf("worker-wait callbacks delivered %u of %u budget, waits held %u, held after the last blank %u, "
                   "owner gate entries %u (T592)\n",
                   (unsigned)callbacks.worker_delivered,(unsigned)callbacks.worker_budget,
                   (unsigned)callbacks.worker_held,(unsigned)callbacks.worker_final_held,
                   (unsigned)callbacks.owner_gate_entries);
        if (callbacks.owner_budget != 0u && callbacks.frames_admitted != 0u)
            printf("frame waits admitted with nothing to wait for %u (T549, no delivery)\n",
                   (unsigned)callbacks.frames_admitted);
    }
    route_probe_mem_stop();
    route_probe_report();
    if (opts.replay_input != NULL) {
        const uint64_t consumed = xinput_source_port_poll_count(0u), recorded_polls = xinput_replay_total_polls();
        printf("input replay: consumed %llu of %llu recorded polls%s\n", (unsigned long long)consumed,
               (unsigned long long)recorded_polls, consumed < recorded_polls ? " (the run ended before the recording did)" : "");
    }
    xdk_thunk_report();
    xonline_hle_report_backlog();
    if (opts.xonline_offline) {
        const xonline_offline_stats online = xonline_offline_stats_get();
        printf("XONLINE offline T904 INFERRED: Startup %llu, GetUsers %llu, Cleanup %llu, refs left %u; no service calls\n",
               (unsigned long long)online.startups,
               (unsigned long long)online.user_queries,
               (unsigned long long)online.cleanups,
               (unsigned)online.references);
    }
    if (opts.passive_audio_completion) {
        const dsound_completion_stats completion = dsound_completion_get_stats();
        printf("passive audio  thunk-routed stream methods %llu, packets %u (completed %u, aborted %u), buffer plays %u (finished %u), "
               "observations %u, buffer stops %u (T681, T733)\n",
               (unsigned long long)xdk_thunk_completion_method_call_count(), (unsigned)completion.stream_packets,
               (unsigned)completion.stream_completed, (unsigned)completion.stream_aborted, (unsigned)completion.buffer_plays,
               (unsigned)completion.buffer_finished, (unsigned)completion.observations,
               (unsigned)completion.buffer_stops);
        printf("stream pitch   %llu accepted owned frequency transactions (T1245); PCM timing/projection INFERRED, no APU pitch writes\n",
               (unsigned long long)dsound_stream_frequency_set_count());
        dsound_completion_census_entry census[DSOUND_COMPLETION_CENSUS_MAX];
        const size_t census_count = dsound_completion_stream_census(census, DSOUND_COMPLETION_CENSUS_MAX);
        for (size_t i = 0u; i < census_count; i++) {
            printf("passive audio  stream format tag %#x channels %u rate %u block %u bits %u: packets %u, bytes %u, first at guest %.1f s, last %.1f s, %s (T1238)\n",
                   (unsigned)census[i].tag, (unsigned)census[i].channels, (unsigned)census[i].rate, (unsigned)census[i].block,
                   (unsigned)census[i].bits, (unsigned)census[i].packets, (unsigned)census[i].bytes,
                   (double)census[i].first_ms / 1000.0, (double)census[i].last_ms / 1000.0,
                   census[i].mixed != 0u ? "MIXED into the sink" : "NOT MIXED (silent)");
        }
    }
    if (opts.async_file_io) {
        const kernel_async_io_stats io = kernel_async_io_get_stats();
        printf("async file io  submitted %u, completed %u, pending %u, refused %u, failed %u, bytes %llu (T743, XEMU-LEVEL timing T763)\n",
               io.submitted, io.completed, io.pending, io.refused, io.failed, (unsigned long long)io.bytes);
        printf("async file io  file object reads %u, blocking waits %u, wait timeouts %u, APC refusals %u (T764)\n",
               io.file_object_reads, io.waits, io.wait_timeouts, io.apc_refused);
        if (interactive_async_io)
            printf("async file io  elapsed-time completions %u, calls %u, backwards samples %u, model rebases %u "
                   "(T940/T963, FABRICATED monotonic host clock)\n",
                   io.elapsed_completions, io.elapsed_calls, io.elapsed_backwards, io.elapsed_rebases);
        if (opts.async_file_io_spin != 0u)
            printf("async file io  spin completions %u (T821, threshold %u safepoints, INFERRED)\n", io.spin_completions,
                   opts.async_file_io_spin);
    }
    if (opts.headless_movie_audio)
        printf("movie stream   streams %zu, operations %llu, completions %llu, thunk-routed methods %llu\n",
               dsound_movie_stream_count(), (unsigned long long)dsound_movie_stream_operation_count(),
               (unsigned long long)dsound_movie_stream_completion_count(),
               (unsigned long long)xdk_thunk_movie_method_call_count());
    if (opts.headless_movie_audio && dsound_audio_runtime_active())
        printf("movie audio    pcm packets %llu, frames %llu (%.3f s at 44100 Hz), refused %llu\n",
               (unsigned long long)dsound_movie_stream_pcm_packets(),
               (unsigned long long)dsound_movie_stream_pcm_frames(),
               (double)dsound_movie_stream_pcm_frames() / 44100.0,
               (unsigned long long)dsound_movie_stream_pcm_refused());
    if (opts.headless_movie_audio)
        printf("movie device   calls %s, SynchPlayback route %s, outstanding references %u, "
               "SynchPlayback calls %llu\n",
               dsound_device_movie_calls_enabled() ? "on" : "off",
               xdk_thunk_synch_route_enabled() ? "on" : "off", dsound_device_movie_references(),
               (unsigned long long)xdk_thunk_synch_call_count());
    if (opts.overlay_consume)
        printf("overlay        buffers consumed by the policy %llu\n",
               (unsigned long long)d3d8_overlay_consumed_count());
    if (g_audio_sink != NULL) {
        const present_audio_counts heard = present_audio_sink_counts(g_audio_sink);
        if (g_audio_kind == PRESENT_AUDIO_WAV_FILE) {
            printf("audio sink     wav-file: %s, %llu frames at 48000 Hz stereo (timing INFERRED)\n",
                   opts.audio_output != NULL ? opts.audio_output : "tsfp-audio.wav",
                   (unsigned long long)present_audio_sink_frames(g_audio_sink));
        } else {
            printf("audio sink     sdl: frames accepted %llu, pulled %llu, underrun %llu (events %llu; a ring that ran dry with the media clock on is a REBUFFER, counted on the next lines, not here), idle silence %llu, "
                   "overrun %llu (timing INFERRED)\n",
                   (unsigned long long)heard.written, (unsigned long long)heard.pulled,
                   (unsigned long long)heard.underrun_frames, (unsigned long long)heard.underrun_events,
                   (unsigned long long)heard.idle_frames, (unsigned long long)heard.overrun_frames);
            printf("audio clock    rebuffers %llu (each one cuts every voice), silence while rebuffering %llu frames (%.0f ms), refill target now %llu frames, peak %llu frames (T1250)\n",
                   (unsigned long long)heard.rebuffer_events, (unsigned long long)heard.rebuffer_frames,
                   (double)heard.rebuffer_frames * 1000.0 / 48000.0,
                   (unsigned long long)heard.prefill_frames, (unsigned long long)heard.prefill_peak_frames);
            {
                present_audio_stall stalls[PRESENT_AUDIO_STALL_MAX];
                const size_t stall_total = present_audio_sink_stalls(g_audio_sink, stalls, PRESENT_AUDIO_STALL_MAX);
                for (size_t index = 0; index < stall_total; index++) {
                    const present_audio_stall *record = &stalls[index];
                    printf("audio stall    #%zu %s at wall %llu ms, media %llu ms, written %llu ms, guest silent %llu ms, "
                           "lasted %llu ms (%s), ring %llu to %llu frames, target %llu, writes during %llu, first write after %llu ms, resume %llu ms after it, clock %llu ms (T1250)\n",
                           index, record->initial ? "start" : "dry", (unsigned long long)record->wall_ms,
                           (unsigned long long)record->media_ms, (unsigned long long)record->written_ms,
                           (unsigned long long)record->since_write_ms, (unsigned long long)record->duration_ms,
                           record->reason == 1u ? "refilled" : record->reason == 2u ? "gave up" : record->reason == 3u ? "kicked" :
                           record->reason == 4u ? "ring full" : "open", (unsigned long long)record->fill_start,
                           (unsigned long long)record->fill_end, (unsigned long long)record->target_frames,
                           (unsigned long long)record->writes_during, (unsigned long long)record->first_write_ms,
                           (unsigned long long)record->resume_wait_ms, (unsigned long long)record->clock_ms);
                }
            }
            printf("audio pump     %llu renders outside DirectSoundDoWork, %llu frames (%.0f ms) (T1250)\n",
                   (unsigned long long)atomic_load(&g_audio_pump_renders), (unsigned long long)atomic_load(&g_audio_pump_frames),
                   (double)atomic_load(&g_audio_pump_frames) * 1000.0 / 48000.0);
            printf("audio timing   ring %llu frames, fill min %llu max %llu (frames), producer blocked %llu times, %.1f ms total, "
                   "%.1f ms longest, writes %llu, device pulls %llu (timing INFERRED)\n",
                   (unsigned long long)heard.ring_frames,
                   (unsigned long long)(heard.fill_min == UINT64_MAX ? 0u : heard.fill_min),
                   (unsigned long long)heard.fill_max, (unsigned long long)heard.block_events,
                   (double)heard.block_ns / 1e6, (double)heard.block_max_ns / 1e6,
                   (unsigned long long)heard.write_calls, (unsigned long long)heard.pulls);
            {
                /* Frames the device actually received: consumed minus the governor's extra, plus the slowed playback's stretch, plus silence. */
                const double played_frames = (double)(heard.pulled - heard.latency_trim_frames + heard.slow_frames + heard.cutout_frames);
                printf("audio cutouts  %llu runs of silence after playback began, %.0f ms total, %.0f ms longest "
                       "(every voice is cut together), %.1f percent of the %.1f s the device played; slowed playback %llu pulls, "
                       "%.0f ms of audio stretched, slowest %.0f per mille of real time (T1248); first resume after a dry ring %llu times, "
                       "%.0f ms mean and %llu ms longest from the guest's first write to sound (T1250)\n",
                       (unsigned long long)heard.cutout_events, (double)heard.cutout_frames * 1000.0 / 48000.0,
                       (double)heard.cutout_max_frames * 1000.0 / 48000.0,
                       played_frames != 0.0 ? 100.0 * (double)heard.cutout_frames / played_frames : 0.0, played_frames / 48000.0,
                       (unsigned long long)heard.slow_pulls, (double)heard.slow_frames * 1000.0 / 48000.0,
                       heard.slow_min_ratio_q16 != 0u ? (double)heard.slow_min_ratio_q16 * 1000.0 / 65536.0 : 1000.0,
                       (unsigned long long)heard.resume_events,
                       heard.resume_events != 0u ? (double)heard.resume_wait_sum_ms / (double)heard.resume_events : 0.0,
                       (unsigned long long)heard.resume_wait_max_ms);
            }
            printf("audio stretch  %s (T1250): %llu blocks, %.0f ms of audio stretched (slowed without a cut), %.0f ms looped while the ring was too thin (%.0f ms of it decaying), %llu fade outs into a rebuffer; "
                   "SUMMARY rebuffers %llu, silence %.0f ms, longest cut %.0f ms, stretched %.0f ms, refill target peak %llu frames\n",
                   heard.stretch_enabled != 0u ? "ON (pitch preserving WSOLA)" : heard.slow_low_frames != 0u ? "off, resampler (the pitch follows the speed)" : "OFF (--audio-min-rate 0: pause and refill)",
                   (unsigned long long)heard.stretch_blocks, (double)heard.slow_frames * 1000.0 / 48000.0,
                   (double)heard.stretch_hold_frames * 1000.0 / 48000.0, (double)heard.stretch_decay_frames * 1000.0 / 48000.0,
                   (unsigned long long)heard.stretch_fadeouts,
                   (unsigned long long)heard.rebuffer_events, (double)heard.cutout_frames * 1000.0 / 48000.0,
                   (double)heard.cutout_max_frames * 1000.0 / 48000.0, (double)heard.slow_frames * 1000.0 / 48000.0,
                   (unsigned long long)heard.prefill_peak_frames);
            printf("audio sync     (T1487) effective: ring fill %.0f ms + SDL device buffer %.1f ms = %.0f ms from the guest mixing a sample to the device pulling it, plus the OS audio stack (not measurable here). "
                   "Video follows the audio clock (frames pulled by the device), so it leads the audible sound by about the device buffer; --av-sync-offset-ms is %d (positive delays the video). "
                   "The stretch left real time %llu times (each an audible onset), %llu stretch blocks, %.0f ms stretched; stretch control %s\n",
                   (double)(heard.written - heard.pulled) * 1000.0 / 48000.0,
                   (double)present_audio_sink_device_frames(g_audio_sink) * 1000.0 / 48000.0,
                   (double)(heard.written - heard.pulled) * 1000.0 / 48000.0 + (double)present_audio_sink_device_frames(g_audio_sink) * 1000.0 / 48000.0,
                   (int)opts.av_sync_offset_ms, (unsigned long long)heard.stretch_engagements, (unsigned long long)heard.stretch_blocks,
                   (double)heard.slow_frames * 1000.0 / 48000.0, opts.audio_stretch_legacy ? "LEGACY" : "smoothed and slew limited (T1487)");
            printf("audio latency  ring fill now %llu frames (%.0f ms, this is the latency the host adds), SDL device buffer %zu frames "
                   "(%.1f ms), governor target %llu frames (%s): silence dropped %llu frames, rate trim consumed %llu extra frames "
                   "(T1235)\n",
                   (unsigned long long)(heard.written - heard.pulled), (double)(heard.written - heard.pulled) * 1000.0 / 48000.0,
                   present_audio_sink_device_frames(g_audio_sink),
                   (double)present_audio_sink_device_frames(g_audio_sink) * 1000.0 / 48000.0,
                   (unsigned long long)heard.latency_target_frames, heard.latency_target_frames != 0u ? "ON" : "off",
                   (unsigned long long)heard.latency_silence_frames, (unsigned long long)heard.latency_trim_frames);
        }
        present_timeline_stop();
        host_audio_pump_stop();
        dsound_audio_runtime_stop();
        /* Presenter jobs may still run while holding/flushing the window. Detach its
         * audio clock before freeing that sink, under the same video lock they use. */
        present_video_sink_set_playback(g_present_sink, NULL);
        if (!present_audio_sink_close(g_audio_sink))
            fprintf(stderr, "audio sink finalization failed\n");
        g_audio_sink = NULL;
    }
    if (g_present_sink != NULL) {
        const present_video_counts shown = present_video_sink_counts(g_present_sink);
        printf("present sink   %s: pictures %llu (failed %llu), modelled vblanks %llu, vblanks that showed a new "
               "picture %llu\n", present_video_name(g_present_kind),
               (unsigned long long)shown.submitted, (unsigned long long)shown.failed,
               (unsigned long long)shown.vblanks, (unsigned long long)shown.presented);
    }
    {
        uint64_t probe_hits = 0u, probe_calls = 0u;
        kernel_guest_probe_stats(&probe_hits, &probe_calls);
        printf("guest probe    readable-page probes: %llu answered from the per-thread cache, %llu system calls (T819)\n",
               (unsigned long long)probe_hits, (unsigned long long)probe_calls);
        uint64_t miss_cold = 0u, miss_conflict = 0u, miss_stale = 0u, epochs = 0u;
        kernel_guest_probe_miss_stats(&miss_cold, &miss_conflict, &miss_stale, &epochs);
        printf("guest probe    cache misses (T1289): %llu cold, %llu conflict (another page held the slot), %llu stale (a guest page change flushed it), %llu epoch flushes\n",
               (unsigned long long)miss_cold, (unsigned long long)miss_conflict, (unsigned long long)miss_stale, (unsigned long long)epochs);
        uint64_t fast_reads = 0u, fast_writes = 0u, copy_calls = 0u;
        kernel_guest_copy_stats(&fast_reads, &fast_writes, &copy_calls);
        printf("guest copy     guest reads %llu and writes %llu done as plain copies, %llu needed a system call (T827)\n",
               (unsigned long long)fast_reads, (unsigned long long)fast_writes, (unsigned long long)copy_calls);
    }
    if (g_live_render) {
        live_render_detach();
        live_render_report_print(stdout);
        live_module_maker_print(g_live_maker, stdout);
    }
    cpu_sampler_stop();
    {
        const kernel_clock_stats clock = kernel_clock_get_stats();
        const double total_s = (double)clock.total_ticks / (double)KERNEL_CLOCK_FREQUENCY_HZ;
        const double frame_s = (double)clock.floor_ticks / (double)KERNEL_CLOCK_FREQUENCY_HZ;
        printf("guest clock    total %.3f s = frames %.3f s (%llu blanks) + above-floor %.3f s: reads %llu (%llu advanced the clock past "
               "the floor), stalls %.3f s, waits advanced to a due time %.3f s in %llu calls (T1237, MEASURED host observation)\n",
               total_s, frame_s, (unsigned long long)clock.frames, total_s - frame_s, (unsigned long long)clock.reads,
               (unsigned long long)clock.reads_above_floor, (double)clock.stall_ticks / (double)KERNEL_CLOCK_FREQUENCY_HZ,
               (double)clock.advance_to_ticks / (double)KERNEL_CLOCK_FREQUENCY_HZ, (unsigned long long)clock.advance_to_calls);
    }
    if (g_present_sink != NULL && g_present_kind == PRESENT_VIDEO_WINDOW) {
        const present_video_timing timing = present_video_sink_timing(g_present_sink);
        printf("present timing wall-clock ms between presented pictures: n %llu, min %.2f, median %.2f, p95 %.2f, max %.2f, "
               "over 25 ms %llu, replaced before shown %llu (timing INFERRED)\n",
               (unsigned long long)timing.intervals, (double)timing.min_us / 1e3, (double)timing.median_us / 1e3,
               (double)timing.p95_us / 1e3, (double)timing.max_us / 1e3, (unsigned long long)timing.over_25ms,
               (unsigned long long)timing.replaced);
        printf("present gaps    longest modelled time between two pictures %.0f ms after picture %llu (the title's own pause, not a stall)\n",
               (double)timing.model_gap_max_ns / 1e6, (unsigned long long)timing.model_gap_after);
        printf("present playback queue max %llu, waits %llu (%.1f ms, %llu gave up), dropped %llu, shown late %llu, left queued %llu\n",
               (unsigned long long)timing.queue_max, (unsigned long long)timing.queue_waits,
               (double)timing.queue_wait_us / 1e3, (unsigned long long)timing.queue_timeouts,
               (unsigned long long)timing.dropped, (unsigned long long)timing.late_shown,
               (unsigned long long)timing.queued_at_end);
        printf("present vblank  wall ms between vblank hook exits (frame pacing, T1235): n %llu, <10 %llu, <15 %llu, <18 %llu, <22 %llu, "
               "<34 %llu, <50 %llu, <100 %llu, >=100 %llu; longest %.1f %.1f %.1f %.1f %.1f ms\n",
               (unsigned long long)timing.vblank_intervals, (unsigned long long)timing.vblank_hist[0],
               (unsigned long long)timing.vblank_hist[1], (unsigned long long)timing.vblank_hist[2],
               (unsigned long long)timing.vblank_hist[3], (unsigned long long)timing.vblank_hist[4],
               (unsigned long long)timing.vblank_hist[5], (unsigned long long)timing.vblank_hist[6],
               (unsigned long long)timing.vblank_hist[7], (double)timing.vblank_spikes_us[0] / 1e3,
               (double)timing.vblank_spikes_us[1] / 1e3, (double)timing.vblank_spikes_us[2] / 1e3,
               (double)timing.vblank_spikes_us[3] / 1e3, (double)timing.vblank_spikes_us[4] / 1e3);
        printf("present pacing vblank hook: slept %llu times, %.1f ms total, late %llu times (max %.2f ms), guest time in "
               "hook %.1f ms (max %.2f ms)\n",
               (unsigned long long)timing.pace_sleeps, (double)timing.pace_slept_us / 1e3,
               (unsigned long long)timing.pace_late, (double)timing.pace_late_max_us / 1e3,
               (double)timing.hook_us_total / 1e3, (double)timing.hook_us_max / 1e3);
    }
    {
        const gft_names names = {xdk_thunk_name_of, xbox_kernel_ordinal_name};
        guest_frame_trace_print(stdout, &names);
    }
    if (g_present_sink != NULL) {
        char described[320];
        if (present_video_sink_describe(g_present_sink, described, sizeof described) != 0u)
            printf("present sink   %s\n", described);
    }
    if (opts.dump_overlay != NULL)
        printf("overlay dump   pictures written %llu, not written %llu\n",
               (unsigned long long)d3d8_overlay_dump_written(),
               (unsigned long long)d3d8_overlay_dump_failed());
    if (opts.overlay_xemu_key) {
        char summary[640];
        if (d3d8_overlay_key_summary(summary, sizeof summary) != 0u)
            printf("overlay key    %s\n", summary);
        if (d3d8_overlay_present_summary(summary, sizeof summary) != 0u)
            printf("overlay present %s\n", summary);
    }
    xgrph_hle_report_backlog();
    monitor_thunk_report();
    report_irql_publishing();
    report_non_volatile_settings();
    report_file_attempts();
    report_symbolic_links();

    printf("\n--- remaining kernel backlog, busiest first ---\n");
    fflush(stdout);
    unsigned imports[XBE_MAX_KERNEL_IMPORTS];
    uint32_t import_count = image.kernel_import_count;
    if (import_count > XBE_MAX_KERNEL_IMPORTS) {
        /* Clamped rather than trusted. The count comes from a parsed file, and this
         * is a fixed-size stack array. */
        fprintf(stderr, "warning: %u kernel imports exceeds the %u-entry bound\n",
                import_count, (unsigned)XBE_MAX_KERNEL_IMPORTS);
        import_count = XBE_MAX_KERNEL_IMPORTS;
    }
    for (uint32_t i = 0; i < import_count; i++) {
        imports[i] = image.kernel_imports[i];
    }
    kernel_hle_report_missing(imports, import_count);

    /* The exit status is decided by the GUEST THREAD where there is one, because
     * the main thread's "entry point returned" is now the uninteresting half. */
    pthread_mutex_lock(&g_thread_stop_lock);
    const host_stop_reason verdict = host_run_verdict(
        host_run_result()->reason, g_thread_stops, GUEST_THREAD_STOPS, still_running);
    pthread_mutex_unlock(&g_thread_stop_lock);

    if (g_shot_configured && g_shot_ready) {
        char shot_summary[SHOT_PATH_MAX + 128u]; /* T1720b: greppable end-of-run line */
        if (shot_hotkey_summary(&g_shot_hotkey, shot_summary, sizeof shot_summary) != 0u) printf("%s\n", shot_summary);
        fflush(stdout);
    }

    if (still_running > 0u && g_controllers != NULL) {
        /* Keep callback storage and SDL sink alive under unjoined guest threads.
         * Only cancel motors; process exit is the existing watchdog policy. */
        controller_sdl_cancel(g_controllers);
        button_dump_stop_for_exit(opts.interactive);
        fflush(stdout); fflush(stderr); _exit(1);
    }

    /* T760: the window stays open at the stop, after the report above (the sink is not touched by a guest thread
     * after the hook is removed). */
    if (g_present_sink != NULL) {
        fflush(stdout);
        (void)d3d8_overlay_set_present_hook(NULL, NULL, NULL, NULL);
        if (opts.present_capture != NULL) {
            const bool captured = present_video_sink_capture(g_present_sink, opts.present_capture);
            printf("present sink   capture %s: %s\n", opts.present_capture, captured ? "written" : "FAILED (window sink only)");
        }
        present_video_sink_hold(g_present_sink, opts.present_hold_ms);
        controller_sdl_close(g_controllers); g_controllers = NULL;
        present_video_sink_close(g_present_sink);
        g_present_sink = NULL;
    }
    if (still_running > 0u) {
        /* Do NOT unmap the guest or free the image: a thread is still executing
         * lifted code against both, and tearing them down under it would replace a
         * clean watchdog report with a crash in the teardown. `_exit` skips
         * atexit handlers for the same reason. */
        button_dump_stop_for_exit(opts.interactive);
        fflush(stdout);
        fflush(stderr);
        _exit(1);
    }

    const bool originals_cleaned = xdk_original_configure(false) && xmv_original_configure(false);
    const bool callback_cleaned = cleanup_first_vblank();
    (void)kernel_clock_bind_tick_count(0u);
    kernel_clock_set_frame_hook(NULL);
    if (!host_xnet_dpc_shutdown())
        fprintf(stderr, "XNET private DPC stack cleanup refused; allocation preserved\n");
    d3d8_cube_surface_reset();
    dsound_listener_reset();
    const bool buffers_cleaned = dsound_buffer_reset_checked();
    if (!buffers_cleaned)
        fprintf(stderr, "buffer cleanup refused changed ownership; allocations preserved\n");
    const bool streams_cleaned = dsound_stream_reset_checked();
    if (!streams_cleaned)
        fprintf(stderr, "stream cleanup refused changed ownership; allocations preserved\n");
    dsound_effects_binding_reset();
    dsound_device_reset();
    xgrph_hle_shutdown();
    if (d3d8_swap_replay_enabled()) {
        d3d8_swap_replay_dump_last();
        const d3d8_swap_replay_stats replay = d3d8_swap_replay_get_stats();
        printf("gpu replay     presents %llu, replayed %llu, empty %llu, refused %llu, skipped %llu, "
               "draws %llu, dumps %llu%s%s\n",
               (unsigned long long)replay.presents, (unsigned long long)replay.frames_replayed,
               (unsigned long long)replay.frames_empty, (unsigned long long)replay.frames_refused,
               (unsigned long long)replay.frames_skipped, (unsigned long long)replay.draws,
               (unsigned long long)replay.dumps_written, replay.latched ? ", STOPPED: " : "",
               replay.latched ? replay.error : "");
        /* T1268: headroom of the per-frame model lists (a frame over a capacity STOPS the replay, so the video freezes) */
        printf("gpu replay     per-frame peaks (T1268): snapshots %llu of %u, draws %llu of %u, indices %llu of %u\n",
               (unsigned long long)replay.snapshots_peak, GPU_PGRAPH_MAX_SNAPSHOTS, (unsigned long long)replay.draws_peak,
               GPU_PGRAPH_MAX_DRAWS, (unsigned long long)replay.indices_peak, GPU_PGRAPH_MAX_INDICES);
        /* T511: its own line, so the one above stays byte for byte what tools/steady_replay.py parsed before */
        printf("gpu replay     polygon offset (T511): applied %llu, unobserved %llu, IGNORED pairs %llu\n",
               (unsigned long long)replay.offset_applied, (unsigned long long)replay.offset_unobserved,
               (unsigned long long)replay.pairs_ignored);
        if (replay.copies_applied + replay.copies_empty + replay.copies_pending != 0u) {
            printf("gpu replay     2D blits (T578): applied %llu, empty %llu, decoded and still pending (no present after them) "
                   "%llu\n",
                   (unsigned long long)replay.copies_applied, (unsigned long long)replay.copies_empty,
                   (unsigned long long)replay.copies_pending);
        }
        char standin_summary[768];
        if (d3d8_swap_replay_standin_summary(standin_summary, sizeof standin_summary) != 0u) {
            printf("gpu replay     %s\n", standin_summary);
        }
        char surface_summary[1536];
        if (d3d8_swap_replay_surface_summary(surface_summary, sizeof surface_summary) != 0u) {
            printf("gpu replay     %s\n", surface_summary);
        }
        char rt_summary[1024];
        if (d3d8_swap_replay_rt_texture_summary(rt_summary, sizeof rt_summary) != 0u) {
            printf("gpu replay     %s\n", rt_summary);
            char census_text[320];
            uint64_t census_draws = 0u;
            for (size_t index = 0u; d3d8_swap_replay_texture_census_at(index, census_text, sizeof census_text, &census_draws); index++) {
                printf("gpu replay     texture census (T510): %s: %llu draw(s)\n", census_text, (unsigned long long)census_draws);
            }
        }
        d3d8_swap_replay_disable();
    }
    xonline_offline_reset();
    xonline_hle_shutdown();
    xnet_hle_shutdown();
    xbe_unmap(&image);
    free(data);
    /* Which verdicts count as the expected end of a bring-up run is host_report.c's
     * host_run_exit_status, where a suite can pin it. */
    return host_run_exit_status(originals_cleaned, callback_cleaned, buffers_cleaned,
                                streams_cleaned, verdict);
}
