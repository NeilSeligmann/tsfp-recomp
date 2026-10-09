/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_bind.h. Every function carries the address of the original it ports.
 */

#include "d3d8_bind.h"
#include "d3d8_reference.h"

#include <stdbool.h>
#include <string.h>

#include "d3d8_gpu.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_surface.h"
#include "d3d8_swap_replay.h"
#include "d3d8_pushbuffer.h"
#include "d3d8_scaled_viewport.h"
#include "d3d8_viewport_matrix.h"
#include "kernel_call.h"

/* Device fields. */
#define DEV_RENDER_TARGET 0x1A04u
#define DEV_DEPTH_SURFACE 0x1A08u
#define DEV_SURFACE_FORMAT 0x1A0Cu
#define DEV_BACK_BUFFER 0x1A14u
#define DEV_DEPTH_MAX 0x0948u
#define DEV_DEPTH_PITCH 0x094Cu
#define DEV_TARGET_WIDTH 0x0954u   /* the render target size the recompute leaves, and the scales */
#define DEV_TARGET_HEIGHT 0x0958u
#define DEV_SCALE_X 0x095Cu
#define DEV_SCALE_Y 0x0960u
#define DEV_SCALE_LAST 0x0964u
#define DEV_SAMPLE_SCALE_X 0x096Cu
#define DEV_SAMPLE_SCALE_Y 0x0970u
#define DEV_VIEWPORT 0x0EE0u       /* x, y, width, height, then the depth range minimum and maximum */
#define DEV_DEPTH_RANGE 0x0EF0u
#define DEV_STAGE_FORMAT 0x000Cu   /* + 4 * stage, the cached format bits */
#define DEV_STAGE_TEXTURE 0x0F88u  /* + 4 * stage, the bound texture */
#define DEV_STAGES 4u

#define GLOBAL_RENDER_TARGET_FLAG 0x003E3F3Cu
#define GLOBAL_MULTISAMPLE_FLAGS 0x003E3F28u
#define GLOBAL_SUPERSAMPLE_KIND 0x003E3F2Cu
#define FLOAT_ONE 0x3F800000u
#define FORMAT_INFO_TABLE 0x003E1828u

#define COMMON_TYPE_MASK 0x00070000u
#define COMMON_TYPE_SURFACE 0x00050000u
#define COMMON_BINDING_UNIT 0x00080000u
#define COMMON_BINDING_MASK 0x00780000u
#define COMMON_BINDING_OR_COUNT 0x0078FFFFu

#define DEV_FLAG_FLIP_PENDING 0x100u
#define DEV_FLAG_MULTISAMPLE_FORMAT 0x1u

#define DIRTY_TEXTURE_UNBIND 0x4800u
#define DIRTY_TEXTURE_FORMAT 0x4000u
#define DIRTY_TEXTURE_SHADOW 0x0800u

static void or_dirty_mask(uint32_t bits)
{
    d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK, d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK) | bits);
}

static float scale_float(uint32_t word)
{
    float value;
    memcpy(&value, &word, sizeof(value));
    if ((word & 0x80000000u) != 0u || (word & 0x7F800000u) == 0u ||
        (word & 0x7F800000u) == 0x7F800000u) {
        d3d8_hle_fatal(0x003D7B80u, "viewport scale %#x is not a positive normal float", (unsigned)word);
    }
    return value;
}

/* The header that holds the fence a surface was last used at: the parent texture when there is one
 * (0x003D3833 and 0x003D3865 pick `[header+0x14]` when it is not 0). */
static uint32_t fence_holder(uint32_t header)
{
    const uint32_t parent = d3d8_guest_load32(header + D3D8_SURFACE_PARENT);
    return parent != 0u ? parent : header;
}

/* Reference operations preflight the complete parent/destruction plan. */
uint32_t d3d8_resource_release(uint32_t header)
{
    return d3d8_reference_release(header);
}
void d3d8_resource_release_binding(uint32_t header)
{
    d3d8_reference_release_binding(header);
}

/* --- 0x003D7220 -------------------------------------------------------------------------- */

void d3d8_set_render_target_flag(uint32_t value)
{
    const uint32_t target = d3d8_device_load32(DEV_RENDER_TARGET);
    const uint32_t format = (d3d8_guest_load32(target + D3D8_SURFACE_FORMAT) >> 8) & 0xFFu;
    const uint32_t bits_per_pixel = (uint32_t)d3d8_guest_load8(FORMAT_INFO_TABLE + format) & 0x3Cu;
    const uint32_t wanted = bits_per_pixel == 0x20u ? value : 0u;
    const uint32_t flags = d3d8_device_load32(D3D8_DEV_FLAGS);
    if (wanted != (flags & DEV_FLAG_MULTISAMPLE_FORMAT)) {
        d3d8_device_store32(D3D8_DEV_FLAGS, flags ^ DEV_FLAG_MULTISAMPLE_FORMAT);
        d3d8_gpu_note_elided(0x003D7220u);
    }
    d3d8_guest_store32(GLOBAL_RENDER_TARGET_FLAG, value);
}

/* --- SetRenderTarget -------------------------------------------------------------------- */

/* Recompute target size/scales, viewport matrix and the command stream emitted by the original's
 * 0x003D7B80 and 0x003D3E40 helpers. Exact behavior is pinned against those helpers in the oracle. */
static void reset_viewport_state(uint32_t target, uint32_t depth)
{
    const bool first_back_buffer = target == d3d8_device_load32(DEV_BACK_BUFFER);
    const uint32_t mode = d3d8_guest_load32(first_back_buffer ? GLOBAL_MULTISAMPLE_FLAGS
                                                              : GLOBAL_SUPERSAMPLE_KIND);
    if ((first_back_buffer && mode > 2u) || (!first_back_buffer && mode > 4u)) {
        d3d8_hle_fatal(0x003D7B80u, "SetRenderTarget sample mode %u is not measured", (unsigned)mode);
    }
    /* T533: 0x003D7B80 is one piece shared with render states 0x9A and 0x9B (d3d8_scaled_viewport.c): the clip
     * size, the scales, the device flag and the words of 0x40208, 0x40380 and 0x41D7C. */
    d3d8_scaled_viewport plan;
    d3d8_scaled_viewport_compute(&plan, mode);
    const uint32_t size_word = d3d8_guest_load32(target + D3D8_SURFACE_SIZE);
    const uint32_t format_word = d3d8_guest_load32(target + D3D8_SURFACE_FORMAT);
    const uint32_t source_width = size_word != 0u ? (size_word & 0xFFFu) + 1u
                                                  : 1u << ((format_word >> 20) & 0xFu);
    const uint32_t source_height =
        size_word != 0u ? ((size_word >> 12) & 0xFFFu) + 1u : 1u << ((format_word >> 24) & 0xFu);
    const uint32_t width = plan.width;
    const uint32_t height = plan.height;
    const uint32_t scale_x = plan.scale_x;
    const uint32_t scale_y = plan.scale_y;
    d3d8_scaled_viewport_store(&plan);
    d3d8_device_store32(DEV_VIEWPORT, 0u);
    d3d8_device_store32(DEV_VIEWPORT + 4u, 0u);
    d3d8_device_store32(DEV_VIEWPORT + 8u,
                        first_back_buffer ? (uint32_t)((float)width / scale_float(scale_x))
                                          : source_width);
    d3d8_device_store32(DEV_VIEWPORT + 12u,
                        first_back_buffer ? (uint32_t)((float)height / scale_float(scale_y))
                                          : source_height);
    d3d8_device_store32(DEV_DEPTH_RANGE, 0u);
    d3d8_device_store32(DEV_DEPTH_RANGE + 4u, FLOAT_ONE);
    (void)d3d8_rebuild_viewport_matrix();

    /* The original's 0x003D7B80 and SetViewport (0x003D3E40) split this same stream into
     * small writers. Each pair below is an NV2A method packet, in the measured order. */
    const uint32_t surface_address = d3d8_guest_load32(target + D3D8_SURFACE_DATA);
    const uint32_t depth_address = depth != 0u ? d3d8_guest_load32(depth + D3D8_SURFACE_DATA) : 0u;
    const uint32_t width_bytes = width << 16;
    const uint32_t height_bytes = height << 16;
    const uint32_t method_208 = plan.method_208;
    /* T860: the original 0x003D38D0..0x003D395D forms method 0x20C from the TARGET's own pitch (the Size word's top byte plus
     * one, times 64, or the width and format of a swizzled one, d3d8_surface_header_pitch) in the low half and the device's
     * depth pitch (0x94C, stored above) in the high half. It was a constant of 0x0A000A00 for the display surfaces and 0x0A000040
     * for every other target, which pushed pitch 64 for a title surface header that aliases a frame buffer (the retail intro
     * binds one at 0x374000: 308 draws and 629 CLEAR_SURFACE events then named a pitch the registered 2560 target does not have). */
    const uint32_t surface_format_command =
        (d3d8_device_load32(DEV_DEPTH_PITCH) << 16) | (d3d8_surface_header_pitch(target) & 0xFFFFu);
    /* Original 0x003D7E75..0x003D7EA8 re-emits the current state shadows,
     * including caller changes made through SetRenderState(0x99). */
    const uint32_t multisample_mask = plan.method_41d7c;
    /* Original 0x003D7A50 forms method 0x40290 from current A0/8F shadows
     * and the bound depth format; it is not a fixed default-state constant. */
    uint32_t target_control = d3d8_guest_load32(0x003E3F40u) != 0u ? 0x10100001u : 0x00100001u;
    if (d3d8_guest_load32(0x003E3EFCu) == 2u) target_control |= 0x10000u;
    if (depth != 0u) {
        const uint32_t depth_format = (d3d8_guest_load32(depth + D3D8_SURFACE_FORMAT) >> 8) & 0xFFu;
        if (depth_format == 0x2Du || depth_format == 0x2Bu || depth_format == 0x31u ||
            depth_format == 0x2Fu) target_control |= 0x1000u;
    }
    /* Actual SetRenderTarget inline branch 0x003D39EC..0x003D3A0F. */
    const uint32_t stencil_enable =
        depth != 0u && d3d8_guest_load32(0x003E3F00u) != 0u ? 1u : 0u;
    uint32_t stream[77] = {
        0x00040100u, 0u, 0x0004020Cu, surface_format_command,
        0x00040100u, 0u,
        0x00040110u, 0u, 0x00040100u, 0u, 0x00040210u, surface_address,
        0x00040100u, 0u, 0x00040110u, 0u, 0x00040100u, 0u,
        0x00040214u, depth_address, 0x00040100u, 0u, 0x00040110u, 0u,
        0x00040100u, 0u, 0x0004020Cu, surface_format_command,
        0x00040100u, 0u,
        0x00040110u, 0u, 0x00040100u, 0u, 0x00040210u, surface_address,
        0x00040100u, 0u, 0x00040110u, 0u, 0x00040100u, 0u,
        0x00040214u, depth_address, 0x00040100u, 0u, 0x00040110u, 0u,
        0x00040290u, target_control, 0x0004030Cu, depth != 0u ? 1u : 0u, 0x0004032Cu, stencil_enable,
        0x00040208u, method_208,
        0x00041D7Cu, multisample_mask,
        0x00080200u, width_bytes, height_bytes,
        0x000402B4u, 0u, 0x000402C0u, width_bytes, 0x000402E0u, height_bytes,
        0x00100A20u, mode != 0u ? 0x3D000000u : 0x3F080000u,
        mode != 0u ? 0x3D000000u : 0x3F080000u, 0u, 0u,
        0x00080394u, 0u, 0x4B7FFFFFu,
    };
    uint32_t count = 75u;
    if (plan.changed) {
        /* Insert the sample-control pair before the depth-range packet. */
        for (uint32_t index = count; index-- > 56u;) stream[index + 2u] = stream[index];
        stream[56] = 0x00040380u;
        stream[57] = plan.method_380;
        count += 2u;
    }
    const uint32_t cursor = d3d8_pushbuffer_begin();
    for (uint32_t index = 0u; index < count; index++) d3d8_guest_store32(cursor + index * 4u, stream[index]);
    d3d8_pushbuffer_end(cursor + count * 4u);
}

void d3d8_set_render_target(uint32_t target, uint32_t depth)
{
    if (target == 0u) {
        target = d3d8_device_load32(DEV_RENDER_TARGET);
    }
    d3d8_swap_replay_on_render_target(d3d8_device_load32(DEV_RENDER_TARGET), target);
    d3d8_resource_add_render_target_ref(target);

    /* The old target is stamped with the fence the GPU may still be reading it until, then released. */
    const uint32_t previous = d3d8_device_load32(DEV_RENDER_TARGET);
    if (previous != 0u) {
        d3d8_guest_store32(fence_holder(previous) + D3D8_SURFACE_LOCK,
                           d3d8_device_load32(D3D8_DEV_FENCE));
        d3d8_resource_release_binding(previous);
    }
    const uint32_t previous_depth = d3d8_device_load32(DEV_DEPTH_SURFACE);
    d3d8_device_store32(DEV_RENDER_TARGET, target);
    if (previous_depth != 0u) {
        d3d8_guest_store32(fence_holder(previous_depth) + D3D8_SURFACE_LOCK,
                           d3d8_device_load32(D3D8_DEV_FENCE));
        d3d8_resource_release_binding(previous_depth);
    }

    d3d8_device_store32(DEV_DEPTH_SURFACE, depth);
    if (depth != 0u) {
        d3d8_resource_add_render_target_ref(depth);
        const uint32_t depth_format = (d3d8_guest_load32(depth + D3D8_SURFACE_FORMAT) >> 8) & 0xFFu;
        d3d8_device_store32(DEV_DEPTH_MAX, d3d8_surface_max_depth_bits(depth_format));
        d3d8_device_store32(DEV_DEPTH_PITCH, d3d8_surface_header_pitch(depth));
    }

    /* The flip-pending bit clears when the target becomes the first back buffer (0x003D3A18). */
    const uint32_t flags = d3d8_device_load32(D3D8_DEV_FLAGS);
    if ((flags & DEV_FLAG_FLIP_PENDING) != 0u && target == d3d8_device_load32(DEV_BACK_BUFFER)) {
        d3d8_device_store32(D3D8_DEV_FLAGS, flags & ~DEV_FLAG_FLIP_PENDING);
    }

    d3d8_device_store32(DEV_SURFACE_FORMAT, d3d8_surface_format_word(target, depth));
    reset_viewport_state(target, depth);
    /* MEASURED from a clear dirty mask (T443): SetRenderTarget leaves 0x300, 0x100 from the
     * SetViewport it runs (the point parameters) and 0x200 from D3D_UpdateProjectionViewportTransform
     * (the matrices). Invisible while the mask is saturated, and the copy composition's cascade
     * clears both, so the next draw must see them. */
    /* 0x10F follows the sample scale changing (d3d8_scaled_viewport_store, 0x003D7DF3). */
    or_dirty_mask(0x300u);
    d3d8_set_render_target_flag(d3d8_guest_load32(GLOBAL_RENDER_TARGET_FLAG));
}

/* --- SetTexture ------------------------------------------------------------------------- */

void d3d8_set_texture(uint32_t stage, uint32_t texture)
{
    if (stage >= DEV_STAGES) {
        d3d8_hle_fatal(0x003D4070u, "SetTexture stage %u: only the four stages the title uses are "
                                    "modelled", (unsigned)stage);
    }
    const uint32_t slot = DEV_STAGE_TEXTURE + stage * 4u;
    const uint32_t previous = d3d8_device_load32(slot);

    if (previous != 0u) {
        const uint32_t dropped =
            d3d8_guest_load32(previous + D3D8_SURFACE_COMMON) - COMMON_BINDING_UNIT;
        if ((dropped & COMMON_BINDING_OR_COUNT) == 0u)
            d3d8_hle_fatal(0x003D4B30u, "SetTexture last binding destruction is not recovered");
        d3d8_guest_store32(previous + D3D8_SURFACE_LOCK, d3d8_device_load32(D3D8_DEV_FENCE));
        d3d8_guest_store32(previous + D3D8_SURFACE_COMMON, dropped);
    }
    d3d8_device_store32(slot, texture);
    d3d8_swap_replay_on_texture(stage, texture); /* T510: no-op unless the render target texture option is on */
    d3d8_gpu_note_elided(0x003D4070u);

    if (texture == 0u) {
        d3d8_device_store32(DEV_STAGE_FORMAT + stage * 4u, 0x80000000u);
        or_dirty_mask(DIRTY_TEXTURE_UNBIND);
        return;
    }

    d3d8_guest_store32(texture + D3D8_SURFACE_COMMON,
                       d3d8_guest_load32(texture + D3D8_SURFACE_COMMON) + COMMON_BINDING_UNIT);
    const uint32_t format_word = d3d8_guest_load32(texture + D3D8_SURFACE_FORMAT);
    uint32_t wanted = format_word & 0x20F4u;
    if (d3d8_device_load32(DEV_STAGE_FORMAT + stage * 4u) == wanted) {
        return;
    }
    if ((wanted & 0x2000u) != 0u) {
        const uint32_t format = format_word & 0xFF00u;
        wanted &= 0xFFFFDFFFu;
        if (format >= 0x2A00u && format <= 0x3100u) {
            wanted |= 0x40000000u;
        }
    }
    d3d8_device_store32(DEV_STAGE_FORMAT + stage * 4u, wanted);
    or_dirty_mask(DIRTY_TEXTURE_FORMAT);
    if (previous == 0u) {
        or_dirty_mask(DIRTY_TEXTURE_SHADOW);
    }
}

/* --- handlers --------------------------------------------------------------------------- */

static uint32_t argument(const void *context, unsigned index, uint32_t address)
{
    uint32_t value = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, index, &value)) {
        d3d8_hle_fatal(address, "argument %u of the call cannot be read", index);
    }
    return value;
}

static uint32_t handler_set_render_target(void *context)
{
    d3d8_set_render_target(argument(context, 0u, 0x003D3800u), argument(context, 1u, 0x003D3800u));
    return 0u;
}

static uint32_t handler_set_texture(void *context)
{
    d3d8_set_texture(argument(context, 0u, 0x003D4070u), argument(context, 1u, 0x003D4070u));
    return 0u;
}

static uint32_t handler_release(void *context)
{
    return d3d8_resource_release(argument(context, 0u, 0x003D4C90u));
}

static uint32_t handler_release_binding(void *context)
{
    d3d8_resource_release_binding(argument(context, 0u, 0x003D4DA0u));
    return 0u;
}

static uint32_t handler_render_target_flag(void *context)
{
    d3d8_set_render_target_flag(argument(context, 0u, 0x003D7220u));
    return 0u;
}

size_t d3d8_bind_register(void)
{
    static const struct {
        uint32_t address;
        d3d8_fn handler;
    } handlers[] = {
        {0x003D3800u, handler_set_render_target}, {0x003D4070u, handler_set_texture},
        {0x003D4C90u, handler_release},           {0x003D4DA0u, handler_release_binding},
        {0x003D7220u, handler_render_target_flag},
    };
    size_t registered = 0u;
    for (size_t index = 0u; index < sizeof(handlers) / sizeof(handlers[0]); index++) {
        if (d3d8_hle_register(handlers[index].address, handlers[index].handler)) {
            registered++;
        }
    }
    return registered;
}
