/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_device.h for what is and is not reproduced. Every function carries the address of
 * the original it ports.
 */

#include "d3d8_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "d3d8_copy.h"
#include "d3d8_visibility.h"
#include "d3d8_display.h"
#include "d3d8_bind.h"
#include "d3d8_frame.h"
#include "d3d8_gpu.h"
#include "d3d8_guest.h"
#include "d3d8_immediate.h"
#include "d3d8_hle.h"
#include "d3d8_present.h"
#include "d3d8_overlay.h"
#include "d3d8_shader.h"
#include "d3d8_pushbuffer.h"
#include "d3d8_resource.h"
#include "d3d8_indexed.h"
#include "d3d8_cube_surface.h"
#include "d3d8_lock.h"
#include "d3d8_method_packet.h"
#include "d3d8_scissor.h"
#include "d3d8_vertex_constants.h"
#include "d3d8_callbacks.h"
#include "d3d8_state.h"
#include "d3d8_surface.h"
#include "guest_mem.h"
#include "kernel_call.h"

#define KERNEL_ORDINAL_AV_SEND_TV_ENCODER_OPTION 2u
#define KERNEL_ORDINAL_MM_ALLOCATE_CONTIGUOUS_EX 166u

/* MmAllocateContiguousMemoryEx arguments for the surfaces: 16 KiB alignment and protect
 * 0x404, from the allocation wrapper's flag word 0xBE800000 (MEASURED in the oracle's
 * allocation log: size 0x12C000, alignment 0x4000, protect 0x404). */
#define SURFACE_ALIGNMENT 0x4000u
#define SURFACE_PROTECT 0x404u

/* Device fields beyond the pushbuffer's (see d3d8_guest.h). */
#define DEV_MULTISAMPLE_TYPE 0x196Cu
#define DEV_SWAP_EFFECT 0x1970u
#define DEV_MS_SCALE_X 0x096Cu
#define DEV_MS_SCALE_Y 0x0970u
#define DEV_SCALE_LAST 0x0964u
#define DEV_SURFACE_COUNT 0x1A10u
#define DEV_SURFACE_LIST 0x1A14u
#define DEV_FRONT_LIST 0x1A18u
#define DEV_DEPTH_HEADER_SLOT 0x1A20u
#define DEV_HEADER_ARRAY 0x1A24u
#define DEV_FRONT_HEADER 0x1A3Cu
#define DEV_DEPTH_HEADER 0x1A6Cu
#define DEV_BACK_ALLOCATION 0x1A84u
#define DEV_FRONT_ALLOCATION 0x1A88u
#define DEV_DEPTH_ALLOCATION 0x1A8Cu
#define DEV_DISPLAY_OBJECT 0x1C28u
#define DEV_REGISTER_BASE 0x0934u
#define DEV_COMPOSITE_MATRIX 0x0CA0u

/* Library globals CreateDevice writes beside the device. */
#define GLOBAL_PRESENT_INTERVAL 0x003E3EBCu
#define GLOBAL_MULTISAMPLE_FLAGS 0x003E3F28u
#define GLOBAL_MULTISAMPLE_KIND 0x003E3EB8u
#define GLOBAL_REGISTER_BASE_COPY 0x003E3AACu
#define GLOBAL_DEVICE_CREATED 0x003E4898u
#define GLOBAL_FLICKER_VALUE 0x003E2B98u
#define GLOBAL_SOFT_FILTER_VALUE 0x003E2B94u
#define GLOBAL_SOFT_FILTER_CACHED 0x003E2B9Cu
#define GLOBAL_FLICKER_CACHED 0x003E2BA0u

#define DIRTY_AT_CREATE 0x00FF7F7Fu
#define DEVICE_BYTES_CLEARED_ON_FAILURE 0x24A0u

#define AV_OPTION_FLICKER_FILTER 0xBu
#define AV_OPTION_SOFT_DISPLAY_FILTER 0xEu

#define MULTISAMPLE_NONE 0x11u
#define SWAP_EFFECT_COPY 3u
#define FLOAT_ONE 0x3F800000u
#define DEV_FLAG_SWAP_TARGET 0x4000u
#define DEV_FLAG_PROJECTION 0x2u

/* D3DPRESENT_PARAMETERS as 0x003DA700 and 0x003DACA0 index it. */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t back_buffer_count;
    uint32_t multisample;
    uint32_t swap_effect;
    uint32_t window;
    uint32_t windowed;
    uint32_t auto_depth_stencil;
    uint32_t depth_format;
    uint32_t flags;
    uint32_t refresh_rate;
    uint32_t presentation_interval;
    uint32_t buffer_surface[3];
    uint32_t depth_surface;
} present_parameters;

static uint32_t frame_argument(const void *context, unsigned index, uint32_t address)
{
    uint32_t value = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, index, &value)) {
        d3d8_hle_fatal(address, "argument %u of the call cannot be read (no frame, or a frame "
                                "shorter than the function's arity)",
                       index);
    }
    return value;
}

static uint32_t frame_register(const void *context, unsigned index, uint32_t address)
{
    uint32_t value = 0u;
    if (!kernel_frame_reg_arg((const kernel_call_frame *)context, index, &value)) {
        d3d8_hle_fatal(address, "register argument %u of the call was not supplied", index);
    }
    return value;
}

/* --- 0x003D9210 ---------------------------------------------------------------------- */

uint32_t d3d8_set_push_buffer_size(uint32_t size, uint32_t kickoff)
{
    d3d8_guest_store32(D3D8_GLOBAL_PUSHBUFFER_SIZE, size);
    d3d8_guest_store32(D3D8_GLOBAL_KICKOFF_SIZE, kickoff);
    return size;
}

/* --- the flicker and soft-display filters, 0x003D3580 and 0x003D35D0 --------------------- */

/* Each caches the last value it sent, and sends again only on a change. The cache words are
 * D3D8 globals, so a later SetFlickerFilter the title makes sees what CreateDevice left. */
void d3d8_set_flicker_filter(uint32_t value)
{
    if (d3d8_guest_load32(GLOBAL_FLICKER_CACHED) != 0u &&
        d3d8_guest_load32(GLOBAL_FLICKER_VALUE) == value) {
        return;
    }
    const uint32_t args[4] = {d3d8_device_load32(DEV_DISPLAY_OBJECT), AV_OPTION_FLICKER_FILTER,
                              value, 0u};
    (void)d3d8_kernel_call(KERNEL_ORDINAL_AV_SEND_TV_ENCODER_OPTION, args, 4u);
    d3d8_guest_store32(GLOBAL_FLICKER_CACHED, 1u);
    d3d8_guest_store32(GLOBAL_FLICKER_VALUE, value);
}

void d3d8_set_soft_display_filter(uint32_t value)
{
    const uint32_t enabled = value != 0u ? 1u : 0u;
    if (d3d8_guest_load32(GLOBAL_SOFT_FILTER_CACHED) != 0u &&
        d3d8_guest_load32(GLOBAL_SOFT_FILTER_VALUE) == enabled) {
        return;
    }
    const uint32_t args[4] = {d3d8_device_load32(DEV_DISPLAY_OBJECT),
                              AV_OPTION_SOFT_DISPLAY_FILTER, value, 0u};
    (void)d3d8_kernel_call(KERNEL_ORDINAL_AV_SEND_TV_ENCODER_OPTION, args, 4u);
    d3d8_guest_store32(GLOBAL_SOFT_FILTER_CACHED, 1u);
    d3d8_guest_store32(GLOBAL_SOFT_FILTER_VALUE, enabled);
}

/* --- surfaces, 0x003DA700 ------------------------------------------------------------ */

static uint32_t allocate_surface_memory(uint32_t bytes)
{
    const uint32_t args[5] = {bytes, 0u, 0xFFFFFFFFu, SURFACE_ALIGNMENT, SURFACE_PROTECT};
    return d3d8_kernel_call(KERNEL_ORDINAL_MM_ALLOCATE_CONTIGUOUS_EX, args, 5u);
}

/* The size rounding 0x003DA999 and 0x003DAB5E apply: up to a whole number of pitches, then
 * pitch by pitch until it is a multiple of 16 KiB. */
static uint32_t round_allocation(uint32_t bytes, uint32_t pitch)
{
    uint32_t rounded = ((bytes + pitch - 1u) / pitch) * pitch;
    while ((rounded & 0x3FFFu) != 0u) {
        rounded += pitch;
    }
    return rounded;
}

static void load_present_parameters(uint32_t address, present_parameters *out)
{
    uint32_t *words = (uint32_t *)out;
    for (unsigned index = 0; index < sizeof(*out) / sizeof(uint32_t); index++) {
        words[index] = d3d8_guest_load32(address + index * 4u);
    }
}

/*
 * Only what the title asks for is ported. These are all `jne` or table branches in the
 * original that lead to code nothing in this boot exercises, and a port of them would be
 * unvalidated. A different title configuration stops here, at the function, rather than
 * running on an unmodelled path.
 */
static void require_supported(const present_parameters *request)
{
    const uint32_t entry = 0x003DA700u;
    if (request->back_buffer_count != 1u) {
        d3d8_hle_fatal(entry, "BackBufferCount %u is not ported (the title uses 1)",
                       (unsigned)request->back_buffer_count);
    }
    if (request->multisample != 0u && request->multisample != MULTISAMPLE_NONE) {
        d3d8_hle_fatal(entry, "MultiSampleType 0x%x is not ported (the title uses 0x11)",
                       (unsigned)request->multisample);
    }
    if (request->swap_effect != SWAP_EFFECT_COPY) {
        d3d8_hle_fatal(entry, "SwapEffect %u is not ported: it takes the contiguous back/front "
                              "allocation, not the separate front buffer the title's effect 3 "
                              "does",
                       (unsigned)request->swap_effect);
    }
    if (request->buffer_surface[0] != 0u) {
        d3d8_hle_fatal(entry, "caller-supplied BufferSurfaces are not ported");
    }
}

/* One surface: pitch, words, allocation, header. `round` is whether the allocation is rounded
 * up (the back and depth buffers are, the front buffer is not). Returns the allocation's
 * address in `*allocation`, false when the allocator refused. */
static bool create_surface(uint32_t header, uint32_t width, uint32_t height, uint32_t format,
                           uint32_t pitch, bool round, uint32_t *allocation)
{
    uint32_t format_word = 0u;
    uint32_t size_word = 0u;
    const uint32_t bytes = d3d8_surface_linear_words(width, height, format, pitch, &format_word,
                                                     &size_word);
    *allocation = allocate_surface_memory(round ? round_allocation(bytes, pitch) : bytes);
    if (*allocation == 0u) {
        return false;
    }
    d3d8_surface_init_header(header, format_word, size_word,
                             guest_physical_address(*allocation));
    return true;
}

static bool create_display_surfaces(const present_parameters *request)
{
    const uint32_t back_format = d3d8_surface_normalise_format(request->format);
    const uint32_t depth_format = d3d8_surface_normalise_format(request->depth_format);
    const uint32_t width = request->width;
    const uint32_t height = request->height;

    d3d8_guest_store32(GLOBAL_PRESENT_INTERVAL, request->presentation_interval);
    d3d8_device_store32(DEV_SWAP_EFFECT, request->swap_effect);
    d3d8_device_store32(DEV_SURFACE_COUNT, request->back_buffer_count + 1u);
    d3d8_device_store32(D3D8_DEV_FLAGS,
                        d3d8_device_load32(D3D8_DEV_FLAGS) | DEV_FLAG_SWAP_TARGET);
    d3d8_device_store32(DEV_MULTISAMPLE_TYPE, MULTISAMPLE_NONE);
    d3d8_device_store32(DEV_MS_SCALE_X, FLOAT_ONE);
    d3d8_device_store32(DEV_MS_SCALE_Y, FLOAT_ONE);
    d3d8_guest_store32(GLOBAL_MULTISAMPLE_FLAGS, 0u);
    d3d8_guest_store32(GLOBAL_MULTISAMPLE_KIND, 2u);

    /* The back buffer: the pitch comes from the legal-pitch table. */
    const uint32_t back_pitch = d3d8_surface_pitch_for_width(width, back_format);
    const uint32_t back_header = D3D8_DEVICE_BASE + DEV_HEADER_ARRAY;
    uint32_t allocation = 0u;
    if (!create_surface(back_header, width, height, back_format, back_pitch, true, &allocation)) {
        return false;
    }
    d3d8_device_store32(DEV_BACK_ALLOCATION, allocation);
    d3d8_device_store32(DEV_SURFACE_LIST, back_header);

    /* The front buffer: allocated separately for this swap effect, and its words are built
     * with a pitch of 0, so the row bytes come from the aligned-row formula and not the table. */
    const uint32_t front_header = D3D8_DEVICE_BASE + DEV_FRONT_HEADER;
    const uint32_t front_pitch = d3d8_surface_aligned_row_bytes(width, back_format);
    if (!create_surface(front_header, width, height, back_format, front_pitch, false,
                        &allocation)) {
        return false;
    }
    d3d8_device_store32(DEV_FRONT_ALLOCATION, allocation);
    d3d8_device_store32(DEV_FRONT_LIST, front_header);

    /* The depth buffer, when asked for. */
    if (request->auto_depth_stencil != 0u) {
        const uint32_t depth_header = D3D8_DEVICE_BASE + DEV_DEPTH_HEADER;
        const uint32_t depth_pitch = d3d8_surface_pitch_for_width(width, depth_format);
        if (!create_surface(depth_header, width, height, depth_format, depth_pitch, true,
                            &allocation)) {
            return false;
        }
        d3d8_device_store32(DEV_DEPTH_ALLOCATION, allocation);
        d3d8_device_store32(DEV_DEPTH_HEADER_SLOT, depth_header);
    }

    /* The tail of 0x003DA700 (0x003DAC24): one reference on each listed surface and on the
     * depth buffer. */
    const uint32_t listed = d3d8_device_load32(DEV_SURFACE_COUNT);
    for (uint32_t slot = 0u; slot < listed; slot++) {
        (void)d3d8_resource_add_ref(d3d8_device_load32(DEV_SURFACE_LIST + slot * 4u));
    }
    const uint32_t depth_slot = d3d8_device_load32(DEV_DEPTH_HEADER_SLOT);
    if (depth_slot != 0u) {
        (void)d3d8_resource_add_ref(depth_slot);
    }
    return true;
}

/* --- 0x003DACA0, the device initialiser ---------------------------------------------- */

static void initialise_gamma_ramps(void)
{
    /* 0x003DCB7A fills each planar channel with its byte index; 0x003DD144
     * invokes it for both display-object ramps. */
    for (uint32_t channel = 0u; channel < 6u; channel++) {
        for (uint32_t entry = 0u; entry < 256u; entry += 4u) {
            const uint32_t value = entry | ((entry + 1u) << 8u) |
                                   ((entry + 2u) << 16u) | ((entry + 3u) << 24u);
            d3d8_device_store32(0x1E04u + channel * 256u + entry, value);
        }
    }
}

static uint32_t initialise_device(uint32_t request_address)
{
    present_parameters request;
    load_present_parameters(request_address, &request);

    /* 0x003DACB0 allocates the control block first and 0x003D6360 the ring and the kick history
     * after it. The GPU model owns the control block, the history and the two events. */
    if (!d3d8_gpu_create() || !d3d8_pushbuffer_create()) {
        return D3D8_E_OUTOFMEMORY;
    }
    d3d8_shader_create();

    d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK,
                       d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | DIRTY_AT_CREATE);

    /* The register window: a literal the original stores at 0x003DCC51 (`mov [ecx], 0xFD000000`).
     * This host maps no registers, and nothing here dereferences the address. */
    d3d8_device_store32(DEV_DISPLAY_OBJECT, D3D8_NV2A_REGISTER_BASE);
    initialise_gamma_ramps();
    d3d8_device_store32(DEV_REGISTER_BASE, D3D8_NV2A_REGISTER_BASE);
    d3d8_guest_store32(GLOBAL_REGISTER_BASE_COPY, D3D8_NV2A_REGISTER_BASE);

    /* The library asks the kernel for the capability word early, through the mode lookup
     * inside 0x003DBE3D, and caches it. Asking here keeps the kernel's record of the query
     * in the same place relative to the allocations. */
    (void)d3d8_display_capabilities();

    require_supported(&request);
    if (!create_display_surfaces(&request)) {
        return D3D8_E_OUTOFMEMORY;
    }

    const d3d8_mode_request match = {
        .width = request.width,
        .height = request.height,
        .refresh = request.refresh_rate,
        .flags = request.flags,
        .format = request.format,
        .interval = request.presentation_interval,
        .pitch = d3d8_surface_header_pitch(d3d8_device_load32(DEV_FRONT_LIST)),
    };
    const uint32_t matched =
        d3d8_display_match_mode(D3D8_DEVICE_BASE + DEV_DISPLAY_OBJECT, &match);
    if (matched != 0u) {
        return matched;
    }

    /* 0x003DB087: the fence after the mode match, which ends the counter at 7 and the kick that goes
     * with it. */
    (void)d3d8_gpu_fence_insert(0u);
    d3d8_device_store32(DEV_SCALE_LAST, FLOAT_ONE);
    /* The original CreateDevice seeds the transform multiplied by the viewport matrix to identity. */
    static const uint32_t identity[16] = {
        0x3F800000u, 0u, 0u, 0u, 0u, 0x3F800000u, 0u, 0u,
        0u, 0u, 0x3F800000u, 0u, 0u, 0u, 0u, 0x3F800000u,
    };
    for (uint32_t index = 0u; index < 16u; index++) {
        d3d8_device_store32(DEV_COMPOSITE_MATRIX + index * 4u, identity[index]);
    }
    d3d8_set_render_target(d3d8_device_load32(DEV_SURFACE_LIST),
                           d3d8_device_load32(DEV_DEPTH_HEADER_SLOT));
    /* The original leaves the render-target flag at 1 (bit 0 of the device flags, and 0x003E3F3C) and
     * bit 1 of the flags set by the projection setup at 0x003D3483, which is not ported. MEASURED. */
    d3d8_set_render_target_flag(1u);
    /* The four texture stages are unbound at creation, which leaves each stage's format cache at
     * 0x80000000 (MEASURED: device+0xC to +0x18 after the original's CreateDevice). */
    for (uint32_t stage = 0u; stage < 4u; stage++) {
        d3d8_set_texture(stage, 0u);
    }
    d3d8_device_store32(D3D8_DEV_FLAGS, d3d8_device_load32(D3D8_DEV_FLAGS) | DEV_FLAG_PROJECTION);
    /* The retail CreateDevice default-state table calls SetRenderState for 0x8B and 0x8C with
     * 0x1B02 (measured at 0x003D73BA/0x003D73DB). Seed both shadows and emit the same fill-mode
     * method through the port's measured setters before copy composition can restore them. */
    /* T1059: full original CreateDevice calls 0x003D7010 with EAX=0;
     * 0x003D7042 emits 0x402A8/0 and 0x003D7053 stores the state8A shadow.
     * This is an actual default-state packet, not a renderer reset guess. */
    d3d8_state_library_set_8a(0u);
    d3d8_state_set_8b(0x1B02u);
    d3d8_state_set_8c(0x1B02u);
    d3d8_set_flicker_filter(5u);
    d3d8_set_soft_display_filter(0u);
    return 0u;
}

/* --- 0x003D9230 ---------------------------------------------------------------------- */

uint32_t d3d8_create_device(uint32_t behavior_flags, uint32_t present_parameters_address,
                            uint32_t out_device)
{
    if (d3d8_guest_load32(D3D8_GLOBAL_PUSHBUFFER_SIZE) == 0u) {
        d3d8_guest_store32(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x80000u);
    }
    if (d3d8_guest_load32(D3D8_GLOBAL_KICKOFF_SIZE) == 0u) {
        d3d8_guest_store32(D3D8_GLOBAL_KICKOFF_SIZE, 0x8000u);
    }

    d3d8_guest_store32(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);
    d3d8_guest_store32(GLOBAL_DEVICE_CREATED, 1u);
    d3d8_device_store32(D3D8_DEV_FLAGS,
                        d3d8_device_load32(D3D8_DEV_FLAGS) | (behavior_flags & 0x10u));

    const uint32_t result = initialise_device(present_parameters_address);
    if ((result & 0x80000000u) != 0u) {
        /* 0x003D9296: the original tears the device down, then clears its struct. */
        if (out_device != 0u) {
            d3d8_guest_store32(out_device, 0u);
        }
        for (uint32_t offset = 0u; offset < DEVICE_BYTES_CLEARED_ON_FAILURE; offset += 4u) {
            d3d8_device_store32(offset, 0u);
        }
        d3d8_guest_store32(D3D8_DEVICE_POINTER_SLOT, 0u);
        return result;
    }
    if (out_device != 0u) {
        d3d8_guest_store32(out_device, D3D8_DEVICE_BASE);
    }
    return 0u;
}

/* --- 0x003D3A80 ---------------------------------------------------------------------- */

uint32_t d3d8_get_back_buffer(int32_t index)
{
    uint32_t slot = 1u;
    if (index != -1) {
        slot = index != 0 ? 2u : 0u;
    }
    const uint32_t header = d3d8_device_load32(DEV_SURFACE_LIST + slot * 4u);
    if (header == 0u) {
        d3d8_hle_fatal(0x003D3A80u, "GetBackBuffer2(%d): slot %u of the surface list is empty",
                       (int)index, (unsigned)slot);
    }
    (void)d3d8_resource_add_ref(header);
    return header;
}

/* --- handlers -------------------------------------------------------------------------- */

static uint32_t handler_create(void *context)
{
    (void)context;
    return 1u;
}

static uint32_t handler_mode_count(void *context)
{
    (void)context;
    return d3d8_adapter_mode_count();
}

static uint32_t handler_enum_mode(void *context)
{
    const uint32_t mode = frame_argument(context, 1u, 0x003D90B0u);
    const uint32_t out = frame_argument(context, 2u, 0x003D90B0u);
    return d3d8_adapter_enum_mode(mode, out);
}

static uint32_t handler_set_push_buffer_size(void *context)
{
    return d3d8_set_push_buffer_size(frame_argument(context, 0u, 0x003D9210u),
                                     frame_argument(context, 1u, 0x003D9210u));
}

static uint32_t handler_create_device(void *context)
{
    /* Arguments: adapter, device type, focus window (all ignored by the original), behaviour
     * flags, presentation parameters, the out-pointer. */
    const uint32_t behavior = frame_argument(context, 3u, 0x003D9230u);
    const uint32_t parameters = frame_argument(context, 4u, 0x003D9230u);
    const uint32_t out = frame_argument(context, 5u, 0x003D9230u);
    return d3d8_create_device(behavior, parameters, out);
}

static uint32_t handler_pushbuffer_pair(void *context)
{
    const uint32_t header = frame_register(context, 0u, 0x003D6C90u);
    const uint32_t value = frame_register(context, 1u, 0x003D6C90u);
    d3d8_pushbuffer_emit_pair(header, value);
    /* The original leaves the advanced cursor in eax. Nothing reads it. */
    return d3d8_device_load32(D3D8_DEV_CURSOR);
}

/* --- 0x003D6660 BeginPush and 0x003D6680 EndPush (T854) ------------------------------- */

/* The block the last BeginPush handed out: [begin, end) is what its reservation covers. The original keeps no such
 * state (EndPush stores whatever it is given), the port does so a pointer that did not come from a BeginPush, or
 * that runs past the reservation, is refused by name instead of becoming the cursor. */
static struct {
    bool open;
    uint32_t begin;
    uint32_t end;
} push_block;

uint32_t d3d8_begin_push(uint32_t count)
{
    const uint32_t dwords = count + 1u; /* `inc [esp+4]`: 32 bit wrap like the original */
    const uint32_t ring_bytes = d3d8_device_load32(0x28u) - d3d8_device_load32(0x24u);
    if (dwords == 0u || (uint64_t)dwords * 4u + D3D8_PUSHBUFFER_SLACK_BYTES > ring_bytes) {
        d3d8_hle_fatal(0x003D6660u, "BeginPush(%#x) reserves %#x dwords, more than the %#x byte ring holds "
                                    "(the original would loop in the refill, the port refuses)",
                       (unsigned)count, (unsigned)dwords, (unsigned)ring_bytes);
    }
    /* The deferred state flush 0x003DEE00(device, 0) is the draw's (d3d8_resource.c): plan everything it would write
     * and the reservation first, so a refusal (an unsupported dirty mask, a refill that must wait, an unmapped span)
     * comes before the first write. */
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    d3d8_draw_plan_deferred(&sim, 0u);
    (void)d3d8_pushbuffer_sim_reserve(&sim, dwords);
    if (kernel_guest_at(sim.cursor, dwords * 4u) == NULL) {
        d3d8_hle_fatal(0x003D6660u, "BeginPush block %#x+%u bytes is not mapped guest memory",
                       (unsigned)sim.cursor, (unsigned)(dwords * 4u));
    }
    d3d8_draw_flush_streams(0u);
    /* `inc [esp+4]; jmp 0x003D6B30`: the sized reservation, whose cursor comes back in eax. */
    const uint32_t cursor = d3d8_pushbuffer_reserve(dwords);
    push_block.open = true;
    push_block.begin = cursor;
    push_block.end = cursor + dwords * 4u;
    return cursor;
}

uint32_t d3d8_end_push(uint32_t pointer)
{
    if (!push_block.open) {
        d3d8_hle_fatal(0x003D6680u, "EndPush(%#x) with no BeginPush block open", (unsigned)pointer);
    }
    if ((pointer & 3u) != 0u || pointer < push_block.begin || pointer > push_block.end) {
        d3d8_hle_fatal(0x003D6680u, "EndPush(%#x) is not a dword position inside the block BeginPush reserved "
                                    "[%#x, %#x]",
                       (unsigned)pointer, (unsigned)push_block.begin, (unsigned)push_block.end);
    }
    d3d8_pushbuffer_end(pointer);
    push_block.open = false;
    return pointer; /* `mov eax, [esp+4]` */
}

static uint32_t handler_begin_push(void *context)
{
    return d3d8_begin_push(frame_argument(context, 0u, 0x003D6660u));
}

static uint32_t handler_end_push(void *context)
{
    return d3d8_end_push(frame_argument(context, 0u, 0x003D6680u));
}

static uint32_t handler_create_buffer(void *context)
{
    return d3d8_create_buffer(frame_argument(context, 0u, 0x003D4EE0u));
}

static uint32_t handler_vertex_buffer_lock2(void *context)
{
    return d3d8_vertex_buffer_lock2(frame_argument(context, 0u, 0x003D4F30u),
                                   frame_argument(context, 1u, 0x003D4F30u));
}

static uint32_t handler_set_stream_source(void *context)
{
    return d3d8_set_stream_source(frame_argument(context, 0u, 0x003D58F0u),
                                 frame_argument(context, 1u, 0x003D58F0u),
                                 frame_argument(context, 2u, 0x003D58F0u));
}

static uint32_t handler_get_back_buffer(void *context)
{
    return d3d8_get_back_buffer((int32_t)frame_argument(context, 0u, 0x003D3A80u));
}

size_t d3d8_device_register(void)
{
    static const struct {
        uint32_t address;
        d3d8_fn handler;
    } handlers[] = {
        {0x003D9000u, handler_create},
        {0x003D9010u, handler_mode_count},
        {0x003D90B0u, handler_enum_mode},
        {0x003D9210u, handler_set_push_buffer_size},
        {0x003D9230u, handler_create_device},
        {0x003D6C90u, handler_pushbuffer_pair},
        {0x003D6660u, handler_begin_push},
        {0x003D6680u, handler_end_push},
        {0x003D3A80u, handler_get_back_buffer},
        {0x003D4EE0u, handler_create_buffer},
        {0x003D4F30u, handler_vertex_buffer_lock2},
        {0x003D58F0u, handler_set_stream_source},
    };
    size_t registered = 0u;
    memset(&push_block, 0, sizeof(push_block));
    for (size_t index = 0u; index < sizeof(handlers) / sizeof(handlers[0]); index++) {
        if (d3d8_hle_register(handlers[index].address, handlers[index].handler)) {
            registered++;
        }
    }
    return registered + d3d8_state_register() + d3d8_frame_register() +
           d3d8_bind_register() + d3d8_gpu_register() + d3d8_present_register() +
           d3d8_shader_register() + d3d8_resource_draw_register() + d3d8_indexed_register() +
           d3d8_cube_surface_register() + d3d8_lock_register() + d3d8_overlay_register() +
           d3d8_callbacks_register() + d3d8_scissor_register() + d3d8_method_packet_register() +
           d3d8_vertex_constants_register() + d3d8_immediate_register() + d3d8_copy_register() + d3d8_visibility_register();
}
