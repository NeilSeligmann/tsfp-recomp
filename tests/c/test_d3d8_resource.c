/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Buffer creation, 0x003D4EE0 (src/gpu/d3d8_resource.c). InitD3D's helper at 0x000192F0 calls it
 * three times with 0x80000 and keeps the headers in its own ring of dynamic vertex buffers.
 *
 * THE HEADER WORDS ARE FROM THE ORIGINAL: `mov [esi], 0x1000001` and `mov [esi+4], eax & 0xFFFFFFF`
 * (0x003D4F1F, 0x003D4F17), with the third dword left zero by the zero-filled heap block.
 */

#include "test_d3d8_support.h"

#include "d3d8_resource.h"
#include "d3d8_gpu.h"
#include "d3d8_gpu_pgraph.h"
#include "live_texture.h"

static void test_header_and_data(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();

    uint32_t length = 0x80000u;
    const uint32_t header = call_stdcall(0x003D4EE0u, &length, 1u);
    CHECK(header != 0u);

    /* Common 0x01000001 (one reference, a vertex buffer), Data the physical address of the
     * contiguous block, Lock zero. MUTATION: a wrong Common or an unmasked Data fails. */
    CHECK_EQ_U32(load(header), 0x01000001u);
    CHECK_EQ_U32(load(header + 8u), 0u);
    const uint32_t physical = load(header + 4u);
    CHECK(physical != 0u);
    CHECK_EQ_U32(physical & 0xF0000000u, 0u);

    /* The registry maps the physical address back to a guest address of the right size, which is
     * what a Lock handler returns instead of `Data | 0x80000000`. */
    const uint32_t data = d3d8_resource_virtual_of_physical(physical);
    CHECK(data != 0u);
    CHECK_EQ_U32(guest_physical_address(data) & 0x0FFFFFFFu, physical);
    const guest_region *region = guest_region_at(data);
    CHECK(region != NULL && region->size == 0x80000u);
    CHECK(d3d8_resource_buffer_count() == 1u);
    CHECK_EQ_U32(d3d8_resource_virtual_of_physical(physical + 0x1000u), 0u);

    /* Three calls give three distinct, non-overlapping buffers, as the title expects. */
    const uint32_t second = call_stdcall(0x003D4EE0u, &length, 1u);
    const uint32_t third = call_stdcall(0x003D4EE0u, &length, 1u);
    CHECK(second != header && third != second && third != header);
    CHECK(load(second + 4u) != physical && load(third + 4u) != load(second + 4u));
    CHECK(d3d8_resource_buffer_count() == 3u);
    environment_end();
}

static void test_failed_allocation_returns_zero(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    (void)d3d8_device_register();
    /* The data allocation fails: the original frees the header and returns NULL, and nothing is
     * registered. MUTATION: returning the header anyway hands the title a buffer with no data. */
    uint32_t length = 0xF0000000u;
    CHECK_EQ_U32(call_stdcall(0x003D4EE0u, &length, 1u), 0u);
    CHECK(d3d8_resource_buffer_count() == 0u);
    environment_end();
}

static void test_lock2_emission_and_fences(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    store(D3D8_DEVICE_POINTER_SLOT, D3D8_DEVICE_BASE);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_gpu_create());
    CHECK(d3d8_pushbuffer_create());
    const uint32_t header = d3d8_create_buffer(0x1000u);
    const uint32_t data = d3d8_resource_virtual_of_physical(load(header + 4u));
    const uint32_t cursor = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_vertex_buffer_lock2(header, 0xB0u), data);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), cursor);
    CHECK_EQ_U32(d3d8_vertex_buffer_lock2(header, 0u), data);
    CHECK_EQ_U32(load(cursor), 0x00041710u);
    CHECK_EQ_U32(load(cursor + 4u), 0u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), cursor + 8u);
    CHECK_EQ_U32(d3d8_gpu_get_stats().fence_waits, 0u);
    const uint32_t fence = d3d8_gpu_fence_insert(3u);
    store(header + 8u, fence);
    (void)d3d8_vertex_buffer_lock2(header, 0x90u);
    CHECK_EQ_U32(d3d8_gpu_get_stats().fence_waits, 0u);
    (void)d3d8_vertex_buffer_lock2(header, 0x10u);
    CHECK_EQ_U32(d3d8_gpu_get_stats().fence_waits, 1u);
    CHECK_EQ_U32(load(d3d8_device_load32(D3D8_DEV_SEMAPHORE)), fence);
    store(header, D3D8_BUFFER_COMMON | 0x00080000u);
    (void)d3d8_vertex_buffer_lock2(header, 0x10u);
    CHECK_EQ_U32(d3d8_gpu_get_stats().fence_waits, 2u);
    store(header + 4u, 0x0F123000u);
    RUN_EXPECTING_FATAL((void)d3d8_vertex_buffer_lock2(header, 0xB0u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D4F30u);
    environment_end();
}

static void test_stream_bindings(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const uint32_t header = d3d8_create_buffer(0x1000u);
    const uint32_t row = 0x003E2BA8u + 12u;
    store(D3D8_DEVICE_BASE + D3D8_DEV_FENCE, 7u);
    CHECK_EQ_U32(d3d8_set_stream_source(1u, header, 24u), 24u);
    CHECK_EQ_U32(load(row), 24u);
    CHECK_EQ_U32(load(row + 8u), header);
    CHECK_EQ_U32(load(header), 0x01080001u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0x70u);
    store(D3D8_GLOBAL_DIRTY_MASK, 0x100u);
    (void)d3d8_set_stream_source(1u, header, 24u);
    CHECK_EQ_U32(load(header), 0x01080001u);
    CHECK_EQ_U32(load(header + 8u), 7u);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0x140u);
    (void)d3d8_set_stream_source(1u, 0u, 0u);
    CHECK_EQ_U32(load(header), D3D8_BUFFER_COMMON);
    CHECK_EQ_U32(load(row + 8u), 0u);
    (void)d3d8_set_stream_source(1u, header, 24u);
    store(header, 0x01080000u); /* no external reference remains */
    RUN_EXPECTING_FATAL((void)d3d8_set_stream_source(1u, 0u, 0u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D58F0u);
    environment_end();
}

static void test_draw_vertices_commands_and_refusals(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    const uint32_t start = d3d8_device_load32(D3D8_DEV_CURSOR);
    CHECK_EQ_U32(d3d8_draw_vertices(8u, 17u, 257u), start + 28u);
    CHECK_EQ_U32(load(start), 0x000417FCu);
    CHECK_EQ_U32(load(start + 4u), 8u);
    CHECK_EQ_U32(load(start + 8u), 0x40081810u);
    CHECK_EQ_U32(load(start + 12u), 0xFF000011u);
    CHECK_EQ_U32(load(start + 16u), 273u);
    CHECK_EQ_U32(load(start + 20u), 0x000417FCu);
    CHECK_EQ_U32(load(start + 24u), 0u);
    /* T443: with no pixel shader bound the stage program is the fixed-function one, which draws. */
    store(D3D8_GLOBAL_DIRTY_MASK, 0x4000u);
    RUN_EXPECTING_FATAL((void)d3d8_draw_vertices(8u, 0u, 4u));
    CHECK(!fatal_seen);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0u);
    /* A malformed fog declaration must refuse before point emission. */
    store(D3D8_DEVICE_BASE + 0x794u, 0x003E2C68u);
    store(D3D8_DEVICE_BASE + 0x794u, 0x003FFFFBu);
    store_byte(0x003FFFFFu, 0u); /* flags readable, whole declaration truncated */
    store(0x003E3E30u, 1u);
    store(D3D8_GLOBAL_DIRTY_MASK, 0x2100u);
    const uint32_t pending = d3d8_device_load32(D3D8_DEV_CURSOR);
    store(pending, 0xCAFEBABEu);
    RUN_EXPECTING_FATAL((void)d3d8_draw_vertices(8u, 0u, 4u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003DDEA0u);
    CHECK_EQ_U32(d3d8_device_load32(D3D8_DEV_CURSOR), pending);
    CHECK_EQ_U32(load(pending), 0xCAFEBABEu);
    CHECK_EQ_U32(load(D3D8_GLOBAL_DIRTY_MASK), 0x2100u);
    store(D3D8_DEVICE_BASE + 0x794u, 0x003E2C68u);
    store(0x003E2C6Cu, 0u);
    store(0x003E3E30u, 0u);
    store(D3D8_GLOBAL_DIRTY_MASK, 0u);
    RUN_EXPECTING_FATAL((void)d3d8_draw_vertices(8u, 0u, 0u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "reservation") != NULL);
    environment_end();
}

static void test_resource_registration_types_and_preflight(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    const d3d8_surface_entry rows[2] = {{0x003D4D70u, NULL, 1u}, {0x003D4FB0u, NULL, 1u}};
    CHECK(d3d8_hle_init(rows, 2u));
    CHECK_EQ_U32(d3d8_resource_draw_register(), 2u);
    map_fixed(0x00665000u, 0x1000u);
    const uint32_t header = 0x00665504u;
    for (uint32_t type = 0u; type < 8u; type++) {
        const uint32_t common = 0xFF800001u | (type << 16u);
        store(header, common);
        store(header + 4u, UINT32_MAX);
        for (uint32_t i = 2u; i < 8u; i++) store(header + i * 4u, 0x12345678u + i);
        const uint32_t args[2] = {header, 0x80000002u};
        const uint32_t expected = type == 2u ? 0x80000001u : 1u;
        CHECK_EQ_U32(call_stdcall(0x003D4D70u, args, 2u), expected);
        CHECK_EQ_U32(load(header), common);
        CHECK_EQ_U32(load(header + 4u), expected);
        for (uint32_t i = 2u; i < 8u; i++) CHECK_EQ_U32(load(header + i * 4u), 0x12345678u + i);
    }
    store(0x00665FFCu, 0xAAAAAAAAu);
    RUN_EXPECTING_FATAL((void)d3d8_register_resource(0x00665FFCu, 0x80000000u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D4D70u);
    CHECK_EQ_U32(load(0x00665FFCu), 0xAAAAAAAAu);
    RUN_EXPECTING_FATAL((void)d3d8_register_resource(0xFFFFFFFCu, 0u));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D4D70u);
    environment_end();
}
static void test_registered_texture_reader_and_cache(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x00665000u, 0x1000u);
    map_fixed(0x0125A000u, 0x1000u); /* unrelated but readable zero low RAM */
    guest_region_request request = {.bytes=128u,.fixed_base=0x9125A000u,.contiguous=true,
                                    .state=MEM_COMMIT,.protect=PAGE_READWRITE};
    nt_status status;
    const uint32_t backing = guest_region_alloc(&request, &status);
    CHECK_EQ_U32(backing, request.fixed_base);
    const uint32_t header = 0x00665500u, second_header = header + 32u;
    const uint32_t format = 0x04410029u; /* 16x16 swizzled Y8, inferred */
    store(header,0x40001u); store(header+4u,0u); store(header+12u,format); store(header+16u,0u);
    const uint32_t data=d3d8_register_resource(header,backing);
    store(backing,0x44332211u);
    live_texture_binding binding={header,format,0u,data, 0u};
    uint8_t raw[4]={0};
    CHECK(d3d8_gpu_read_guest(NULL,data,raw,sizeof raw));
    CHECK_EQ_U32(raw[0],0u); /* old path demonstrably wrong */
    uint32_t address=0u; uint64_t identity=0u; const char *why=NULL;
    CHECK(d3d8_gpu_resolve_texture(NULL,&binding,4u,&address,&identity,&why));
    CHECK_EQ_U32(address,backing); CHECK(identity!=0u);
    CHECK(d3d8_gpu_read_virtual(NULL,address,raw,sizeof raw)); CHECK_EQ_U32(raw[0],0x11u);
    CHECK(!d3d8_gpu_resolve_texture(NULL,&binding,129u,&address,&identity,&why));
    CHECK(strstr(why,"span")!=NULL);
    live_texture_cache cache; live_texture_cache_init(&cache,true);
    live_texture_result first,changed,other;
    /* T1016: exact VA/physical/size reuse still has a different allocation
     * lifetime. The old Register sidecar must not validate the new allocation. */
    const uint32_t old_physical=guest_physical_address(backing);
    const uint64_t old_generation=guest_allocation_generation(backing);
    CHECK(guest_region_free(backing));
    request.lowest_physical=old_physical;request.highest_physical=old_physical+4095u;
    CHECK_EQ_U32(guest_region_alloc(&request,&status),backing);
    CHECK_EQ_U32(guest_physical_address(backing),old_physical);
    CHECK(guest_allocation_generation(backing)>old_generation);
    CHECK(!d3d8_gpu_resolve_texture(NULL,&binding,4u,&address,&identity,&why));
    CHECK(strstr(why,"stale")!=NULL);
    request.lowest_physical=0u;request.highest_physical=0u;
    /* Make the allocation large enough for the 256-byte texture only after refusal was proven. */
    CHECK(guest_region_free(backing));
    request.bytes=256u;
    const uint32_t recycled=guest_region_alloc(&request,&status);
    CHECK_EQ_U32(recycled,backing);
    CHECK(!d3d8_gpu_resolve_texture(NULL,&binding,4u,&address,&identity,&why));
    CHECK(strstr(why,"stale")!=NULL);
    store(header+4u,0u); (void)d3d8_register_resource(header,recycled); store(recycled,0x44332211u);
    live_texture_lookup_resolved(&cache,&binding,d3d8_gpu_read_virtual,NULL,d3d8_gpu_resolve_texture,NULL,&first);
    CHECK(first.source==LIVE_TEXTURE_SOURCE_GUEST); CHECK_EQ_U32(first.rgba[0],0x11u);
    live_texture_mark_uploaded(&cache,first.entry,first.generation);
    store(recycled,0x88776655u); /* direct actual VA write, no lock/watch notification */
    live_texture_lookup_resolved(&cache,&binding,d3d8_gpu_read_virtual,NULL,d3d8_gpu_resolve_texture,NULL,&changed);
    CHECK(changed.needs_upload && changed.generation==first.generation+1u); CHECK_EQ_U32(changed.rgba[0],0x55u);
    CHECK_EQ_U32(live_texture_note_write(&cache,recycled,1u),1u);
    request.fixed_base=0xA125A000u;
    const uint32_t another=guest_region_alloc(&request,&status);
    CHECK_EQ_U32(another,request.fixed_base);
    store(second_header,0x40001u); store(second_header+4u,0u); store(second_header+12u,format); store(second_header+16u,0u);
    CHECK_EQ_U32(d3d8_register_resource(second_header,another),data); store(another,0xDDCCBBAAu);
    binding.header=second_header;
    live_texture_lookup_resolved(&cache,&binding,d3d8_gpu_read_virtual,NULL,d3d8_gpu_resolve_texture,NULL,&other);
    CHECK(other.source==LIVE_TEXTURE_SOURCE_GUEST); CHECK_EQ_U32(other.rgba[0],0xAAu); CHECK(other.entry!=first.entry);
    binding.header=header;
    CHECK(d3d8_gpu_resolve_texture(NULL,&binding,4u,&address,&identity,&why)); CHECK_EQ_U32(address,recycled);
    const uint64_t before=identity;
    store(header+4u,0u); (void)d3d8_register_resource(header,recycled);
    CHECK(d3d8_gpu_resolve_texture(NULL,&binding,4u,&address,&identity,&why));
    CHECK(identity!=before); /* same bytes/address but fresh registration cannot share old cache identity */
    uint32_t resolved=0u; uint64_t token=0u;
    CHECK(d3d8_resource_try_alias(header,data+255u,1u,&resolved,&token,&why)==D3D8_RESOURCE_ALIAS_RESOLVED);
    CHECK_EQ_U32(resolved,recycled+255u);
    CHECK(d3d8_resource_try_alias(header,data+255u,2u,&resolved,&token,&why)==D3D8_RESOURCE_ALIAS_REFUSED);
    CHECK(d3d8_resource_try_alias(header,data-1u,1u,&resolved,&token,&why)==D3D8_RESOURCE_ALIAS_REFUSED);
    CHECK(d3d8_resource_try_alias(header,0x0FFFFFFFu,2u,&resolved,&token,&why)==D3D8_RESOURCE_ALIAS_REFUSED);
    store(header,0x50001u);
    CHECK(!d3d8_gpu_resolve_texture(NULL,&binding,4u,&address,&identity,&why));
    CHECK(strstr(why,"modified")!=NULL);
    store(header,0x40001u);
    store(header+4u,data+4u);
    live_texture_lookup_resolved(&cache,&binding,d3d8_gpu_read_virtual,NULL,d3d8_gpu_resolve_texture,NULL,&other);
    CHECK(other.source==LIVE_TEXTURE_SOURCE_REFUSED); CHECK(strstr(other.detail,"header")!=NULL);
    store(header+4u,data);
    CHECK(guest_region_free(recycled));
    live_texture_lookup_resolved(&cache,&binding,d3d8_gpu_read_virtual,NULL,d3d8_gpu_resolve_texture,NULL,&other);
    CHECK(other.source==LIVE_TEXTURE_SOURCE_REFUSED); CHECK(strstr(other.detail,"stale")!=NULL);
    live_texture_binding physical={0u,format,0u,guest_physical_address(another), 0u};
    CHECK(d3d8_gpu_resolve_texture(NULL,&physical,256u,&address,&identity,&why)); CHECK_EQ_U32(address,another);
    live_texture_result direct_first,direct_second;
    live_texture_lookup_resolved(&cache,&physical,d3d8_gpu_read_virtual,NULL,d3d8_gpu_resolve_texture,NULL,&direct_first);
    CHECK(direct_first.source==LIVE_TEXTURE_SOURCE_GUEST);
    live_texture_mark_uploaded(&cache,direct_first.entry,direct_first.generation);
    uint8_t saved[256];memcpy(saved,(const void *)(uintptr_t)another,sizeof saved);
    const uint64_t direct_identity=identity;
    request.lowest_physical=physical.data;request.highest_physical=physical.data+4095u;
    CHECK(guest_region_free(another));
    CHECK_EQ_U32(guest_region_alloc(&request,&status),another);
    memcpy((void *)(uintptr_t)another,saved,sizeof saved);
    CHECK(d3d8_gpu_resolve_texture(NULL,&physical,256u,&address,&identity,&why));
    CHECK(identity!=direct_identity && identity!=0u);
    live_texture_lookup_resolved(&cache,&physical,d3d8_gpu_read_virtual,NULL,d3d8_gpu_resolve_texture,NULL,&direct_second);
    CHECK(direct_second.source==LIVE_TEXTURE_SOURCE_GUEST && direct_second.needs_upload);
    physical.data=UINT32_MAX-2u;
    CHECK(!d3d8_gpu_resolve_texture(NULL,&physical,4u,&address,&identity,&why)); CHECK(strstr(why,"wrap")!=NULL);
    live_texture_cache_free(&cache);
    environment_end();
}

int main(void)
{
    test_registered_texture_reader_and_cache();
    test_resource_registration_types_and_preflight();
    test_header_and_data();
    test_failed_allocation_returns_zero();
    test_lock2_emission_and_fences();
    test_stream_bindings();
    test_draw_vertices_commands_and_refusals();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
