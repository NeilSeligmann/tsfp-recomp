/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_lock.h"
#include "live_texture_watch.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */
#define PARENT 0x00665504u
#define SURFACE 0x00665604u
static uint32_t backing;
static void fill_headers(uint32_t offset, uint32_t format)
{
    const uint32_t parent[6] = {0x40001u,offset,0u,format,0u,0u};
    for (uint32_t i=0u;i<6u;i++) store(PARENT+i*4u,parent[i]);
}
static void initialise(uint32_t requested, uint32_t interior, uint32_t offset, uint32_t format)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x00665000u,0x1000u);
    store_byte(0x3E1828u+7u,0xA1u);
    guest_region_request request = {.bytes=requested,.fixed_base=0x9125A000u,.contiguous=true,
                                    .state=MEM_COMMIT,.protect=PAGE_READWRITE};
    nt_status status;
    backing=guest_region_alloc(&request,&status);
    CHECK_EQ_U32(backing,0x9125A000u);
    fill_headers(offset,format);
    (void)d3d8_register_resource(PARENT,backing+interior);
    const uint32_t surface[6] = {0x01050001u,load(PARENT+4u),0u,format,0u,PARENT};
    for(uint32_t i=0u;i<6u;i++) store(SURFACE+i*4u,surface[i]);
    store(PARENT,0x40002u); /* The real surface AddRef changes only refs. */
    store(0x3E3F58u,D3D8_DEVICE_BASE);
    for(uint32_t i=0u;i<4u;i++) store(SCRATCH_DATA+i*4u,0xAABBCCDDu);
}
static void check_refusal(uint32_t rect,uint32_t flags)
{
    for(uint32_t i=0u;i<4u;i++) store(SCRATCH_DATA+i*4u,0xAABBCCDDu);
    RUN_EXPECTING_FATAL((void)d3d8_surface_lock_rect(SURFACE,SCRATCH_DATA,rect,flags));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address,0x003D4AC0u);
    CHECK_EQ_U32(load(SCRATCH_DATA),0xAABBCCDDu);
    CHECK_EQ_U32(load(SCRATCH_DATA+4u),0xAABBCCDDu);
    CHECK_EQ_U32(load(SCRATCH_DATA+8u),0xAABBCCDDu);
}
static void test_live_alias_face_and_rectangle(void)
{
    initialise(0x18000u,0u,0u,0x0661072Du);
    const d3d8_surface_entry row={0x003D4AC0u,NULL,1u};
    CHECK(d3d8_hle_init(&row,1u));
    CHECK_EQ_U32(d3d8_lock_register(),1u);
    const uint32_t args[4]={SURFACE,SCRATCH_DATA,0u,0u};
    live_texture_watch_reset();
    CHECK_EQ_U32(call_stdcall(0x003D4AC0u,args,4u),SCRATCH_DATA);
    { /* T792: the lock tells the live texture cache which Data range the title is about to write */
        uint32_t noted_address=0u,noted_bytes=0u,unused=0u;
        CHECK(live_texture_watch_pending(0u,&noted_address,&noted_bytes));
        CHECK(!live_texture_watch_pending(1u,&unused,&unused));
        CHECK_EQ_U32(noted_address,load(SURFACE+4u)&0x0FFFFFFFu);
        CHECK_EQ_U32(noted_bytes,0x4000u);
    }
    CHECK_EQ_U32(load(SCRATCH_DATA),256u);
    CHECK_EQ_U32(load(SCRATCH_DATA+4u),backing);
    CHECK(load(SCRATCH_DATA+4u)!=(load(SURFACE+4u)|0x80000000u));
    CHECK(guest_physical_address(backing)!=load(SURFACE+4u));
    CHECK_EQ_U32(load(SCRATCH_DATA+8u),0xAABBCCDDu);
    store(SURFACE+8u,99u); /* Parent Lock is the active fence source. */
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE,SCRATCH_DATA,0u,0u),SCRATCH_DATA);
    store(SURFACE+8u,0u);
    store(load(SCRATCH_DATA+4u),0x12345678u);
    CHECK_EQ_U32(load(backing),0x12345678u);
    store(SURFACE+4u,load(PARENT+4u)+5u*0x4000u);
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE,SCRATCH_DATA,0u,0u),SCRATCH_DATA);
    CHECK_EQ_U32(load(SCRATCH_DATA+4u),backing+5u*0x4000u);
    store(SCRATCH_DATA+64u,1u);store(SCRATCH_DATA+68u,1u);
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE,SCRATCH_DATA,SCRATCH_DATA+64u,0u),256u);
    CHECK_EQ_U32(load(SCRATCH_DATA+4u),backing+5u*0x4000u+260u);
    environment_end();
}
static void test_alias_binding_mutation_and_rebind(void)
{
    initialise(0x18000u,0u,0u,0x0661072Du);
    store(SURFACE,0x01050000u);check_refusal(0u,0u);store(SURFACE,0x01050001u);
    store(PARENT,0x40000u);check_refusal(0u,0u);store(PARENT,0x40002u);
    store(PARENT+12u,0x0551072Du);check_refusal(0u,0u);store(PARENT+12u,0x0661072Du);
    store(PARENT+16u,1u);check_refusal(0u,0u);store(PARENT+16u,0u);
    store(PARENT+4u,load(PARENT+4u)+4u);check_refusal(0u,0u);store(PARENT+4u,backing&0x0FFFFFFFu);
    store(PARENT,0x50002u);check_refusal(0u,0u);store(PARENT,0x40002u);
    /* Busy/ref changes are not immutable identity, but the wait gate still applies. */
    store(PARENT,0xC0002u);check_refusal(0u,0u);
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE,SCRATCH_DATA,0u,0x20u),SCRATCH_DATA);
    store(PARENT,0x40002u);store(PARENT+8u,7u);check_refusal(0u,0u);
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE,SCRATCH_DATA,0u,0x20u),SCRATCH_DATA);
    check_refusal(0u,0x80u); /* Unlike Lock2, flag80 does not skip the surface wait. */
    store(0x3E3F58u,0u);
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE,SCRATCH_DATA,0u,0u),SCRATCH_DATA);
    store(0x3E3F58u,D3D8_DEVICE_BASE);
    store(PARENT+8u,0u);
    /* A Register against an unknown base invalidates the previous verified alias. */
    store(PARENT+4u,0u);(void)d3d8_register_resource(PARENT,0x8125A000u);check_refusal(0u,0u);
    store(PARENT+4u,0u);(void)d3d8_register_resource(PARENT,backing);
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE,SCRATCH_DATA,0u,0u),SCRATCH_DATA);
    const uint32_t old_physical=guest_physical_address(backing);
    CHECK(guest_region_free(backing));check_refusal(0u,0u);
    guest_region_request request={.bytes=0x18000u,.fixed_base=backing,.contiguous=true,
                                 .state=MEM_COMMIT,.protect=PAGE_READWRITE};nt_status status;
    CHECK_EQ_U32(guest_region_alloc(&request,&status),backing);
    CHECK(guest_physical_address(backing)!=old_physical);check_refusal(0u,0u);
    store(PARENT+4u,0u);(void)d3d8_register_resource(PARENT,backing);
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE,SCRATCH_DATA,0u,0u),SCRATCH_DATA);
    environment_end();
}
static void test_requested_bounds_interior_and_overlap(void)
{
    initialise(1024u,128u,32u,0x0221072Du);
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE,SCRATCH_DATA,0u,0u),SCRATCH_DATA);
    CHECK_EQ_U32(load(SCRATCH_DATA+4u),backing+160u);
    store(SCRATCH_DATA+64u,UINT32_MAX);store(SCRATCH_DATA+68u,0u);check_refusal(SCRATCH_DATA+64u,0u);
    /* An output overlapping the registered parent changes its binding. */
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE,PARENT,0u,0u),PARENT);
    CHECK_EQ_U32(load(PARENT+4u),backing+160u);
    check_refusal(0u,0u);
    environment_end();
    initialise(64u,0u,1u,0x0221072Du); /* mapped4096 bytes, requested only64. */
    CHECK(kernel_guest_at(backing,4096u)!=NULL);check_refusal(0u,0u);
    environment_end();
}
static void test_equal_masked_addresses_have_explicit_owners(void)
{
    initialise(0x18000u,0u,0u,0x0661072Du);
    guest_region_request request={.bytes=0x18000u,.fixed_base=0xA125A000u,.contiguous=true,
                                 .state=MEM_COMMIT,.protect=PAGE_READWRITE};nt_status status;
    CHECK_EQ_U32(guest_region_alloc(&request,&status),0xA125A000u);
    const uint32_t other=0x665804u;
    const uint32_t words[5]={0x40001u,0u,0u,0x0661072Du,0u};
    for(uint32_t i=0u;i<5u;i++)store(other+i*4u,words[i]);
    (void)d3d8_register_resource(other,0xA125A000u);
    CHECK_EQ_U32(load(other+4u),load(PARENT+4u));
    CHECK_EQ_U32(d3d8_resource_resolve_registered_alias(PARENT,load(PARENT+4u),0x4000u),backing);
    CHECK_EQ_U32(d3d8_resource_resolve_registered_alias(other,load(other+4u),0x4000u),0xA125A000u);
    environment_end();
}
/* Lock into SCRATCH_DATA and report the three published words and the return value. */
static uint32_t lock_words(uint32_t rect, uint32_t flags, uint32_t words[3])
{
    for (uint32_t i = 0u; i < 3u; i++) store(SCRATCH_DATA + i * 4u, 0xAABBCCDDu);
    const uint32_t result = d3d8_surface_lock_rect(SURFACE, SCRATCH_DATA, rect, flags);
    for (uint32_t i = 0u; i < 3u; i++) words[i] = load(SCRATCH_DATA + i * 4u);
    return result;
}

/* T393: the original swaps only Bits high bits 0xF0000000 for 0x80000000 under flags 0x40. */
static void test_uncached_alias_flag40_matches_cached(void)
{
    initialise(1024u, 128u, 32u, 0x0221072Du);
    const d3d8_surface_entry row = {0x003D4AC0u, NULL, 1u};
    CHECK(d3d8_hle_init(&row, 1u));
    CHECK_EQ_U32(d3d8_lock_register(), 1u);
    const uint32_t flag_sets[] = {0x40u, 0x60u, 0x20u};
    for (uint32_t pass = 0u; pass < 2u; pass++) {
        const uint32_t rect = pass == 0u ? 0u : SCRATCH_DATA + 64u;
        store(SCRATCH_DATA + 64u, 1u); store(SCRATCH_DATA + 68u, 1u);
        uint32_t cached[3];
        const uint32_t cached_result = lock_words(rect, 0u, cached);
        CHECK(cached[0] != 0u && cached[0] != 0xAABBCCDDu);
        CHECK(cached[1] >= backing && cached[1] != 0xAABBCCDDu);
        CHECK_EQ_U32(cached[2], 0xAABBCCDDu);
        for (size_t i = 0u; i < sizeof(flag_sets) / sizeof(flag_sets[0]); i++) {
            uint32_t other[3];
            const uint32_t other_result = lock_words(rect, flag_sets[i], other);
            CHECK_EQ_U32(other_result, cached_result);
            CHECK_EQ_U32(other[0], cached[0]);
            CHECK_EQ_U32(other[1], cached[1]);
            CHECK_EQ_U32(other[2], 0xAABBCCDDu);
        }
    }
    uint32_t words[3];
    CHECK_EQ_U32(lock_words(0u, 0x40u, words), SCRATCH_DATA);
    CHECK_EQ_U32(words[0], 16u);
    CHECK_EQ_U32(words[1], backing + 160u); /* Interior offset, not the 0xF0000000 domain. */
    CHECK(words[1] != (load(SURFACE + 4u) | 0xF0000000u));
    CHECK_EQ_U32(lock_words(SCRATCH_DATA + 64u, 0x40u, words), 16u);
    CHECK_EQ_U32(words[1], backing + 160u + 20u);
    /* Other bits are ignored as before. */
    CHECK_EQ_U32(lock_words(0u, 0x80u | 0x40u, words), SCRATCH_DATA);
    CHECK_EQ_U32(words[1], backing + 160u);
    /* Through the registered handler, as the XMV converter reaches it. */
    const uint32_t args[4] = {SURFACE, SCRATCH_DATA, 0u, 0x40u};
    CHECK_EQ_U32(call_stdcall(0x003D4AC0u, args, 4u), SCRATCH_DATA);
    CHECK_EQ_U32(load(SCRATCH_DATA + 4u), backing + 160u);
    /* The pixels written through Bits are the registered allocation itself. */
    store(load(SCRATCH_DATA + 4u), 0x5A5A1234u);
    CHECK_EQ_U32(load(backing + 160u), 0x5A5A1234u);
    environment_end();
}

static void test_uncached_alias_keeps_every_refusal(void)
{
    initialise(0x18000u, 0u, 0u, 0x0661072Du);
    uint32_t words[3];
    /* The fence wait is not bypassed by flag 0x40, only flag 0x20 skips it. */
    store(PARENT + 8u, 7u);
    check_refusal(0u, 0x40u);
    check_refusal(SCRATCH_DATA + 64u, 0x40u);
    CHECK_EQ_U32(lock_words(0u, 0x60u, words), SCRATCH_DATA);
    CHECK_EQ_U32(words[1], backing);
    store(PARENT + 8u, 0u);
    store(PARENT, 0xC0002u); /* Busy parent. */
    check_refusal(0u, 0x40u);
    CHECK_EQ_U32(lock_words(0u, 0x60u, words), SCRATCH_DATA);
    store(PARENT, 0x40002u);
    store(0x3E3F58u, 0u); /* No device, no wait needed. */
    CHECK_EQ_U32(lock_words(0u, 0x40u, words), SCRATCH_DATA);
    CHECK_EQ_U32(words[1], backing);
    store(0x3E3F58u, D3D8_DEVICE_BASE);
    /* Zero-ref surface and parent, unreadable and non-registered bindings. */
    store(SURFACE, 0x01050000u); check_refusal(0u, 0x40u); store(SURFACE, 0x01050001u);
    store(PARENT, 0x40000u); check_refusal(0u, 0x60u); store(PARENT, 0x40002u);
    const uint32_t registered = load(PARENT + 4u);
    store(PARENT + 4u, registered + 4u); check_refusal(0u, 0x40u); /* Stale binding. */
    store(PARENT + 4u, registered);
    store(PARENT + 4u, 0u); check_refusal(0u, 0x40u); /* Unregistered data address. */
    store(PARENT + 4u, registered);
    CHECK_EQ_U32(lock_words(0u, 0x40u, words), SCRATCH_DATA);
    CHECK(mprotect((void *)(uintptr_t)0x665000u, 4096u, PROT_NONE) == 0);
    check_refusal(0u, 0x40u); /* Unreadable header. */
    CHECK(mprotect((void *)(uintptr_t)0x665000u, 4096u, PROT_READ | PROT_WRITE) == 0);
    /* An unmappable rectangle offset refuses without publishing. */
    store(SCRATCH_DATA + 64u, UINT32_MAX); store(SCRATCH_DATA + 68u, 0u);
    check_refusal(SCRATCH_DATA + 64u, 0x40u);
    check_refusal(SCRATCH_DATA + 64u, 0u);
    /* Read-only output page: the output span is not writable, nothing is published. */
    map_fixed(0x00A00000u, 0x2000u);
    const uint32_t out = 0x00A00FFCu;
    store(out, 0x12345678u); store(out + 4u, 0x87654321u);
    CHECK(mprotect((void *)(uintptr_t)0xA01000u, 4096u, PROT_READ) == 0);
    RUN_EXPECTING_FATAL((void)d3d8_surface_lock_rect(SURFACE, out, 0u, 0x40u));
    CHECK(fatal_seen); CHECK_EQ_U32(fatal_address, 0x003D4AC0u);
    CHECK_EQ_U32(load(out), 0x12345678u); CHECK_EQ_U32(load(out + 4u), 0x87654321u);
    CHECK(mprotect((void *)(uintptr_t)0xA01000u, 4096u, PROT_READ | PROT_WRITE) == 0);
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE, out, 0u, 0x40u), out);
    CHECK_EQ_U32(load(out), 256u); CHECK_EQ_U32(load(out + 4u), backing);
    environment_end();
}

static void test_surface_nested_parent_wait_gate(void)
{
    initialise(0x18000u, 0u, 0u, 0x0661072Du);
    const uint32_t grandparent = 0x00665804u;
    store(PARENT, 0x50002u);
    store(PARENT + 4u, 0u);
    store(PARENT + 20u, grandparent);
    (void)d3d8_register_resource(PARENT, backing);
    store(SURFACE + 4u, load(PARENT + 4u));
    store(grandparent, 0xC0001u); /* Busy grandparent, idle surface and parent. */
    check_refusal(0u, 0u);
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE, SCRATCH_DATA, 0u, 0x20u), SCRATCH_DATA);
    store(0x3E3F58u, 0u);
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE, SCRATCH_DATA, 0u, 0u), SCRATCH_DATA);
    store(0x3E3F58u, D3D8_DEVICE_BASE);
    store(grandparent, 0x40001u);
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE, SCRATCH_DATA, 0u, 0u), SCRATCH_DATA);
    /* No-device and flag20 paths must not inspect an inaccessible grandparent. */
    store(PARENT + 20u, 0x7FFF0000u);
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE, SCRATCH_DATA, 0u, 0x20u), SCRATCH_DATA);
    store(0x3E3F58u, 0u);
    CHECK_EQ_U32(d3d8_surface_lock_rect(SURFACE, SCRATCH_DATA, 0u, 0u), SCRATCH_DATA);
    store(0x3E3F58u, D3D8_DEVICE_BASE);
    check_refusal(0u, 0u);
    environment_end();
}

static void initialise_texture(uint32_t requested, uint32_t format)
{
    initialise(requested, 0u, 0u, format);
    store_byte(0x3E1828u + 6u, 0x20u);
}

static void texture_refusal(uint32_t level, uint32_t rect, uint32_t flags)
{
    store(SCRATCH_DATA, 0xAABBCCDDu);
    store(SCRATCH_DATA + 4u, 0x11223344u);
    RUN_EXPECTING_FATAL((void)d3d8_texture_lock_rect(PARENT, level, SCRATCH_DATA, rect, flags));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D4E60u);
    CHECK_EQ_U32(load(SCRATCH_DATA), 0xAABBCCDDu);
    CHECK_EQ_U32(load(SCRATCH_DATA + 4u), 0x11223344u);
}

static void test_texture_entry_and_raw_swizzled_alias(void)
{
    initialise_texture(16384u, 0x00C10629u);
    const d3d8_surface_entry rows[] = {{0x003D4AC0u, NULL, 1u}, {0x003D4E60u, NULL, 1u}};
    CHECK(d3d8_hle_init(rows, 2u));
    CHECK_EQ_U32(d3d8_lock_register(), 2u);
    const uint32_t args[5] = {PARENT, 0u, SCRATCH_DATA, 0u, 0u};
    uint32_t header[5], device[4];
    CHECK(kernel_guest_read_bytes(PARENT, header, sizeof(header)));
    CHECK(kernel_guest_read_bytes(D3D8_DEVICE_BASE, device, sizeof(device)));
    store(backing, 0x12345678u);
    store(backing + 16380u, 0x87654321u);
    CHECK_EQ_U32(call_stdcall(0x003D4E60u, args, 5u), SCRATCH_DATA);
    CHECK_EQ_U32(load(SCRATCH_DATA), 16384u);
    CHECK_EQ_U32(load(SCRATCH_DATA + 4u), backing);
    CHECK(load(SCRATCH_DATA + 4u) != (load(PARENT + 4u) | 0x80000000u));
    CHECK_EQ_U32(load(SCRATCH_DATA + 8u), 0xAABBCCDDu);
    CHECK_EQ_U32(load(backing), 0x12345678u);
    CHECK_EQ_U32(load(backing + 16380u), 0x87654321u);
    CHECK(memcmp(kernel_guest_at(PARENT, sizeof(header)), header, sizeof(header)) == 0);
    CHECK(memcmp(kernel_guest_at(D3D8_DEVICE_BASE, sizeof(device)), device, sizeof(device)) == 0);
    CHECK_EQ_U32(d3d8_texture_lock_rect(PARENT, 0u, SCRATCH_DATA, 0u, 0x20u), SCRATCH_DATA);
    /* Bits is a CPU view of the exact registered allocation, not a copy. */
    store(load(SCRATCH_DATA + 4u) + 16380u, 0xCAFEBABEu);
    CHECK_EQ_U32(load(backing + 16380u), 0xCAFEBABEu);
    environment_end();
}

static void test_texture_scope_and_stale_alias_refusals(void)
{
    initialise_texture(16384u, 0x00C10629u);
    texture_refusal(1u, 0u, 0u);
    texture_refusal(0u, SCRATCH_DATA + 64u, 0u);
    texture_refusal(0u, 0u, 0x40u);
    texture_refusal(0u, 0u, 0x60u);
    texture_refusal(0u, 0u, 0x80u);
    const uint32_t good_common = load(PARENT);
    store(PARENT, 0x40000u); texture_refusal(0u, 0u, 0u);
    store(PARENT, 0x50001u); texture_refusal(0u, 0u, 0u);
    store(PARENT, good_common | 0x80000u); texture_refusal(0u, 0u, 0u);
    texture_refusal(0u, 0u, 0x20u); /* Policy scope remains idle even for no-wait flags. */
    store(PARENT, good_common);
    store(PARENT + 8u, 1u); texture_refusal(0u, 0u, 0u);
    texture_refusal(0u, 0u, 0x20u); store(PARENT + 8u, 0u);
    const uint32_t invalid_formats[] = {0x00C10729u, 0x00C1062Du, 0x00C20629u,
                                        0x10C10629u, 0x00C10639u};
    for (size_t i = 0u; i < sizeof(invalid_formats) / sizeof(invalid_formats[0]); i++) {
        store(PARENT + 12u, invalid_formats[i]); texture_refusal(0u, 0u, 0u);
    }
    store(PARENT + 12u, 0x00C10629u);
    store(PARENT + 16u, 1u); texture_refusal(0u, 0u, 0u); store(PARENT + 16u, 0u);
    const uint32_t data = load(PARENT + 4u);
    store(PARENT + 4u, data + 4u); texture_refusal(0u, 0u, 0u); store(PARENT + 4u, data);
    store_byte(0x3E1828u + 6u, 0u); texture_refusal(0u, 0u, 0u);
    store_byte(0x3E1828u + 6u, 0x20u);
    CHECK(guest_region_free(backing)); texture_refusal(0u, 0u, 0u);
    guest_region_request request = {.bytes=16384u, .fixed_base=backing, .contiguous=true,
                                    .state=MEM_COMMIT, .protect=PAGE_READWRITE};
    nt_status status;
    CHECK_EQ_U32(guest_region_alloc(&request, &status), backing);
    texture_refusal(0u, 0u, 0u);
    store(PARENT + 4u, 0u);
    (void)d3d8_register_resource(PARENT, backing);
    CHECK_EQ_U32(d3d8_texture_lock_rect(PARENT, 0u, SCRATCH_DATA, 0u, 0u), SCRATCH_DATA);
    environment_end();
    initialise_texture(64u, 0x00C10629u);
    texture_refusal(0u, 0u, 0u); /* A mapped page is not a 16KiB registered request. */
    environment_end();
}

static void test_texture_guarded_outputs_and_aliases(void)
{
    initialise_texture(16388u, 0x00C10629u);
    const uint32_t forbidden[] = {PARENT, PARENT + 4u, backing, backing + 16380u,
                                  0x3E3F58u, D3D8_DEVICE_BASE, 0x3E1828u};
    for (size_t i = 0u; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        uint32_t before[2], after[2];
        CHECK(kernel_guest_read_bytes(forbidden[i], before, sizeof(before)));
        RUN_EXPECTING_FATAL((void)d3d8_texture_lock_rect(PARENT, 0u, forbidden[i], 0u, 0u));
        CHECK(fatal_seen);
        CHECK_EQ_U32(fatal_address, 0x003D4E60u);
        CHECK(kernel_guest_read_bytes(forbidden[i], after, sizeof(after)));
        CHECK(memcmp(before, after, sizeof(before)) == 0);
    }
    map_fixed(0x00A00000u, 0x2000u);
    const uint32_t out = 0x00A00FFCu;
    store(out, 0x12345678u); store(out + 4u, 0x87654321u);
    CHECK(mprotect((void *)(uintptr_t)0xA01000u, 4096u, PROT_READ) == 0);
    RUN_EXPECTING_FATAL((void)d3d8_texture_lock_rect(PARENT, 0u, out, 0u, 0u));
    CHECK(fatal_seen); CHECK_EQ_U32(fatal_address, 0x003D4E60u);
    CHECK_EQ_U32(load(out), 0x12345678u); CHECK_EQ_U32(load(out + 4u), 0x87654321u);
    CHECK(mprotect((void *)(uintptr_t)0xA01000u, 4096u, PROT_NONE) == 0);
    RUN_EXPECTING_FATAL((void)d3d8_texture_lock_rect(PARENT, 0u, out, 0u, 0u));
    CHECK(fatal_seen); CHECK_EQ_U32(fatal_address, 0x003D4E60u);
    CHECK_EQ_U32(load(out), 0x12345678u);
    CHECK(mprotect((void *)(uintptr_t)0xA01000u, 4096u, PROT_READ | PROT_WRITE) == 0);
    CHECK_EQ_U32(load(out + 4u), 0x87654321u);
    CHECK_EQ_U32(d3d8_texture_lock_rect(PARENT, 0u, out, 0u, 0u), out);
    CHECK_EQ_U32(load(out), 16384u); CHECK_EQ_U32(load(out + 4u), backing);
    CHECK(mprotect((void *)(uintptr_t)0xA00000u, 4096u, PROT_READ) == 0);
    RUN_EXPECTING_FATAL((void)d3d8_texture_lock_rect(PARENT, 0u, 0xA00040u, 0u, 0u));
    CHECK(fatal_seen); CHECK_EQ_U32(fatal_address, 0x003D4E60u);
    CHECK(mprotect((void *)(uintptr_t)0xA00000u, 4096u, PROT_READ | PROT_WRITE) == 0);
    RUN_EXPECTING_FATAL((void)d3d8_texture_lock_rect(PARENT, 0u, UINT32_MAX - 3u, 0u, 0u));
    CHECK(fatal_seen); CHECK_EQ_U32(fatal_address, 0x003D4E60u);
    /* Guarded header read: protected guest memory must stop, not segfault. */
    CHECK(mprotect((void *)(uintptr_t)0x665000u, 4096u, PROT_NONE) == 0);
    RUN_EXPECTING_FATAL((void)d3d8_texture_lock_rect(PARENT, 0u, SCRATCH_DATA, 0u, 0u));
    CHECK(fatal_seen); CHECK_EQ_U32(fatal_address, 0x003D4E60u);
    CHECK(mprotect((void *)(uintptr_t)0x665000u, 4096u, PROT_READ) == 0);
    CHECK_EQ_U32(d3d8_texture_lock_rect(PARENT, 0u, SCRATCH_DATA, 0u, 0u), SCRATCH_DATA);
    CHECK(mprotect((void *)(uintptr_t)0x665000u, 4096u, PROT_READ | PROT_WRITE) == 0);
    environment_end();
}

/* T755: the XMV movie player 0x30280 builds its two YUY2 texture headers as LOCALS
 * (XGSetTextureHeader(w, h, 1, 0, 0x24, 0, hdr, 0, w * 4) then Register(hdr, memory it owns)) and
 * takes level 0 with GetSurfaceLevel2, which hands back a heap surface whose parent is the local.
 * The attract movie's Register came after 490 front end textures, so a 128 slot table dropped it
 * and the lock stopped at "no verified registered allocation alias". MOVIE_TEXTURE stands for the
 * player's stack header, FILLER_HEADERS for the textures registered before it. */
#define MOVIE_TEXTURE 0x00665804u
#define FILLER_HEADERS 0x00700000u
#define FILLER_STRIDE 0x20u
#define FILLER_BYTES 0x82000u
#define ALIAS_LIMIT_ROWS 16384u
static uint32_t movie_size_word(uint32_t width, uint32_t height)
{
    return (((width * 4u) / 64u - 1u) << 24) | ((height - 1u) << 12) | (width - 1u);
}
static void register_fillers(uint32_t count)
{
    for (uint32_t index = 0u; index < count; index++) {
        const uint32_t header = FILLER_HEADERS + index * FILLER_STRIDE;
        const uint32_t words[5] = {0x40001u, (index & 0xFFu) * 0x100u, 0u, 0x0221072Du, 0u};
        for (uint32_t word = 0u; word < 5u; word++) store(header + word * 4u, words[word]);
        (void)d3d8_register_resource(header, backing);
    }
}
static void initialise_movie(uint32_t width, uint32_t height, uint32_t fillers)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(0x00665000u, 0x1000u);
    map_fixed(FILLER_HEADERS, FILLER_BYTES);
    store_byte(GUEST_FORMAT_INFO + 0x24u, 0x12u); /* the YUY2 row of the retail table */
    const uint32_t bytes = 2u * ((width * height * 4u + 0x7Fu) & ~0x7Fu);
    guest_region_request request = {.bytes = bytes, .fixed_base = 0x9125A000u, .contiguous = true,
                                    .state = MEM_COMMIT, .protect = PAGE_READWRITE};
    nt_status status;
    backing = guest_region_alloc(&request, &status);
    CHECK_EQ_U32(backing, 0x9125A000u);
    register_fillers(fillers);
    const uint32_t texture[5] = {0x40001u, 0u, 0u, 0x00012429u, movie_size_word(width, height)};
    for (uint32_t word = 0u; word < 5u; word++) store(MOVIE_TEXTURE + word * 4u, texture[word]);
    (void)d3d8_register_resource(MOVIE_TEXTURE, backing);
    const uint32_t surface[6] = {0x01050001u, load(MOVIE_TEXTURE + 4u), 0u, 0x00012429u,
                                 movie_size_word(width, height), MOVIE_TEXTURE};
    for (uint32_t word = 0u; word < 6u; word++) store(SURFACE + word * 4u, surface[word]);
    store(MOVIE_TEXTURE, 0x40002u); /* GetSurfaceLevel2 AddRefs the texture */
    store(0x3E3F58u, D3D8_DEVICE_BASE);
    for (uint32_t word = 0u; word < 4u; word++) store(SCRATCH_DATA + word * 4u, 0xAABBCCDDu);
}
static void check_movie_lock(uint32_t pitch)
{
    uint32_t words[3];
    CHECK_EQ_U32(lock_words(0u, 0x40u, words), SCRATCH_DATA);
    CHECK_EQ_U32(words[0], pitch);
    CHECK_EQ_U32(words[1], backing);
    CHECK_EQ_U32(words[2], 0xAABBCCDDu);
    store(SCRATCH_DATA + 64u, 3u); store(SCRATCH_DATA + 68u, 2u);
    CHECK_EQ_U32(lock_words(SCRATCH_DATA + 64u, 0x40u, words), pitch);
    CHECK_EQ_U32(words[1], backing + 2u * pitch + 6u); /* top 2 rows, left 3 texels of 2 bytes */
}
static void test_movie_texture_header_locks_after_many_registrations(void)
{
    static const uint32_t sizes[][2] = {{640u, 480u}, {720u, 480u}, {320u, 240u}};
    static const uint32_t fillers[] = {0u, 127u, 128u, 129u, 600u};
    for (size_t size = 0u; size < sizeof(sizes) / sizeof(sizes[0]); size++) {
        for (size_t index = 0u; index < sizeof(fillers) / sizeof(fillers[0]); index++) {
            initialise_movie(sizes[size][0], sizes[size][1], fillers[index]);
            CHECK_EQ_U32(d3d8_resource_alias_count(), fillers[index] + 1u);
            check_movie_lock(sizes[size][0] * 4u);
            /* The first registration is still there, the table did not forget its head. */
            if (fillers[index] != 0u)
                CHECK_EQ_U32(d3d8_resource_resolve_registered_alias(FILLER_HEADERS, load(FILLER_HEADERS + 4u), 0x100u),
                             backing);
            environment_end();
        }
    }
}
/* A header nothing registered keeps the named refusal, however full or empty the table is. */
static void test_unregistered_header_keeps_the_named_refusal(void)
{
    initialise_movie(720u, 480u, 200u);
    const uint32_t stranger = 0x00665A04u;
    const uint32_t words[5] = {0x40001u, 0x1234u, 0u, 0x00012429u, movie_size_word(720u, 480u)};
    for (uint32_t word = 0u; word < 5u; word++) store(stranger + word * 4u, words[word]);
    store(SURFACE + 20u, stranger);
    check_refusal(0u, 0x40u);
    CHECK(strstr(fatal_text, "no verified registered allocation alias") != NULL);
    store(SURFACE + 20u, MOVIE_TEXTURE);
    check_movie_lock(2880u);
    environment_end();
}
static void make_extra_header(uint32_t header, uint32_t offset)
{
    const uint32_t words[5] = {0x40001u, offset, 0u, 0x0221072Du, 0u};
    for (uint32_t word = 0u; word < 5u; word++) store(header + word * 4u, words[word]);
}
static void expect_no_alias(uint32_t header)
{
    RUN_EXPECTING_FATAL((void)d3d8_resource_resolve_registered_alias(header, load(header + 4u), 0x40u));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "no verified registered allocation alias") != NULL ||
          strstr(fatal_text, "was registered at base") != NULL);
}
static void expect_lock_rejection_reason(const char *reason)
{
    check_refusal(0u, 0x40u);
    CHECK(strstr(fatal_text, "was registered at base") != NULL);
    CHECK(strstr(fatal_text, reason) != NULL);
}
static void test_register_rejection_reasons_reach_lock(void)
{
    initialise_movie(640u, 480u, 0u);
    (void)d3d8_register_resource(MOVIE_TEXTURE, 0x8125A000u);
    expect_lock_rejection_reason("base has no committed contiguous region");
    environment_end();

    initialise_movie(640u, 480u, 0u);
    guest_region_request request = {.bytes = 0x10000000u, .contiguous = true,
                                    .state = MEM_COMMIT, .protect = PAGE_READWRITE};
    nt_status status;
    const uint32_t oversized = guest_region_alloc(&request, &status);
    CHECK(oversized != 0u);
    (void)d3d8_register_resource(MOVIE_TEXTURE, oversized);
    expect_lock_rejection_reason("region is too large for the 28-bit alias");
    environment_end();

    initialise_movie(640u, 480u, 0u);
    store(MOVIE_TEXTURE + 4u, 0x300001u);
    (void)d3d8_register_resource(MOVIE_TEXTURE, backing);
    expect_lock_rejection_reason("Data offset is outside the region");
    CHECK(strstr(fatal_text, "Data offset 0x300001") != NULL);
    environment_end();

    initialise_movie(640u, 480u, 0u);
    store(MOVIE_TEXTURE, 0x00020001u);
    (void)d3d8_register_resource(MOVIE_TEXTURE, backing);
    expect_lock_rejection_reason("resource is virtual retained and has no physical alias");
    environment_end();
}
static void test_alias_table_limit_reuses_only_what_cannot_verify(void)
{
    initialise_movie(720u, 480u, ALIAS_LIMIT_ROWS - 1u);
    CHECK_EQ_U32(d3d8_resource_alias_count(), ALIAS_LIMIT_ROWS);
    check_movie_lock(2880u);
    const uint32_t extras[3] = {0x00665B04u, 0x00665B24u, 0x00665B44u};
    /* Every record verifies and the table is at its limit: a further Register stops by name. */
    make_extra_header(extras[0], 0x40u);
    RUN_EXPECTING_FATAL((void)d3d8_register_resource(extras[0], backing));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D4D70u);
    CHECK(strstr(fatal_text, "alias table is full") != NULL);
    CHECK_EQ_U32(load(extras[0] + 4u), 0x40u); /* the refused Register left the header untouched */
    CHECK_EQ_U32(d3d8_resource_alias_count(), ALIAS_LIMIT_ROWS);
    /* A record a later Register invalidated (its header is intact) is the first to be reused. */
    const uint32_t invalidated = FILLER_HEADERS + 3u * FILLER_STRIDE;
    const uint32_t registered_data = load(invalidated + 4u);
    (void)d3d8_register_resource(invalidated, 0x8125A000u); /* no allocation there: no binding */
    store(invalidated + 4u, registered_data); /* the title rewrites the header as it was: it verifies */
    CHECK_EQ_U32(d3d8_resource_alias_count(), ALIAS_LIMIT_ROWS - 1u);
    (void)d3d8_register_resource(extras[0], backing);
    CHECK_EQ_U32(d3d8_resource_alias_count(), ALIAS_LIMIT_ROWS);
    CHECK_EQ_U32(d3d8_resource_resolve_registered_alias(extras[0], load(extras[0] + 4u), 0x40u),
                 backing + 0x40u);
    expect_no_alias(invalidated);
    /* A record whose header no longer carries its identity (a dead stack local) is reused next. */
    const uint32_t dead = FILLER_HEADERS + 5u * FILLER_STRIDE;
    store(dead + 4u, load(dead + 4u) + 4u);
    make_extra_header(extras[1], 0x80u);
    (void)d3d8_register_resource(extras[1], backing);
    CHECK_EQ_U32(d3d8_resource_alias_count(), ALIAS_LIMIT_ROWS);
    CHECK_EQ_U32(d3d8_resource_resolve_registered_alias(extras[1], load(extras[1] + 4u), 0x40u),
                 backing + 0x80u);
    expect_no_alias(dead);
    /* Live neighbours were never taken over, and with the table full again a Register stops. */
    CHECK_EQ_U32(d3d8_resource_resolve_registered_alias(FILLER_HEADERS, load(FILLER_HEADERS + 4u), 0x100u),
                 backing);
    const uint32_t neighbour = FILLER_HEADERS + 6u * FILLER_STRIDE;
    CHECK_EQ_U32(d3d8_resource_resolve_registered_alias(neighbour, load(neighbour + 4u), 0x100u),
                 backing + 6u * 0x100u);
    make_extra_header(extras[2], 0xC0u);
    RUN_EXPECTING_FATAL((void)d3d8_register_resource(extras[2], backing));
    CHECK(fatal_seen);
    CHECK(strstr(fatal_text, "alias table is full") != NULL);
    check_movie_lock(2880u);
    environment_end();
}
/* A Register that records nothing (no allocation behind the base) leaves no binding, and every
 * Register retires the header's previous one: the live count follows. */
static void test_register_without_allocation_leaves_no_binding(void)
{
    initialise_movie(640u, 480u, 3u);
    CHECK_EQ_U32(d3d8_resource_alias_count(), 4u);
    store(FILLER_HEADERS + 4u, 0u);
    (void)d3d8_register_resource(FILLER_HEADERS, 0x8125A000u);
    CHECK_EQ_U32(d3d8_resource_alias_count(), 3u);
    expect_no_alias(FILLER_HEADERS);
    check_movie_lock(2560u);
    environment_end();
}

int main(void)
{
    test_live_alias_face_and_rectangle();
    test_alias_binding_mutation_and_rebind();
    test_requested_bounds_interior_and_overlap();
    test_equal_masked_addresses_have_explicit_owners();
    test_surface_nested_parent_wait_gate();
    test_uncached_alias_flag40_matches_cached();
    test_uncached_alias_keeps_every_refusal();
    test_texture_entry_and_raw_swizzled_alias();
    test_texture_scope_and_stale_alias_refusals();
    test_texture_guarded_outputs_and_aliases();
    test_movie_texture_header_locks_after_many_registrations();
    test_unregistered_header_keeps_the_named_refusal();
    test_alias_table_limit_reuses_only_what_cannot_verify();
    test_register_without_allocation_leaves_no_binding();
    test_register_rejection_reasons_reach_lock();
    printf("%d checks, %d failures\n",checks,failures);
    return failures==0?0:1;
}
