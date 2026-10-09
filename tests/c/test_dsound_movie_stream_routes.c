/* SPDX-License-Identifier: GPL-3.0-or-later */
/* T392: the movie stream through the REAL lifted trampolines and the XDK dispatcher: Create,
 * SetVolume, Pause, FlushEx and DirectSoundDoWork as the retained XMV and the title call them, and
 * the completion callback run by recomp_guest_call on the guest stack. The callback is the real
 * lifted XMV function 0x445071, which returns at once for a nonzero status (the abort path). */
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

#define CREATE 0x40967Cu
#define VOLUME 0x407B14u
#define PAUSE 0x407B23u
#define FLUSH_EX 0x407B28u
#define WORK 0x407B40u
#define XMV_CALLER 0x445381u
#define CREATE_CALLER 0x4451F4u
#define TITLE_VOLUME_CALLER 0x30364u
#define DECODER 0x41376170u
#define ABORT 0x80004004u

static bool irql(uint8_t *out) { *out = 0u; return true; }
static void fatal(uint32_t address, const char *reason) { host_run_stop(HOST_STOP_UNIMPLEMENTED, address, 0u, reason); }
static uint64_t fake_now = 1000u;
static uint64_t clock_now(void) { return fake_now; }
static unsigned callbacks_run;
static uint32_t seen_status;
static bool run_callback(uint32_t callback, uint32_t stream_context, uint32_t packet_context, uint32_t status)
{
    const uint32_t arguments[3] = {stream_context, packet_context, status};
    callbacks_run++;
    seen_status = status;
    /* The registers the nested call must give back are the ones the XDK dispatcher holds now. */
    const uint32_t ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp, esp = g_esp;
    const bool ok = recomp_guest_call_stdcall(callback, arguments, 3u);
    CHECK(g_ebx == ebx && g_esi == esi && g_edi == edi && g_ebp == ebp && g_esp == esp);
    return ok;
}

static uint32_t stack;
/* Call the real lifted trampoline for `address` with a stdcall frame and report EAX. */
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
    recomp_func_t function = recomp_lookup(address);
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

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    dsound_hle_init();
    map_fixed(0x412000u, 4096u);
    map_fixed(0x4A1000u, 4096u);
    map_fixed(0x42000000u, 0x100000u);
    for (unsigned i = 0u; i < 15u; i++) store(0x4A1CF0u + 4u * i, 0x406879u);
    dsound_hle_set_codec_state(DSOUND_CODEC_READY);
    CHECK_EQ_U32(dsound_device_create(0u, SCRATCH_DATA, 0u), 0u);
    const uint32_t device = load(SCRATCH_DATA) - 8u, desc = SCRATCH_DATA + 256u, format = SCRATCH_DATA + 320u;
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
    dsound_movie_stream_set_fatal(fatal);
    dsound_movie_stream_set_irql_provider(irql);
    dsound_movie_stream_set_clock(clock_now, 733333333u);
    dsound_movie_stream_set_callback_runner(run_callback);
    dsound_movie_stream_set_enabled(true);
    dsound_stream_set_extension(dsound_movie_stream_route_public, dsound_movie_stream_reset_checked);
    dsound_listener_set_work_route(dsound_movie_stream_route_work);
    const xdk_dispatch_entry rows[5] = {
        {CREATE, "DirectSoundCreateStream", XDK_MODULE_DSOUND}, {VOLUME, "IDirectSoundStream_SetVolume", XDK_MODULE_DSOUND},
        {PAUSE, "IDirectSoundStream_Pause", XDK_MODULE_DSOUND}, {FLUSH_EX, "IDirectSoundStream_FlushEx", XDK_MODULE_DSOUND},
        {WORK, "DirectSoundDoWork", XDK_MODULE_DSOUND},
    };
    CHECK(xdk_thunk_init(rows, 5u));
    CHECK(xdk_thunk_declare_abi(CREATE, XDK_CC_STDCALL, 2u, 0u));
    CHECK(xdk_thunk_declare_abi(VOLUME, XDK_CC_STDCALL, 2u, 0u));
    CHECK(xdk_thunk_declare_abi(PAUSE, XDK_CC_STDCALL, 2u, 0u));
    CHECK(xdk_thunk_declare_abi(FLUSH_EX, XDK_CC_STDCALL, 4u, 0u));
    CHECK(xdk_thunk_declare_abi(WORK, XDK_CC_STDCALL, 0u, 0u));
    CHECK_EQ_U32(dsound_stream_register(), 10u);
    CHECK(dsound_listener_register() != 0u);
    guest_region_request request = {.bytes = 8192u, .alignment = 4096u, .protect = PAGE_READWRITE, .state = MEM_COMMIT};
    nt_status status;
    const uint32_t allocation = guest_region_alloc(&request, &status);
    CHECK(allocation != 0u);
    stack = allocation + 4096u - 64u;
    CHECK(mprotect((void *)(uintptr_t)(allocation + 4096u), 4096u, PROT_NONE) == 0);

    uint32_t eax = 0xFFFFFFFFu;
    const uint32_t output = SCRATCH_DATA + 128u;
    store(output, 0xAAAAAAAAu);
    /* The startup caller of Create still gets the startup policy, which refuses the PCM descriptor. */
    const uint32_t create_args[2] = {desc, output};
    CHECK(!call_row(CREATE, 0x29943u, create_args, 2u, &eax));
    CHECK(host_run_result()->guest_address == CREATE);
    CHECK_EQ_U32(load(output), 0xAAAAAAAAu);
    CHECK_EQ_U32(dsound_movie_stream_count(), 0u);
    /* The one EnableAudioStream site creates the movie stream. */
    CHECK(call_row(CREATE, CREATE_CALLER, create_args, 2u, &eax));
    CHECK_EQ_U32(eax, 0u);
    const uint32_t stream = load(output);
    CHECK(stream != 0u && dsound_movie_stream_owns(stream));
    CHECK_EQ_U32(load(device + 4u), 7u);
    /* The title's SetVolume call, then the XMV Pause(2). */
    const uint32_t volume_args[2] = {stream, (uint32_t)-1200};
    CHECK(call_row(VOLUME, TITLE_VOLUME_CALLER, volume_args, 2u, &eax));
    CHECK_EQ_U32(eax, 0u);
    const uint32_t pause_args[2] = {stream, 2u};
    CHECK(call_row(PAUSE, XMV_CALLER, pause_args, 2u, &eax));
    CHECK_EQ_U32(eax, 0u);
    dsound_movie_stream_snapshot snapshot;
    CHECK(dsound_movie_stream_get_snapshot(stream, &snapshot));
    CHECK(snapshot.volume_seen && snapshot.volume == -1200 && snapshot.pause_mode == 2u);
    /* A movie stream from the startup caller is fatal, never the startup policy's answer. */
    const uint32_t startup_pause[2] = {stream, 1u};
    CHECK(!call_row(PAUSE, 0x29B5Au, startup_pause, 2u, &eax));
    CHECK(strstr(host_run_result()->detail, "outside the movie callers") != NULL);
    /* DoWork from a movie caller runs the model, from the startup caller it is the old refusal. */
    CHECK(call_row(WORK, 0x4457D9u, NULL, 0u, &eax));
    CHECK_EQ_U32(callbacks_run, 0u);
    CHECK(!call_row(WORK, 0x1CE492u + 1u, NULL, 0u, &eax));
    /* Two packets, an async flush, and DoWork completes both with the abort status. The guest callback
     * is the real lifted 0x445071 and returns at once for a nonzero status. */
    for (unsigned index = 0u; index < 2u; index++) {
        const uint32_t base = SCRATCH_DATA + 0x400u + 0x40u * index;
        const uint32_t words[6] = {0x42000000u + 0x1000u * index, 0x1000u * (index + 1u), base + 0x20u, base + 0x24u, index, 0u};
        CHECK(kernel_guest_write_bytes(base, words, sizeof(words)));
        CHECK_EQ_U32(dsound_movie_stream_process(stream, base, 0u), 0u);
    }
    const uint32_t flush_args[4] = {stream, 0u, 0u, 1u};
    CHECK(call_row(FLUSH_EX, XMV_CALLER, flush_args, 4u, &eax));
    CHECK_EQ_U32(callbacks_run, 0u);
    thunk_trace_reset();
    CHECK(call_row(WORK, 0x4457D9u, NULL, 0u, &eax));
    CHECK_EQ_U32(callbacks_run, 2u);
    CHECK_EQ_U32(seen_status, ABORT);
    CHECK_EQ_U32(load(SCRATCH_DATA + 0x400u + 0x24u), ABORT);
    CHECK_EQ_U32(load(SCRATCH_DATA + 0x440u + 0x24u), ABORT);
    CHECK_EQ_U32(load(SCRATCH_DATA + 0x400u + 0x20u), 0x1000u);
    CHECK_EQ_U32(load(SCRATCH_DATA + 0x440u + 0x20u), 0x2000u);
    size_t entries_seen;
    const thunk_trace_entry *trace = thunk_trace_entries(&entries_seen);
    CHECK(entries_seen == 1u);
    if (entries_seen == 1u) CHECK(trace[0].address == WORK && trace[0].return_address == 0x4457D9u);
    /* The nested call is refused cleanly for a missing function and an unwritable stack. */
    const uint32_t none[3] = {0u, 0u, 0u};
    g_esp = stack;
    CHECK(!recomp_guest_call_stdcall(0x1u, none, 3u));
    CHECK(g_esp == stack);
    CHECK(!recomp_guest_call_stdcall(0x445071u, none, 5u));
    g_esp = 0x10u;
    CHECK(!recomp_guest_call_stdcall(0x445071u, none, 3u));
    CHECK(g_esp == 0x10u);
    g_esp = allocation + 4096u + 8u;  /* the frame would straddle into the PROT_NONE guard page */
    CHECK(!recomp_guest_call_stdcall(0x445071u, none, 3u));
    CHECK(g_esp == allocation + 4096u + 8u);
    /* The caller-saved registers come back too. The title's own _alldiv (0x3CA970, stdcall, four
     * dwords) clobbers EAX, ECX and EDX with its quotient and its scratch, and pops 16 bytes. */
    const uint32_t divide_args[4] = {1000000u, 0u, 1000u, 0u};
    g_esp = stack;
    g_eax = 0x1111u;
    g_ecx = 0x2222u;
    g_edx = 0x3333u;
    g_ebx = 0x4444u;
    g_esi = 0x5555u;
    g_edi = 0x6666u;
    g_ebp = 0x7777u;
    CHECK(recomp_guest_call_stdcall(0x3CA970u, divide_args, 4u));
    CHECK(g_eax == 0x1111u && g_ecx == 0x2222u && g_edx == 0x3333u && g_esp == stack);
    CHECK(g_ebx == 0x4444u && g_esi == 0x5555u && g_edi == 0x6666u && g_ebp == 0x7777u);
    /* A frame that straddles into the guard page is refused before any byte of it is written. */
    const uint32_t straddle = allocation + 4096u - 8u;
    store(straddle, 0x13579BDFu);
    store(straddle + 4u, 0x2468ACE0u);
    g_esp = allocation + 4096u + 8u;
    CHECK(!recomp_guest_call_stdcall(0x445071u, none, 3u));
    CHECK_EQ_U32(load(straddle), 0x13579BDFu);
    CHECK_EQ_U32(load(straddle + 4u), 0x2468ACE0u);
    /* A wrong argument count is a callee that does not pop what was pushed. */
    g_esp = stack;
    CHECK(!recomp_guest_call_stdcall(0x445071u, none, 2u));
    CHECK(g_esp == stack);
    /* The final release through the model frees the device reference again. */
    CHECK_EQ_U32(dsound_movie_stream_release(stream), 0u);
    CHECK_EQ_U32(load(device + 4u), 6u);
    CHECK(dsound_stream_reset_checked());
    CHECK(dsound_device_reset_checked());
    xdk_thunk_shutdown();
    environment_end();
    printf("movie stream routes: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
