/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T633, T596: the guest memory surface model (d3d8_surface_model.c) and the device-free byte copy of the BLIT group
 * (gpu_pgraph_replay_byte_copy). Guest memory is a flat array behind the model's injectable reader, so no guest and no Vulkan device is needed.
 */
#include "d3d8_surface_model.h"
#include "gpu_pgraph.h"
#include "gpu_pgraph_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition)                                                                      \
    do {                                                                                      \
        checks++;                                                                             \
        if (!(condition)) {                                                                   \
            failures++;                                                                       \
            printf("  FAIL line %d: %s\n", __LINE__, #condition);                             \
        }                                                                                     \
    } while (0)

#define MEMORY_BYTES (1u << 20)
static uint8_t memory[MEMORY_BYTES];
static unsigned reads;
static uint32_t last_address;
static size_t last_bytes;
static size_t largest_ask;
static void *last_context;
static int context_marker;

/* A flat guest: the address is the offset. Past the end is unreadable. What the model asked is recorded (the pieces, the tail of a growth). */
static bool flat_read(void *context, uint32_t address, void *out, size_t bytes)
{
    reads++;
    last_context = context;
    last_address = address;
    last_bytes = bytes;
    if (bytes > largest_ask) {
        largest_ask = bytes;
    }
    if ((uint64_t)address + bytes > MEMORY_BYTES) {
        return false;
    }
    memcpy(out, memory + address, bytes);
    return true;
}

static void fresh(void)
{
    memset(memory, 0, sizeof memory);
    d3d8_surface_model_reset();
    d3d8_surface_model_set_reader(flat_read, &context_marker);
    reads = 0u;
    last_address = 0u;
    last_bytes = 0u;
    largest_ask = 0u;
    last_context = NULL;
}

static void test_probe(void)
{
    printf("test_probe\n");
    fresh();
    CHECK(d3d8_surface_model_probe(0x1000u, 4096u) == D3D8_SURFACE_ZERO);
    CHECK(d3d8_surface_model_probe(0x1000u, 0u) == D3D8_SURFACE_ZERO);
    memory[0x1000u + 4095u] = 1u; /* the last byte of the range */
    CHECK(d3d8_surface_model_probe(0x1000u, 4096u) == D3D8_SURFACE_NONZERO);
    CHECK(d3d8_surface_model_probe(0x1000u, 4095u) == D3D8_SURFACE_ZERO); /* one byte short does not see it */
    memset(memory, 0, sizeof memory);
    memory[3u * 65536u + 7u] = 9u; /* in the fourth 64 KiB piece */
    CHECK(d3d8_surface_model_probe(0u, 4u * 65536u) == D3D8_SURFACE_NONZERO);
    CHECK(d3d8_surface_model_probe(0u, 3u * 65536u) == D3D8_SURFACE_ZERO);
    CHECK(d3d8_surface_model_probe(MEMORY_BYTES - 16u, 32u) == D3D8_SURFACE_UNREADABLE);
}

static void test_read_argb(void)
{
    printf("test_read_argb\n");
    fresh();
    /* 3 x 2 pixels, rows 16 bytes apart (pitch wider than the pixels): guest bytes B, G, R, A */
    for (uint32_t row = 0u; row < 2u; row++) {
        for (uint32_t x = 0u; x < 3u; x++) {
            uint8_t *pixel = memory + 0x400u + row * 16u + x * 4u;
            pixel[0] = (uint8_t)(0x10u + x);       /* B */
            pixel[1] = (uint8_t)(0x20u + x);       /* G */
            pixel[2] = (uint8_t)(0x30u + x + row); /* R */
            pixel[3] = (uint8_t)(0x40u + row);     /* A */
        }
        memset(memory + 0x400u + row * 16u + 12u, 0xEEu, 4u); /* padding that must not be read as a pixel */
    }
    uint8_t rgba[3u * 2u * 4u];
    memset(rgba, 0xAAu, sizeof rgba);
    CHECK(d3d8_surface_model_read_argb(0x400u, 3u, 2u, 16u, rgba) == D3D8_SURFACE_MODEL_OK);
    CHECK(rgba[0] == 0x30u && rgba[1] == 0x20u && rgba[2] == 0x10u && rgba[3] == 0x40u);          /* R G B A of pixel 0 */
    CHECK(rgba[8] == 0x32u && rgba[9] == 0x22u && rgba[10] == 0x12u && rgba[11] == 0x40u);        /* pixel 2 */
    CHECK(rgba[12] == 0x31u && rgba[13] == 0x20u && rgba[14] == 0x10u && rgba[15] == 0x41u);      /* row 1 pixel 0 */
    CHECK(rgba[23] == 0x41u);                                                                     /* the last byte is row 1's alpha */
    CHECK(d3d8_surface_model_read_argb(0x400u, 3u, 2u, 11u, rgba) == D3D8_SURFACE_MODEL_UNREADABLE); /* pitch narrower than the row */
    CHECK(d3d8_surface_model_read_argb(0x400u, 0u, 2u, 16u, rgba) == D3D8_SURFACE_MODEL_UNREADABLE);
    CHECK(d3d8_surface_model_read_argb(0x400u, 3u, 0u, 16u, rgba) == D3D8_SURFACE_MODEL_UNREADABLE);
    CHECK(d3d8_surface_model_read_argb(0x400u, 3u, 2u, 16u, NULL) == D3D8_SURFACE_MODEL_UNREADABLE);
    CHECK(d3d8_surface_model_read_argb(MEMORY_BYTES - 8u, 3u, 2u, 16u, rgba) == D3D8_SURFACE_MODEL_UNREADABLE);
}

static void test_byte_surfaces(void)
{
    printf("test_byte_surfaces\n");
    fresh();
    for (uint32_t i = 0u; i < 256u; i++) {
        memory[0x2000u + i] = (uint8_t)(i + 1u);
    }
    uint8_t *bytes = NULL;
    CHECK(!d3d8_surface_model_has_bytes(0x2000u));
    CHECK(d3d8_surface_model_acquire(0x2000u, 64u, &bytes) == D3D8_SURFACE_MODEL_OK);
    CHECK(bytes != NULL && bytes[0] == 1u && bytes[63] == 64u);
    CHECK(d3d8_surface_model_has_bytes(0x2000u) && !d3d8_surface_model_has_bytes(0x2001u));
    CHECK(d3d8_surface_model_byte_surface_length(0x2000u) == 64u && d3d8_surface_model_byte_surface_count() == 1u);
    bytes[10] = 0xF0u; /* a blit wrote it, the guest still holds 11 */
    CHECK(memory[0x2000u + 10u] == 11u);
    /* growing reads only the new tail from the guest: the written byte survives */
    memory[0x2000u + 10u] = 99u;
    memory[0x2000u + 100u] = 77u;
    CHECK(d3d8_surface_model_acquire(0x2000u, 128u, &bytes) == D3D8_SURFACE_MODEL_OK);
    CHECK(bytes[10] == 0xF0u && bytes[100] == 77u && d3d8_surface_model_byte_surface_length(0x2000u) == 128u);
    /* a shorter ask keeps the length */
    CHECK(d3d8_surface_model_acquire(0x2000u, 16u, &bytes) == D3D8_SURFACE_MODEL_OK && d3d8_surface_model_byte_surface_length(0x2000u) == 128u);
    /* reading: the kept bytes over the guest's */
    uint8_t out[200];
    memset(out, 0, sizeof out);
    CHECK(d3d8_surface_model_read_bytes(0x2000u, out, 200u) == D3D8_SURFACE_MODEL_OK);
    CHECK(out[10] == 0xF0u && out[100] == 77u && out[150] == 151u); /* past the kept 128 the guest's own byte */
    CHECK(d3d8_surface_model_read_bytes(0x2000u, out, 5u) == D3D8_SURFACE_MODEL_OK && out[4] == 5u);
    /* an overlap with another Data word is an alias, both ways */
    CHECK(d3d8_surface_model_acquire(0x2040u, 16u, &bytes) == D3D8_SURFACE_MODEL_ALIAS);
    CHECK(d3d8_surface_model_acquire(0x1FF0u, 32u, &bytes) == D3D8_SURFACE_MODEL_ALIAS);
    CHECK(d3d8_surface_model_read_bytes(0x2040u, out, 16u) == D3D8_SURFACE_MODEL_ALIAS);
    CHECK(d3d8_surface_model_overlaps_bytes(0x207Fu, 1u) && !d3d8_surface_model_overlaps_bytes(0x2080u, 16u));
    CHECK(d3d8_surface_model_overlaps_bytes(0x2000u, 1u) && !d3d8_surface_model_overlaps_bytes(0x1FF0u, 16u));
    /* adjacent but not overlapping is fine */
    CHECK(d3d8_surface_model_acquire(0x2080u, 16u, &bytes) == D3D8_SURFACE_MODEL_OK);
    /* limits */
    CHECK(d3d8_surface_model_acquire(0x9000u, 0u, &bytes) == D3D8_SURFACE_MODEL_TOO_LARGE);
    CHECK(d3d8_surface_model_acquire(0x9000u, D3D8_SURFACE_MODEL_MAX_BYTES + 1u, &bytes) == D3D8_SURFACE_MODEL_TOO_LARGE);
    CHECK(d3d8_surface_model_acquire(MEMORY_BYTES - 8u, 16u, &bytes) == D3D8_SURFACE_MODEL_UNREADABLE);
    CHECK(!d3d8_surface_model_has_bytes(MEMORY_BYTES - 8u)); /* a failed first read leaves no surface behind */
    /* a failed GROW keeps what was held */
    CHECK(d3d8_surface_model_acquire(0xFFF00u, 16u, &bytes) == D3D8_SURFACE_MODEL_OK);
    CHECK(d3d8_surface_model_acquire(0xFFF00u, 4096u, &bytes) == D3D8_SURFACE_MODEL_UNREADABLE);
    CHECK(d3d8_surface_model_byte_surface_length(0xFFF00u) == 16u);
    CHECK(d3d8_surface_model_acquire(0xFFF00u, 16u, &bytes) == D3D8_SURFACE_MODEL_OK && bytes != NULL);
    /* the store is bounded and never evicts */
    fresh();
    for (uint32_t i = 0u; i < D3D8_SURFACE_MODEL_BYTE_SURFACES; i++) {
        CHECK(d3d8_surface_model_acquire(0x1000u * (i + 1u), 16u, &bytes) == D3D8_SURFACE_MODEL_OK);
    }
    CHECK(d3d8_surface_model_acquire(0x1000u * 20u, 16u, &bytes) == D3D8_SURFACE_MODEL_FULL);
    CHECK(d3d8_surface_model_byte_surface_count() == D3D8_SURFACE_MODEL_BYTE_SURFACES);
    CHECK(d3d8_surface_model_acquire(0x1000u, 16u, &bytes) == D3D8_SURFACE_MODEL_OK); /* an existing one is still reachable when full */
    d3d8_surface_model_reset();
    CHECK(d3d8_surface_model_byte_surface_count() == 0u && !d3d8_surface_model_has_bytes(0x1000u));
    for (int status = 0; status <= (int)D3D8_SURFACE_MODEL_MEMORY; status++) {
        CHECK(d3d8_surface_model_status_string((d3d8_surface_model_status)status)[0] != '\0');
    }
}

/* How the model reads: the probe in pieces of at most 64 KiB, each piece read once, nothing read past the range or after the first nonzero byte. */
static void test_probe_pieces(void)
{
    printf("test_probe_pieces\n");
    fresh();
    CHECK(d3d8_surface_model_probe(0x1000u, 4096u) == D3D8_SURFACE_ZERO && reads == 1u && last_address == 0x1000u && last_bytes == 4096u);
    reads = 0u;
    CHECK(d3d8_surface_model_probe(0x1000u, 0u) == D3D8_SURFACE_ZERO && reads == 0u); /* an empty range reads nothing */
    CHECK(d3d8_surface_model_probe(0u, 65536u) == D3D8_SURFACE_ZERO && reads == 1u && last_bytes == 65536u); /* one whole piece */
    reads = 0u;
    CHECK(d3d8_surface_model_probe(0x100u, 65537u) == D3D8_SURFACE_ZERO && reads == 2u && last_address == 0x100u + 65536u && last_bytes == 1u);
    reads = 0u;
    largest_ask = 0u;
    CHECK(d3d8_surface_model_probe(0x10u, 3u * 65536u + 5u) == D3D8_SURFACE_ZERO && reads == 4u && largest_ask == 65536u && last_bytes == 5u);
    /* the first nonzero byte ends the probe: no later piece is read */
    memory[7] = 1u;
    reads = 0u;
    CHECK(d3d8_surface_model_probe(0u, 4u * 65536u) == D3D8_SURFACE_NONZERO && reads == 1u);
    memory[7] = 0u;
    /* a nonzero byte just past the range is not the range's, whatever the piece */
    memory[0x1000u + 100u] = 7u;
    CHECK(d3d8_surface_model_probe(0x1000u, 100u) == D3D8_SURFACE_ZERO);
    CHECK(d3d8_surface_model_probe(0x1000u, 101u) == D3D8_SURFACE_NONZERO);
    memory[0x1000u + 100u] = 0u;
    /* a byte below 0x80 is nonzero too (the last byte of each of two pieces) */
    memory[65535u] = 0x01u;
    CHECK(d3d8_surface_model_probe(0u, 2u * 65536u) == D3D8_SURFACE_NONZERO);
    memory[65535u] = 0u;
    memory[2u * 65536u - 1u] = 0x02u;
    CHECK(d3d8_surface_model_probe(0u, 2u * 65536u) == D3D8_SURFACE_NONZERO);
    memory[2u * 65536u - 1u] = 0u;
    /* a later piece that cannot be read after a clean first one is unreadable, not zero */
    CHECK(d3d8_surface_model_probe(MEMORY_BYTES - 65536u, 65536u + 16u) == D3D8_SURFACE_UNREADABLE);
    CHECK(d3d8_surface_model_probe(MEMORY_BYTES - 65536u, 65536u) == D3D8_SURFACE_ZERO);
}

static void test_read_argb_edges(void)
{
    printf("test_read_argb_edges\n");
    fresh();
    uint8_t rgba[4u * 3u * 4u];
    /* a tightly packed surface (pitch == width * 4, the back buffer) is fine */
    for (uint32_t i = 0u; i < 4u * 3u * 4u; i++) {
        memory[0x800u + i] = (uint8_t)(i + 1u);
    }
    memset(rgba, 0xAAu, sizeof rgba);
    reads = 0u;
    largest_ask = 0u;
    CHECK(d3d8_surface_model_read_argb(0x800u, 4u, 3u, 16u, rgba) == D3D8_SURFACE_MODEL_OK);
    CHECK(reads == 3u && largest_ask == 16u && last_address == 0x800u + 2u * 16u); /* a row a read, a row's pixels only, a pitch apart */
    CHECK(rgba[0] == 3u && rgba[1] == 2u && rgba[2] == 1u && rgba[3] == 4u && rgba[44] == 47u && rgba[47] == 48u);
    /* a pitch wider than the pixels reads the pixels only */
    reads = 0u;
    largest_ask = 0u;
    CHECK(d3d8_surface_model_read_argb(0x800u, 2u, 2u, 32u, rgba) == D3D8_SURFACE_MODEL_OK && reads == 2u && largest_ask == 8u && last_address == 0x800u + 32u);
    /* an unreadable first row stops at the first read */
    reads = 0u;
    CHECK(d3d8_surface_model_read_argb(MEMORY_BYTES, 2u, 2u, 32u, rgba) == D3D8_SURFACE_MODEL_UNREADABLE && reads == 1u);
    /* an unreadable later row is unreadable: the first row reads, the second is past the end */
    reads = 0u;
    CHECK(d3d8_surface_model_read_argb(MEMORY_BYTES - 16u, 2u, 2u, 16u, rgba) == D3D8_SURFACE_MODEL_UNREADABLE && reads == 2u);
    reads = 0u;
    CHECK(d3d8_surface_model_read_argb(MEMORY_BYTES - 16u, 2u, 1u, 16u, rgba) == D3D8_SURFACE_MODEL_OK && reads == 1u);
    CHECK(d3d8_surface_model_read_argb(0x800u, 1u, 1u, 4u, rgba) == D3D8_SURFACE_MODEL_OK && rgba[0] == 3u && rgba[2] == 1u);
    CHECK(d3d8_surface_model_read_argb(0x800u, 1u, 1u, 3u, rgba) == D3D8_SURFACE_MODEL_UNREADABLE);
    CHECK(d3d8_surface_model_read_argb(0x800u, 4u, 1u, 15u, rgba) == D3D8_SURFACE_MODEL_UNREADABLE);
}

/* The store's edges: what a re-ask reads, where a growth reads, what a failed growth keeps, the bound, adjacency and the readers. */
static void test_byte_surface_edges(void)
{
    printf("test_byte_surface_edges\n");
    fresh();
    uint8_t *bytes = NULL;
    uint8_t out[300];
    CHECK(!d3d8_surface_model_has_bytes(0u) && d3d8_surface_model_byte_surface_length(0u) == 0u && d3d8_surface_model_byte_surface_length(0x5000u) == 0u);
    CHECK(d3d8_surface_model_acquire(0x3000u, 16u, &bytes) == D3D8_SURFACE_MODEL_OK);
    CHECK(!d3d8_surface_model_has_bytes(0u) && d3d8_surface_model_byte_surface_length(0u) == 0u); /* an empty slot is not Data 0 */
    CHECK(last_context == &context_marker); /* the injected reader is given its context */
    /* a re-ask of the held length reads nothing and hands the pointer back */
    reads = 0u;
    bytes = NULL;
    CHECK(d3d8_surface_model_acquire(0x3000u, 16u, &bytes) == D3D8_SURFACE_MODEL_OK && reads == 0u && bytes != NULL);
    bytes = NULL;
    CHECK(d3d8_surface_model_acquire(0x3000u, 8u, &bytes) == D3D8_SURFACE_MODEL_OK && reads == 0u && bytes != NULL);
    /* a growth reads the new tail and nothing else: from held, length - held bytes */
    reads = 0u;
    CHECK(d3d8_surface_model_acquire(0x3000u, 80u, &bytes) == D3D8_SURFACE_MODEL_OK && reads == 1u && last_address == 0x3010u && last_bytes == 64u);
    /* a growth that reaches the very end of guest memory reads the tail only (the whole length would run past the end) */
    CHECK(d3d8_surface_model_acquire(MEMORY_BYTES - 256u, 16u, &bytes) == D3D8_SURFACE_MODEL_OK);
    CHECK(d3d8_surface_model_acquire(MEMORY_BYTES - 256u, 256u, &bytes) == D3D8_SURFACE_MODEL_OK && d3d8_surface_model_byte_surface_length(MEMORY_BYTES - 256u) == 256u);
    /* the bound itself is not too large: it is unreadable here (the guest has 1 MiB), a byte more is too large */
    fresh();
    CHECK(d3d8_surface_model_acquire(0x10000u, D3D8_SURFACE_MODEL_MAX_BYTES, &bytes) == D3D8_SURFACE_MODEL_UNREADABLE);
    CHECK(d3d8_surface_model_acquire(0x10000u, D3D8_SURFACE_MODEL_MAX_BYTES + 1u, &bytes) == D3D8_SURFACE_MODEL_TOO_LARGE);
    CHECK(!d3d8_surface_model_has_bytes(0x10000u));
    /* a failed growth keeps the bytes the blits wrote, at the address they were at (the allocation may have moved) */
    fresh();
    for (uint32_t i = 0u; i < 16u; i++) {
        memory[0xFFF00u + i] = (uint8_t)(0x40u + i);
    }
    CHECK(d3d8_surface_model_acquire(0xFFF00u, 16u, &bytes) == D3D8_SURFACE_MODEL_OK);
    for (uint32_t i = 0u; i < 16u; i++) {
        bytes[i] = (uint8_t)(0x90u + i);
    }
    CHECK(d3d8_surface_model_acquire(0xFFF00u, 64u * 1024u, &bytes) == D3D8_SURFACE_MODEL_UNREADABLE);
    CHECK(d3d8_surface_model_byte_surface_length(0xFFF00u) == 16u);
    bytes = NULL;
    CHECK(d3d8_surface_model_acquire(0xFFF00u, 16u, &bytes) == D3D8_SURFACE_MODEL_OK && bytes != NULL);
    for (uint32_t i = 0u; bytes != NULL && i < 16u; i++) {
        CHECK(bytes[i] == (uint8_t)(0x90u + i));
    }
    memset(out, 0, sizeof out);
    CHECK(d3d8_surface_model_read_bytes(0xFFF00u, out, 16u) == D3D8_SURFACE_MODEL_OK && out[0] == 0x90u && out[15] == 0x9Fu);
    /* a failed first acquire leaves the slot free for the next one */
    fresh();
    CHECK(d3d8_surface_model_acquire(MEMORY_BYTES - 8u, 16u, &bytes) == D3D8_SURFACE_MODEL_UNREADABLE && d3d8_surface_model_byte_surface_count() == 0u);
    CHECK(d3d8_surface_model_acquire(0x100u, 16u, &bytes) == D3D8_SURFACE_MODEL_OK && d3d8_surface_model_byte_surface_count() == 1u);
    /* surfaces that touch are not aliases: below and above a kept surface, for the store and for a read */
    fresh();
    CHECK(d3d8_surface_model_acquire(0x2000u, 64u, &bytes) == D3D8_SURFACE_MODEL_OK);
    CHECK(d3d8_surface_model_acquire(0x1FF0u, 16u, &bytes) == D3D8_SURFACE_MODEL_OK); /* ends where the kept one starts */
    CHECK(d3d8_surface_model_acquire(0x2040u, 16u, &bytes) == D3D8_SURFACE_MODEL_OK); /* starts where the kept one ends */
    CHECK(d3d8_surface_model_acquire(0x1FF1u, 16u, &bytes) == D3D8_SURFACE_MODEL_ALIAS); /* one byte inside the one below */
    CHECK(d3d8_surface_model_acquire(0x2000u, 16u, &bytes) == D3D8_SURFACE_MODEL_OK); /* the same Data word, shorter: its own */
    fresh();
    CHECK(d3d8_surface_model_acquire(0x2000u, 64u, &bytes) == D3D8_SURFACE_MODEL_OK);
    CHECK(d3d8_surface_model_read_bytes(0x1FF0u, out, 16u) == D3D8_SURFACE_MODEL_OK);
    CHECK(d3d8_surface_model_read_bytes(0x2040u, out, 16u) == D3D8_SURFACE_MODEL_OK);
    CHECK(d3d8_surface_model_read_bytes(0x1FF1u, out, 16u) == D3D8_SURFACE_MODEL_ALIAS);
    CHECK(d3d8_surface_model_read_bytes(0x203Fu, out, 16u) == D3D8_SURFACE_MODEL_ALIAS);
    /* read_bytes with no kept surface at all is the guest's bytes; a short read touches only its own bytes; an empty one reads nothing */
    fresh();
    for (uint32_t i = 0u; i < 300u; i++) {
        memory[0x4000u + i] = (uint8_t)(i + 1u);
    }
    memset(out, 0xEEu, sizeof out);
    CHECK(d3d8_surface_model_read_bytes(0x4000u, out, 300u) == D3D8_SURFACE_MODEL_OK && out[0] == 1u && out[299] == (uint8_t)300u);
    CHECK(d3d8_surface_model_acquire(0x4000u, 200u, &bytes) == D3D8_SURFACE_MODEL_OK);
    for (uint32_t i = 0u; i < 200u; i++) {
        bytes[i] = (uint8_t)(0x80u + (i % 64u));
    }
    memset(out, 0xEEu, sizeof out);
    CHECK(d3d8_surface_model_read_bytes(0x4000u, out, 5u) == D3D8_SURFACE_MODEL_OK && out[4] == 0x84u && out[5] == 0xEEu && out[199] == 0xEEu);
    memset(out, 0xEEu, sizeof out);
    CHECK(d3d8_surface_model_read_bytes(0x4000u, out, 300u) == D3D8_SURFACE_MODEL_OK && out[199] == (uint8_t)(0x80u + (199u % 64u)) && out[200] == 201u && out[299] == (uint8_t)300u);
    reads = 0u;
    CHECK(d3d8_surface_model_read_bytes(MEMORY_BYTES * 4u, out, 0u) == D3D8_SURFACE_MODEL_OK && reads == 0u); /* an empty read asks the guest for nothing */
    CHECK(d3d8_surface_model_read_bytes(MEMORY_BYTES - 8u, out, 16u) == D3D8_SURFACE_MODEL_UNREADABLE);
    /* the status texts say what each status is */
    CHECK(strcmp(d3d8_surface_model_status_string(D3D8_SURFACE_MODEL_OK), "ok") == 0);
    CHECK(strstr(d3d8_surface_model_status_string(D3D8_SURFACE_MODEL_UNREADABLE), "cannot be read") != NULL);
    CHECK(strstr(d3d8_surface_model_status_string(D3D8_SURFACE_MODEL_ALIAS), "overlaps") != NULL);
    CHECK(strstr(d3d8_surface_model_status_string(D3D8_SURFACE_MODEL_FULL), "slot") != NULL);
    CHECK(strstr(d3d8_surface_model_status_string(D3D8_SURFACE_MODEL_TOO_LARGE), "larger") != NULL);
    CHECK(strstr(d3d8_surface_model_status_string(D3D8_SURFACE_MODEL_MEMORY), "memory") != NULL);
    CHECK(strcmp(d3d8_surface_model_status_string((d3d8_surface_model_status)99), "unknown") == 0);
}

static gpu_pgraph_copy make_copy(uint32_t format, uint32_t source_pitch, uint32_t destination_pitch, uint32_t width, uint32_t height)
{
    gpu_pgraph_copy copy;
    memset(&copy, 0, sizeof copy);
    copy.color_format = format;
    copy.source_pitch = source_pitch;
    copy.destination_pitch = destination_pitch;
    copy.width = width;
    copy.height = height;
    copy.operation = GPU_PGRAPH_BLIT_OPERATION_SRCCOPY;
    return copy;
}

/* T769, the reference of the byte blit in xemu (docs/t769-xemu-copyrects.md), written per pixel and independent of the implementation: the
 * row is clamped in PIXELS to the narrower pitch, rows ascend, a row is read whole into a buffer then written, the addresses are flat (a row
 * that passes its pitch runs on into the next row's bytes), formats 7 and 6 write only the alpha byte of each pixel. `source` may be
 * `destination`. */
static void reference_byte_blit(const gpu_pgraph_copy *copy, const uint8_t *source, uint8_t *destination)
{
    const uint32_t bpp = copy->color_format == GPU_PGRAPH_BLIT_FORMAT_Y8 ? 1u : (copy->color_format == GPU_PGRAPH_BLIT_FORMAT_R5G6B5 ? 2u : 4u);
    uint32_t pixels = copy->width;
    pixels = copy->source_pitch / bpp < pixels ? copy->source_pitch / bpp : pixels;
    pixels = copy->destination_pitch / bpp < pixels ? copy->destination_pitch / bpp : pixels;
    uint8_t buffer[4096];
    for (uint32_t line = 0u; line < copy->height; line++) {
        for (uint32_t pixel = 0u; pixel < pixels; pixel++) {
            for (uint32_t byte = 0u; byte < bpp; byte++) {
                buffer[pixel * bpp + byte] = source[(size_t)(copy->in_y + line) * copy->source_pitch + (size_t)(copy->in_x + pixel) * bpp + byte];
            }
        }
        for (uint32_t pixel = 0u; pixel < pixels; pixel++) {
            for (uint32_t byte = 0u; byte < bpp; byte++) {
                uint8_t value = buffer[pixel * bpp + byte];
                if (byte == 3u && copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF) {
                    value = 0xFFu;
                } else if (byte == 3u && copy->color_format == GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0) {
                    value = 0u;
                }
                destination[(size_t)(copy->out_y + line) * copy->destination_pitch + (size_t)(copy->out_x + pixel) * bpp + byte] = value;
            }
        }
    }
}

/* The fixture shapes of T769 plus the clamp, spill and overlap shapes, each on one surface or two, every one against the reference. A wrong
 * model scores below 1.0: the cases name the alternative the shape rules out in `note`. */
static void test_byte_copy_oracle(void)
{
    printf("test_byte_copy_oracle\n");
    gpu_pgraph_backend backend;
    memset(&backend, 0, sizeof backend);
    backend.output_groups = GPU_PGRAPH_OUTPUT_BLIT;
    backend.allowed_inferences = GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL;
    static const struct {
        const char *note;
        uint32_t format;
        uint32_t source_pitch, destination_pitch;
        uint32_t width, height, in_x, in_y, out_x, out_y;
        bool same;
    } cases[] = {
        {"Y8 rect, different points", 1u, 128u, 128u, 96u, 8u, 5u, 3u, 20u, 9u, false},
        {"R5G6B5 rect, different points", 4u, 128u, 128u, 48u, 8u, 6u, 2u, 14u, 5u, false},
        {"A8R8G8B8 pitch 288 (4.5 x 64)", 0xAu, 288u, 288u, 64u, 6u, 0u, 0u, 0u, 0u, false},
        {"A8R8G8B8 pitches 288 and 352", 0xAu, 288u, 352u, 70u, 5u, 1u, 1u, 3u, 2u, false},
        {"Y8 pitch 100, 120 px: clamp to 100", 1u, 100u, 100u, 120u, 4u, 0u, 0u, 0u, 0u, false},
        {"Y8 narrow source pitch 64, 96 px", 1u, 64u, 128u, 96u, 8u, 0u, 0u, 0u, 0u, false},
        {"Y8 narrow destination pitch 64, 96 px", 1u, 128u, 64u, 96u, 8u, 0u, 0u, 0u, 0u, false},
        {"R5G6B5 narrow source pitch 128 (64 px), 96 px", 4u, 128u, 256u, 96u, 8u, 0u, 0u, 0u, 0u, false},
        {"R5G6B5 narrow destination pitch 128, 96 px", 4u, 256u, 128u, 96u, 8u, 0u, 0u, 0u, 0u, false},
        {"A8R8G8B8 pitch 150: 37 px = 148 bytes", 0xAu, 150u, 150u, 40u, 4u, 0u, 0u, 0u, 0u, false},
        {"R5G6B5 pitch 151: 75 px = 150 bytes", 4u, 151u, 151u, 80u, 4u, 0u, 0u, 0u, 0u, false},
        {"Y8 source x 40 + 40 over pitch 64: runs into the next row", 1u, 64u, 128u, 40u, 6u, 40u, 0u, 0u, 0u, false},
        {"A8R8G8B8 destination x 20 + 24 px over pitch 128 (32 px)", 0xAu, 256u, 128u, 24u, 6u, 0u, 0u, 20u, 0u, false},
        {"Y8 overlap 3 px right", 1u, 128u, 128u, 100u, 8u, 0u, 0u, 3u, 0u, true},
        {"R5G6B5 overlap 3 px right", 4u, 256u, 256u, 100u, 8u, 0u, 0u, 3u, 0u, true},
        {"A8R8G8B8 overlap 40 px right (160 bytes of a 384 byte row)", 0xAu, 512u, 512u, 96u, 6u, 0u, 0u, 40u, 0u, true},
        {"Y8 overlap 1 row down: every row becomes row 0", 1u, 64u, 64u, 64u, 8u, 0u, 0u, 0u, 1u, true},
        {"Y8 overlap 3 rows down", 1u, 64u, 64u, 64u, 8u, 0u, 0u, 0u, 3u, true},
        {"R5G6B5 overlap 3 rows down", 4u, 128u, 128u, 64u, 8u, 0u, 0u, 0u, 3u, true},
        {"Y8 overlap 8 px right and 5 rows down", 1u, 64u, 64u, 48u, 8u, 0u, 0u, 8u, 5u, true},
        {"A8R8G8B8 overlap 8 px right and 5 rows down", 0xAu, 256u, 256u, 48u, 8u, 0u, 0u, 8u, 5u, true},
        {"Y8 overlap 5 rows up", 1u, 64u, 64u, 64u, 8u, 0u, 5u, 0u, 0u, true},
        {"Y8 one surface, pitches 192 then 256", 1u, 192u, 256u, 128u, 8u, 0u, 0u, 0u, 0u, true},
        {"A8R8G8B8 one surface, pitches 256 then 192", 0xAu, 256u, 192u, 40u, 8u, 0u, 0u, 4u, 0u, true},
        {"format 7 whole", 7u, 256u, 256u, 40u, 6u, 0u, 0u, 5u, 1u, false},
        {"format 6 whole", 6u, 256u, 256u, 40u, 6u, 0u, 0u, 5u, 1u, false},
        {"format 7 overlap 3 rows down", 7u, 256u, 256u, 40u, 8u, 0u, 0u, 0u, 3u, true},
        {"format 6 clamped narrow source", 6u, 128u, 256u, 60u, 4u, 0u, 0u, 2u, 0u, false},
    };
    for (size_t i = 0u; i < sizeof cases / sizeof cases[0]; i++) {
        static uint8_t source[8192];
        static uint8_t actual[8192];
        static uint8_t expected[8192];
        for (size_t byte = 0u; byte < sizeof source; byte++) {
            source[byte] = (uint8_t)((byte * 2654435761u) >> 13);
            actual[byte] = (uint8_t)(0x30u + (byte & 3u));
        }
        memcpy(expected, actual, sizeof expected);
        gpu_pgraph_copy copy = make_copy(cases[i].format, cases[i].source_pitch, cases[i].destination_pitch, cases[i].width, cases[i].height);
        copy.in_x = cases[i].in_x;
        copy.in_y = cases[i].in_y;
        copy.out_x = cases[i].out_x;
        copy.out_y = cases[i].out_y;
        gpu_pgraph_report report;
        memset(&report, 0, sizeof report);
        uint32_t used = 0u;
        gpu_pgraph_result result;
        if (cases[i].same) {
            memcpy(actual, source, sizeof actual);
            memcpy(expected, source, sizeof expected);
            result = gpu_pgraph_replay_byte_copy(&copy, &backend, actual, sizeof actual, actual, sizeof actual, &used, &report);
            reference_byte_blit(&copy, expected, expected);
        } else {
            result = gpu_pgraph_replay_byte_copy(&copy, &backend, source, sizeof source, actual, sizeof actual, &used, &report);
            reference_byte_blit(&copy, source, expected);
        }
        if (result != GPU_PGRAPH_OK) {
            printf("  case %s: %s\n", cases[i].note, report.error);
        }
        CHECK(result == GPU_PGRAPH_OK && used == GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL);
        CHECK(memcmp(actual, expected, sizeof actual) == 0);
        CHECK(memcmp(actual, source, sizeof actual) != 0); /* something moved */
    }
}

static void test_byte_copy(void)
{
    printf("test_byte_copy\n");
    gpu_pgraph_backend backend;
    memset(&backend, 0, sizeof backend);
    backend.output_groups = GPU_PGRAPH_OUTPUT_BLIT;
    backend.allowed_inferences = GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL;
    gpu_pgraph_report report;
    uint32_t used = 0u;
    uint8_t source[256];
    uint8_t destination[256];
    for (unsigned i = 0u; i < sizeof source; i++) {
        source[i] = (uint8_t)i;
    }
    /* extents and bytes per pixel */
    CHECK(gpu_pgraph_blit_bytes_per_pixel(GPU_PGRAPH_BLIT_FORMAT_Y8) == 1u && gpu_pgraph_blit_bytes_per_pixel(GPU_PGRAPH_BLIT_FORMAT_R5G6B5) == 2u &&
          gpu_pgraph_blit_bytes_per_pixel(GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8) == 4u && gpu_pgraph_blit_bytes_per_pixel(2u) == 0u);
    CHECK(gpu_pgraph_blit_bytes_per_pixel(GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF) == 4u && gpu_pgraph_blit_bytes_per_pixel(GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHA0) == 4u);
    static const uint32_t unmeasured_formats[] = {0u, 2u, 3u, 5u, 8u, 9u, 0xBu, 0xCu, 0x10u};
    for (size_t i = 0u; i < sizeof unmeasured_formats / sizeof unmeasured_formats[0]; i++) {
        CHECK(gpu_pgraph_blit_bytes_per_pixel(unmeasured_formats[i]) == 0u);
    }
    /* the clamped row, in pixels, of every format and either pitch (T769: A8R8G8B8 pitch 150 is 37 px, R5G6B5 pitch 151 is 75 px) */
    {
        gpu_pgraph_copy clamp = make_copy(GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8, 150u, 150u, 40u, 2u);
        CHECK(gpu_pgraph_blit_row_pixels(&clamp) == 37u);
        clamp = make_copy(GPU_PGRAPH_BLIT_FORMAT_R5G6B5, 151u, 151u, 80u, 2u);
        CHECK(gpu_pgraph_blit_row_pixels(&clamp) == 75u);
        clamp = make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 64u, 128u, 96u, 2u);
        CHECK(gpu_pgraph_blit_row_pixels(&clamp) == 64u);
        clamp = make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 128u, 64u, 96u, 2u);
        CHECK(gpu_pgraph_blit_row_pixels(&clamp) == 64u);
        clamp = make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 128u, 128u, 96u, 2u);
        CHECK(gpu_pgraph_blit_row_pixels(&clamp) == 96u);
        clamp = make_copy(GPU_PGRAPH_BLIT_FORMAT_X8R8G8B8_ALPHAFF, 256u, 512u, 100u, 2u);
        CHECK(gpu_pgraph_blit_row_pixels(&clamp) == 64u);
        clamp = make_copy(2u, 256u, 512u, 100u, 2u);
        CHECK(gpu_pgraph_blit_row_pixels(&clamp) == 0u);
    }
    CHECK(gpu_pgraph_blit_extent_bytes(GPU_PGRAPH_BLIT_FORMAT_R5G6B5, 32u, 3u, 2u, 5u, 4u) == (size_t)(2u + 4u - 1u) * 32u + 3u * 2u + 5u * 2u);
    CHECK(gpu_pgraph_blit_extent_bytes(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 0u, 0u, 32u, 1u) == 32u);
    CHECK(gpu_pgraph_blit_extent_bytes(2u, 32u, 0u, 0u, 1u, 1u) == 0u && gpu_pgraph_blit_extent_bytes(1u, 0u, 0u, 0u, 1u, 1u) == 0u &&
          gpu_pgraph_blit_extent_bytes(1u, 32u, 0u, 0u, 0u, 1u) == 0u && gpu_pgraph_blit_extent_bytes(1u, 32u, 0u, 0u, 1u, 0u) == 0u);
    /* each guard on its own, with a rectangle that would otherwise have rows (a row, a pitch or a column) to count */
    CHECK(gpu_pgraph_blit_extent_bytes(2u, 32u, 3u, 2u, 4u, 5u) == 0u && gpu_pgraph_blit_extent_bytes(1u, 0u, 3u, 2u, 4u, 5u) == 0u &&
          gpu_pgraph_blit_extent_bytes(1u, 32u, 3u, 2u, 0u, 5u) == 0u && gpu_pgraph_blit_extent_bytes(1u, 32u, 3u, 2u, 4u, 0u) == 0u);
    CHECK(gpu_pgraph_blit_extent_bytes(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 3u, 2u, 4u, 5u) == (size_t)(2u + 5u - 1u) * 32u + 3u + 4u);
    CHECK(gpu_pgraph_blit_extent_bytes(GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8, 64u, 3u, 2u, 4u, 5u) == (size_t)(2u + 5u - 1u) * 64u + 3u * 4u + 4u * 4u);
    CHECK(gpu_pgraph_blit_extent_bytes(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 3u, 0u, 4u, 1u) == 7u && gpu_pgraph_blit_extent_bytes(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 0u, 3u, 4u, 1u) == 3u * 32u + 4u);

    /* Y8, a 6 x 3 rectangle from (2, 1) of a 32 byte pitch surface to (5, 0) of a 16 byte pitch one */
    gpu_pgraph_copy copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 16u, 6u, 3u);
    copy.in_x = 2u;
    copy.in_y = 1u;
    copy.out_x = 5u;
    copy.out_y = 0u;
    memset(destination, 0xCCu, sizeof destination);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_OK);
    CHECK((used & GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL) != 0u);
    for (uint32_t row = 0u; row < 3u; row++) {
        for (uint32_t x = 0u; x < 6u; x++) {
            CHECK(destination[row * 16u + 5u + x] == source[(1u + row) * 32u + 2u + x]);
        }
        CHECK(destination[row * 16u + 4u] == 0xCCu && destination[row * 16u + 11u] == 0xCCu); /* neighbours untouched */
    }
    CHECK(destination[3u * 16u + 5u] == 0xCCu); /* the row after the rectangle */

    /* R5G6B5 moves two bytes a pixel */
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_R5G6B5, 32u, 32u, 4u, 2u);
    copy.in_x = 1u;
    copy.out_x = 3u;
    copy.out_y = 1u;
    memset(destination, 0xCCu, sizeof destination);
    used = 0u;
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_OK);
    CHECK(destination[32u + 6u] == source[2u] && destination[32u + 13u] == source[9u] && destination[32u + 14u] == 0xCCu &&
          destination[32u + 5u] == 0xCCu && destination[64u + 6u] == source[34u]);

    /* A8R8G8B8 is four bytes a pixel */
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8, 64u, 64u, 2u, 1u);
    memset(destination, 0xCCu, sizeof destination);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_OK);
    CHECK(memcmp(destination, source, 8u) == 0 && destination[8] == 0xCCu);

    /* a zero width or height moves nothing and needs no surface and no inference */
    memset(destination, 0xCCu, sizeof destination);
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 32u, 0u, 4u);
    gpu_pgraph_backend bare;
    memset(&bare, 0, sizeof bare);
    bare.output_groups = GPU_PGRAPH_OUTPUT_BLIT;
    used = 0u;
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &bare, NULL, 0u, NULL, 0u, &used, &report) == GPU_PGRAPH_OK && used == 0u);
    copy.width = 4u;
    copy.height = 0u;
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &bare, NULL, 0u, NULL, 0u, &used, &report) == GPU_PGRAPH_OK && used == 0u);

    /* refusals by name, each leaving the destination as it was */
    memset(destination, 0xCCu, sizeof destination);
    uint8_t reference[256];
    memcpy(reference, destination, sizeof reference);
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 32u, 8u, 2u);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &bare, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED &&
          strstr(report.error, "INFERRED and not allowed") != NULL);
    gpu_pgraph_backend no_group = backend;
    no_group.output_groups = 0u;
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &no_group, source, sizeof source, destination, sizeof destination, &used, &report) ==
              GPU_PGRAPH_ERR_UNMEASURED &&
          strstr(report.error, "blit group") != NULL);
    gpu_pgraph_copy bad = copy;
    bad.color_format = 2u;
    CHECK(gpu_pgraph_replay_byte_copy(&bad, &backend, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED &&
          strstr(report.error, "colour format") != NULL);
    bad = copy;
    bad.operation = 5u;
    CHECK(gpu_pgraph_replay_byte_copy(&bad, &backend, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED &&
          strstr(report.error, "SRCCOPY") != NULL);
    /* T769: a row wider than a pitch is not refused, it is clamped in pixels to the narrower one (the rest of the destination row stays) */
    for (int variant = 0; variant < 3; variant++) {
        bad = copy;
        bad.width = 33u; /* wider than both pitches: 32 bytes a row */
        if (variant == 1) {
            bad.destination_pitch = 7u; /* wider than only the destination's */
        } else if (variant == 2) {
            bad.source_pitch = 7u; /* wider than only the source's */
        }
        uint8_t clamped[256];
        memcpy(clamped, reference, sizeof clamped);
        memcpy(destination, reference, sizeof destination);
        used = 0u;
        CHECK(gpu_pgraph_replay_byte_copy(&bad, &backend, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_OK);
        reference_byte_blit(&bad, source, clamped);
        CHECK(memcmp(destination, clamped, sizeof destination) == 0 && memcmp(destination, reference, sizeof destination) != 0);
        CHECK((used & GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL) != 0u);
    }
    for (size_t i = 0u; i < sizeof unmeasured_formats / sizeof unmeasured_formats[0]; i++) {
        bad = copy;
        bad.color_format = unmeasured_formats[i];
        CHECK(gpu_pgraph_replay_byte_copy(&bad, &backend, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED &&
              strstr(report.error, "colour format") != NULL);
    }
    memcpy(destination, reference, sizeof destination);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, 32u + 7u, destination, sizeof destination, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED &&
          strstr(report.error, "past the held bytes") != NULL); /* the second row reaches byte 39 (needs 40) of the source */
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, 40u, destination, sizeof destination, &used, &report) == GPU_PGRAPH_OK);
    memcpy(destination, reference, sizeof destination);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, sizeof source, destination, 39u, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED &&
          strstr(report.error, "past the held bytes") != NULL);
    CHECK(memcmp(destination, reference, sizeof destination) == 0);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, NULL, 0u, destination, sizeof destination, &used, &report) == GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, sizeof source, NULL, 0u, &used, &report) == GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(gpu_pgraph_replay_byte_copy(NULL, &backend, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, NULL, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_ERR_ARGUMENT);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, sizeof source, destination, sizeof destination, NULL, &report) == GPU_PGRAPH_ERR_ARGUMENT);

    /* one surface: disjoint rectangles copy, and (T769) overlapping ones and two pitches are copied row by row, each row read whole first */
    uint8_t surface[256];
    memcpy(surface, source, sizeof surface);
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 32u, 4u, 2u);
    copy.out_x = 8u;
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, surface, sizeof surface, surface, sizeof surface, &used, &report) == GPU_PGRAPH_OK);
    CHECK(surface[8] == source[0] && surface[32u + 11u] == source[32u + 3u]);
    static const struct {
        uint32_t out_x, out_y, destination_pitch;
    } shifts[] = {{3u, 0u, 32u}, {0u, 1u, 32u}, {3u, 1u, 32u}, {4u, 0u, 32u}, {0u, 2u, 32u}, {0u, 0u, 64u}, {1u, 0u, 16u}};
    for (size_t i = 0u; i < sizeof shifts / sizeof shifts[0]; i++) {
        uint8_t expected[256];
        memcpy(surface, source, sizeof surface);
        memcpy(expected, source, sizeof expected);
        copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, shifts[i].destination_pitch, 4u, 2u);
        copy.out_x = shifts[i].out_x;
        copy.out_y = shifts[i].out_y;
        CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, surface, sizeof surface, surface, sizeof surface, &used, &report) == GPU_PGRAPH_OK);
        reference_byte_blit(&copy, expected, expected);
        CHECK(memcmp(surface, expected, sizeof surface) == 0);
    }
    /* two different buffers never count as an overlap */
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 32u, 4u, 2u);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_OK);
}

/* The edges of the byte copy: a row exactly a pitch wide, a rectangle ending exactly at the held length, rectangles of one surface that touch. */
static void test_byte_copy_edges(void)
{
    printf("test_byte_copy_edges\n");
    gpu_pgraph_backend backend;
    memset(&backend, 0, sizeof backend);
    backend.output_groups = GPU_PGRAPH_OUTPUT_BLIT;
    backend.allowed_inferences = GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL;
    gpu_pgraph_report report;
    uint32_t used = 0u;
    uint8_t source[256];
    uint8_t destination[256];
    for (unsigned i = 0u; i < sizeof source; i++) {
        source[i] = (uint8_t)(i + 1u);
    }
    /* a row exactly as wide as one pitch is no clamp, whichever side it fills */
    gpu_pgraph_copy copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 64u, 32u, 2u);
    memset(destination, 0xCCu, sizeof destination);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_OK);
    CHECK(memcmp(destination, source, 32u) == 0 && memcmp(destination + 64u, source + 32u, 32u) == 0 && destination[32] == 0xCCu);
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 64u, 32u, 32u, 2u);
    memset(destination, 0xCCu, sizeof destination);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_OK);
    CHECK(memcmp(destination, source, 32u) == 0 && memcmp(destination + 32u, source + 64u, 32u) == 0);
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_R5G6B5, 64u, 64u, 32u, 1u); /* 64 bytes a row: the pitch */
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_OK);
    CHECK(memcmp(destination, source, 64u) == 0);
    /* a rectangle that ends exactly at the end of the destination (its extent 40 bytes), and one byte less is clamped */
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 32u, 8u, 2u);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, sizeof source, destination, 40u, &used, &report) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, 40u, destination, 40u, &used, &report) == GPU_PGRAPH_OK);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, 39u, destination, 40u, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, 40u, destination, 39u, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    /* one surface, rectangles that only touch, on every side, or are apart: copied */
    uint8_t surface[256];
    for (unsigned side = 0u; side < 6u; side++) {
        memcpy(surface, source, sizeof surface);
        copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 32u, 4u, 2u);
        switch (side) {
        case 0: copy.in_x = 4u; copy.out_x = 0u; break; /* the destination ends where the source starts */
        case 1: copy.in_x = 0u; copy.out_x = 4u; break; /* the destination starts where the source ends */
        case 2: copy.in_y = 2u; copy.out_y = 0u; break; /* above */
        case 3: copy.in_y = 0u; copy.out_y = 2u; break; /* below */
        case 4: copy.in_x = 12u; copy.out_x = 0u; break; /* apart on the left */
        default: copy.in_y = 5u; copy.out_y = 0u; break; /* apart above */
        }
        CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, surface, sizeof surface, surface, sizeof surface, &used, &report) == GPU_PGRAPH_OK);
        for (uint32_t row = 0u; row < 2u; row++) {
            CHECK(memcmp(surface + (copy.out_y + row) * 32u + copy.out_x, source + (copy.in_y + row) * 32u + copy.in_x, 4u) == 0);
        }
    }
    /* diagonal: the rectangles share neither a column nor a row range */
    memcpy(surface, source, sizeof surface);
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 32u, 4u, 2u);
    copy.out_x = 8u;
    copy.out_y = 3u;
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, surface, sizeof surface, surface, sizeof surface, &used, &report) == GPU_PGRAPH_OK);
    /* a corner overlap and the same surface with another pitch are copied too (T769: no overlap and no two pitches refusal) */
    for (int variant = 0; variant < 2; variant++) {
        uint8_t expected[256];
        memcpy(surface, source, sizeof surface);
        memcpy(expected, source, sizeof expected);
        copy = variant == 0 ? make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 32u, 4u, 2u) : make_copy(GPU_PGRAPH_BLIT_FORMAT_Y8, 32u, 64u, 4u, 2u);
        copy.out_x = variant == 0 ? 3u : 16u;
        copy.out_y = variant == 0 ? 1u : 0u;
        CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, surface, sizeof surface, surface, sizeof surface, &used, &report) == GPU_PGRAPH_OK);
        reference_byte_blit(&copy, expected, expected);
        CHECK(memcmp(surface, expected, sizeof surface) == 0 && memcmp(surface, source, sizeof surface) != 0);
    }
}


/* T769, the row clamp in whole pixels and the extents a byte copy needs, each component on its own. A pitch that is not a multiple of the pixel size
 * rounds DOWN (xemu: pitch 150 over 4 byte pixels is 37 pixels, pitch 151 over 2 byte pixels is 75). The shape below gives every component of the
 * two extents a different value, so a length one byte short of either extent is refused and the exact length is accepted. */
static void test_byte_copy_extents(void)
{
    printf("test_byte_copy_extents\n");
    gpu_pgraph_copy copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8, 150u, 600u, 100u, 1u);
    CHECK(gpu_pgraph_blit_row_pixels(&copy) == 37u);
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8, 600u, 150u, 100u, 1u);
    CHECK(gpu_pgraph_blit_row_pixels(&copy) == 37u);
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_R5G6B5, 151u, 600u, 200u, 1u);
    CHECK(gpu_pgraph_blit_row_pixels(&copy) == 75u);
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_R5G6B5, 600u, 151u, 200u, 1u);
    CHECK(gpu_pgraph_blit_row_pixels(&copy) == 75u);

    gpu_pgraph_backend backend;
    memset(&backend, 0, sizeof backend);
    backend.output_groups = GPU_PGRAPH_OUTPUT_BLIT;
    backend.allowed_inferences = GPU_PGRAPH_INFER_OUTPUT_BLIT_MODEL;
    gpu_pgraph_report report;
    uint32_t used = 0u;
    static uint8_t source[1024];
    static uint8_t destination[1024];
    memset(source, 0x11, sizeof source);

    /* a pitch under one pixel clamps the row to nothing: no pixel moves, no inference is asked for (a backend that allows none still succeeds) */
    gpu_pgraph_backend bare = backend;
    bare.allowed_inferences = 0u;
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_A8R8G8B8, 3u, 64u, 8u, 2u);
    memset(destination, 0xEE, sizeof destination);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &bare, source, sizeof source, destination, sizeof destination, &used, &report) == GPU_PGRAPH_OK && used == 0u);
    CHECK(destination[0] == 0xEEu && destination[63] == 0xEEu);

    /* R5G6B5, source pitch 40 (20 pixels), destination pitch 56 (28), 30 requested: 20 pixels a row; 3 rows from (4, 2) to (6, 1) */
    copy = make_copy(GPU_PGRAPH_BLIT_FORMAT_R5G6B5, 40u, 56u, 30u, 3u);
    copy.in_x = 4u;
    copy.in_y = 2u;
    copy.out_x = 6u;
    copy.out_y = 1u;
    CHECK(gpu_pgraph_blit_row_pixels(&copy) == 20u);
    const size_t source_need = (size_t)(2u + 3u - 1u) * 40u + 4u * 2u + 20u * 2u;      /* 208 */
    const size_t destination_need = (size_t)(1u + 3u - 1u) * 56u + 6u * 2u + 20u * 2u; /* 220 */
    CHECK(source_need == 208u && destination_need == 220u);
    CHECK(gpu_pgraph_blit_extent_bytes(copy.color_format, copy.source_pitch, copy.in_x, copy.in_y, 20u, copy.height) == source_need);
    CHECK(gpu_pgraph_blit_extent_bytes(copy.color_format, copy.destination_pitch, copy.out_x, copy.out_y, 20u, copy.height) == destination_need);
    memset(destination, 0xEE, sizeof destination);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, source_need, destination, destination_need, &used, &report) == GPU_PGRAPH_OK);
    CHECK(destination[(1u * 56u) + 12u] == 0x11u && destination[(3u * 56u) + 12u + 39u] == 0x11u && destination[(3u * 56u) + 12u + 40u] == 0xEEu);
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, source_need - 1u, destination, destination_need, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "past the held bytes") != NULL); /* the reason is named, not only the result */
    CHECK(gpu_pgraph_replay_byte_copy(&copy, &backend, source, source_need, destination, destination_need - 1u, &used, &report) == GPU_PGRAPH_ERR_UNMEASURED);
    CHECK(strstr(report.error, "past the held bytes") != NULL);
}

int main(void)
{
    test_probe();
    test_read_argb();
    test_byte_surfaces();
    test_probe_pieces();
    test_read_argb_edges();
    test_byte_surface_edges();
    test_byte_copy();
    test_byte_copy_edges();
    test_byte_copy_extents();
    test_byte_copy_oracle();
    d3d8_surface_model_set_reader(NULL, NULL);
    d3d8_surface_model_reset();
    printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
