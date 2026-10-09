/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T421: the three device calls of the XMV GetNextFrame through the REAL lifted trampolines and the XDK
 * dispatcher, in the order and from the return addresses the retained XMV code makes them:
 *   DirectSoundCreate(NULL, &device, NULL)  return 0x44570B   second reference on the startup device
 *   SynchPlayback(device)                   return 0x44571B   starts the Pause(2) movie streams
 *   device Release(device)                  return 0x445726   gives the reference back
 * SynchPlayback is a direct call the re-lift routes through recomp_lookup_manual (one stop boundary in
 * tools/config/movie_synch_boundaries.json). A lift without it skips this test (exit 77). */
#include "test_d3d8_support.h"
#include "dsound_device.h"
#include "dsound_hle.h"
#include "dsound_listener.h"
#include "dsound_movie_stream.h"
#include "dsound_stream.h"
#include "host_runtime.h"
#include "recomp_abi.h"
#include "recomp_guest_call.h"
#include "thunk_trace.h"
#include "xdk_thunk.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

#define CREATE_STREAM 0x40967Cu
#define PAUSE 0x407B23u
#define WORK 0x407B40u
#define DEVICE_CREATE 0x409635u
#define DEVICE_RELEASE 0x406A8Au
#define SYNCH 0x407A4Cu
#define STREAM_CREATE_CALLER 0x4451F4u
#define PAUSE_CALLER 0x445381u
#define CREATE_CALLER 0x44570Bu
#define SYNCH_CALLER 0x44571Bu
#define RELEASE_CALLER 0x445726u
#define WORK_CALLER 0x4457D9u
#define STARTUP_CREATE_CALLER 0x279D5u
#define DECODER 0x41376170u
#define TICKS_PER_BYTE_RATE 733333333u

static bool irql(uint8_t *out) { *out = 0u; return true; }
static void fatal(uint32_t address, const char *reason) { host_run_stop(HOST_STOP_UNIMPLEMENTED, address, 0u, reason); }
static uint64_t fake_now = 1000u;
static uint64_t clock_now(void) { return fake_now; }
static unsigned callbacks_run;
static bool run_callback(uint32_t callback, uint32_t stream_context, uint32_t packet_context, uint32_t status)
{
    const uint32_t arguments[3] = {stream_context, packet_context, status};
    callbacks_run++;
    return recomp_guest_call_stdcall(callback, arguments, 3u);
}

static uint32_t stack;
/* Resolve like RECOMP_ICALL_SAFE: the manual lookup first, then the lifted table. */
static recomp_func_t resolve(uint32_t address)
{
    recomp_func_t function = recomp_lookup_manual(address);
    if (function == NULL) function = recomp_lookup(address);
    return function;
}
/* Call `address` with a stdcall frame from `caller`. False when the run stopped, true with EAX in `eax`. */
static bool call_row(uint32_t address, uint32_t caller, const uint32_t *args, unsigned count, uint32_t *eax)
{
    uint32_t frame[8] = {caller};
    for (unsigned i = 0u; i < count; i++) frame[1u + i] = args[i];
    CHECK(kernel_guest_write_bytes(stack, frame, 4u * (count + 1u)));
    g_esp = stack;
    g_eax = 0x77u;
    g_ecx = 2u;
    g_edx = 3u;
    g_ebx = 4u;
    g_esi = 5u;
    g_edi = 6u;
    g_ebp = 7u;
    g_fs_base = 8u;
    recomp_func_t function = resolve(address);
    CHECK(function != NULL);
    bool returned = false;
    if (sigsetjmp(*host_run_jmp(), 1) == 0) {
        host_run_arm();
        function();
        returned = true;
    }
    host_run_disarm();
    if (returned) {
        CHECK(g_esp == stack + 4u + 4u * count);
        CHECK(g_ecx == 2u && g_edx == 3u && g_ebx == 4u && g_esi == 5u && g_edi == 6u && g_ebp == 7u);
        *eax = g_eax;
    } else {
        CHECK(g_esp == stack);
    }
    return returned;
}
#define REFUSED(address, caller, args, count, text)                                          \
    do {                                                                                     \
        uint32_t ignored;                                                                    \
        CHECK(!call_row(address, caller, args, count, &ignored));                            \
        CHECK(host_run_result()->guest_address == (address));                                \
        CHECK(strstr(host_run_result()->detail, text) != NULL);                              \
    } while (0)

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    dsound_hle_init();
    if (!xdk_thunk_synch_stop_ready()) {
        printf("SKIP: the lift has no SynchPlayback stop boundary (re-lift with tools/config/movie_synch_boundaries.json)\n");
        return 77;
    }
    map_fixed(0x412000u, 4096u);
    map_fixed(0x4A1000u, 4096u);
    map_fixed(0x42000000u, 0x100000u);
    for (unsigned i = 0u; i < 15u; i++) store(0x4A1CF0u + 4u * i, 0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    const uint32_t desc = SCRATCH_DATA + 256u, format = SCRATCH_DATA + 320u, output = SCRATCH_DATA + 128u;
    const uint32_t descriptor[6] = {0u, 2u, format, 0x445071u, DECODER, 0u};
    uint8_t wave[18] = {1u, 0u, 2u, 0u};
    const uint32_t rate = 44100u, average = 176400u;
    const uint16_t block = 4u, bits = 16u;
    memcpy(wave + 4u, &rate, 4u);
    memcpy(wave + 8u, &average, 4u);
    memcpy(wave + 12u, &block, 2u);
    memcpy(wave + 14u, &bits, 2u);
    CHECK(kernel_guest_write_bytes(desc, descriptor, sizeof(descriptor)));
    CHECK(kernel_guest_write_bytes(format, wave, sizeof(wave)));
    dsound_stream_set_enabled(true);
    dsound_stream_set_irql_provider(irql);
    dsound_stream_set_fatal(fatal);
    dsound_listener_set_fatal(fatal);
    dsound_device_set_fatal(fatal);
    dsound_movie_stream_set_fatal(fatal);
    dsound_movie_stream_set_irql_provider(irql);
    dsound_movie_stream_set_clock(clock_now, TICKS_PER_BYTE_RATE);
    dsound_movie_stream_set_callback_runner(run_callback);
    dsound_movie_stream_set_enabled(true);
    dsound_stream_set_extension(dsound_movie_stream_route_public, dsound_movie_stream_reset_checked);
    dsound_listener_set_work_route(dsound_movie_stream_route_work);
    const xdk_dispatch_entry rows[5] = {
        {CREATE_STREAM, "DirectSoundCreateStream", XDK_MODULE_DSOUND}, {PAUSE, "IDirectSoundStream_Pause", XDK_MODULE_DSOUND},
        {WORK, "DirectSoundDoWork", XDK_MODULE_DSOUND}, {DEVICE_CREATE, "DirectSoundCreate", XDK_MODULE_DSOUND},
        {DEVICE_RELEASE, "IDirectSound_Release", XDK_MODULE_DSOUND},
    };
    CHECK(xdk_thunk_init(rows, 5u));
    CHECK(xdk_thunk_declare_abi(CREATE_STREAM, XDK_CC_STDCALL, 2u, 0u));
    CHECK(xdk_thunk_declare_abi(PAUSE, XDK_CC_STDCALL, 2u, 0u));
    CHECK(xdk_thunk_declare_abi(WORK, XDK_CC_STDCALL, 0u, 0u));
    CHECK(xdk_thunk_declare_abi(DEVICE_CREATE, XDK_CC_STDCALL, 3u, 0u));
    CHECK(xdk_thunk_declare_abi(DEVICE_RELEASE, XDK_CC_STDCALL, 1u, 0u));
    CHECK_EQ_U32(dsound_device_register(), 1u);
    CHECK_EQ_U32(dsound_stream_register(), 10u);
    CHECK(dsound_listener_register() != 0u);
    guest_region_request request = {.bytes = 8192u, .alignment = 4096u, .protect = PAGE_READWRITE, .state = MEM_COMMIT};
    nt_status status;
    const uint32_t allocation = guest_region_alloc(&request, &status);
    CHECK(allocation != 0u);
    stack = allocation + 4096u - 64u;
    CHECK(mprotect((void *)(uintptr_t)(allocation + 4096u), 4096u, PROT_NONE) == 0);

    uint32_t eax = 0xFFFFFFFFu;
    /* The startup made the device, the movie stream holds one reference of its own. */
    store(output, 0xAAAAAAAAu);
    const uint32_t startup_args[3] = {0u, output, 0u};
    CHECK(call_row(DEVICE_CREATE, STARTUP_CREATE_CALLER, startup_args, 3u, &eax));
    CHECK_EQ_U32(eax, 0u);
    const uint32_t device = load(output) - 8u;
    CHECK_EQ_U32(load(device + 4u), 6u);

    /* Nothing is enabled: every one of the three calls from the XMV site stops, each with its own reason. */
    CHECK(!dsound_device_movie_calls_enabled());
    REFUSED(DEVICE_CREATE, CREATE_CALLER, startup_args, 3u, "startup caller");
    const uint32_t synch_args[1] = {device + 8u};
    REFUSED(SYNCH, SYNCH_CALLER, synch_args, 1u, "reads the omitted APU object");
    REFUSED(DEVICE_RELEASE, RELEASE_CALLER, synch_args, 1u, "IDirectSound_Release");
    CHECK_EQ_U32(load(device + 4u), 6u);
    CHECK_EQ_U32(xdk_thunk_synch_call_count(), 0u);

    /* The movie stream and its Pause(2), as EnableAudioStream and GetNextFrame make them. */
    const uint32_t create_args[2] = {desc, output};
    CHECK(call_row(CREATE_STREAM, STREAM_CREATE_CALLER, create_args, 2u, &eax));
    CHECK_EQ_U32(eax, 0u);
    const uint32_t stream = load(output);
    CHECK(dsound_movie_stream_owns(stream));
    CHECK_EQ_U32(load(device + 4u), 7u);
    const uint32_t pause_args[2] = {stream, 2u};
    CHECK(call_row(PAUSE, PAUSE_CALLER, pause_args, 2u, &eax));
    for (unsigned index = 0u; index < 2u; index++) {
        const uint32_t base = SCRATCH_DATA + 0x400u + 0x40u * index;
        const uint32_t words[6] = {0x42000000u + 0x1000u * index, 0x1000u * (index + 1u), base + 0x20u, base + 0x24u, index, 0u};
        CHECK(kernel_guest_write_bytes(base, words, sizeof(words)));
        CHECK_EQ_U32(dsound_movie_stream_process(stream, base, 0u), 0u);
    }
    dsound_movie_stream_snapshot held;
    CHECK(dsound_movie_stream_get_snapshot(stream, &held));
    CHECK(held.pause_mode == 2u && held.queued == 2u);

    /* Turn the XMV device calls on, the way main.c does for --headless-movie-audio. */
    CHECK_EQ_U32(dsound_device_register_movie(), 1u);
    dsound_device_set_movie_calls(true);
    CHECK(xdk_thunk_set_synch_handler(dsound_movie_stream_route_synch));

    /* GetNextFrame's sequence: Create, SynchPlayback, Release. */
    store(output, 0xAAAAAAAAu);
    CHECK(call_row(DEVICE_CREATE, CREATE_CALLER, startup_args, 3u, &eax));
    CHECK_EQ_U32(eax, 0u);
    CHECK_EQ_U32(load(output), device + 8u);
    CHECK_EQ_U32(load(device + 4u), 8u);
    fake_now = 5000u;
    thunk_trace_reset();
    CHECK(call_row(SYNCH, SYNCH_CALLER, synch_args, 1u, &eax));
    CHECK_EQ_U32(eax, 0u);
    dsound_movie_stream_snapshot started;
    CHECK(dsound_movie_stream_get_snapshot(stream, &started));
    CHECK_EQ_U32(started.pause_mode, 0u);  /* resumed */
    CHECK(started.head_deadline > 5000u);  /* the clock restarted at the SynchPlayback tick */
    CHECK_EQ_U32(started.queued, 2u);
    CHECK_EQ_U32(callbacks_run, 0u);  /* starting completes nothing */
    CHECK_EQ_U32(xdk_thunk_synch_call_count(), 1u);
    size_t entries_seen;
    const thunk_trace_entry *trace = thunk_trace_entries(&entries_seen);
    CHECK(entries_seen == 1u);
    if (entries_seen == 1u) CHECK(trace[0].address == SYNCH && trace[0].return_address == SYNCH_CALLER);
    CHECK(call_row(DEVICE_RELEASE, RELEASE_CALLER, synch_args, 1u, &eax));
    CHECK_EQ_U32(eax, 7u);  /* the new count, the stream still holds its own */
    CHECK_EQ_U32(load(device + 4u), 7u);
    CHECK_EQ_U32(dsound_device_movie_references(), 0u);

    /* A second SynchPlayback changes nothing: no stream is paused with mode 2 any more. */
    const uint64_t deadline = started.head_deadline;
    fake_now = 9000u;
    CHECK(call_row(SYNCH, SYNCH_CALLER, synch_args, 1u, &eax));
    CHECK_EQ_U32(eax, 0u);
    CHECK(dsound_movie_stream_get_snapshot(stream, &started));
    CHECK(started.head_deadline == deadline);

    /* Scope: the wrong caller, the wrong argument and a nonzero IRQL or global state all stop the run. */
    REFUSED(SYNCH, SYNCH_CALLER + 1u, synch_args, 1u, "outside the measured XMV site");
    REFUSED(SYNCH, 0x445716u, synch_args, 1u, "outside the measured XMV site");
    const uint32_t other[1] = {device + 12u};
    REFUSED(SYNCH, SYNCH_CALLER, other, 1u, "not the owned device interface");
    const uint32_t null_device[1] = {0u};
    REFUSED(SYNCH, SYNCH_CALLER, null_device, 1u, "not the owned device interface");
    store(0x4124A8u, 1u);
    REFUSED(SYNCH, SYNCH_CALLER, synch_args, 1u, "global audio state");
    store(0x4124A8u, 0u);
    dsound_movie_stream_set_enabled(false);
    REFUSED(SYNCH, SYNCH_CALLER, synch_args, 1u, "disabled");
    dsound_movie_stream_set_enabled(true);
    CHECK_EQ_U32(xdk_thunk_synch_call_count(), 2u);  /* refusals are not answered calls */

    /* Release without a movie reference, from the wrong caller, or with a stream argument stops. */
    REFUSED(DEVICE_RELEASE, RELEASE_CALLER, synch_args, 1u, "no movie reference");
    CHECK(call_row(DEVICE_CREATE, CREATE_CALLER, startup_args, 3u, &eax));
    REFUSED(DEVICE_RELEASE, RELEASE_CALLER + 1u, synch_args, 1u, "measured movie caller");
    const uint32_t stream_arg[1] = {stream};
    REFUSED(DEVICE_RELEASE, RELEASE_CALLER, stream_arg, 1u, "not the owned device interface");
    CHECK(call_row(DEVICE_RELEASE, RELEASE_CALLER, synch_args, 1u, &eax));
    CHECK_EQ_U32(eax, 7u);

    /* DoWork from the three movie sites runs the model: GetNextFrame, CloseDecoder and the title loop. A
     * stranger is the old refusal. The model has operations counted for each answered site. */
    const uint64_t operations = dsound_movie_stream_operation_count();
    const uint32_t work_sites[3] = {WORK_CALLER, 0x444FDAu, 0x305A3u};
    for (unsigned site = 0u; site < 3u; site++) CHECK(call_row(WORK, work_sites[site], NULL, 0u, &eax));
    CHECK(dsound_movie_stream_operation_count() == operations + 3u);
    REFUSED(WORK, 0x1CE493u, NULL, 0u, "unsupported caller");
    CHECK(dsound_movie_stream_operation_count() == operations + 3u);

    /* The disabled route returns to the compiled stop: nothing is answered. */
    CHECK(xdk_thunk_set_synch_handler(NULL));
    REFUSED(SYNCH, SYNCH_CALLER, synch_args, 1u, "reads the omitted APU object");
    CHECK_EQ_U32(xdk_thunk_synch_call_count(), 2u);

    CHECK_EQ_U32(dsound_movie_stream_release(stream), 0u);
    CHECK_EQ_U32(load(device + 4u), 6u);
    CHECK(dsound_stream_reset_checked());
    CHECK(dsound_device_reset_checked());
    xdk_thunk_shutdown();
    environment_end();
    printf("synch routes: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
