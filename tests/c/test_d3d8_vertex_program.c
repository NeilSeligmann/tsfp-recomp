/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_d3d8_support.h"
#include "d3d8_vertex_program.h"
#include <sys/mman.h>
#include "probe_cache_wrap.h" /* T819: mprotect and munmap flush the guest probe cache */

#define ARENA 0x00D00000u
#define ARENA_BYTES 0x9000u
#define STREAM ARENA
#define SOURCE (ARENA + 0x5000u)
#define CACHE (D3D8_DEVICE_BASE + 0x10A8u)
static uint8_t saved_device[D3D_REGION_BYTES], saved_arena[ARENA_BYTES];
static uint64_t saved_dwords;

static void controls(uint32_t cursor, uint32_t limit, uint32_t flags)
{
    store(0x3E3F58u, D3D8_DEVICE_BASE);
    store(D3D8_DEVICE_BASE, cursor);
    store(D3D8_DEVICE_BASE + 4u, limit);
    store(D3D8_DEVICE_BASE + 8u, flags);
}

static void program(uint32_t address, uint32_t count)
{
    store(address, (count << 16u) | 0x2078u);
    for (uint32_t i = 0u; i < count * 4u; i++)
        store(address + 4u + i * 4u, 0xA55AA55Au ^ (i * 0x17263541u));
}

static void snapshot(void)
{
    CHECK(kernel_guest_read_bytes(D3D_REGION_BASE, saved_device, sizeof(saved_device)));
    CHECK(kernel_guest_read_bytes(ARENA, saved_arena, sizeof(saved_arena)));
    saved_dwords = d3d8_pushbuffer_dwords_written();
}

static void unchanged(void)
{
    CHECK(memcmp(saved_device, kernel_guest_at(D3D_REGION_BASE, sizeof(saved_device)),
                 sizeof(saved_device)) == 0);
    CHECK(memcmp(saved_arena, kernel_guest_at(ARENA, sizeof(saved_arena)), sizeof(saved_arena)) == 0);
    CHECK(d3d8_pushbuffer_dwords_written() == saved_dwords);
}

static void refusal(uint32_t source, uint32_t slot)
{
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_program(source, slot));
    CHECK(fatal_seen);
    CHECK_EQ_U32(fatal_address, 0x003D59E0u);
}

static void test_packets_and_cache(void)
{
    const uint32_t counts[] = {0u, 1u, 4u, 8u, 9u, 32u, 136u};
    for (uint32_t flag = 0u; flag < 2u; flag++) {
        for (size_t c = 0u; c < sizeof(counts) / sizeof(counts[0]); c++) {
            const uint32_t n = counts[c], slot = n == 0u ? 135u : 136u - n;
            controls(STREAM, STREAM + 4096u, flag == 0u ? 0x4203u : 0x4213u);
            program(SOURCE, n);
            memset(kernel_guest_at(STREAM, 4096u), 0xCD, 4096u);
            memset(kernel_guest_at(CACHE, 136u * 16u), 0xEF, 136u * 16u);
            uint8_t input[4u + 136u * 16u];
            CHECK(kernel_guest_read_bytes(SOURCE, input, 4u + n * 16u));
            const uint64_t before = d3d8_pushbuffer_dwords_written();
            const uint32_t chunks = n == 0u ? 1u : (n * 4u + 31u) / 32u;
            const uint32_t bytes = (2u + n * 4u + chunks) * 4u;
            CHECK_EQ_U32(d3d8_upload_vertex_program(SOURCE, slot), STREAM + bytes);
            CHECK_EQ_U32(load(D3D8_DEVICE_BASE), STREAM + bytes);
            CHECK(d3d8_pushbuffer_dwords_written() == before + bytes / 4u);
            CHECK_EQ_U32(load(STREAM), 0x41E9Cu);
            CHECK_EQ_U32(load(STREAM + 4u), slot);
            uint32_t at = STREAM + 8u, copied = 0u;
            do {
                const uint32_t left = n * 4u - copied;
                const uint32_t chunk = left < 32u ? left : 32u;
                CHECK_EQ_U32(load(at), (chunk << 18u) + 0xB00u);
                at += 4u;
                for (uint32_t i = 0u; i < chunk; i++, at += 4u)
                    CHECK_EQ_U32(load(at), load(SOURCE + 4u + (copied + i) * 4u));
                copied += chunk;
            } while (copied != n * 4u);
            CHECK_EQ_U32(at, STREAM + bytes);
            CHECK_EQ_U32(load(STREAM + bytes), 0xCDCDCDCDu);
            for (uint32_t i = 0u; i < 136u * 4u; i++) {
                const bool copied_word = flag == 0u && i >= slot * 4u && i < (slot + n) * 4u;
                CHECK_EQ_U32(load(CACHE + i * 4u), copied_word
                              ? load(SOURCE + 4u + (i - slot * 4u) * 4u) : 0xEFEFEFEFu);
            }
            CHECK(memcmp(input, kernel_guest_at(SOURCE, 4u + n * 16u), 4u + n * 16u) == 0);
        }
    }
}

static void test_scope_reservation_and_aliases(void)
{
    controls(STREAM, STREAM + 4096u, 0x4203u);
    program(SOURCE, 4u);
    snapshot(); refusal(SOURCE, 136u); unchanged();
    refusal(SOURCE, UINT32_MAX); unchanged();
    refusal(SOURCE, 133u); unchanged();
    program(SOURCE, 137u); snapshot(); refusal(SOURCE, 0u); unchanged();
    program(SOURCE, 4u);
    const uint32_t reservation = (16u + 19u) * 4u;
    controls(STREAM + 512u, STREAM + reservation, 0x4203u);
    /* Exact equality with limit+512 rolls over in the original (0x003D6B30); with no ring that is a named stop. */
    snapshot(); RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_program(SOURCE, 0u));
    CHECK(fatal_seen); unchanged();
    controls(STREAM + 512u, STREAM + reservation + 1u, 0x4203u);
    CHECK_EQ_U32(d3d8_upload_vertex_program(SOURCE, 0u), STREAM + 512u + 76u);
    controls(0xFFFFFFF0u, 0xFFFFFDFFu, 0x4203u);
    snapshot(); refusal(SOURCE, 0u); unchanged();
    controls(STREAM, UINT32_MAX, 0x4203u);
    snapshot(); refusal(SOURCE, 0u); unchanged(); /* limit+512 wraps in original. */
    controls(STREAM, STREAM + 4096u, 0x4203u);
    snapshot(); refusal(0xFFFFFFF0u, 0u); unchanged();
    refusal(0x7FFF0000u, 0u); unchanged();
    store(0x3E3F58u, D3D8_DEVICE_BASE + 4u);
    snapshot(); refusal(SOURCE, 0u); unchanged();
    controls(STREAM, STREAM + 4096u, 0x4203u);
    program(STREAM, 4u); snapshot(); refusal(STREAM, 0u); unchanged();
    program(CACHE, 4u); snapshot(); refusal(CACHE, 0u); unchanged();
    program(D3D8_DEVICE_BASE + 0x2400u, 4u);
    snapshot(); refusal(D3D8_DEVICE_BASE + 0x2400u, 0u); unchanged();
    program(SOURCE, 4u);
    const uint32_t cursors[] = {D3D8_DEVICE_BASE, D3D8_DEVICE_BASE + 0x2400u,
                                0x3E3F48u, 0x7FFF0000u};
    for (size_t i = 0u; i < sizeof(cursors) / sizeof(cursors[0]); i++) {
        controls(cursors[i], cursors[i] + 4096u, 0x4203u);
        snapshot(); refusal(SOURCE, 0u); unchanged();
    }
}

static void test_permissions_unaligned_and_cross_page(void)
{
    controls(STREAM, STREAM + 4096u, 0x4203u);
    program(SOURCE, 4u); snapshot();
    CHECK(mprotect((void *)(uintptr_t)SOURCE, 4096u, PROT_NONE) == 0);
    refusal(SOURCE, 0u);
    CHECK(mprotect((void *)(uintptr_t)SOURCE, 4096u, PROT_READ | PROT_WRITE) == 0); unchanged();
    CHECK(mprotect((void *)(uintptr_t)0x3E3000u, 4096u, PROT_NONE) == 0);
    refusal(SOURCE, 0u);
    CHECK(mprotect((void *)(uintptr_t)0x3E3000u, 4096u, PROT_READ | PROT_WRITE) == 0); unchanged();
    /* Header readable, but the complete bounded program leaves the mapping. */
    store(ARENA + ARENA_BYTES - 256u, (136u << 16u) | 0x2078u);
    snapshot(); refusal(ARENA + ARENA_BYTES - 256u, 0u); unchanged();
    refusal(ARENA + ARENA_BYTES - 2u, 0u); unchanged();
    const uint32_t pages[] = {STREAM, 0x3E3000u, 0x3E5000u};
    for (size_t i = 0u; i < sizeof(pages) / sizeof(pages[0]); i++) {
        snapshot(); CHECK(mprotect((void *)(uintptr_t)pages[i], 4096u, PROT_READ) == 0);
        refusal(SOURCE, 0u);
        CHECK(mprotect((void *)(uintptr_t)pages[i], 4096u, PROT_READ | PROT_WRITE) == 0); unchanged();
    }
    controls(STREAM + 4092u, STREAM + 8192u, 0x4203u);
    snapshot(); CHECK(mprotect((void *)(uintptr_t)(STREAM + 4096u), 4096u, PROT_READ) == 0);
    refusal(SOURCE, 0u);
    CHECK(mprotect((void *)(uintptr_t)(STREAM + 4096u), 4096u, PROT_READ | PROT_WRITE) == 0);
    unchanged();
    CHECK(mprotect((void *)(uintptr_t)(STREAM + 4096u), 4096u, PROT_NONE) == 0);
    refusal(SOURCE, 0u);
    CHECK(mprotect((void *)(uintptr_t)(STREAM + 4096u), 4096u, PROT_READ | PROT_WRITE) == 0);
    unchanged();
    const uint32_t source = SOURCE + 4095u;
    program(source, 4u);
    controls(STREAM + 4092u, STREAM + 8192u, 0x4203u);
    CHECK(mprotect((void *)(uintptr_t)SOURCE, 8192u, PROT_READ) == 0);
    CHECK_EQ_U32(d3d8_upload_vertex_program(source, 0u), STREAM + 4092u + 76u);
    CHECK_EQ_U32(load(STREAM + 4104u), load(source + 4u));
    CHECK(mprotect((void *)(uintptr_t)SOURCE, 8192u, PROT_READ | PROT_WRITE) == 0);
    /* Original does not read low two header bytes on the inaccessible first page. */
    const uint32_t split_header = SOURCE + 4094u;
    program(split_header, 4u);
    controls(STREAM, STREAM + 4096u, 0x4203u);
    CHECK(mprotect((void *)(uintptr_t)SOURCE, 4096u, PROT_NONE) == 0);
    CHECK_EQ_U32(d3d8_upload_vertex_program(split_header, 0u), STREAM + 76u);
    CHECK_EQ_U32(load(STREAM + 12u), load(split_header + 4u));
    CHECK(mprotect((void *)(uintptr_t)SOURCE, 4096u, PROT_READ | PROT_WRITE) == 0);
    controls(STREAM, STREAM + 4096u, 0x4213u); program(SOURCE, 4u);
    CHECK(mprotect((void *)(uintptr_t)0x3E5000u, 4096u, PROT_NONE) == 0);
    CHECK_EQ_U32(d3d8_upload_vertex_program(SOURCE, 0u), STREAM + 76u);
    CHECK(mprotect((void *)(uintptr_t)0x3E5000u, 4096u, PROT_READ | PROT_WRITE) == 0);
    controls(STREAM, STREAM + 4096u, 0x4203u); program(SOURCE, 0u);
    CHECK(mprotect((void *)(uintptr_t)0x3E5000u, 4096u, PROT_READ) == 0);
    CHECK_EQ_U32(d3d8_upload_vertex_program(SOURCE, 135u), STREAM + 12u);
    CHECK(mprotect((void *)(uintptr_t)0x3E5000u, 4096u, PROT_READ | PROT_WRITE) == 0);
    /* An exclusive source end at2^32 is valid; the published cursor must not wrap. */
    map_fixed(0xFFFFF000u, 4096u);
    controls(STREAM, STREAM + 4096u, 0x4203u);
    program(0xFFFFFFFCu, 0u);
    CHECK_EQ_U32(d3d8_upload_vertex_program(0xFFFFFFFCu, 135u), STREAM + 12u);
    program(0xFFFFFFECu, 1u);
    controls(STREAM, STREAM + 4096u, 0x4203u);
    CHECK_EQ_U32(d3d8_upload_vertex_program(0xFFFFFFECu, 135u), STREAM + 28u);
    CHECK_EQ_U32(load(STREAM + 12u), load(0xFFFFFFF0u));
}

/* T1078: the sized reservation 0x003D6B30(dwords + 19) over a real ring. */
static void test_sized_reservation_rolls_over(void)
{
    environment_end();
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(ARENA, ARENA_BYTES);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    CHECK(d3d8_pushbuffer_create());
    const uint32_t ring = load(D3D8_DEVICE_BASE + 0x24u);
    store(0x3E3F58u, D3D8_DEVICE_BASE);
    program(SOURCE, 33u);
    memset(kernel_guest_at(CACHE, 136u * 16u), 0xEF, 136u * 16u);
    /* At limit + 0x200 - reservation + 1 the reservation fits: no roll-over, packet at the cursor. */
    const uint32_t reserve = (33u * 4u + 19u) * 4u, packet = (2u + 33u * 4u + 5u) * 4u;
    store(D3D8_DEVICE_BASE + 8u, 0x4203u);
    store(D3D8_DEVICE_BASE, ring + 0x1000u);
    store(D3D8_DEVICE_BASE + 4u, ring + 0x1000u + reserve - 0x200u + 1u);
    const uint64_t rollovers = d3d8_pushbuffer_rollovers();
    CHECK_EQ_U32(d3d8_upload_vertex_program(SOURCE, 2u), ring + 0x1000u + packet);
    CHECK(d3d8_pushbuffer_rollovers() == rollovers);
    CHECK_EQ_U32(load(CACHE + 2u * 16u), load(SOURCE + 4u));
    /* One byte less room (equality with limit + 0x200): the ring rolls over, the packet starts at the new cursor. */
    store(D3D8_DEVICE_BASE, ring + 0x1000u);
    store(D3D8_DEVICE_BASE + 4u, ring + 0x1000u + reserve - 0x200u);
    const uint32_t result = d3d8_upload_vertex_program(SOURCE, 2u);
    CHECK(d3d8_pushbuffer_rollovers() == rollovers + 1u);
    const uint32_t start = result - packet;
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE), result);
    CHECK_EQ_U32(load(D3D8_DEVICE_BASE + 4u), start + 0x10000u - 0x204u); /* the refill raised the limit */
    CHECK_EQ_U32(load(start), 0x41E9Cu);
    CHECK_EQ_U32(load(start + 4u), 2u);
    CHECK_EQ_U32(load(start + 8u), (32u << 18u) + 0xB00u);
    CHECK_EQ_U32(load(start + 12u), load(SOURCE + 4u));
    CHECK_EQ_U32(load(start + packet - 4u), load(SOURCE + 4u + 131u * 4u));
    /* A source inside the span the refill writes is refused by name, atomically. */
    store(D3D8_DEVICE_BASE, ring + 0x1000u);
    store(D3D8_DEVICE_BASE + 4u, ring + 0x1000u + reserve - 0x200u);
    program(start, 4u);
    snapshot(); refusal(start, 2u); unchanged();
    /* No ring: the roll-over is fatal before any write. */
    program(SOURCE, 33u);
    store(D3D8_DEVICE_BASE + 0x24u, 0u);
    store(D3D8_DEVICE_BASE, ring + 0x1000u);
    snapshot();
    RUN_EXPECTING_FATAL((void)d3d8_upload_vertex_program(SOURCE, 2u));
    CHECK(fatal_seen); unchanged();
}

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(ARENA, ARENA_BYTES);
    test_packets_and_cache();
    test_scope_reservation_and_aliases();
    test_permissions_unaligned_and_cross_page();
    test_sized_reservation_rolls_over();
    environment_end();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
