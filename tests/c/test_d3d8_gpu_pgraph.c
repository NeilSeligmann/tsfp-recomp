/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The recording the instantaneous GPU makes (d3d8_gpu.c), decoded by the command interpreter
 * (gpu_pgraph.c, T84), with the commands written by the REAL ported emitters: the vertex program
 * upload (0x003D59E0) and the vertex constant upload (0x003D58B0) run over a real pushbuffer ring,
 * the kick records them, and the decoder must find the program words in the program file and the
 * constants in the constant file at the rows the emitters named. This is what ties the units of
 * the decoder (slots and rows of 16 bytes, the 0x0B00 and 0x0B80 windows, the running cursor) to
 * what the emitters write rather than to the decoder's own idea of them.
 *
 * The draw commands are written the way d3d8_draw_vertices writes them (its header formula, with
 * the draw's deferred state flush left out) and the viewport pair the way vertex_viewport_emit does.
 * Synthetic guest memory, not original-XBE equivalence evidence.
 */
#include "test_d3d8_support.h"

#include "d3d8_gpu.h"
#include "d3d8_gpu_pgraph.h"
#include "d3d8_shader.h"
#include "d3d8_state.h"
#include "d3d8_vertex_constants.h"
#include "d3d8_vertex_program.h"

#include <sys/mman.h>

#define ARENA 0x00D00000u
#define PROGRAM_SOURCE (ARENA + 0x0000u)
#define CONSTANT_SOURCE (ARENA + 0x1000u)
#define VERTEX_MEMORY (ARENA + 0x2000u)

static uint32_t program_words[9 * 4];

static void write_pairs(const uint32_t *words, uint32_t count)
{
    uint32_t cursor = d3d8_pushbuffer_begin();
    for (uint32_t i = 0u; i < count; i++) {
        d3d8_guest_store32(cursor + i * 4u, words[i]);
    }
    d3d8_pushbuffer_end(cursor + count * 4u);
}

/* BEGIN_END(op), DRAW_ARRAYS in chunks of 256, BEGIN_END(0): d3d8_draw_vertices. */
static void draw(uint32_t op, uint32_t first, uint32_t count)
{
    const uint32_t chunks = ((count - 1u) >> 8) + 1u;
    uint32_t words[2 + 1 + 64 + 2];
    uint32_t at = 0u;
    words[at++] = 0x000417FCu;
    words[at++] = op;
    words[at++] = (chunks << 18) + 0x40001810u;
    for (uint32_t chunk = 0u; chunk < chunks; chunk++) {
        const uint32_t vertices = count - chunk * 256u > 256u ? 256u : count - chunk * 256u;
        words[at++] = ((vertices - 1u) << 24) | (first + chunk * 256u);
    }
    words[at++] = 0x000417FCu;
    words[at++] = 0u;
    write_pairs(words, at);
}

int main(void)
{
    environment_begin(KERNEL_AV_PACK_HDTV);
    map_fixed(ARENA, 0x10000u);
    store(D3D8_GLOBAL_PUSHBUFFER_SIZE, 0x100000u);
    store(D3D8_GLOBAL_KICKOFF_SIZE, 0x10000u);
    store(0x003E3F58u, D3D8_DEVICE_BASE);
    CHECK(d3d8_gpu_create());
    CHECK(d3d8_pushbuffer_create());
    d3d8_device_store32(D3D8_DEV_FLAGS, 0x4203u);

    /* 1. the program: header, then 9 instructions, FINAL on the last only */
    store(PROGRAM_SOURCE, (9u << 16) | 0x2078u);
    for (uint32_t i = 0u; i < 9u * 4u; i++) {
        program_words[i] = 0x31000000u + i * 0x01010101u;
        if (i % 4u == 3u) {
            program_words[i] = (program_words[i] & ~1u) | (i == 9u * 4u - 1u ? 1u : 0u);
        }
        store(PROGRAM_SOURCE + 4u + i * 4u, program_words[i]);
    }
    (void)d3d8_upload_vertex_program(PROGRAM_SOURCE, 7u);
    /* 2. constants: 40 dwords at row 3 (chunks of 16, 16 and 8) and 8 dwords at row 190 */
    float constants[40];
    for (uint32_t i = 0u; i < 40u; i++) {
        constants[i] = 1.25f + (float)i;
        uint32_t bits;
        memcpy(&bits, &constants[i], sizeof bits);
        store(CONSTANT_SOURCE + i * 4u, bits);
    }
    (void)d3d8_upload_vertex_constants(3u, CONSTANT_SOURCE, 40u);
    (void)d3d8_upload_vertex_constants(190u, CONSTANT_SOURCE, 8u);
    /* 3. viewport pair, execution mode and start, as vertex_viewport_emit and the binding do */
    const float offset[4] = {320.0f, 240.0f, 0.0f, 0.0f};
    const float scale[4] = {320.0f, -240.0f, 16777215.0f, 0.0f};
    uint32_t viewport[10];
    viewport[0] = 0x00100A20u;
    viewport[5] = 0x00100AF0u;
    for (uint32_t i = 0u; i < 4u; i++) {
        memcpy(&viewport[1 + i], &offset[i], 4u);
        memcpy(&viewport[6 + i], &scale[i], 4u);
    }
    write_pairs(viewport, 10u);
    d3d8_pushbuffer_emit_pair(0x00041E94u, 6u);
    d3d8_pushbuffer_emit_pair(0x00041EA0u, 7u);
    /* 4. the vertex arrays: a 16 format run, then one offset per enabled slot */
    uint32_t formats[17];
    formats[0] = 0x00401760u;
    for (uint32_t slot = 0u; slot < 16u; slot++) {
        formats[1u + slot] = slot == 1u ? (12u << 8) + 0x32u : 2u;
    }
    write_pairs(formats, 17u);
    d3d8_pushbuffer_emit_pair(0x00041724u, VERTEX_MEMORY);
    /* 5. one DrawVertices of 300 vertices, then a 3 vertex one */
    draw(6u, 0u, 300u);
    draw(5u, 4u, 3u);
    d3d8_gpu_kick();

    CHECK(d3d8_gpu_stream_count() > 100u);
    CHECK_EQ_U32(d3d8_gpu_get_stats().malformed_dwords, 0u);
    gpu_pgraph *pgraph = gpu_pgraph_create();
    size_t next = 0u;
    CHECK(d3d8_gpu_decode_recording(pgraph, &next) == GPU_PGRAPH_OK);
    CHECK(next == d3d8_gpu_stream_count());
    CHECK(strcmp(gpu_pgraph_error(pgraph), "") == 0);
    CHECK(gpu_pgraph_unhandled_count(pgraph) == 0u); /* every emitted method is a measured one */
    CHECK(gpu_pgraph_get_stats(pgraph).pairs == next);
    CHECK(gpu_pgraph_draw_count(pgraph) == 2u);
    const gpu_pgraph_state *now = gpu_pgraph_state_now(pgraph);
    CHECK(now->program_start_set && now->program_start == 7u);
    CHECK(now->execution_mode_set && (now->execution_mode & 3u) == GPU_PGRAPH_EXECUTION_MODE_PROGRAM);
    for (uint32_t slot = 0u; slot < GPU_PGRAPH_PROGRAM_SLOTS; slot++) {
        CHECK(now->slot_written[slot] == (slot >= 7u && slot < 16u));
    }
    int program_equal = 1;
    for (uint32_t i = 0u; i < 9u * 4u; i++) {
        program_equal &= now->program[7u * 4u + i] == program_words[i];
    }
    CHECK(program_equal);
    CHECK(now->program[6u * 4u + 3u] == 0u && now->program[16u * 4u] == 0u);
    for (uint32_t row = 0u; row < GPU_PGRAPH_CONSTANT_ROWS; row++) {
        const bool written = (row >= 3u && row < 13u) || row == 190u || row == 191u;
        CHECK(now->constant_written[row] == written);
    }
    int constants_equal = 1;
    for (uint32_t i = 0u; i < 40u; i++) {
        constants_equal &= now->constants[3u * 4u + i] == constants[i];
    }
    for (uint32_t i = 0u; i < 8u; i++) {
        constants_equal &= now->constants[190u * 4u + i] == constants[i];
    }
    CHECK(constants_equal);
    CHECK(now->viewport_offset[0] == 320.0f && now->viewport_offset[1] == 240.0f);
    CHECK(now->viewport_scale[1] == -240.0f && now->viewport_scale[2] == 16777215.0f);

    const gpu_pgraph_draw *first = gpu_pgraph_draw_at(pgraph, 0u);
    const gpu_pgraph_draw *second = gpu_pgraph_draw_at(pgraph, 1u);
    CHECK(first != NULL && second != NULL);
    if (first != NULL && second != NULL) {
        CHECK(first->primitive == 6u && first->index_count == 300u);
        CHECK(second->primitive == 5u && second->index_count == 3u);
        const uint32_t *indices = gpu_pgraph_indices(pgraph);
        CHECK(indices[first->first_index] == 0u && indices[first->first_index + 299u] == 299u);
        CHECK(indices[second->first_index] == 4u && indices[second->first_index + 2u] == 6u);
        CHECK(first->arrays[1].format == (12u << 8) + 0x32u && !first->arrays[0].address_set);
        CHECK(first->arrays[1].address_set && first->arrays[1].address == VERTEX_MEMORY);
        CHECK(first->arrays[0].format == 2u && gpu_pgraph_decode_format(2u).size == 0u);
    }

    /* the cursor is stable across kicks: a second kick's pairs decode on top, nothing twice */
    draw(5u, 0u, 3u);
    d3d8_gpu_kick();
    const size_t before = next;
    CHECK(d3d8_gpu_decode_recording(pgraph, &next) == GPU_PGRAPH_OK);
    CHECK(next == d3d8_gpu_stream_count() && next == before + 3u);
    CHECK(gpu_pgraph_draw_count(pgraph) == 3u);
    /* and a refusal leaves the cursor AT the offending pair */
    const uint32_t stray[2] = {0x00041810u, 0x02000000u}; /* DRAW_ARRAYS with no BEGIN_END open */
    write_pairs(stray, 2u);
    d3d8_gpu_kick();
    const size_t total = d3d8_gpu_stream_count();
    CHECK(d3d8_gpu_decode_recording(pgraph, &next) == GPU_PGRAPH_ERR_MALFORMED);
    CHECK(next == total - 1u);
    CHECK(strstr(gpu_pgraph_error(pgraph), "outside a BEGIN_END") != NULL);
    /* guest reads go to guest memory */
    uint32_t word = 0u;
    store(VERTEX_MEMORY, 0xCAFEF00Du);
    CHECK(d3d8_gpu_read_guest(NULL, VERTEX_MEMORY, &word, 4u) && word == 0xCAFEF00Du);
    CHECK(!d3d8_gpu_read_guest(NULL, 0xFFFFFFF0u, &word, 32u));
    /* T441: an address the stream carries is the buffer's PHYSICAL Data word (d3d8_create_buffer), not the
     * virtual address the title wrote through. A contiguous region's synthetic physical range reads through
     * the region, a span that leaves it is refused, and its virtual address still reads as before. */
    {
        guest_region_request request;
        memset(&request, 0, sizeof request);
        request.bytes = 0x2000u;
        request.contiguous = true;
        request.protect = PAGE_READWRITE;
        request.state = MEM_COMMIT;
        nt_status status = STATUS_SUCCESS;
        const kernel_guest_ptr region = guest_region_alloc(&request, &status);
        CHECK(region != 0u && status == STATUS_SUCCESS);
        const uint32_t physical = guest_physical_address(region + 0x40u);
        CHECK(physical != 0u && physical != region + 0x40u);
        store(region + 0x40u, 0x11223344u);
        store(region + 0x44u, 0x55667788u);
        store(region + 0x1FFCu, 0x99AABBCCu);
        uint32_t pair[2] = {0u, 0u};
        CHECK(d3d8_gpu_read_guest(NULL, physical, pair, 8u) && pair[0] == 0x11223344u && pair[1] == 0x55667788u);
        word = 0u;
        const uint32_t tail = guest_physical_address(region + 0x1FFCu);
        CHECK(d3d8_gpu_read_guest(NULL, tail, &word, 4u) && word == 0x99AABBCCu);
        static uint32_t big[0x800];
        CHECK(!d3d8_gpu_read_guest(NULL, tail, big, 8u)); /* leaves the region's physical range */
        CHECK(!d3d8_gpu_read_guest(NULL, physical, big, 0x2000u)); /* the same, from the other end */
        (void)guest_region_free(region);
    }
    /* Two regions whose synthetic physical ranges touch but whose virtual ranges do not: a span across the
     * boundary is not one linear buffer and is refused (the physical lookup of both ends succeeds, only the
     * linearity check sees it). */
    {
        guest_region_request request;
        memset(&request, 0, sizeof request);
        request.bytes = 0x1000u;
        request.contiguous = true;
        request.protect = PAGE_READWRITE;
        request.state = MEM_COMMIT;
        nt_status status = STATUS_SUCCESS;
        const kernel_guest_ptr lower = guest_region_alloc(&request, &status);
        const kernel_guest_ptr upper = guest_region_alloc(&request, &status);
        CHECK(lower != 0u && upper != 0u);
        const uint32_t lower_end = guest_physical_address(lower) + 0x1000u;
        CHECK(guest_physical_address(upper) == lower_end); /* the allocator hands out adjacent physical pages */
        uint32_t span[4] = {0u, 0u, 0u, 0u};
        CHECK(lower + 0x1000u != upper);
        /* Something else is mapped right after the lower region in virtual memory, so a plain read of the
         * span would succeed and return the wrong bytes: only the linearity check refuses it. */
        map_fixed(lower + 0x1000u, 0x1000u);
        CHECK(!d3d8_gpu_read_guest(NULL, lower_end - 8u, span, 16u));
        CHECK(d3d8_gpu_read_guest(NULL, lower_end - 8u, span, 8u)); /* wholly inside the lower still reads */
        CHECK(d3d8_gpu_read_guest(NULL, lower_end, span, 8u));      /* and the upper on its own */
        (void)guest_region_free(lower + 0x1000u);
        (void)guest_region_free(upper);
        (void)guest_region_free(lower);
    }
    gpu_pgraph_destroy(pgraph);

    /* T391: the fence packet 0x003D67B0 writes is in the recording, and a STRICT decode accepts it: the software
     * method on subchannel 5 is counted and replays nothing, SEMAPHORE_RELEASE is a handled no-op and the two
     * colour clear value writes of 0 are consumed with it. The pair count stays aligned with the recording (one
     * pair per recorded command), which is what d3d8_swap_replay's stream indices rely on. */
    d3d8_gpu_stream_discard(d3d8_gpu_stream_count());
    gpu_pgraph *fenced = gpu_pgraph_create();
    gpu_pgraph_set_strict(fenced, true);
    d3d8_pushbuffer_emit_pair(0x00041E94u, 6u);
    const uint32_t fence = d3d8_gpu_fence_insert(0u);
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 5u);
    size_t fenced_next = 0u;
    CHECK(d3d8_gpu_decode_recording(fenced, &fenced_next) == GPU_PGRAPH_OK);
    CHECK(fenced_next == 5u);
    const gpu_pgraph_stats fenced_stats = gpu_pgraph_get_stats(fenced);
    CHECK(fenced_stats.pairs == 5u && fenced_stats.pairs_handled == 5u);
    CHECK(fenced_stats.software_methods == 1u && fenced_stats.semaphore_releases == 1u);
    CHECK(fenced_stats.fence_clear_values == 2u);
    CHECK(gpu_pgraph_unhandled_count(fenced) == 0u && gpu_pgraph_draw_count(fenced) == 0u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(3).data, 0u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(2).data, fence);
    /* Any other subchannel in the recording is refused, naming it, at its own index. */
    d3d8_gpu_stream_discard(d3d8_gpu_stream_count());
    gpu_pgraph_reset(fenced);
    fenced_next = 0u;
    d3d8_pushbuffer_emit_pair(0x00041E94u, 6u);
    d3d8_pushbuffer_emit_pair((3u << 13) | 0x00040184u, 0x19u); /* subchannel 3, SET_OBJECT-like method 0x184 */
    d3d8_gpu_kick();
    CHECK(d3d8_gpu_decode_recording(fenced, &fenced_next) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(fenced_next == 1u);
    CHECK(strstr(gpu_pgraph_error(fenced), "subchannel 3") != NULL);
    /* A fence inside an open bracket is refused at the software method. */
    d3d8_gpu_stream_discard(d3d8_gpu_stream_count());
    gpu_pgraph_reset(fenced);
    fenced_next = 0u;
    d3d8_pushbuffer_emit_pair(0x000417FCu, 5u);
    (void)d3d8_gpu_fence_insert(0u);
    CHECK(d3d8_gpu_decode_recording(fenced, &fenced_next) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(fenced_next == 1u);
    CHECK(strstr(gpu_pgraph_error(fenced), "BEGIN_END") != NULL);
    /* T521: the REAL CreateDevice execution-mode packet (d3d8_shader_create, the original's `0x00081E94, 6, 0`) is a
     * header of count 2, so the recorder splits it into 0x1E94 = 6 and 0x1E98 = 0. A STRICT decode accepts both:
     * the second is a counted, kept, pixel-free no-op (before T521 it was the first refusal of every real boot). */
    d3d8_gpu_stream_discard(d3d8_gpu_stream_count());
    gpu_pgraph_reset(fenced);
    fenced_next = 0u;
    d3d8_shader_create();
    d3d8_gpu_kick();
    CHECK_EQ_U32(d3d8_gpu_stream_count(), 2u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(0).method, 0x1E94u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(1).method, 0x1E98u);
    CHECK_EQ_U32(d3d8_gpu_stream_at(1).data, 0u);
    CHECK(d3d8_gpu_decode_recording(fenced, &fenced_next) == GPU_PGRAPH_OK);
    CHECK(fenced_next == 2u);
    const gpu_pgraph_stats mode_stats = gpu_pgraph_get_stats(fenced);
    CHECK(mode_stats.cxt_write_en_pairs == 1u);
    CHECK(mode_stats.pairs == 2u && mode_stats.pairs_handled == 2u);
    CHECK(gpu_pgraph_unhandled_count(fenced) == 0u);
    CHECK(gpu_pgraph_state_now(fenced)->execution_mode_set && gpu_pgraph_state_now(fenced)->execution_mode == 6u);
    CHECK(gpu_pgraph_state_now(fenced)->cxt_write_en_set && gpu_pgraph_state_now(fenced)->cxt_write_en == 0u);
    gpu_pgraph_destroy(fenced);
    environment_end();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0 || checks < 100;
}
