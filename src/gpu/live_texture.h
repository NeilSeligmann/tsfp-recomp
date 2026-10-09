/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TSFP_GPU_LIVE_TEXTURE_H
#define TSFP_GPU_LIVE_TEXTURE_H
/* T792: live NV2A textures. Device-free half of the Vulkan texture path: a texture binding (Format word, Size word, Data
 * address, as the Xbox D3D header holds them) becomes a tight RGBA8 buffer (VK_FORMAT_R8G8B8A8_UNORM, row 0 on top) plus an
 * upload plan. Guest writes invalidate by range, render target textures resolve to a registered target (sampled in place,
 * never copied), and every texture binding that cannot be sampled exactly is refused BY NAME and counted. No stand-in image.
 * The Format word layout, the swizzled DXT1 decode and the T510 render target rules are the ones of d3d8_swap_replay.c
 * (T632, T633, docs/t736-xemu-surfaces.md): the DXT1 decode lives here and the replay calls it. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIVE_TEXTURE_MAX_DIMENSION 2048u
#define LIVE_TEXTURE_MAX_LEVELS 12u
#define LIVE_TEXTURE_CACHE_ENTRIES 256u /* T1255: was 64, a frame binds up to 75 distinct textures (the cutscene), LRU over a cycle longer than the cache misses every bind */
#define LIVE_TEXTURE_TARGETS 16u
#define LIVE_TEXTURE_CENSUS_ENTRIES 32u
#define LIVE_TEXTURE_DETAIL_BYTES 200u

typedef enum {
    LIVE_TEXTURE_KIND_NONE = 0,
    LIVE_TEXTURE_Y8, LIVE_TEXTURE_AY8, LIVE_TEXTURE_A1R5G5B5, LIVE_TEXTURE_X1R5G5B5, LIVE_TEXTURE_A4R4G4B4,
    LIVE_TEXTURE_R5G6B5, LIVE_TEXTURE_A8R8G8B8, LIVE_TEXTURE_X8R8G8B8,
    LIVE_TEXTURE_DXT1, LIVE_TEXTURE_DXT3, LIVE_TEXTURE_DXT5,
    LIVE_TEXTURE_A8, LIVE_TEXTURE_A8Y8, LIVE_TEXTURE_G8B8, LIVE_TEXTURE_R8B8
} live_texture_kind;

typedef struct {
    uint32_t header; /* guest resource header identity; 0 for synthetic bindings without a header */
    uint32_t format; /* header Format word */
    uint32_t size_word; /* header Size word: 0 for a swizzled texture, else (w-1) | (h-1)<<12 | (pitch/64-1)<<24 */
    uint32_t data;   /* 28 bit physical Data address */
    uint32_t mip_limit; /* 0: declared chain; otherwise Control0 maxLOD + 1 */
} live_texture_binding;

typedef struct {
    uint32_t width, height;
    uint32_t source_offset, source_bytes;
    uint32_t rgba_offset, rgba_bytes;
} live_texture_level;

typedef struct {
    bool ok;
    live_texture_kind kind;
    uint32_t color_byte;
    uint32_t width, height;
    uint32_t pitch;         /* source row pitch in bytes (linear only) */
    uint32_t source_bytes;  /* bytes read from guest memory across active levels */
    uint32_t rgba_bytes, levels;
    live_texture_level mip[LIVE_TEXTURE_MAX_LEVELS];
    bool swizzled, compressed, measured;
    /* T1490: a cube map (Format bit 2, INFERRED). Six square faces +X -X +Y -Y +Z -Z (xemu pgraph/gl/texture.c upload order), each a full mip
     * chain at `face_source_stride` bytes in guest memory (the chain length rounded up to 128, NV2A_CUBEMAP_FACE_ALIGNMENT) and at
     * `face_rgba_bytes` in the RGBA buffer (face f at f * face_rgba_bytes, mip offsets within a face). `rgba_bytes` is the sum over faces,
     * `source_bytes` ends at the last face's last mip (no trailing pad). 1 face, strides 0 for every other texture. */
    bool cube;
    uint32_t faces, face_source_stride, face_rgba_bytes;
    const char *refusal;    /* category when !ok, a stable string, never NULL when !ok */
    char detail[LIVE_TEXTURE_DETAIL_BYTES];
} live_texture_plan;

/* Plan a binding. `allow_inferred` admits the formats no xemu fixture measured yet (everything except swizzled DXT1 0x0C and
 * linear A8R8G8B8 0x12); without it they refuse as "inferred format". */
bool live_texture_plan_binding(const live_texture_binding *binding, bool allow_inferred, live_texture_plan *plan);
/* Decode `source` (plan->source_bytes long) into plan->rgba_bytes packed mip RGBA bytes.
 * Level0 remains first. False when any source level is short. */
bool live_texture_decode(const live_texture_plan *plan, const uint8_t *source, size_t source_length, uint8_t *rgba);
/* Offset of texel/block (x, y) of a swizzled width x height grid (both powers of two): the Morton interleave of the
 * shorter side then the remaining bits of the longer one above (the replay's rule, square case byte-identical). */
uint32_t live_texture_swizzle_offset(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
/* Square DXT1 with row-major compressed blocks, `size` texels a side; source is size/4 * size/4 * 8 bytes. */
void live_texture_decode_dxt1_square(const uint8_t *blocks, uint32_t size, uint8_t *rgba);

/* --- cache, dirty tracking, census ------------------------------------------------------------------------------ */
typedef bool (*live_texture_reader)(void *context, uint32_t address, void *out, size_t bytes);

/* Called only for guest bytes, after exact render-target resolution. */
typedef bool (*live_texture_resolver)(void *context, const live_texture_binding *binding,
                                     uint32_t bytes, uint32_t *address, uint64_t *identity,
                                     const char **refusal);

typedef struct {
    bool in_use, valid; /* valid: rgba holds the current decode of the guest bytes */
    live_texture_binding binding;
    live_texture_plan plan;
    uint8_t *rgba;
    uint8_t *source; /* exact backing-byte snapshot for resolved guest resources */
    uint32_t read_address;
    uint64_t backing_identity;
    uint32_t generation;          /* bumps at every (re)decode */
    uint32_t uploaded_generation; /* what the Vulkan image holds, 0 never */
    uint64_t last_use;
    uint64_t checked_serial; /* T1255: the input_serial at which `source` was last compared with the frame's captured bytes */
} live_texture_entry;

typedef struct {
    bool resolved, used;
    uint32_t data, width, height, pitch, id;
    bool swizzled; /* T1489: a swizzled render target (Morton layout, drawn in picture order by the image, sampled as a swizzled texture only) */
} live_texture_target;

typedef struct {
    const char *category;
    uint32_t format, size_word, data;
    uint64_t count;
    char detail[LIVE_TEXTURE_DETAIL_BYTES];
} live_texture_census_entry;

typedef struct {
    bool allow_inferred;
    live_texture_entry entries[LIVE_TEXTURE_CACHE_ENTRIES];
    live_texture_target targets[LIVE_TEXTURE_TARGETS];
    live_texture_census_entry census[LIVE_TEXTURE_CENSUS_ENTRIES];
    size_t census_count;
    uint64_t census_overflow, clock;
    uint64_t hits, decodes, invalidations, refusals, target_samples, writes_noted;
    /* T1255: nonzero while lookups read one immutable frame capture (a frame job): a resolved binding already compared with that
     * capture is not read and compared again for the next draw that binds it. 0 (default) compares every lookup. */
    uint64_t input_serial;
    uint64_t compares_skipped;
} live_texture_cache;

typedef enum { LIVE_TEXTURE_SOURCE_REFUSED = 0, LIVE_TEXTURE_SOURCE_GUEST, LIVE_TEXTURE_SOURCE_TARGET } live_texture_source;

typedef struct {
    live_texture_source source;
    const uint8_t *rgba;
      /* GUEST: tight RGBA8, owned by the cache, valid until the next lookup of another binding */
    uint32_t width, height, levels;
    bool cube;                /* GUEST: the image is a six layer cube map (T1490) */
    uint32_t generation;
    bool needs_upload;        /* GUEST: the Vulkan image is missing or older than `generation` */
    uint32_t upload_bytes;    /* packed RGBA byte count of all active levels when needs_upload */
    uint32_t entry;           /* GUEST: index to hand to live_texture_mark_uploaded */
    uint32_t target_id;       /* TARGET: id given to live_texture_register_target */
    bool target_swizzled;     /* TARGET: the target is swizzled, so the stage takes normalised coordinates (T1489) */
    const char *refusal;      /* REFUSED */
    char detail[LIVE_TEXTURE_DETAIL_BYTES];
} live_texture_result;

void live_texture_cache_init(live_texture_cache *cache, bool allow_inferred);
void live_texture_cache_free(live_texture_cache *cache);
/* The guest wrote [address, address + bytes): every cached texture whose source bytes overlap is stale. Returns the count. */
size_t live_texture_note_write(live_texture_cache *cache, uint32_t address, size_t bytes);
/* A render target this frame draws into (T793 owns the image): linear A8R8G8B8 of the given Data/size/pitch. */
bool live_texture_register_target(live_texture_cache *cache, uint32_t id, uint32_t data, uint32_t width, uint32_t height, uint32_t pitch);
/* T1489: a swizzled A8R8G8B8 target (pitch = width * 4). A swizzled texture header of the same size over its Data samples the image
 * itself (the surface and the texture share the Morton layout), a linear header over it is refused (a conversion no measurement covers). */
bool live_texture_register_swizzled_target(live_texture_cache *cache, uint32_t id, uint32_t data, uint32_t width, uint32_t height);
void live_texture_clear_targets(live_texture_cache *cache);
/* Resolve one texture binding for a draw: a decoded guest texture (re-decoded only if invalidated), a render target, or a
 * named refusal that is counted in the census. */
void live_texture_lookup_resolved(live_texture_cache *cache, const live_texture_binding *binding,
                                  live_texture_reader reader, void *context,
                                  live_texture_resolver resolver, void *resolver_context,
                                  live_texture_result *result);
void live_texture_lookup(live_texture_cache *cache, const live_texture_binding *binding, live_texture_reader reader,
    void *context, live_texture_result *result);
void live_texture_mark_uploaded(live_texture_cache *cache, uint32_t entry, uint32_t generation);
/* Census line n ("refused texture binding: <category> Format 0x.. Size 0x.. Data 0x.. xN: detail"), false past the end. */
bool live_texture_census_line(const live_texture_cache *cache, size_t index, char *out, size_t out_bytes);
#endif
