/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_frame.h.
 */

#include "d3d8_frame.h"

#include <stdbool.h>
#include <string.h>

#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "d3d8_surface.h"
#include "kernel_call.h"

#define ENTRY 0x003D5EB0u

/* Device fields Clear reads (all MEASURED from 0x003D5EB0): the render target and depth surface
 * headers, the viewport rectangle (x, y, width, height) and the two viewport scales. */
#define DEV_RENDER_TARGET 0x1A04u
#define DEV_DEPTH_STENCIL 0x1A08u
#define DEV_VIEWPORT 0x0EE0u
#define DEV_SCALE_X 0x095Cu
#define DEV_SCALE_Y 0x0960u

/* Library constants it reads. The class table (0x003D625C, 0x1A bytes indexed by colour format - 3)
 * picks the colour conversion, the format table (0x003E1828) decides by bit 0 whether the surface
 * format is rewritten around the clear. The float constants convert the depth value. */
#define FORMAT_INFO_TABLE 0x003E1828u
#define COLOUR_CLASS_TABLE 0x003D625Cu
#define COLOUR_CLASS_ENTRIES 0x1Au
#define VIEWPORT_BIAS 0x00475CD4u
#define FLOAT_ZERO 0x00475CACu
#define DEPTH_SCALE_24 0x004A1BA0u
#define DEPTH_SCALE_16 0x004A1BA4u
#define DEPTH_SCALE_F24 0x004A1B90u /* a double */
#define DEPTH_SCALE_F16 0x004A1B98u /* a double */

#define SURFACE_FORMAT_BYTE (D3D8_SURFACE_FORMAT + 1u)

/* The D3D section: the device, its tables and the library's code and constants Clear reads. */
#define D3D_REGION_BASE 0x003D0000u
#define D3D_REGION_BYTES 0x00030000u

/* NV2A commands: SET_SURFACE_FORMAT (0x208) one dword, the clear rectangle (0x1D98) two, and the
 * clear values plus CLEAR_SURFACE (0x1D8C) three. */
#define HEADER_SURFACE_FORMAT 0x00040208u
#define HEADER_CLEAR_RECT 0x00081D98u
#define HEADER_CLEAR_VALUES 0x000C1D8Cu
#define SURFACE_FORMAT_BYTES 8u
#define CLEAR_PACKET_BYTES 28u

/* How many rectangles Clear plans before writing. The original has no bound. The oracle covers up to
 * a handful, the cap only keeps the plan on the stack. */
#define MAX_RECTANGLES 256u

static uint64_t clears;
static uint64_t nonblack_clears;
static d3d8_clear_call last_clear;

void d3d8_frame_reset(void)
{
    clears = 0u;
    nonblack_clears = 0u;
    last_clear = (d3d8_clear_call){0};
}

uint64_t d3d8_clear_count(void)
{
    return clears;
}

uint64_t d3d8_clear_nonblack_count(void)
{
    return nonblack_clears;
}

d3d8_clear_call d3d8_clear_last(void)
{
    return last_clear;
}

/* --- guest reads ------------------------------------------------------------------------ */

static uint32_t load_guest(uint32_t address)
{
    return d3d8_guest_load32(address);
}

static uint64_t load_guest64(uint32_t address)
{
    uint64_t value;
    if (!kernel_guest_read_bytes(address, &value, sizeof(value))) {
        d3d8_hle_fatal(ENTRY, "Clear constant at %#x is unreadable", (unsigned)address);
    }
    return value;
}

static uint32_t device_word(uint32_t offset)
{
    return load_guest(D3D8_DEVICE_BASE + offset);
}

/* --- arithmetic the original does on the x87 and SSE units -------------------------------- */

/* The depth and viewport conversions run the original's own instruction sequences (fild, fmul,
 * fadd, fstp to a float, cvttss2si) so the rounding is the original's. They are only claimed for
 * the masked default x87 extended precision and SSE state the oracle ran under. */
static void require_host_arithmetic(void)
{
#if defined(__x86_64__) || defined(__i386__)
    uint32_t mxcsr;
    uint16_t control;
    __asm__ volatile("stmxcsr %0\n\tfnstcw %1" : "=m"(mxcsr), "=m"(control));
    if ((mxcsr & 0xE040u) != 0u || (mxcsr & 0x1F80u) != 0x1F80u || (control & 0xF00u) != 0x300u ||
        (control & 0x3Fu) != 0x3Fu) {
        d3d8_hle_fatal(ENTRY, "Clear requires masked default host rounding and extended precision");
    }
#else
    d3d8_hle_fatal(ENTRY, "Clear arithmetic requires x86 x87 and SSE");
#endif
}

/* 0x003DB320 is `cvttss2si eax, [esp+4]`, called on the float the sequence stored. */
static int32_t viewport_scaled(int32_t value, uint32_t scale, uint32_t bias)
{
#if defined(__x86_64__) || defined(__i386__)
    int32_t result;
    uint32_t spill;
    __asm__ volatile("fildl %2\n\tfmuls %3\n\tfadds %4\n\tfstps %1\n\tcvttss2si %1,%0"
                     : "=r"(result), "=m"(spill)
                     : "m"(value), "m"(scale), "m"(bias)
                     : "st");
    return result;
#else
    (void)value;
    (void)scale;
    (void)bias;
    return 0;
#endif
}

static int32_t depth_scaled(uint32_t depth, uint32_t scale)
{
#if defined(__x86_64__) || defined(__i386__)
    int32_t result;
    uint32_t spill;
    __asm__ volatile("flds %2\n\tfmuls %3\n\tfstps %1\n\tcvttss2si %1,%0"
                     : "=r"(result), "=m"(spill)
                     : "m"(depth), "m"(scale)
                     : "st");
    return result;
#else
    (void)depth;
    (void)scale;
    return 0;
#endif
}

/* The high dword of depth * constant, stored as a double. */
static uint32_t depth_product_high(uint32_t depth, uint64_t constant)
{
#if defined(__x86_64__) || defined(__i386__)
    uint64_t product;
    __asm__ volatile("flds %1\n\tfmull %2\n\tfstpl %0"
                     : "=m"(product)
                     : "m"(depth), "m"(constant)
                     : "st");
    return (uint32_t)(product >> 32u);
#else
    (void)depth;
    (void)constant;
    return 0u;
#endif
}

static bool float_equals(uint32_t left, uint32_t right)
{
    float a;
    float b;
    memcpy(&a, &left, sizeof(a));
    memcpy(&b, &right, sizeof(b));
    return a == b;
}

/* The clamp the depth conversions share: `cmp eax, limit; jle; mov eax, limit`, then negative to 0. */
static uint32_t clamp_depth(int32_t value, int32_t limit)
{
    if (value > limit) {
        value = limit;
    }
    return value < 0 ? 0u : (uint32_t)value;
}

/* The clear value of the depth buffer for D3DCLEAR_ZBUFFER (0x003D5F9F to 0x003D607F). The jump
 * through 0x003D6278 is unchecked in the original: a depth format past the eight entries lands in
 * padding, which this port refuses. */
static uint32_t depth_clear_value(uint32_t depth_header, uint32_t depth_bits)
{
    const uint32_t index = (uint32_t)d3d8_guest_load8(depth_header + SURFACE_FORMAT_BYTE) - 0x2Au;
    if (index > 7u) {
        d3d8_hle_fatal(ENTRY, "Clear of a depth buffer with format byte %#x is unmeasured",
                       (unsigned)(index + 0x2Au) & 0xFFu);
    }
    require_host_arithmetic();
    switch (index & 3u) {
    case 0u: /* 24-bit integer depth, shifted above the stencil byte */
        return clamp_depth(depth_scaled(depth_bits, load_guest(DEPTH_SCALE_24)), 0xFFFFFF) << 8u;
    case 2u: /* 16-bit integer depth */
        return clamp_depth(depth_scaled(depth_bits, load_guest(DEPTH_SCALE_16)), 0xFFFF);
    case 1u: /* 24-bit float depth, zero stays zero */
        if (float_equals(depth_bits, load_guest(FLOAT_ZERO))) {
            return 0u;
        }
        return ((depth_product_high(depth_bits, load_guest64(DEPTH_SCALE_F24)) + 0xF8000000u) &
                0xFFFFFFF0u)
               << 4u;
    default: /* 16-bit float depth */
        if (float_equals(depth_bits, load_guest(FLOAT_ZERO))) {
            return 0u;
        }
        return ((depth_product_high(depth_bits, load_guest64(DEPTH_SCALE_F16)) >> 8u) - 0x8000u) &
               0xFFFFu;
    }
}

/* The colour converted to the target's own bit layout (0x003D5F24 to 0x003D5F81). Class 0 packs
 * 5-5-5, class 1 packs 5-6-5 and class 2, or a format outside the table, keeps the colour as given. */
static uint32_t colour_clear_value(uint32_t format, uint32_t color)
{
    const uint32_t index = format - 3u;
    if (index > COLOUR_CLASS_ENTRIES - 1u) {
        return color;
    }
    const uint32_t class_byte = d3d8_guest_load8(COLOUR_CLASS_TABLE + index);
    if (class_byte > 2u) {
        d3d8_hle_fatal(ENTRY, "Clear colour class %u of format %#x is unmeasured",
                       (unsigned)class_byte, (unsigned)format);
    }
    if (class_byte == 2u) {
        return color;
    }
    const uint32_t shift = class_byte == 0u ? 3u : 2u;
    const uint32_t green = class_byte == 0u ? 0xF800u : 0xFC00u;
    uint32_t packed = (((color >> 3u) & 0x1F0000u) | (color & green)) >> shift;
    packed |= color & 0xF8u;
    return packed >> 3u;
}

/* --- the plan --------------------------------------------------------------------------- */

typedef struct {
    uint32_t rect_horizontal;
    uint32_t rect_vertical;
    uint32_t zstencil;
    uint32_t color;
    uint32_t flags;
} clear_packet;

typedef struct {
    bool set_format;
    uint32_t format_word;
    bool restore_format;
    uint32_t restore_word;
    uint32_t packets;
    clear_packet packet[MAX_RECTANGLES];
} clear_plan;

static bool overlaps(uint32_t a, uint32_t a_bytes, uint32_t b, uint32_t b_bytes)
{
    return a_bytes != 0u && b_bytes != 0u && (uint64_t)a < (uint64_t)b + b_bytes &&
           (uint64_t)b < (uint64_t)a + a_bytes;
}

/* Everything Clear decides, from the arguments and the guest state, before it writes a byte. */
static void plan_clear(const d3d8_clear_call *call, clear_plan *plan, uint32_t rects[][4])
{
    memset(plan, 0, sizeof(*plan));
    if (load_guest(D3D8_DEVICE_POINTER_SLOT) != D3D8_DEVICE_BASE) {
        d3d8_hle_fatal(ENTRY, "Clear requires the recovered device");
    }
    const uint32_t target = device_word(DEV_RENDER_TARGET);
    const uint32_t depth = device_word(DEV_DEPTH_STENCIL);
    if (target == 0u) {
        d3d8_hle_fatal(ENTRY, "Clear with no render target is unmeasured");
    }
    const uint32_t format = d3d8_guest_load8(target + SURFACE_FORMAT_BYTE);
    uint32_t color = call->color;
    uint32_t flags = call->flags;

    /* 0x003D5EE0: a format that wants it is rewritten for the clear (bit 9 off, bit 8 on) and put
     * back afterwards. */
    if ((d3d8_guest_load8(FORMAT_INFO_TABLE + format) & 1u) != 0u) {
        const uint32_t word = d3d8_surface_format_word(target, depth);
        plan->set_format = true;
        plan->format_word = (word & 0xFFFFFDFFu) | 0x100u;
        plan->restore_word = word;
    }
    if ((flags & 0xF0u) != 0u) {
        color = colour_clear_value(format, color);
    }
    if (depth == 0u) {
        flags &= 0xFFFFFFFCu;
        if (flags == 0u) {
            /* 0x003D5F90 returns here and does NOT restore the surface format. */
            return;
        }
    }
    plan->restore_format = plan->restore_word != 0u;
    const uint32_t depth_value = (flags & 1u) != 0u ? depth_clear_value(depth, call->depth_bits) : 0u;

    const int32_t x0 = (int32_t)device_word(DEV_VIEWPORT);
    const int32_t y0 = (int32_t)device_word(DEV_VIEWPORT + 4u);
    const int32_t x1 = (int32_t)((uint32_t)x0 + device_word(DEV_VIEWPORT + 8u));
    const int32_t y1 = (int32_t)((uint32_t)y0 + device_word(DEV_VIEWPORT + 12u));
    const uint32_t count = call->count == 0u ? 1u : call->count;
    if (count > MAX_RECTANGLES) {
        d3d8_hle_fatal(ENTRY, "Clear with %u rectangles is unmeasured (limit %u)",
                       (unsigned)call->count, (unsigned)MAX_RECTANGLES);
    }
    if (call->count == 0u) {
        rects[0][0] = (uint32_t)x0;
        rects[0][1] = (uint32_t)y0;
        rects[0][2] = (uint32_t)x1;
        rects[0][3] = (uint32_t)y1;
    } else if (call->rects == 0u || !kernel_guest_read_bytes(call->rects, rects, count * 16u)) {
        d3d8_hle_fatal(ENTRY, "Clear rectangles at %#x are unreadable", (unsigned)call->rects);
    }

    for (uint32_t index = 0u; index < count; index++) {
        const int32_t left = (int32_t)rects[index][0] > x0 ? (int32_t)rects[index][0] : x0;
        const int32_t top = (int32_t)rects[index][1] > y0 ? (int32_t)rects[index][1] : y0;
        const int32_t right = (int32_t)rects[index][2] < x1 ? (int32_t)rects[index][2] : x1;
        const int32_t bottom = (int32_t)rects[index][3] < y1 ? (int32_t)rects[index][3] : y1;
        if (left >= right || top >= bottom) {
            continue;
        }
        require_host_arithmetic();
        const uint32_t scale_x = device_word(DEV_SCALE_X);
        const uint32_t scale_y = device_word(DEV_SCALE_Y);
        const uint32_t bias = load_guest(VIEWPORT_BIAS);
        const uint32_t scaled_left = (uint32_t)viewport_scaled(left, scale_x, bias);
        const uint32_t scaled_right = (uint32_t)viewport_scaled(right, scale_x, bias);
        const uint32_t scaled_top = (uint32_t)viewport_scaled(top, scale_y, bias);
        const uint32_t scaled_bottom = (uint32_t)viewport_scaled(bottom, scale_y, bias);
        clear_packet *packet = &plan->packet[plan->packets++];
        packet->rect_horizontal = ((scaled_right << 16u) - 0x10000u) | scaled_left;
        packet->rect_vertical = ((scaled_bottom << 16u) - 0x10000u) | scaled_top;
        packet->zstencil = depth_value | call->stencil;
        packet->color = color;
        packet->flags = flags;
    }
}

/* One emitter site: the reservation preamble the original has before each packet, then the words. */
static void emit_words(const uint32_t *words, uint32_t count)
{
    const uint32_t cursor = d3d8_pushbuffer_begin();
    for (uint32_t index = 0u; index < count; index++) {
        d3d8_guest_store32(cursor + index * 4u, words[index]);
    }
    d3d8_pushbuffer_end(cursor + count * 4u);
}

/* Walk the sites over a simulated writer first, so a refusal (a refill the model does not support, a
 * span that is not mapped, a span that overlaps something Clear reads) happens before the first
 * write. Each site may roll over, so the spans are the planned ones and not one run from the cursor. */
static void plan_sites(const clear_plan *plan, uint32_t rect_pointer, uint32_t rect_count)
{
    uint32_t site_bytes[MAX_RECTANGLES + 2u];
    uint32_t sites = 0u;
    if (plan->set_format) {
        site_bytes[sites++] = SURFACE_FORMAT_BYTES;
    }
    for (uint32_t index = 0u; index < plan->packets; index++) {
        site_bytes[sites++] = CLEAR_PACKET_BYTES;
    }
    if (plan->restore_format) {
        site_bytes[sites++] = SURFACE_FORMAT_BYTES;
    }
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    for (uint32_t site = 0u; site < sites; site++) {
        const uint32_t start = d3d8_pushbuffer_sim_site(&sim, ENTRY, site_bytes[site]);
        if (overlaps(start, site_bytes[site], D3D_REGION_BASE, D3D_REGION_BYTES) ||
            overlaps(start, site_bytes[site], rect_pointer, rect_count * 16u) ||
            overlaps(start, site_bytes[site], VIEWPORT_BIAS, 4u) ||
            overlaps(start, site_bytes[site], FLOAT_ZERO, 4u) ||
            overlaps(start, site_bytes[site], DEPTH_SCALE_F24, 0x18u)) {
            d3d8_hle_fatal(ENTRY, "Clear command span %#x aliases state it reads", (unsigned)start);
        }
    }
}

void d3d8_clear(const d3d8_clear_call *call)
{
    clears++;
    last_clear = *call;
    /* D3DCLEAR_TARGET is 0xF0 on the Xbox (the four colour channels), so the colour only
     * matters when one of those bits is set. */
    if ((call->flags & 0xF0u) != 0u && call->color != 0u) {
        nonblack_clears++;
    }
    clear_plan plan;
    uint32_t rects[MAX_RECTANGLES][4];
    plan_clear(call, &plan, rects);
    plan_sites(&plan, call->rects, call->count);

    if (plan.set_format) {
        const uint32_t words[2] = {HEADER_SURFACE_FORMAT, plan.format_word};
        emit_words(words, 2u);
    }
    for (uint32_t index = 0u; index < plan.packets; index++) {
        const clear_packet *packet = &plan.packet[index];
        const uint32_t words[7] = {HEADER_CLEAR_RECT,    packet->rect_horizontal,
                                   packet->rect_vertical, HEADER_CLEAR_VALUES,
                                   packet->zstencil,      packet->color,
                                   packet->flags};
        emit_words(words, 7u);
    }
    if (plan.restore_format) {
        const uint32_t words[2] = {HEADER_SURFACE_FORMAT, plan.restore_word};
        emit_words(words, 2u);
    }
    d3d8_hle_note_unmodelled(ENTRY,
                             "Clear writes its commands into the pushbuffer but no pixels; the "
                             "surfaces stay zero (black) and the host has no GPU to run them");
}

static uint32_t handler_clear(void *context)
{
    uint32_t words[6];
    for (unsigned index = 0u; index < 6u; index++) {
        if (!kernel_frame_arg((const kernel_call_frame *)context, index, &words[index])) {
            d3d8_hle_fatal(ENTRY, "argument %u of Clear cannot be read", index);
        }
    }
    const d3d8_clear_call call = {words[0], words[1], words[2], words[3], words[4], words[5]};
    d3d8_clear(&call);
    return 0u;
}

size_t d3d8_frame_register(void)
{
    return d3d8_hle_register(ENTRY, handler_clear) ? 1u : 0u;
}
