/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_present.h. Every function carries the address of the original it ports.
 */

#include "d3d8_present.h"

#include <stdbool.h>
#include <string.h>

#include "d3d8_bind.h"
#include "d3d8_copy.h"
#include "d3d8_flip.h"
#include "d3d8_gpu.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_surface.h"
#include "d3d8_swap_replay.h"
#include "d3d8_vblank_effects.h"
#include "kernel_av.h"
#include "kernel_call.h"

#define KERNEL_ORDINAL_AV_GET_SAVED_DATA_ADDRESS 1u
#define KERNEL_ORDINAL_AV_SEND_TV_ENCODER_OPTION 2u
#define KERNEL_ORDINAL_AV_SET_DISPLAY_MODE 3u
#define KERNEL_ORDINAL_AV_SET_SAVED_DATA_ADDRESS 4u
#define KERNEL_ORDINAL_MM_FREE_CONTIGUOUS_MEMORY 171u
#define KERNEL_ORDINAL_MM_PERSIST_CONTIGUOUS_MEMORY 178u
#define KERNEL_ORDINAL_MM_QUERY_ALLOCATION_SIZE 180u

/* Device fields. */
#define DEV_CALLBACK_A 0x08D4u
#define DEV_CALLBACK_B 0x08D8u
#define DEV_VIEWPORT 0x0EE0u
#define DEV_SURFACE_COUNT 0x1A10u
#define DEV_BACK_BUFFER 0x1A14u
#define DEV_FRONT_LIST 0x1A18u
#define DEV_RENDER_TARGET 0x1A04u
#define DEV_DEPTH_SURFACE 0x1A08u
#define DEV_SAVED_TARGET 0x1A90u
#define DEV_SAVED_DEPTH 0x1A94u
#define DEV_SAVED_TEXTURE 0x1A98u
#define DEV_SAVED_VIEWPORT 0x1AA8u
#define DEV_TEXTURE 0x0F88u
#define DEV_FRAME_FENCE 0x1974u    /* two entries, indexed by the swap counter's low bit */
#define DEV_SWAP_COUNTER 0x2478u
#define DEV_MODE_PENDING 0x1DE0u
#define DEV_DISPLAY_FLAGS 0x1DDCu
#define DEV_DISPLAY_OBJECT 0x1C28u /* the register base; the pitch, mode word and format follow */
#define DEV_DISPLAY_PITCH 0x1C2Cu
#define DEV_DISPLAY_MODE 0x1C30u
#define DEV_DISPLAY_FORMAT 0x1C34u
#define DEV_FIELD_COUNTER 0x1DE8u

#define DEV_FLAG_SWAP_COPY 0x4000u
#define DISPLAY_FLAG_FIELD 0x01000000u

#define GLOBAL_PRESENT_INTERVAL 0x003E3EBCu
#define GLOBAL_FLICKER_CACHED 0x003E2BA0u
#define GLOBAL_SOFT_FILTER_CACHED 0x003E2B9Cu

#define DIRTY_AFTER_PRESENT 0x00FF1000u

#define SAVED_TEXTURES 4u
#define VIEWPORT_DWORDS 6u

#define AV_OPTION_FIELD 0xBu
#define AV_OPTION_QUERY_FIELD 0xFu
#define AV_OPTION_QUERY_STEP 0x10u
#define AV_OPTION_BLANK 0x9u
#define MODE_STEP_LIMIT 1024u

static d3d8_frame_record queue[D3D8_FRAME_QUEUE_CAPACITY];
static uint64_t presented;
static uint64_t overruns;
static d3d8_present_observer present_observer;
/* A later AvSetDisplayMode supersedes earlier modeled register writes. */
static uint64_t mode_write_floor;
static uint64_t mode_reset_serial;
static uint64_t mode_gpu_reset;
static bool mode_floor_valid;

void d3d8_present_set_observer(d3d8_present_observer observer)
{
    present_observer = observer;
}

static d3d8_present_observer route_observer; /* T1633, the host's route probe */
void d3d8_present_set_route_observer(d3d8_present_observer observer)
{
    route_observer = observer;
}

static d3d8_present_observer second_observer;

void d3d8_present_set_second_observer(d3d8_present_observer observer)
{
    second_observer = observer;
}

void d3d8_present_reset(void)
{
    memset(queue, 0, sizeof(queue));
    presented = 0u;
    overruns = 0u;
}

size_t d3d8_frame_queue_count(void)
{
    return presented < D3D8_FRAME_QUEUE_CAPACITY ? (size_t)presented : D3D8_FRAME_QUEUE_CAPACITY;
}

uint64_t d3d8_frame_queue_total(void)
{
    return presented;
}

uint64_t d3d8_frame_queue_overruns(void)
{
    return overruns;
}

d3d8_frame_record d3d8_frame_queue_at(size_t index)
{
    const size_t held = d3d8_frame_queue_count();
    if (index >= held) {
        return (d3d8_frame_record){0};
    }
    const uint64_t oldest = presented - held;
    return queue[(oldest + index) % D3D8_FRAME_QUEUE_CAPACITY];
}

/* --- the frame queue -------------------------------------------------------------------- */

static void record_frame(uint32_t flags, bool helper_completed)
{
    const uint32_t header = d3d8_device_load32(DEV_FRONT_LIST);
    d3d8_frame_record record;
    memset(&record, 0, sizeof(record));
    presented++;
    if (presented > D3D8_FRAME_QUEUE_CAPACITY) {
        overruns++;
    }
    record.number = presented;
    record.swap_counter = d3d8_device_load32(DEV_SWAP_COUNTER);
    record.interval = d3d8_guest_load32(GLOBAL_PRESENT_INTERVAL);
    record.vblank = d3d8_gpu_vblank_count();
    record.header = header;
    record.common = d3d8_guest_load32(header + D3D8_SURFACE_COMMON);
    record.data = d3d8_guest_load32(header + D3D8_SURFACE_DATA);
    record.lock = d3d8_guest_load32(header + D3D8_SURFACE_LOCK);
    record.format_word = d3d8_guest_load32(header + D3D8_SURFACE_FORMAT);
    record.size_word = d3d8_guest_load32(header + D3D8_SURFACE_SIZE);
    record.parent = d3d8_guest_load32(header + D3D8_SURFACE_PARENT);
    /* The Size word of a linear surface: (pitch / 64 - 1) << 24, (height - 1) << 12, width - 1. */
    record.width = (record.size_word & 0xFFFu) + 1u;
    record.height = ((record.size_word >> 12) & 0xFFFu) + 1u;
    record.pitch = d3d8_surface_header_pitch(header);
    record.format = (record.format_word >> 8) & 0xFFu;
    kernel_av_display display;
    const bool display_known = kernel_av_display_get(&display);
    record.scanout = display_known ? display.frame_buffer : 0u;
    if (d3d8_flip_enabled() && d3d8_vblank_effects_enabled()) {
        /* A completed helper runs process_locked/follow_device before this getter. */
        if (!helper_completed) {
            d3d8_hle_fatal(0x003D8E50u, "modeled scanout needs a completed vblank helper");
        }
        const d3d8_flip_hardware hardware = d3d8_flip_hardware_get();
        const bool same_mode_epoch = mode_floor_valid &&
            mode_gpu_reset == d3d8_gpu_reset_count() &&
            mode_reset_serial == hardware.reset_serial;
        const uint64_t floor = same_mode_epoch ? mode_write_floor : 0u;
        if (hardware.display_start_writes != 0u && hardware.display_start_writes != floor) {
            record.scanout = hardware.display_start;
        } else if (!display_known) {
            d3d8_hle_fatal(0x003D8E50u, "modeled scanout has no display-start or mode record");
        }
    }
    record.stream_commands = d3d8_gpu_get_stats().commands_recorded;
    (void)flags;
    queue[(presented - 1u) % D3D8_FRAME_QUEUE_CAPACITY] = record;
    if (present_observer != NULL) {
        present_observer(&record);
    }
    if (second_observer != NULL) {
        second_observer(&record);
    }
    if (route_observer != NULL) {
        route_observer(&record);
    }
}

/* --- 0x003D8890 and 0x003D8920 ---------------------------------------------------------- */

void d3d8_present_save_state(void)
{
    const uint32_t target = d3d8_device_load32(DEV_RENDER_TARGET);
    d3d8_device_store32(DEV_SAVED_TARGET, target);
    (void)d3d8_resource_add_ref(target);
    const uint32_t depth = d3d8_device_load32(DEV_DEPTH_SURFACE);
    d3d8_device_store32(DEV_SAVED_DEPTH, depth);
    if (depth != 0u) {
        (void)d3d8_resource_add_ref(depth);
    }
    for (uint32_t stage = 0u; stage < SAVED_TEXTURES; stage++) {
        const uint32_t texture = d3d8_device_load32(DEV_TEXTURE + stage * 4u);
        d3d8_device_store32(DEV_SAVED_TEXTURE + stage * 4u, texture);
        if (texture != 0u) {
            (void)d3d8_resource_add_ref(texture);
        }
    }
    for (uint32_t index = 0u; index < VIEWPORT_DWORDS; index++) {
        d3d8_device_store32(DEV_SAVED_VIEWPORT + index * 4u,
                            d3d8_device_load32(DEV_VIEWPORT + index * 4u));
    }
}

void d3d8_present_restore_state(void)
{
    const uint32_t target = d3d8_device_load32(DEV_SAVED_TARGET);
    const uint32_t depth = d3d8_device_load32(DEV_SAVED_DEPTH);
    d3d8_set_render_target(target, depth);
    d3d8_hle_note_unmodelled(0x003D3E40u,
                             "SetViewport (the swap restore) recomputes the viewport words at "
                             "device+0xEE0, the transform matrix and the dirty bits, and writes "
                             "its commands");
    d3d8_gpu_note_elided(0x003D3E40u);
    (void)d3d8_resource_release(target);
    if (depth != 0u) {
        (void)d3d8_resource_release(depth);
    }
    for (uint32_t stage = 0u; stage < SAVED_TEXTURES; stage++) {
        const uint32_t texture = d3d8_device_load32(DEV_SAVED_TEXTURE + stage * 4u);
        d3d8_set_texture(stage, texture);
        if (texture != 0u) {
            (void)d3d8_resource_release(texture);
        }
    }
}

/* --- 0x003D8B10 ------------------------------------------------------------------------- */

void d3d8_present_prepare(uint32_t flags)
{
    uint32_t copy_frame[D3D8_COPY_FRAME_DWORDS];
    if ((flags & 3u) != 0u) {
        if (d3d8_device_load32(DEV_SURFACE_COUNT) == 3u) {
            d3d8_hle_fatal(0x003D8B10u, "a triple-buffered device (0x003D8B30) is not ported");
        }
        d3d8_present_save_state();
        d3d8_set_render_target(d3d8_device_load32(DEV_FRONT_LIST), 0u);
        d3d8_set_texture(0u, d3d8_device_load32(DEV_BACK_BUFFER));
        d3d8_set_texture(1u, 0u);
        d3d8_set_texture(2u, 0u);
        d3d8_set_texture(3u, 0u);
        if ((flags & 1u) != 0u) {
            /* The copy composition (T443): snapshot the state and force the copy state. */
            d3d8_copy_snapshot(copy_frame);
            d3d8_copy_setup();
        }
        /* The commands of 0x003D8B10's own body (the render target, the texture bindings and the
         * 0x40100 group between the setup and the triangle) stay elided, as they always were.
         * The group's flip is queued by the model instead (T407, only with the coupled vblank
         * effects, which are what completes it). */
        d3d8_flip_queue_for_swap();
        d3d8_gpu_note_elided(0x003D8B10u);
    }
    if ((flags & 1u) != 0u) {
        d3d8_copy_draw_triangle();
        d3d8_copy_restore(copy_frame);
    }
    if ((flags & 4u) != 0u) {
        d3d8_present_restore_state();
    }
}

/* --- 0x003D8450 ------------------------------------------------------------------------- */

static uint32_t encoder_option(uint32_t base, uint32_t option, uint32_t parameter, uint32_t result)
{
    const uint32_t args[4] = {base, option, parameter, result};
    return d3d8_kernel_call(KERNEL_ORDINAL_AV_SEND_TV_ENCODER_OPTION, args, 4u);
}

void d3d8_present_set_mode(void)
{
    const uint32_t base = d3d8_device_load32(DEV_DISPLAY_OBJECT);
    uint32_t pitch = d3d8_device_load32(DEV_DISPLAY_PITCH);
    const uint32_t mode = d3d8_device_load32(DEV_DISPLAY_MODE);
    const uint32_t format = d3d8_device_load32(DEV_DISPLAY_FORMAT);

    d3d8_gpu_fence_wait(d3d8_device_load32(D3D8_DEV_FENCE), 2u);
    const uint32_t display_flags = d3d8_device_load32(DEV_DISPLAY_FLAGS);
    if ((display_flags & DISPLAY_FLAG_FIELD) != 0u) {
        pitch >>= 1;
    }
    const uint32_t scanout = d3d8_guest_load32(d3d8_device_load32(DEV_FRONT_LIST) + D3D8_SURFACE_DATA);

    const bool progressive_format = format == 0x11u || format == 0x10u || format == 0x1Cu;
    const bool known_mode =
        mode == 0x88070701u || mode == 0x88080801u || mode == 0x88110F01u;
    const uint32_t result = d3d8_guest_scratch();
    if (progressive_format && known_mode) {
        d3d8_guest_store32(result, 0u);
        (void)encoder_option(0u, AV_OPTION_QUERY_STEP, 0u, result);
        if (d3d8_guest_load32(result) == 2u) {
            d3d8_hle_fatal(0x003D8450u, "the encoder answered 2 to option 0x10, which sends the "
                                        "original down a mode-set loop with a 0x1E argument: "
                                        "not ported");
        }
    }

    /* Wait for a blank, then set the mode, until the display says it is finished. */
    uint32_t step = 0u;
    for (unsigned attempt = 0u;; attempt++) {
        if (attempt >= MODE_STEP_LIMIT) {
            d3d8_hle_fatal(0x003D8450u, "AvSetDisplayMode still asked for another step after %u",
                           (unsigned)MODE_STEP_LIMIT);
        }
        d3d8_gpu_wait_vblank();
        const uint32_t args[6] = {base, step, mode, format, pitch, scanout};
        step = d3d8_kernel_call(KERNEL_ORDINAL_AV_SET_DISPLAY_MODE, args, 6u);
        if (step == 0u) {
            break;
        }
    }

    if ((display_flags & DISPLAY_FLAG_FIELD) != 0u) {
        (void)encoder_option(d3d8_device_load32(DEV_DISPLAY_OBJECT), AV_OPTION_FIELD, 0u, 0u);
    }
    d3d8_guest_store32(result, 0u);
    (void)encoder_option(d3d8_device_load32(DEV_DISPLAY_OBJECT), AV_OPTION_QUERY_FIELD, 0u, result);
    const uint32_t field = d3d8_guest_load32(result);
    const uint32_t counter = d3d8_device_load32(DEV_FIELD_COUNTER);
    if (((counter ^ field) & 1u) == 0u) {
        d3d8_device_store32(DEV_FIELD_COUNTER, counter + 1u);
    }
    d3d8_guest_store32(GLOBAL_FLICKER_CACHED, 0u);
    d3d8_guest_store32(GLOBAL_SOFT_FILTER_CACHED, 0u);
    d3d8_hle_note_unmodelled(0x003DD0FDu,
                             "the display clock registers (0x003DD0FD writes NV2A PRAMDAC "
                             "registers): there is no register window");

    const uint32_t saved = d3d8_kernel_call(KERNEL_ORDINAL_AV_GET_SAVED_DATA_ADDRESS, NULL, 0u);
    if (saved != 0u) {
        const uint32_t size_args[1] = {saved};
        const uint32_t size = d3d8_kernel_call(KERNEL_ORDINAL_MM_QUERY_ALLOCATION_SIZE, size_args, 1u);
        const uint32_t persist_args[3] = {saved, size, 0u};
        (void)d3d8_kernel_call(KERNEL_ORDINAL_MM_PERSIST_CONTIGUOUS_MEMORY, persist_args, 3u);
        (void)d3d8_kernel_call(KERNEL_ORDINAL_MM_FREE_CONTIGUOUS_MEMORY, size_args, 1u);
        const uint32_t clear_args[1] = {0u};
        (void)d3d8_kernel_call(KERNEL_ORDINAL_AV_SET_SAVED_DATA_ADDRESS, clear_args, 1u);
    }
    (void)encoder_option(base, AV_OPTION_BLANK, 0u, 0u);
    d3d8_device_store32(DEV_MODE_PENDING, 0u);
    /* Capture after the final successful AvSetDisplayMode and its preceding waits. */
    const d3d8_flip_hardware hardware = d3d8_flip_hardware_get();
    mode_write_floor = hardware.display_start_writes;
    mode_reset_serial = hardware.reset_serial;
    mode_gpu_reset = d3d8_gpu_reset_count();
    mode_floor_valid = true;
}

/* --- 0x003D8E10 ------------------------------------------------------------------------- */

void d3d8_present_finish(void)
{
    const uint32_t front = d3d8_device_load32(DEV_FRONT_LIST);
    const uint32_t parent = d3d8_guest_load32(front + D3D8_SURFACE_PARENT);
    d3d8_guest_store32((parent != 0u ? parent : front) + D3D8_SURFACE_LOCK,
                       d3d8_device_load32(D3D8_DEV_FENCE));
    const uint32_t fence = d3d8_gpu_fence_insert(0u);
    d3d8_device_store32(DEV_FRAME_FENCE + (d3d8_device_load32(DEV_SWAP_COUNTER) & 1u) * 4u, fence);
    if (d3d8_device_load32(DEV_MODE_PENDING) != 0u) {
        d3d8_present_set_mode();
    }
}

/* --- Swap ------------------------------------------------------------------------------- */

uint32_t d3d8_swap(uint32_t flags)
{
    if (d3d8_device_load32(DEV_CALLBACK_A) != 0u || d3d8_device_load32(DEV_CALLBACK_B) != 0u) {
        d3d8_hle_fatal(0x003D8E50u, "a swap callback is installed (device+0x8D4 or +0x8D8): "
                                    "0x003DEFF0 and 0x003DF420 are not ported");
    }
    if (flags == 0u) {
        flags = 5u;
    }
    if ((d3d8_device_load32(D3D8_DEV_FLAGS) & DEV_FLAG_SWAP_COPY) == 0u) {
        d3d8_hle_fatal(0x003D8E50u, "the device was not created with swap effect 3 (copy): "
                                    "CreateDevice refuses every other effect, so this cannot be "
                                    "reached by a device this port made");
    }
    if ((flags & 3u) != 0u) {
        (void)d3d8_gpu_fence_insert(0u);
        const uint32_t counter = d3d8_device_load32(DEV_SWAP_COUNTER);
        const uint32_t previous =
            d3d8_device_load32(DEV_FRAME_FENCE + ((counter - 1u) & 1u) * 4u);
        if (previous != 0u) {
            d3d8_gpu_fence_wait(previous, 1u);
        }
        d3d8_device_store32(DEV_SWAP_COUNTER, counter + 1u);
    }
    d3d8_present_prepare(flags);
    if ((flags & 4u) != 0u) {
        d3d8_present_finish();
        d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK,
                           d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | DIRTY_AFTER_PRESENT);
        const uint64_t helpers_before = d3d8_vblank_effects_applied();
        d3d8_gpu_wait_vblank();
        record_frame(flags, d3d8_vblank_effects_applied() != helpers_before);
        d3d8_swap_replay_on_present(presented);
    }
    return d3d8_device_load32(DEV_SWAP_COUNTER);
}

static uint32_t argument(const void *context, unsigned index, uint32_t address)
{
    uint32_t value = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, index, &value)) {
        d3d8_hle_fatal(address, "argument %u of the call cannot be read", index);
    }
    return value;
}

static uint32_t handler_swap(void *context)
{
    return d3d8_swap(argument(context, 0u, 0x003D8E50u));
}

/* The internals take the device in a register (eax or esi) or as an argument. This port loads it
 * from its global, so those values are ignored. */
static uint32_t handler_prepare(void *context)
{
    d3d8_present_prepare(argument(context, 0u, 0x003D8B10u));
    return 0u;
}

static uint32_t handler_save(void *context)
{
    (void)context;
    d3d8_present_save_state();
    return 0u;
}

static uint32_t handler_restore(void *context)
{
    (void)context;
    d3d8_present_restore_state();
    return 0u;
}

static uint32_t handler_finish(void *context)
{
    (void)context;
    d3d8_present_finish();
    return 0u;
}

static uint32_t handler_set_mode(void *context)
{
    (void)context;
    d3d8_present_set_mode();
    return 0u;
}

size_t d3d8_present_register(void)
{
    static const struct {
        uint32_t address;
        d3d8_fn handler;
    } handlers[] = {
        {0x003D8E50u, handler_swap},    {0x003D8B10u, handler_prepare},
        {0x003D8890u, handler_save},    {0x003D8920u, handler_restore},
        {0x003D8E10u, handler_finish},  {0x003D8450u, handler_set_mode},
    };
    size_t registered = 0u;
    for (size_t index = 0u; index < sizeof(handlers) / sizeof(handlers[0]); index++) {
        if (d3d8_hle_register(handlers[index].address, handlers[index].handler)) {
            registered++;
        }
    }
    return registered;
}
