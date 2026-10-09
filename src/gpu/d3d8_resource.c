/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * See d3d8_resource.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include "d3d8_resource.h"

#include "d3d8_guest.h"
#include "d3d8_combiner.h"
#include "d3d8_dirty.h"
#include "d3d8_texture_dirty.h"
#include <float.h>
#include "d3d8_gpu.h"
#include "d3d8_hle.h"
#include "d3d8_pushbuffer.h"
#include "guest_mem.h"
#include "kernel_call.h"

#define KERNEL_ORDINAL_MM_ALLOCATE_CONTIGUOUS_EX 166u

/* The allocation wrapper's flag word for the data block is 0xB2800000: alignment 1 << 2 and
 * protect 0x404 (MEASURED from the wrapper's decode, 0x00380C57 onwards). */
#define BUFFER_ALIGNMENT 4u
#define BUFFER_PROTECT 0x404u

#define REGISTRY_CAPACITY 128u

typedef struct {
    uint32_t physical;
    uint32_t virtual_address;
    uint64_t allocation_generation;
} buffer_record;

static buffer_record registry[REGISTRY_CAPACITY];
static size_t registry_count;
static uint32_t header_heap;
/* Original Register masks a virtual allocation address into a 28-bit Data word.
 * This sidecar records that exact relationship; it is not the synthetic physical
 * address used by MmGetPhysicalAddress, and never searches possible VA aliases. */
typedef struct {
    uint32_t header, common_identity, data, format, size;
    uint32_t region_address, region_physical, region_requested, region_size;
    uint32_t virtual_data;
    uint64_t identity, allocation_generation;
    bool valid;
} resource_alias;
/* T755: the table grows (it was a fixed 128 whose overflow DROPPED the record silently, and the
 * title registers 490 front end textures before the first movie surface, so the attract movie's
 * own Register was lost and its LockRect stopped as "no verified registered allocation alias").
 * A header is never dropped for want of room: the table doubles to ALIAS_LIMIT, and at the limit a
 * record that can no longer verify (stale stack header, freed allocation) is reused, else the
 * Register stops by name. */
#define ALIAS_INITIAL_CAPACITY 128u
#define ALIAS_LIMIT 16384u
static resource_alias *aliases;
static size_t alias_count;
static size_t alias_capacity;
static uint64_t alias_identity;
static pthread_mutex_t alias_lock = PTHREAD_MUTEX_INITIALIZER;

typedef enum {
    ALIAS_REJECTION_NONE,
    ALIAS_REJECTION_NO_CONTIGUOUS_REGION,
    ALIAS_REJECTION_REGION_TOO_LARGE,
    ALIAS_REJECTION_DATA_OUTSIDE_REGION,
    ALIAS_REJECTION_VIRTUAL_RETAINED,
    ALIAS_REJECTION_COPY_UNVERIFIED
} alias_rejection_reason;

typedef struct {
    uint32_t header;
    uint32_t base;
    uint32_t offset;
    alias_rejection_reason reason;
} alias_rejection;

/* Diagnostics are bounded like the alias table. If every row has been used, a row is
 * replaced cyclically; the next Register clears the header's previous rejection. */
static alias_rejection rejected_aliases[ALIAS_LIMIT];
static size_t rejection_count;
static size_t rejection_next;
#define IMMUTABLE_COMMON_MASK 0xFF870000u /* all bits except low16 refs and 0x780000 busy bits */


void d3d8_resource_reset(void)
{
    registry_count = 0u;
    pthread_mutex_lock(&alias_lock);
    alias_count = 0u;
    header_heap = 0u;
    memset(rejected_aliases, 0, sizeof(rejected_aliases));
    rejection_count = 0u;
    rejection_next = 0u;
    pthread_mutex_unlock(&alias_lock);
}

size_t d3d8_resource_buffer_count(void)
{
    return registry_count;
}

uint32_t d3d8_resource_virtual_of_physical(uint32_t physical)
{
    for (size_t index = 0u; index < registry_count; index++) {
        if (registry[index].physical == physical &&
            guest_allocation_generation(registry[index].virtual_address) == registry[index].allocation_generation) {
            return registry[index].virtual_address;
        }
    }
    return 0u;
}

uint32_t d3d8_create_buffer(uint32_t length)
{
    if (header_heap == 0u || !guest_heap_valid(header_heap)) {
        header_heap = guest_heap_create(0u, GUEST_HEAP_CHUNK_MIN, 0u);
        if (header_heap == 0u) {
            return 0u;
        }
    }
    const uint32_t header = guest_heap_alloc(header_heap, D3D8_BUFFER_HEADER_BYTES);
    if (header == 0u) {
        return 0u;
    }

    const uint32_t args[5] = {length, 0u, 0xFFFFFFFFu, BUFFER_ALIGNMENT, BUFFER_PROTECT};
    const uint32_t data = d3d8_kernel_call(KERNEL_ORDINAL_MM_ALLOCATE_CONTIGUOUS_EX, args, 5u);
    if (data == 0u) {
        (void)guest_heap_free(header_heap, header);
        return 0u;
    }

    if (registry_count >= REGISTRY_CAPACITY) {
        d3d8_hle_fatal(0x003D4EE0u,
                       "more than %u live buffers: the (physical, virtual) registry is full",
                       (unsigned)REGISTRY_CAPACITY);
    }
    const uint32_t physical = guest_physical_address(data) & 0x0FFFFFFFu;
    registry[registry_count].physical = physical;
    registry[registry_count].virtual_address = data;
    registry[registry_count].allocation_generation = guest_allocation_generation(data);
    registry_count++;

    d3d8_guest_store32(header, D3D8_BUFFER_COMMON);
    d3d8_guest_store32(header + 4u, physical);
    d3d8_guest_store32(header + 8u, 0u);
    return header;
}

uint32_t d3d8_vertex_buffer_lock2(uint32_t header, uint32_t flags)
{
    const uint32_t data = d3d8_guest_load32(header + 4u);
    const uint32_t virtual_address = d3d8_resource_virtual_of_physical(data);
    if (virtual_address == 0u) {
        d3d8_hle_fatal(0x003D4F30u,
                       "buffer %#x has unregistered physical Data %#x; Lock2 cannot map it",
                       (unsigned)header, (unsigned)data);
    }
    /* 0x003D4F35..5D: the cache-invalidation method is emitted unless bit 0x10 is set. */
    if ((flags & 0x10u) == 0u) {
        d3d8_pushbuffer_emit_pair(0x00041710u, 0u);
    }
    /* 0x003D6C20: resource Common bits select the device fence; otherwise Lock is the fence.
     * Route both through the shared instantaneous consumer rather than discard synchronization. */
    if ((flags & 0xA0u) == 0u && d3d8_guest_load32(D3D8_DEVICE_POINTER_SLOT) != 0u) {
        const uint32_t common = d3d8_guest_load32(header);
        const uint32_t lock = d3d8_guest_load32(header + 8u);
        if ((common & 0x00780000u) != 0u) {
            d3d8_gpu_fence_wait(d3d8_device_load32(D3D8_DEV_FENCE), 2u);
        } else if (lock != 0u) {
            d3d8_gpu_fence_wait(lock, 0x10u);
        }
    }
    return virtual_address;
}

uint32_t d3d8_set_stream_source(uint32_t stream, uint32_t header, uint32_t stride)
{
    const uint32_t table = 0x003E2BA8u;
    if (stream > (UINT32_MAX - table - 8u) / 12u) {
        d3d8_hle_fatal(0x003D58F0u, "stream index %#x overflows the guest table", (unsigned)stream);
    }
    const uint32_t row = table + stream * 12u;
    if (header != 0u) {
        d3d8_guest_store32(header, d3d8_guest_load32(header) + 0x00080000u);
    }
    const uint32_t old = d3d8_guest_load32(row + 8u);
    if (old != 0u) {
        d3d8_guest_store32(old + 8u, d3d8_device_load32(D3D8_DEV_FENCE));
        const uint32_t common = d3d8_guest_load32(old) - 0x00080000u;
        d3d8_guest_store32(old, common);
        if ((common & 0x0078FFFFu) == 0u) {
            d3d8_hle_fatal(0x003D58F0u,
                           "stream release of buffer %#x needs unimplemented resource destruction",
                           (unsigned)old);
        }
    }
    const uint32_t old_stride = d3d8_guest_load32(row);
    d3d8_guest_store32(row, stride);
    d3d8_guest_store32(row + 8u, header);
    const uint32_t dirty = d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK);
    d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK, dirty | (stride == old_stride ? 0x40u : 0x70u));
    return stride; /* eax at both measured returns; the public function is void. */
}

/* Plan the recovered 0x003DED80 family before any deferred-state writes. Each emitter has its own
 * reservation preamble, so a roll-over can fall between two of them (and between texture
 * stages): the plan walks a simulated writer through the same sites in the original order. */
static void plan_draw_dirty(uint32_t dirty, d3d8_pushbuffer_sim *sim)
{
    if ((dirty & ~0xC0FF7F7Fu) != 0u)
        d3d8_hle_fatal(0x003D4FB0u, "unsupported draw dirty mask %#x", (unsigned)dirty);
    if ((dirty & 0x100u) != 0u) d3d8_plan_point_state(sim);
    /* 0x003E11D0: with a bound pixel shader it returns the mask without writes, otherwise it is the
     * fixed-function selection (T443) and may add 0x400F. */
    if ((dirty & 0x800u) != 0u && d3d8_device_load32(0x784u) == 0u)
        dirty = d3d8_plan_fixed_function_combiner(dirty, sim);
    if ((dirty & 0x4000u) != 0u) d3d8_plan_shader_stage_program(sim);
    if ((dirty & 0xFu) != 0u) d3d8_plan_texture_stages(dirty, sim);
    if ((dirty & 0x2000u) != 0u) d3d8_plan_fog(sim);
    if ((dirty & 0x400u) != 0u) d3d8_plan_texture_transforms(sim);
    if ((dirty & 0xFF1000u) != 0u) d3d8_plan_default_lighting(dirty, sim);
    if ((dirty & 0x200u) != 0u) d3d8_plan_fixed_function_matrices(dirty, sim);
}

static void emit_draw_dirty(uint32_t dirty)
{
    if ((dirty & 0x100u) != 0u) (void)d3d8_emit_point_state();
    if ((dirty & 0x800u) != 0u && d3d8_device_load32(0x784u) == 0u)
        dirty = d3d8_emit_fixed_function_combiner(dirty);
    if ((dirty & 0x4000u) != 0u) (void)d3d8_emit_shader_stage_program();
    if ((dirty & 0xFu) != 0u) (void)d3d8_emit_texture_stages(dirty);
    if ((dirty & 0x2000u) != 0u) (void)d3d8_emit_disabled_fog();
    /* 0x003DE080 and 0x003DEB80 return early for a vertex shader or the programmable mode. */
    if ((dirty & 0x400u) != 0u) d3d8_emit_texture_transforms();
    if ((dirty & 0xFF1000u) != 0u) (void)d3d8_emit_default_lighting_state(dirty);
    if ((dirty & 0x200u) != 0u) d3d8_emit_fixed_function_matrices(dirty);
    d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK, dirty & 0xC0000070u);
}

/* 0x003DED80: the controller Begin and the swap composition call directly. It reads the mask as it
 * stands (DrawVertices clears 0x50 first, through 0x003DEE00, and calls the cascade with the rest). */
void d3d8_run_dirty_cascade(void)
{
    const uint32_t dirty = d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK);
    if ((dirty & 0x3FFFFF8Fu) != 0u) {
        d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
        plan_draw_dirty(dirty, &sim);
    }
    emit_draw_dirty(dirty);
}

/* The stream work of 0x003DEE00 after its cascade: whether it runs, and the bytes it writes. The
 * descriptor and header spans are validated here so a truncated declaration refuses before writes. */
static bool stream_work_bytes(uint32_t dirty, uint32_t base_vertex, uint32_t entry, uint32_t *bytes_out)
{
    *bytes_out = 0u;
    if ((dirty & 0x40000000u) != 0u ||
        ((dirty & 0x40u) == 0u && d3d8_device_load32(0x20u) == base_vertex)) return false;
    const uint32_t declaration = d3d8_device_load32(0x794u);
    const uint32_t mapping = 0x005496A0u + (d3d8_guest_load32(declaration + 4u) & 0x10u);
    uint32_t bytes = (dirty & 0x10u) != 0u ? 68u : 0u;
    for (uint32_t attribute = 0u; attribute < 16u; attribute++) {
        const uint32_t slot = d3d8_guest_load8(mapping + attribute);
        const uint64_t descriptor64 = (uint64_t)declaration + slot * 16u;
        if (descriptor64 + 32u > UINT64_C(0x100000000) || kernel_guest_at((uint32_t)descriptor64, 32u) == NULL)
            d3d8_hle_fatal(entry, "stream declaration is truncated");
        const uint32_t descriptor = (uint32_t)descriptor64;
        const uint32_t stream = d3d8_guest_load32(descriptor + 0x14u);
        if (stream >= 16u) d3d8_hle_fatal(entry, "stream index exceeds guest table");
        const uint32_t row = 0x003E2BA8u + stream * 12u;
        if (d3d8_guest_load32(descriptor + 0x1Cu) == 2u) continue;
        const uint32_t buffer = d3d8_guest_load32(row + 8u);
        if (buffer == 0u) continue;
        if ((uint64_t)buffer + 8u > UINT64_C(0x100000000) || kernel_guest_at(buffer, 8u) == NULL)
            d3d8_hle_fatal(entry, "stream buffer header is truncated");
        bytes += 8u;
    }
    *bytes_out = bytes;
    return true;
}

/* 0x003DEE00, called by DrawVertices with its argument zero. */
static void flush_draw_streams(uint32_t base_vertex)
{
    /* The original keeps this saved mask for the stream work even after the
     * cascade clears the global. 0x3DEE1C clears 0x50 BEFORE the cascade reads it. */
    const uint32_t dirty = d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK);
    const bool cascade = (dirty & 0x3FFFFF8Fu) != 0u;
    if (cascade) {
        d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
        plan_draw_dirty(dirty, &sim);
    }
    d3d8_guest_store32(D3D8_GLOBAL_DIRTY_MASK, dirty & 0xFFFFFFAFu);
    if (cascade) emit_draw_dirty(dirty & 0xFFFFFFAFu);
    if ((dirty & 0x40000000u) != 0u ||
        ((dirty & 0x40u) == 0u && d3d8_device_load32(0x20u) == base_vertex)) return;
    const uint32_t declaration = d3d8_device_load32(0x794u);
    const uint32_t mapping = 0x005496A0u + (d3d8_guest_load32(declaration + 4u) & 0x10u);
    d3d8_device_store32(0x20u, base_vertex);
    uint32_t cursor = d3d8_pushbuffer_begin();
    if ((dirty & 0x10u) != 0u) {
        d3d8_guest_store32(cursor, 0x00401760u);
        cursor += 4u;
        for (uint32_t attribute = 0u; attribute < 16u; attribute++) {
            const uint32_t slot = (uint32_t)d3d8_guest_load8(mapping + attribute);
            const uint32_t descriptor = declaration + slot * 16u;
            const uint32_t stream = d3d8_guest_load32(descriptor + 0x14u);
            const uint32_t stride = d3d8_guest_load32(0x003E2BA8u + stream * 12u);
            d3d8_guest_store32(cursor, (stride << 8) + d3d8_guest_load32(descriptor + 0x1Cu));
            cursor += 4u;
        }
    }
    for (uint32_t attribute = 0u; attribute < 16u; attribute++) {
        const uint32_t slot = (uint32_t)d3d8_guest_load8(mapping + attribute);
        const uint32_t descriptor = declaration + slot * 16u;
        if (d3d8_guest_load32(descriptor + 0x1Cu) == 2u) continue;
        const uint32_t stream = d3d8_guest_load32(descriptor + 0x14u);
        const uint32_t row = 0x003E2BA8u + stream * 12u;
        const uint32_t buffer = d3d8_guest_load32(row + 8u);
        if (buffer == 0u) continue;
        const uint32_t address = d3d8_guest_load32(row) * base_vertex + d3d8_guest_load32(buffer + 4u) +
                                 d3d8_guest_load32(descriptor + 0x18u) +
                                 d3d8_guest_load32(row + 4u);
        d3d8_guest_store32(cursor, 0x00041720u + attribute * 4u);
        d3d8_guest_store32(cursor + 4u, address);
        cursor += 8u;
    }
    d3d8_pushbuffer_end(cursor);
}

/* The deferred work of 0x003DEE00, the cascade and then the stream work, planned over the caller's simulation (T546).
 * Each emitter's own preamble may refill, so a caller lays what follows out from `sim->cursor`, not from the entry
 * cursor plus a byte count (the indexed draw used to refuse every planned refill for that reason). */
void d3d8_draw_plan_deferred(d3d8_pushbuffer_sim *sim, uint32_t base_vertex)
{
    const uint32_t dirty = d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK);
    if ((dirty & 0x3FFFFF8Fu) != 0u) plan_draw_dirty(dirty, sim);
    uint32_t stream_bytes;
    if (stream_work_bytes(dirty, base_vertex, 0x003D5050u, &stream_bytes))
        d3d8_pushbuffer_sim_site(sim, 0x003D5050u, stream_bytes);
}
void d3d8_draw_flush_streams(uint32_t base_vertex) { flush_draw_streams(base_vertex); }

/* Plan a whole DrawVertices before its first write: the deferred-state cascade, the stream work and
 * the draw packet's sized reservation (0x003D6B30) over one simulated writer. */
static void plan_draw_vertices(uint32_t chunks)
{
    const uint32_t dirty = d3d8_guest_load32(D3D8_GLOBAL_DIRTY_MASK);
    d3d8_pushbuffer_sim sim = d3d8_pushbuffer_sim_start();
    if ((dirty & 0x3FFFFF8Fu) != 0u) plan_draw_dirty(dirty, &sim);
    uint32_t stream_bytes;
    if (stream_work_bytes(dirty, 0u, 0x003D4FB0u, &stream_bytes))
        d3d8_pushbuffer_sim_site(&sim, 0x003D4FB0u, stream_bytes);
    const uint32_t dwords = chunks + 5u;
    (void)d3d8_pushbuffer_sim_reserve(&sim, dwords);
    if (kernel_guest_at(sim.cursor, dwords * 4u) == NULL)
        d3d8_hle_fatal(0x003D4FB0u, "draw packet span %#x+%u is not mapped guest memory",
                       (unsigned)sim.cursor, (unsigned)(dwords * 4u));
}

uint32_t d3d8_draw_vertices(uint32_t primitive, uint32_t first, uint32_t count)
{
    if (count == 0u) {
        d3d8_hle_fatal(0x003D4FB0u,
                       "zero-count DrawVertices underflows the original chunk reservation; "
                       "that malformed path is not implemented");
    }
    const uint32_t chunks = ((count - 1u) >> 8) + 1u;
    plan_draw_vertices(chunks);
    flush_draw_streams(0u);
    /* 0x003D4FD1: the sized reservation, which may roll the ring over. */
    uint32_t cursor = d3d8_pushbuffer_reserve(chunks + 5u);
    d3d8_guest_store32(cursor, 0x000417FCu);
    d3d8_guest_store32(cursor + 4u, primitive);
    d3d8_guest_store32(cursor + 8u, (chunks << 18) + 0x40001810u);
    cursor += 12u;
    uint32_t remaining = count;
    for (uint32_t chunk = 0u; chunk < chunks; chunk++) {
        const uint32_t vertices = remaining > 256u ? 256u : remaining;
        d3d8_guest_store32(cursor, ((vertices - 1u) << 24) | first);
        cursor += 4u;
        first += 256u;
        remaining -= vertices;
    }
    d3d8_guest_store32(cursor, 0x000417FCu);
    d3d8_guest_store32(cursor + 4u, 0u);
    cursor += 8u;
    d3d8_pushbuffer_end(cursor);
    return cursor;
}

static uint32_t handler_draw_vertices(void *context)
{
    uint32_t arguments[3];
    for (unsigned i = 0u; i < 3u; i++) {
        if (!kernel_frame_arg((const kernel_call_frame *)context, i, &arguments[i])) {
            d3d8_hle_fatal(0x003D4FB0u, "DrawVertices argument %u cannot be read", i);
        }
    }
    return d3d8_draw_vertices(arguments[0], arguments[1], arguments[2]);
}

typedef enum {
    ALIAS_OK,
    ALIAS_HEADER_UNREADABLE,
    ALIAS_BINDING_MODIFIED,
    ALIAS_ALLOCATION_STALE
} alias_state;

/* The one verification of a recorded binding, shared by the resolver and by the reuse of a dead
 * record: the header still carries the identity it was registered with and the allocation it
 * named is still the live contiguous region it was. */
static alias_state alias_verify(const resource_alias *record)
{
    uint32_t words[5];
    if (kernel_guest_at(record->header, 20u) == NULL ||
        !kernel_guest_read_bytes(record->header, words, sizeof(words)))
        return ALIAS_HEADER_UNREADABLE;
    if ((words[0] & IMMUTABLE_COMMON_MASK) != record->common_identity ||
        words[1] != record->data || words[3] != record->format || words[4] != record->size)
        return ALIAS_BINDING_MODIFIED;
    const guest_region *region = guest_region_at(record->region_address);
    if (region == NULL || !region->contiguous || region->state != MEM_COMMIT ||
        region->physical != record->region_physical || region->generation != record->allocation_generation ||
        region->size != record->region_size ||
        region->requested != record->region_requested)
        return ALIAS_ALLOCATION_STALE;
    return ALIAS_OK;
}

/* A slot for a new record. Never drops: grow, then reuse a record that cannot verify any more,
 * then stop by name. */
static resource_alias *alias_new_slot(bool fatal)
{
    if (alias_count == alias_capacity && alias_capacity < ALIAS_LIMIT) {
        const size_t grown = alias_capacity == 0u ? ALIAS_INITIAL_CAPACITY : alias_capacity * 2u;
        resource_alias *table = realloc(aliases, grown * sizeof(*aliases));
        if (table == NULL) {
            if (!fatal) return NULL;
            pthread_mutex_unlock(&alias_lock);
            d3d8_hle_fatal(0x003D4D70u, "cannot grow the registered allocation alias table to %zu", grown);
        }
        aliases = table;
        alias_capacity = grown;
    }
    if (alias_count < alias_capacity) return &aliases[alias_count++];
    for (size_t i = 0u; i < alias_count; i++) {
        if (!aliases[i].valid || alias_verify(&aliases[i]) != ALIAS_OK) return &aliases[i];
    }
    if (!fatal) return NULL;
    pthread_mutex_unlock(&alias_lock);
    d3d8_hle_fatal(0x003D4D70u,
                   "registered allocation alias table is full: %u resources still verify, none can be reused",
                   (unsigned)ALIAS_LIMIT);
    return NULL;
}

static alias_rejection *rejection_find(uint32_t header)
{
    for (size_t i = 0u; i < ALIAS_LIMIT; i++) {
        if (rejected_aliases[i].reason != ALIAS_REJECTION_NONE &&
            rejected_aliases[i].header == header)
            return &rejected_aliases[i];
    }
    return NULL;
}

static void rejection_clear(uint32_t header)
{
    alias_rejection *rejection = rejection_find(header);
    if (rejection != NULL) {
        memset(rejection, 0, sizeof(*rejection));
        rejection_count--;
    }
}

static void rejection_record(uint32_t header, uint32_t base, uint32_t offset,
                             alias_rejection_reason reason)
{
    alias_rejection *rejection = rejection_find(header);
    if (rejection == NULL) {
        if (rejection_count < ALIAS_LIMIT) {
            for (size_t i = 0u; i < ALIAS_LIMIT; i++) {
                if (rejected_aliases[i].reason == ALIAS_REJECTION_NONE) {
                    rejection = &rejected_aliases[i];
                    rejection_count++;
                    break;
                }
            }
        } else {
            rejection = &rejected_aliases[rejection_next];
            rejection_next = (rejection_next + 1u) % ALIAS_LIMIT;
        }
    }
    if (rejection != NULL)
        *rejection = (alias_rejection){.header = header, .base = base, .offset = offset,
                                       .reason = reason};
}

static const char *rejection_description(alias_rejection_reason reason)
{
    switch (reason) {
    case ALIAS_REJECTION_NO_CONTIGUOUS_REGION:
        return "base has no committed contiguous region";
    case ALIAS_REJECTION_REGION_TOO_LARGE:
        return "region is too large for the 28-bit alias";
    case ALIAS_REJECTION_DATA_OUTSIDE_REGION:
        return "Data offset is outside the region";
    case ALIAS_REJECTION_VIRTUAL_RETAINED:
        return "resource is virtual retained and has no physical alias";
    case ALIAS_REJECTION_COPY_UNVERIFIED:
        return "resource header copy has no verified complete live source";
    case ALIAS_REJECTION_NONE: break;
    }
    return "Register recorded no alias";
}

static void record_registered_alias_unlocked(uint32_t header, uint32_t base, uint32_t offset,
                                    uint32_t common, uint32_t data)
{
    resource_alias *record = NULL;
    for (size_t i = 0u; i < alias_count; i++) {
        if (aliases[i].header == header) { record = &aliases[i]; break; }
    }
    if (record != NULL) record->valid = false; /* Every Register invalidates the prior binding. */
    if (rejection_count != 0u) rejection_clear(header);
    const guest_region *region = guest_region_containing(base);
    if (region == NULL || !region->contiguous || region->state != MEM_COMMIT) {
        const alias_rejection_reason reason = ALIAS_REJECTION_NO_CONTIGUOUS_REGION;
        rejection_record(header, base, offset, reason);
        return;
    }
    if (region->requested >= 0x10000000u) {
        const alias_rejection_reason reason = ALIAS_REJECTION_REGION_TOO_LARGE;
        rejection_record(header, base, offset, reason);
        return;
    }
    if (kernel_guest_at(header, 20u) == NULL) return;
    if ((common & 0x70000u) == 0x20000u) {
        const alias_rejection_reason reason = ALIAS_REJECTION_VIRTUAL_RETAINED;
        rejection_record(header, base, offset, reason);
        return;
    }
    const uint64_t virtual_data = (uint64_t)base + offset;
    const uint64_t end = (uint64_t)region->address + region->requested;
    if (virtual_data < region->address || virtual_data >= end || virtual_data > UINT32_MAX) {
        const alias_rejection_reason reason = ALIAS_REJECTION_DATA_OUTSIDE_REGION;
        rejection_record(header, base, offset, reason);
        return;
    }
    if (record == NULL) record = alias_new_slot(true);
    record->header = header;
    record->common_identity = common & IMMUTABLE_COMMON_MASK;
    record->data = data;
    record->format = d3d8_guest_load32(header + 12u);
    record->size = d3d8_guest_load32(header + 16u);
    record->region_address = region->address;
    record->region_physical = region->physical;
    record->allocation_generation = region->generation;
    record->region_requested = region->requested;
    record->region_size = region->size;
    record->virtual_data = (uint32_t)virtual_data;
    record->identity = ++alias_identity;
    record->valid = true;
}

static void record_registered_alias(uint32_t header, uint32_t base, uint32_t offset,
                                    uint32_t common, uint32_t data)
{
    pthread_mutex_lock(&alias_lock);
    record_registered_alias_unlocked(header, base, offset, common, data);
    pthread_mutex_unlock(&alias_lock);
}

typedef struct {
    resource_alias record;
    uint32_t expected[5];
    bool verified;
    uint32_t source_header;
} copied_alias;

typedef struct {
    size_t count;
    copied_alias entries[];
} alias_copy;

static void *resource_copy_begin_unlocked(uint32_t destination, uint32_t source, uint32_t bytes)
{
    if (bytes == 0u || (uint64_t)source + bytes > UINT32_MAX ||
        (uint64_t)destination + bytes > UINT32_MAX ||
        kernel_guest_at(source, bytes) == NULL || kernel_guest_at(destination, bytes) == NULL)
        return NULL;
    size_t count = 0u;
    for (size_t i = 0u; i < alias_count; i++) {
        const resource_alias *record = &aliases[i];
        if ((uint64_t)record->header < (uint64_t)source + bytes &&
            (uint64_t)record->header + 20u > source) count++;
    }
    if (count == 0u) return NULL;
    alias_copy *copy = malloc(sizeof(*copy) + count * sizeof(copy->entries[0]));
    if (copy == NULL) return NULL;
    copy->count = 0u;
    for (size_t i = 0u; i < alias_count; i++) {
        const resource_alias *record = &aliases[i];
        if ((uint64_t)record->header >= (uint64_t)source + bytes ||
            (uint64_t)record->header + 20u <= source) continue;
        const int64_t destination_header = (int64_t)destination + record->header - source;
        if (destination_header <= 0 || destination_header + 20 > UINT32_MAX) continue;
        copied_alias *entry = &copy->entries[copy->count];
        if (!kernel_guest_read_bytes(record->header, entry->expected, sizeof(entry->expected))) continue;
        entry->record = *record;
        entry->source_header = record->header;
        entry->record.header = (uint32_t)destination_header;
        entry->verified = record->valid && record->header >= source &&
            (uint64_t)record->header + 20u <= (uint64_t)source + bytes && alias_verify(record) == ALIAS_OK;
        copy->count++;
    }
    return copy;
}

static size_t resource_copy_end_unlocked(void *transaction)
{
    alias_copy *copy = transaction;
    if (copy == NULL) return 0u;
    size_t transferred = 0u;
    for (size_t i = 0u; i < copy->count; i++) {
        const copied_alias *entry = &copy->entries[i];
        uint32_t actual[5];
        if (!entry->verified || !kernel_guest_read_bytes(entry->record.header, actual, sizeof(actual)) ||
            memcmp(actual, entry->expected, sizeof(actual)) != 0 || alias_verify(&entry->record) != ALIAS_OK) {
            rejection_record(entry->record.header, 0u, 0u, ALIAS_REJECTION_COPY_UNVERIFIED);
            continue;
        }
        resource_alias *target = NULL;
        for (size_t j = 0u; j < alias_count; j++) {
            if (aliases[j].header == entry->record.header) { target = &aliases[j]; break; }
        }
        if (target == NULL) target = alias_new_slot(false);
        if (target == NULL) {
            rejection_record(entry->record.header, 0u, 0u, ALIAS_REJECTION_COPY_UNVERIFIED);
            continue;
        }
        *target = entry->record;
        target->identity = ++alias_identity;
        rejection_clear(target->header);
        transferred++;
        static unsigned traced;
        if (traced < 32u && getenv("TSFP_RESOURCE_COPY_TRACE") != NULL) {
            fprintf(stderr, "RESOURCE_COPY source=%08x destination=%08x Data=%08x Format=%08x VA=%08x identity=%llu\n",
                    entry->source_header, target->header, target->data, target->format, target->virtual_data,
                    (unsigned long long)target->identity);
            traced++;
        }
    }
    free(copy);
    return transferred;
}

void *d3d8_resource_copy_begin(uint32_t destination, uint32_t source, uint32_t bytes)
{
    pthread_mutex_lock(&alias_lock);
    void *copy = resource_copy_begin_unlocked(destination, source, bytes);
    pthread_mutex_unlock(&alias_lock);
    return copy;
}

size_t d3d8_resource_copy_end(void *transaction)
{
    if (transaction == NULL) return 0u;
    pthread_mutex_lock(&alias_lock);
    const size_t transferred = resource_copy_end_unlocked(transaction);
    pthread_mutex_unlock(&alias_lock);
    return transferred;
}

size_t d3d8_resource_alias_count(void)
{
    size_t live = 0u;
    pthread_mutex_lock(&alias_lock);
    for (size_t i = 0u; i < alias_count; i++) live += aliases[i].valid ? 1u : 0u;
    pthread_mutex_unlock(&alias_lock);
    return live;
}

static d3d8_resource_alias_result resource_try_alias_unlocked(uint32_t header, uint32_t data, uint32_t bytes,
                                                  uint32_t *address, uint64_t *identity,
                                                  const char **refusal)
{
    const resource_alias *record = NULL;
    for (size_t i = 0u; i < alias_count; i++) {
        if (aliases[i].header != header) continue;
        if (record != NULL) {
            *refusal = "ambiguous registered resource header";
            return D3D8_RESOURCE_ALIAS_REFUSED;
        }
        record = &aliases[i];
    }
    if (record == NULL) {
        const alias_rejection *rejection = rejection_find(header);
        if (rejection == NULL) return D3D8_RESOURCE_ALIAS_ABSENT;
        *refusal = rejection_description(rejection->reason);
        return D3D8_RESOURCE_ALIAS_REFUSED;
    }
    if (!record->valid) {
        *refusal = "registered resource binding is retired";
        return D3D8_RESOURCE_ALIAS_REFUSED;
    }
    switch (alias_verify(record)) {
    case ALIAS_HEADER_UNREADABLE: *refusal = "registered resource header is unreadable"; break;
    case ALIAS_BINDING_MODIFIED: *refusal = "registered resource binding was modified"; break;
    case ALIAS_ALLOCATION_STALE: *refusal = "registered resource allocation is stale"; break;
    case ALIAS_OK: {
        const uint64_t delta = (uint64_t)data - record->data;
        const uint64_t at = (uint64_t)record->virtual_data + delta;
        const uint64_t end = (uint64_t)record->region_address + record->region_requested;
        if (data > 0x0FFFFFFFu || data < record->data || bytes == 0u ||
            (uint64_t)data + bytes > 0x10000000u || at + bytes > end ||
            at + bytes > UINT32_MAX || kernel_guest_at((uint32_t)at, bytes) == NULL) {
            *refusal = "registered resource span exceeds its requested allocation";
            break;
        }
        *address = (uint32_t)at;
        *identity = record->identity;
        return D3D8_RESOURCE_ALIAS_RESOLVED;
    }
    }
    return D3D8_RESOURCE_ALIAS_REFUSED;
}

d3d8_resource_alias_result d3d8_resource_try_alias(uint32_t header, uint32_t data, uint32_t bytes,
                                                  uint32_t *address, uint64_t *identity,
                                                  const char **refusal)
{
    pthread_mutex_lock(&alias_lock);
    const d3d8_resource_alias_result result = resource_try_alias_unlocked(header,data,bytes,address,identity,refusal);
    pthread_mutex_unlock(&alias_lock);
    return result;
}

uint32_t d3d8_resource_resolve_registered_alias_at(uint32_t entry, uint32_t header,
                                                    uint32_t data, uint32_t bytes)
{
    resource_alias snapshot;
    const resource_alias *record = NULL;
    alias_rejection rejected = {0};
    pthread_mutex_lock(&alias_lock);
    for (size_t i = 0u; i < alias_count; i++) {
        if (aliases[i].header == header && aliases[i].valid) { snapshot=aliases[i]; record=&snapshot; break; }
    }
    const alias_rejection *found = rejection_find(header);
    if (found != NULL) rejected=*found;
    pthread_mutex_unlock(&alias_lock);
    if (record == NULL || kernel_guest_at(header, 20u) == NULL) {
        const alias_rejection *rejection = rejected.reason != ALIAS_REJECTION_NONE ? &rejected : NULL;
        if (rejection != NULL && kernel_guest_at(header, 20u) != NULL)
            d3d8_hle_fatal(entry,
                           "resource %#x was registered at base %#x but no alias was recorded: %s (Data offset %#x)",
                           header, rejection->base, rejection_description(rejection->reason),
                           rejection->offset);
        d3d8_hle_fatal(entry, "resource %#x has no verified registered allocation alias", header);
    }
    switch (alias_verify(record)) {
    case ALIAS_OK: break;
    case ALIAS_HEADER_UNREADABLE:
        d3d8_hle_fatal(entry, "registered resource header is unreadable");
        break;
    case ALIAS_BINDING_MODIFIED:
        d3d8_hle_fatal(entry, "registered resource %#x binding was modified", header);
        break;
    case ALIAS_ALLOCATION_STALE:
        d3d8_hle_fatal(entry, "registered resource %#x allocation is stale", header);
        break;
    }
    const guest_region *region = guest_region_at(record->region_address);
    if (data > 0x0FFFFFFFu)
        d3d8_hle_fatal(entry, "surface Data %#x is outside the 28-bit alias domain", data);
    const uint32_t delta = (data - record->data) & 0x0FFFFFFFu;
    const uint64_t address = (uint64_t)record->virtual_data + delta;
    const uint64_t end = (uint64_t)region->address + region->requested;
    if (bytes == 0u || address + bytes > end || address + bytes > UINT32_MAX ||
        kernel_guest_at((uint32_t)address, bytes) == NULL)
        d3d8_hle_fatal(entry, "surface alias span exceeds its requested contiguous allocation");
    return (uint32_t)address;
}

uint32_t d3d8_resource_virtual_of_registered_data(uint32_t data)
{
    uint32_t address=0u;
    pthread_mutex_lock(&alias_lock);
    for (size_t i = 0u; i < alias_count; i++) {
        if (aliases[i].valid && aliases[i].data == data) { address=aliases[i].virtual_data; break; }
    }
    pthread_mutex_unlock(&alias_lock);
    return address;
}

uint32_t d3d8_resource_resolve_registered_alias(uint32_t header, uint32_t data, uint32_t bytes)
{
    return d3d8_resource_resolve_registered_alias_at(0x003D4AC0u, header, data, bytes);
}

uint32_t d3d8_register_resource(uint32_t header, uint32_t base)
{
    /* Validate the complete two-word input/output span before any mutation,
     * including a header that straddles an unmapped page or wraps the address. */
    if (kernel_guest_at(header, 8u) == NULL)
        d3d8_hle_fatal(0x003D4D70u, "resource header %#x is not mapped for 8 bytes", header);
    const uint32_t offset = d3d8_guest_load32(header + 4u);
    const uint32_t common = d3d8_guest_load32(header);
    uint32_t data = offset + base;
    if ((common & 0x70000u) != 0x20000u) data &= 0x0FFFFFFFu;
    /* The binding is recorded BEFORE Data is relocated: a table that cannot take it stops here with
     * the header untouched (T755). */
    record_registered_alias(header, base, offset, common, data);
    d3d8_guest_store32(header + 4u, data);
    return data;
}

static uint32_t handler_register_resource(void *context)
{
    uint32_t arguments[2];
    for (unsigned i = 0u; i < 2u; i++) {
        if (!kernel_frame_arg((const kernel_call_frame *)context, i, &arguments[i]))
            d3d8_hle_fatal(0x003D4D70u, "resource registration argument %u is unreadable", i);
    }
    return d3d8_register_resource(arguments[0], arguments[1]);
}

size_t d3d8_resource_draw_register(void)
{
    return (size_t)d3d8_hle_register(0x003D4FB0u, handler_draw_vertices) +
           (size_t)d3d8_hle_register(0x003D4D70u, handler_register_resource);
}
