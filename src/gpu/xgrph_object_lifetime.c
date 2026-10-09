/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "xgrph_object_lifetime.h"

#include "kernel_call.h"
#include "guest_mem.h"
#include "xgrph_hle.h"

#define COMPMASK_VTABLE_RESET 0x003EF236u
#define COMPMASK_VTABLE 0x004A1BD0u
#define SECONDARY_VTABLE_RESET 0x003EF3B7u
#define SECONDARY_VTABLE 0x004A1BD4u
#define THIRD_VTABLE_RESET 0x004029BDu
#define THIRD_VTABLE 0x004A1C70u
#define XGBUFFER_GET_BUFFER_POINTER 0x003E6714u

static bool tracked_object_range_writable(uint32_t object)
{
    const guest_region *region = guest_region_containing(object);
    if (region == NULL) {
        /* Image/other externally mapped memory is checked by the guarded host write. */
        return true;
    }
    const uint64_t end = (uint64_t)object + sizeof(uint32_t);
    if (region->state != MEM_COMMIT || end > (uint64_t)region->address + region->size ||
        (region->protect & PAGE_GUARD) != 0u) {
        return false;
    }
    switch (region->protect & PAGE_ACCESS_MASK) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

static uint32_t reset_object_vtable(void *context, uint32_t entry, uint32_t vtable, const char *label)
{
    uint32_t object = 0u;
    if (!kernel_frame_reg_arg((const kernel_call_frame *)context, 0u, &object)) {
        xgrph_hle_fatal(entry, "%s object register argument is unavailable", label);
    }
    if (!tracked_object_range_writable(object)) {
        xgrph_hle_fatal(entry,
                        "%s object vtable at %#x is not writable in the tracked guest region",
                        label, object);
    }
    if (!kernel_guest_write_u32(object, vtable)) {
        xgrph_hle_fatal(entry, "%s object vtable at %#x is not writable", label, object);
    }
    return 0u;
}

static uint32_t compmask_vtable_reset(void *context)
{
    return reset_object_vtable(context, COMPMASK_VTABLE_RESET, COMPMASK_VTABLE, "CompMask");
}

static uint32_t secondary_vtable_reset(void *context)
{
    return reset_object_vtable(context, SECONDARY_VTABLE_RESET, SECONDARY_VTABLE, "XGRPH");
}

static uint32_t third_vtable_reset(void *context)
{
    return reset_object_vtable(context, THIRD_VTABLE_RESET, THIRD_VTABLE, "XGRPH");
}

static uint32_t xg_buffer_get_buffer_pointer(void *context)
{
    uint32_t object = 0u;
    uint32_t buffer = 0u;
    if (!kernel_frame_arg((const kernel_call_frame *)context, 0u, &object)) {
        xgrph_hle_fatal(XGBUFFER_GET_BUFFER_POINTER,
                        "XGBuffer_GetBufferPointer object stack argument is unavailable");
    }
    if (!kernel_guest_read_u32((kernel_guest_ptr)(object + 4u), &buffer)) {
        xgrph_hle_fatal(XGBUFFER_GET_BUFFER_POINTER,
                        "XGBuffer_GetBufferPointer field at %#x is not readable", object + 4u);
    }
    return buffer;
}

size_t xgrph_object_lifetime_register(void)
{
    const bool compmask = xgrph_hle_register_preserving_eax(COMPMASK_VTABLE_RESET, compmask_vtable_reset);
    const bool secondary = xgrph_hle_register_preserving_eax(SECONDARY_VTABLE_RESET, secondary_vtable_reset);
    const bool third = xgrph_hle_register_preserving_eax(THIRD_VTABLE_RESET, third_vtable_reset);
    const bool get_buffer =
        xgrph_hle_register(XGBUFFER_GET_BUFFER_POINTER, xg_buffer_get_buffer_pointer);
    return (compmask ? 1u : 0u) + (secondary ? 1u : 0u) + (third ? 1u : 0u) +
           (get_buffer ? 1u : 0u);
}
