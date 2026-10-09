/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_overlay.h. Every function carries the address of the original it ports.
 */

#include "d3d8_overlay.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d8_device.h"
#include "d3d8_guest.h"
#include "d3d8_hle.h"
#include "d3d8_overlay_image.h"
#include "d3d8_overlay_key.h"
#include "d3d8_surface.h"
#include "kernel_call.h"

/* Device fields. */
#define DEV_REGISTER_BASE 0x0934u
#define DEV_OVERLAY_SURFACE 0x1C1Cu
#define DEV_FIELD_COUNTER 0x1DE8u
#define DEV_OVERLAY_COUNTER 0x2410u

/* The registers the three functions touch, as offsets from the window base. */
#define REG_BUFFER_CONTROL 0x8100u
#define REG_STATE_CONTROL 0x8140u
#define REG_PENDING 0x8700u
#define REG_PENDING_KICK 0x8704u
#define REG_ZERO_A 0x8900u
#define REG_UPDATE_LIMIT 0x8908u
#define REG_ENABLE_B 0x8910u
#define REG_ENABLE_C 0x8918u
#define REG_SOURCE_START 0x8920u
#define REG_SOURCE_SIZE 0x8928u
#define REG_SOURCE_PHASE 0x8930u
#define REG_HORIZONTAL_STEP 0x8938u
#define REG_VERTICAL_STEP 0x8940u
#define REG_DESTINATION_ORIGIN 0x8948u
#define REG_DESTINATION_SIZE 0x8950u
#define REG_CONTROL 0x8958u
#define REG_COLOR_KEY 0x8B00u

#define SHADOW_DWORDS ((D3D8_OVERLAY_LAST_REGISTER - D3D8_OVERLAY_FIRST_REGISTER) / 4u + 1u)
#define UNIT_STEP 0x100000u
#define DATA_MASK 0x0FFFFFFFu
#define PITCH_BIT 0x10000u
#define KEY_BIT 0x100000u
#define HEADER_BYTES 0x18u
#define RECT_BYTES 16u

static uint32_t shadow[SHADOW_DWORDS];
static d3d8_overlay_write write_log[D3D8_OVERLAY_WRITE_LOG_CAPACITY];
static d3d8_overlay_descriptor descriptor;
static bool consume_policy;
static uint64_t consumed_buffers;

/* The image dump (T537). */
#define DUMP_MAX_DIMENSION 1920u
#define DUMP_PATH_BYTES 512u
static const char *dump_directory;
static uint32_t dump_maximum;
static d3d8_overlay_data_resolver dump_resolver;
/* T760: the present hook, one call per UpdateOverlay, one per modelled vblank. */
static d3d8_overlay_picture_hook present_picture_hook;
static d3d8_overlay_vblank_hook present_vblank_hook;
static void *present_context;
static d3d8_overlay_data_resolver present_resolver;
static bool dump_index_started;
static uint64_t dump_written;
static uint64_t dump_failed;
static uint8_t *dump_yuy2; /* the source rows as the guest holds them */
static uint8_t *dump_source_rgb;
static uint8_t *dump_destination_rgb;
static uint8_t *dump_planes; /* Y, U, V of the source rectangle, one after the other */
/* T831, both default off: the xemu-level picture (matrix, bilinear scale, box one pixel larger) and the key
 * composition's own resolver. The crop holds the declared-size picture the present hook gets from the box. */
static bool xemu_image;
static d3d8_overlay_data_resolver key_resolver;
static uint8_t *dump_crop_rgb;

void d3d8_overlay_reset(void)
{
    memset(shadow, 0, sizeof(shadow));
    memset(write_log, 0, sizeof(write_log));
    dump_index_started = false;
    dump_written = 0u;
    dump_failed = 0u;
    d3d8_overlay_key_reset();
    memset(&descriptor, 0, sizeof(descriptor));
    consumed_buffers = 0u;
}

void d3d8_overlay_set_consume_policy(bool enabled)
{
    consume_policy = enabled;
}

void d3d8_overlay_consume_vblank(void)
{
    if (present_vblank_hook != NULL) {
        present_vblank_hook(present_context);
    }
    const size_t pending_index = (REG_PENDING - D3D8_OVERLAY_FIRST_REGISTER) / 4u;
    if (!consume_policy || shadow[pending_index] == 0u) {
        return;
    }
    /* T540: inferred scanout timing, opt-in. The port has a completed modeled vblank, but no
     * hardware capture that establishes the NV2A overlay's actual latch/retirement phase. */
    shadow[pending_index] = 0u;
    consumed_buffers++;
}

uint64_t d3d8_overlay_consumed_count(void)
{
    return consumed_buffers;
}

d3d8_overlay_descriptor d3d8_overlay_state(void)
{
    return descriptor;
}

static bool in_range(uint32_t offset)
{
    return offset >= D3D8_OVERLAY_FIRST_REGISTER && offset <= D3D8_OVERLAY_LAST_REGISTER &&
           (offset & 3u) == 0u;
}

bool d3d8_overlay_register_value(uint32_t offset, uint32_t *value)
{
    if (!in_range(offset) || value == NULL) {
        return false;
    }
    *value = shadow[(offset - D3D8_OVERLAY_FIRST_REGISTER) / 4u];
    return true;
}

bool d3d8_overlay_hardware_write(uint32_t offset, uint32_t value)
{
    if (!in_range(offset)) {
        return false;
    }
    shadow[(offset - D3D8_OVERLAY_FIRST_REGISTER) / 4u] = value;
    return true;
}

size_t d3d8_overlay_write_log(d3d8_overlay_write *out, size_t capacity)
{
    const uint64_t total = descriptor.write_count;
    const size_t held = total < D3D8_OVERLAY_WRITE_LOG_CAPACITY ? (size_t)total
                                                                : D3D8_OVERLAY_WRITE_LOG_CAPACITY;
    const size_t copied = held < capacity ? held : capacity;
    if (out == NULL) {
        return 0u;
    }
    for (size_t index = 0u; index < copied; index++) {
        out[index] = write_log[(total - held + index) % D3D8_OVERLAY_WRITE_LOG_CAPACITY];
    }
    return copied;
}

/* One register write exactly as the original issues it. */
static void register_write(uint32_t offset, uint32_t value)
{
    if (!in_range(offset)) {
        d3d8_hle_fatal(D3D8_OVERLAY_UPDATE, "internal: write to overlay register %#x is outside "
                                            "the shadowed range", (unsigned)offset);
    }
    write_log[descriptor.write_count % D3D8_OVERLAY_WRITE_LOG_CAPACITY] =
        (d3d8_overlay_write){offset, value};
    descriptor.write_count++;
    shadow[(offset - D3D8_OVERLAY_FIRST_REGISTER) / 4u] = value;
}

static uint32_t register_read(uint32_t offset)
{
    return shadow[(offset - D3D8_OVERLAY_FIRST_REGISTER) / 4u];
}

/* The window base every function loads from device+0x934. Only the literal CreateDevice stores
 * is modelled, any other base would be a window this port cannot shadow. */
static uint32_t window_base(uint32_t entry)
{
    const uint32_t base = d3d8_device_load32(DEV_REGISTER_BASE);
    if (base != D3D8_NV2A_REGISTER_BASE) {
        d3d8_hle_fatal(entry, "the register window base at device+0x934 is %#x, not the "
                              "0xFD000000 CreateDevice stores, and only that window is shadowed",
                       (unsigned)base);
    }
    return base;
}

/* --- 0x003D97F0 ------------------------------------------------------------------------- */

uint32_t d3d8_overlay_update_status(void)
{
    return d3d8_device_load32(DEV_OVERLAY_COUNTER) != d3d8_device_load32(DEV_FIELD_COUNTER) ? 1u
                                                                                           : 0u;
}

/* --- 0x003D9990 ------------------------------------------------------------------------- */

uint32_t d3d8_overlay_enable(void)
{
    const uint32_t base = window_base(D3D8_OVERLAY_ENABLE);
    const uint32_t pending = register_read(REG_PENDING);
    if (pending != 0u) {
        d3d8_hle_fatal(D3D8_OVERLAY_ENABLE,
                       "EnableOverlay would spin on register 0x8700 = %#x; the opt-in T540 "
                       "policy only consumes at a completed modeled vblank, and that blank has "
                       "not arrived. Hardware wait scheduling is not modeled, so it refuses and "
                       "writes nothing",
                       (unsigned)pending);
    }
    descriptor.enables++;
    register_write(REG_BUFFER_CONTROL, 0x11u);
    register_write(REG_SOURCE_START, 0u);
    register_write(REG_SOURCE_SIZE, 0xFFFFFFFFu);
    register_write(REG_SOURCE_PHASE, 0u);
    register_write(REG_ZERO_A, 0u);
    register_write(REG_ENABLE_B, 0x1000u);
    register_write(REG_ENABLE_C, 0x1000u);
    d3d8_overlay_key_disable(); /* T839, xemu-level: the overlay is not displayed after these writes, whichever argument */
    return base;
}

/* --- the image dump (T537) -------------------------------------------------------------- */

bool d3d8_overlay_set_dump(const char *directory, uint32_t maximum,
                           d3d8_overlay_data_resolver resolver)
{
    free(dump_yuy2);
    free(dump_source_rgb);
    free(dump_destination_rgb);
    free(dump_planes);
    free(dump_crop_rgb);
    dump_yuy2 = dump_source_rgb = dump_destination_rgb = dump_planes = dump_crop_rgb = NULL;
    dump_directory = NULL;
    dump_maximum = maximum;
    dump_resolver = resolver;
    dump_index_started = false;
    dump_written = 0u;
    dump_failed = 0u;
    if (directory == NULL && present_picture_hook == NULL && !d3d8_overlay_key_enabled()) {
        return true;
    }
    const size_t pixels = (size_t)DUMP_MAX_DIMENSION * DUMP_MAX_DIMENSION;
    dump_yuy2 = malloc(pixels * 2u);
    dump_source_rgb = malloc(pixels * 3u);
    /* the xemu box is one pixel wider and taller than the declared rectangle */
    dump_destination_rgb = malloc((DUMP_MAX_DIMENSION + 1u) * (DUMP_MAX_DIMENSION + 1u) * 3u);
    dump_planes = malloc(pixels * 2u);
    if (dump_yuy2 == NULL || dump_source_rgb == NULL || dump_destination_rgb == NULL ||
        dump_planes == NULL) {
        free(dump_yuy2);
        free(dump_source_rgb);
        free(dump_destination_rgb);
        free(dump_planes);
        dump_yuy2 = dump_source_rgb = dump_destination_rgb = dump_planes = NULL;
        return false;
    }
    dump_directory = directory;
    return true;
}

bool d3d8_overlay_set_present_hook(d3d8_overlay_picture_hook picture_hook,
                                   d3d8_overlay_vblank_hook vblank_hook, void *context,
                                   d3d8_overlay_data_resolver resolver)
{
    present_picture_hook = picture_hook;
    present_vblank_hook = vblank_hook;
    present_context = context;
    if (picture_hook == NULL) {
        return true;
    }
    present_resolver = resolver;
    if (dump_yuy2 != NULL) {
        return true; /* the dump already allocated the picture buffers */
    }
    return d3d8_overlay_set_dump(NULL, dump_maximum, dump_resolver) && dump_yuy2 != NULL;
}

void d3d8_overlay_set_xemu_image(bool enabled)
{
    xemu_image = enabled;
}

bool d3d8_overlay_xemu_image(void)
{
    return xemu_image;
}

bool d3d8_overlay_set_key_composition(bool enabled, d3d8_overlay_data_resolver resolver)
{
    d3d8_overlay_key_set_enabled(enabled);
    key_resolver = resolver;
    if (!enabled || dump_yuy2 != NULL) {
        return true; /* nothing to build, or the dump or the hook already allocated the picture buffers */
    }
    return d3d8_overlay_set_dump(NULL, dump_maximum, dump_resolver) && dump_yuy2 != NULL;
}

uint64_t d3d8_overlay_dump_written(void)
{
    return dump_written;
}

uint64_t d3d8_overlay_dump_failed(void)
{
    return dump_failed;
}

/* T760: tell the present hook about update `number`, a NULL picture when it could not be built. */
static void present_notify(uint64_t number, const uint8_t *rgb, uint32_t width, uint32_t height)
{
    if (present_picture_hook != NULL) {
        const d3d8_overlay_picture picture = {number, rgb != NULL ? width : 0u,
                                              rgb != NULL ? height : 0u, rgb};
        present_picture_hook(&picture, present_context);
    }
}

static void dump_fail(uint64_t number, const char *reason)
{
    dump_failed++;
    fprintf(stderr, "overlay-dump: update %llu not written: %s\n", (unsigned long long)number,
            reason);
}

/* Write the picture of the update just recorded in `descriptor`. Reads guest memory only. */
static void dump_update(void)
{
    const uint64_t number = descriptor.updates - 1u;
    const bool write_files =
        dump_directory != NULL && (dump_maximum == 0u || number < dump_maximum);
    if (!write_files && present_picture_hook == NULL && !d3d8_overlay_key_enabled()) {
        return;
    }
    d3d8_overlay_key_drop(); /* a picture that cannot be built must not leave the previous layer displayed */
    const uint32_t left = descriptor.source[0] & 0xFFFFFFFEu; /* the driver rounds both down */
    const uint32_t top = descriptor.source[1] & 0xFFFFFFFEu;
    const uint32_t width = descriptor.source[2] - descriptor.source[0];
    const uint32_t height = descriptor.source[3] - descriptor.source[1];
    const uint32_t destination_width = descriptor.destination[2] - descriptor.destination[0];
    const uint32_t destination_height = descriptor.destination[3] - descriptor.destination[1];
    const uint32_t pitch = descriptor.surface_pitch;
    if (width == 0u || height == 0u || width > DUMP_MAX_DIMENSION || height > DUMP_MAX_DIMENSION ||
        destination_width == 0u || destination_height == 0u ||
        destination_width > DUMP_MAX_DIMENSION || destination_height > DUMP_MAX_DIMENSION) {
        present_notify(number, NULL, 0u, 0u);
        dump_fail(number, "a rectangle is empty or larger than the dump buffers");
        return;
    }
    const size_t row_bytes = d3d8_overlay_image_yuy2_row_bytes(width);
    if (pitch < row_bytes) {
        present_notify(number, NULL, 0u, 0u);
        dump_fail(number, "the pitch is smaller than one source row");
        return;
    }
    d3d8_overlay_data_resolver resolver = dump_directory != NULL ? dump_resolver : key_resolver;
    if (dump_directory == NULL && present_picture_hook != NULL) {
        resolver = present_resolver; /* the hook's resolver wins over the key composition's */
    }
    const uint32_t base = resolver != NULL ? resolver(descriptor.surface_data) : 0u;
    if (base == 0u) {
        present_notify(number, NULL, 0u, 0u);
        dump_fail(number, "the surface Data word is not a registered allocation");
        return;
    }
    for (uint32_t row = 0u; row < height; row++) {
        const uint32_t address = base + (top + row) * pitch + left * 2u;
        if (!kernel_guest_read_bytes(address, dump_yuy2 + (size_t)row * row_bytes, row_bytes)) {
            present_notify(number, NULL, 0u, 0u);
            dump_fail(number, "the source rows are not readable guest memory");
            return;
        }
    }
    const uint32_t chroma_width = (width + 1u) / 2u;
    uint8_t *const plane_y = dump_planes;
    uint8_t *const plane_u = plane_y + (size_t)width * height;
    uint8_t *const plane_v = plane_u + (size_t)chroma_width * height;
    char path[DUMP_PATH_BYTES];
    /* The picture: the XMV library matrix and the nearest scale by the title's steps (default), or with
     * the T831 opt-in the xemu matrix and the xemu bilinear scale, whose box is one pixel larger. */
    const uint32_t picture_width = xemu_image ? destination_width + 1u : destination_width;
    const uint32_t picture_height = xemu_image ? destination_height + 1u : destination_height;
    bool good = (xemu_image ? d3d8_overlay_image_yuy2_to_rgb_xemu(dump_yuy2, row_bytes, width,
                                                                  height, dump_source_rgb)
                            : d3d8_overlay_image_yuy2_to_rgb(dump_yuy2, row_bytes, width, height,
                                                             dump_source_rgb)) &&
                d3d8_overlay_image_yuy2_to_planes(dump_yuy2, row_bytes, width, height, plane_y,
                                                  plane_u, plane_v) &&
                (xemu_image ? d3d8_overlay_image_scale_xemu(dump_source_rgb, width, height,
                                                            dump_destination_rgb, destination_width,
                                                            destination_height)
                            : d3d8_overlay_image_scale(dump_source_rgb, width, height,
                                                       descriptor.horizontal_step,
                                                       descriptor.vertical_step, dump_destination_rgb,
                                                       destination_width, destination_height));
    if (good && d3d8_overlay_key_enabled()) {
        const d3d8_overlay_key_layer layer = {dump_destination_rgb, picture_width, picture_height,
                                              descriptor.destination[0], descriptor.destination[1],
                                              descriptor.control, descriptor.color_key};
        if (!d3d8_overlay_key_latch(&layer)) {
            dump_fail(number, "the overlay key layer could not be stored");
        }
    }
    if (good && xemu_image && present_picture_hook != NULL) {
        /* the sink gets the declared rectangle, the box's extra row and column are off screen when the
         * overlay fills it (they stay in the dump file and the key layer) */
        if (dump_crop_rgb == NULL) {
            dump_crop_rgb = malloc((size_t)DUMP_MAX_DIMENSION * DUMP_MAX_DIMENSION * 3u);
        }
        if (dump_crop_rgb == NULL) {
            present_notify(number, NULL, 0u, 0u);
            dump_fail(number, "the present crop buffer could not be allocated");
        } else {
            for (uint32_t row = 0u; row < destination_height; row++) {
                memcpy(dump_crop_rgb + (size_t)row * destination_width * 3u,
                       dump_destination_rgb + (size_t)row * picture_width * 3u,
                       (size_t)destination_width * 3u);
            }
            present_notify(number, dump_crop_rgb, destination_width, destination_height);
        }
    } else if (good) {
        present_notify(number, dump_destination_rgb, destination_width, destination_height);
    } else {
        present_notify(number, NULL, 0u, 0u);
    }
    if (!write_files) {
        return;
    }
    if (good) {
        snprintf(path, sizeof(path), "%s/overlay_%05u.png", dump_directory, (unsigned)number);
        good = d3d8_overlay_image_write_png(path, dump_destination_rgb, picture_width,
                                            picture_height);
    }
    if (good) {
        snprintf(path, sizeof(path), "%s/overlay_%05u.yuv422p", dump_directory, (unsigned)number);
        FILE *planes = fopen(path, "wb");
        const size_t total = (size_t)width * height + 2u * (size_t)chroma_width * height;
        good = planes != NULL && fwrite(dump_planes, 1u, total, planes) == total;
        if (planes != NULL) {
            good = fclose(planes) == 0 && good;
        }
    }
    if (good) {
        snprintf(path, sizeof(path), "%s/overlay_index.txt", dump_directory);
        FILE *index = fopen(path, dump_index_started ? "ab" : "wb");
        if (index != NULL) {
            dump_index_started = true;
            good = fprintf(index,
                           "update %llu surface %#x data %#x pitch %u source %u,%u,%u,%u "
                           "destination %u,%u,%u,%u steps %#x,%#x key %u,%#x control %#x planes %ux%u "
                           "matrix %s\n",
                           (unsigned long long)number, (unsigned)descriptor.surface,
                           (unsigned)descriptor.surface_data, (unsigned)pitch,
                           (unsigned)descriptor.source[0], (unsigned)descriptor.source[1],
                           (unsigned)descriptor.source[2], (unsigned)descriptor.source[3],
                           (unsigned)descriptor.destination[0], (unsigned)descriptor.destination[1],
                           (unsigned)descriptor.destination[2], (unsigned)descriptor.destination[3],
                           (unsigned)descriptor.horizontal_step, (unsigned)descriptor.vertical_step,
                           (unsigned)descriptor.color_key_enable, (unsigned)descriptor.color_key,
                           (unsigned)descriptor.control, (unsigned)width, (unsigned)height,
                           xemu_image ? "xemu-level(bt601-studio-298-409-100-208-516,bilinear,box+1)"
                                      : "UNMEASURED-for-overlay(xmv-library-bt601-studio)") > 0;
            good = fclose(index) == 0 && good;
        } else {
            good = false;
        }
    }
    if (good) {
        dump_written++;
    } else {
        dump_fail(number, "an image or index file could not be written");
    }
}

/* --- 0x003D9810 ------------------------------------------------------------------------- */

static void read_input(uint32_t address, const char *what, void *out, size_t bytes)
{
    if (address == 0u) {
        d3d8_hle_fatal(D3D8_OVERLAY_UPDATE,
                       "UpdateOverlay was given a null %s pointer, which the original "
                       "dereferences and would fault on", what);
    }
    if (!kernel_guest_read_bytes(address, out, bytes)) {
        d3d8_hle_fatal(D3D8_OVERLAY_UPDATE,
                       "UpdateOverlay %s at %#x is not readable guest memory, which the original "
                       "dereferences and would fault on",
                       what, (unsigned)address);
    }
}

/* The unscaled step is 1.0 in 12.20 fixed point, otherwise ((source - 1) << 20) / (destination
 * - 1) with the unsigned 32-bit division the original uses. */
static uint32_t scale_step(uint32_t source, uint32_t destination)
{
    if (destination <= 1u) {
        return UNIT_STEP;
    }
    return ((source - 1u) << 20) / (destination - 1u);
}

uint32_t d3d8_overlay_update(uint32_t surface, uint32_t source, uint32_t destination,
                             uint32_t color_key_enable, uint32_t color_key)
{
    /* Every input is checked before the first write, so a refusal leaves everything untouched. */
    (void)window_base(D3D8_OVERLAY_UPDATE);
    uint32_t header[HEADER_BYTES / 4u];
    uint32_t from[4];
    uint32_t to[4];
    read_input(surface, "surface header", header, sizeof(header));
    read_input(source, "source rectangle", from, RECT_BYTES);
    read_input(destination, "destination rectangle", to, RECT_BYTES);

    const uint32_t counter = d3d8_device_load32(DEV_FIELD_COUNTER);
    d3d8_device_store32(DEV_OVERLAY_SURFACE, surface);
    d3d8_device_store32(DEV_OVERLAY_COUNTER, counter);

    const uint32_t source_width = from[2] - from[0];
    const uint32_t source_height = from[3] - from[1];
    const uint32_t destination_width = to[2] - to[0];
    const uint32_t destination_height = to[3] - to[1];

    register_write(REG_PENDING_KICK, 0u);
    register_write(REG_COLOR_KEY, color_key);
    const uint32_t pitch = d3d8_surface_header_pitch(surface);
    const uint32_t start = (from[1] & 0xFFFFFFFEu) * pitch + (header[1] & DATA_MASK) +
                           (from[0] & 0xFFFFFFFEu) * 2u;
    const uint32_t phase = (start & 0x3Fu) << 3;
    const uint32_t size_phase = ((source_height << 16) | source_width) + (phase >> 4);
    register_write(REG_SOURCE_START, start & 0xFFFFFFC0u);
    register_write(REG_SOURCE_PHASE, phase);
    register_write(REG_SOURCE_SIZE, size_phase);
    const uint32_t horizontal = scale_step(source_width, destination_width);
    register_write(REG_HORIZONTAL_STEP, horizontal);
    const uint32_t vertical = scale_step(source_height, destination_height);
    register_write(REG_VERTICAL_STEP, vertical);
    const uint32_t origin = (to[1] << 16) | to[0];
    register_write(REG_DESTINATION_ORIGIN, origin);
    const uint32_t size = (destination_height << 16) | destination_width;
    register_write(REG_DESTINATION_SIZE, size);
    const uint32_t control = pitch | PITCH_BIT | (color_key_enable != 0u ? KEY_BIT : 0u);
    register_write(REG_CONTROL, control);
    register_write(REG_UPDATE_LIMIT, 0x07FFFFFFu);
    /* A read-modify-write of a register the hardware owns. */
    const uint32_t state = register_read(REG_STATE_CONTROL) | 1u;
    register_write(REG_STATE_CONTROL, state);
    register_write(REG_PENDING, 1u);

    descriptor.updates++;
    descriptor.surface = surface;
    descriptor.counter = counter;
    memcpy(descriptor.source, from, sizeof(from));
    memcpy(descriptor.destination, to, sizeof(to));
    descriptor.color_key_enable = color_key_enable;
    descriptor.color_key = color_key;
    descriptor.surface_data = header[1];
    descriptor.surface_pitch = pitch;
    descriptor.source_start = start & 0xFFFFFFC0u;
    descriptor.source_phase = phase;
    descriptor.source_size_phase = size_phase;
    descriptor.horizontal_step = horizontal;
    descriptor.vertical_step = vertical;
    descriptor.destination_origin = origin;
    descriptor.destination_size = size;
    descriptor.control = control;
    descriptor.result = state;
    dump_update();
    return state;
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

static uint32_t handler_enable(void *context)
{
    (void)context; /* the one stack argument is never read, the thunk pops it */
    return d3d8_overlay_enable();
}

static uint32_t handler_update(void *context)
{
    const uint32_t surface = argument(context, 0u, D3D8_OVERLAY_UPDATE);
    const uint32_t source = argument(context, 1u, D3D8_OVERLAY_UPDATE);
    const uint32_t destination = argument(context, 2u, D3D8_OVERLAY_UPDATE);
    const uint32_t enable = argument(context, 3u, D3D8_OVERLAY_UPDATE);
    const uint32_t key = argument(context, 4u, D3D8_OVERLAY_UPDATE);
    return d3d8_overlay_update(surface, source, destination, enable, key);
}

static uint32_t handler_status(void *context)
{
    (void)context;
    return d3d8_overlay_update_status();
}

size_t d3d8_overlay_register(void)
{
    static const struct {
        uint32_t address;
        d3d8_fn handler;
    } handlers[] = {
        {0x003D9990u, handler_enable},
        {0x003D9810u, handler_update},
        {0x003D97F0u, handler_status},
    };
    d3d8_overlay_reset();
    size_t registered = 0u;
    for (size_t index = 0u; index < sizeof(handlers) / sizeof(handlers[0]); index++) {
        if (d3d8_hle_register(handlers[index].address, handlers[index].handler)) {
            registered++;
        }
    }
    return registered;
}
