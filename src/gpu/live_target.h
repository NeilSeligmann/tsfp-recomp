/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T793, live render targets, CopyRects blits and the present schedule: the part that needs no swapchain.
 * Device free, so it is testable headless and is checked against the replay's own blit (gpu_pgraph_replay_copy,
 * the oracle) and the surface rules the replay uses (d3d8_surface.h). The Vulkan side (VkImage per target,
 * vkCmdCopyImage, composition on the swapchain) consumes these plans and is NOT here: it needs T790's device layer.
 *
 * THREE PIECES
 *  1. A persistent target registry. A target is named by its surface header Data word (the physical address, the
 *     name a 2D blit gives it) and is decoded from the header's Format and Size words by the same rules as
 *     d3d8_surface_dimensions: a Size word means linear (width - 1 in bits 0-11, height - 1 in 12-23, pitch
 *     ((bits 24-31) + 1) * 64), none means swizzled with the log2 width and height in the Format word's bits 20-23
 *     and 24-27 (the height exponent INFERRED, as there). Each target has a generation that every write bumps, so
 *     the texture side (T792) can invalidate, and an optional CPU image, the headless stand-in for the VkImage.
 *  2. A CopyRects blit planner (live_target_plan_blit) with the xemu-level rules of HQ58 (T736, never NV2A silicon):
 *     SRCCOPY copies row by row in ASCENDING order with a buffered (memmove) row, so a destination 3 pixels right of
 *     the source in one row is exact and 3 rows below smears with period 3; a row wider than the narrower pitch is
 *     clamped to it (measured with both offsets 0, anything else refused as unmeasured); a zero width or height moves
 *     nothing; colour format 0xA keeps alpha, 7 forces 0xFF and 6 forces 0. R5G6B5 (4) and Y8 (1) are byte copies
 *     that are NOT measured (T769): INFERRED, refused unless LIVE_TARGET_INFER_BYTE_FORMATS is allowed (opt-in until
 *     M4 measures it). The plan says whether a plain vkCmdCopyImage is valid (no overlap, no alpha patch, A8R8G8B8)
 *     or a staged copy is needed.
 *  3. The present schedule: Swap(4) queues the front buffer (d3d8_frame_record's Data, Format, Size, vblank and
 *     interval), and each modelled vblank shows the NEWEST eligible one. A present is eligible `latency` vblanks
 *     after the vblank it was made at (default 1, the one-vblank-late INFERRED of present_video_sink, T540) and no
 *     sooner than `interval` vblanks (0 counts as 1, the scanout is vblank locked, INFERRED) after the last shown.
 *     Older ones it replaces are counted as superseded. With nothing new the last front is held. A movie overlay
 *     (the T760 sink) is composed ABOVE the front on the same swapchain: the layer list is [front, overlay].
 *
 * Every refusal is counted by reason (the census), never guessed and never silently skipped.
 */

#ifndef TSFP_GPU_LIVE_TARGET_H
#define TSFP_GPU_LIVE_TARGET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LIVE_TARGET_MAX_TARGETS 64u
#define LIVE_TARGET_MAX_EDGE 4096u
#define LIVE_TARGET_INFER_BYTE_FORMATS 0x1u /* R5G6B5 and Y8 blits are byte copies (INFERRED, T769) */

/* Blit colour format codes (gpu_pgraph.h GPU_PGRAPH_BLIT_FORMAT_*, plus 6 and 7 of HQ58). */
#define LIVE_BLIT_FORMAT_Y8 0x01u
#define LIVE_BLIT_FORMAT_R5G6B5 0x04u
#define LIVE_BLIT_FORMAT_X8R8G8B8_ALPHA0 0x06u
#define LIVE_BLIT_FORMAT_X8R8G8B8_ALPHAFF 0x07u
#define LIVE_BLIT_FORMAT_A8R8G8B8 0x0Au
#define LIVE_BLIT_OPERATION_SRCCOPY 3u

typedef enum {
    LIVE_TARGET_OK = 0,
    LIVE_TARGET_REFUSE_ARGUMENT,
    LIVE_TARGET_REFUSE_FORMAT_UNKNOWN,     /* a surface Format byte outside the measured colour set */
    LIVE_TARGET_REFUSE_SIZE_UNKNOWN,       /* no Size word and no exponents, or an edge past the limit */
    LIVE_TARGET_REFUSE_REGISTRY_FULL,
    LIVE_TARGET_REFUSE_NO_SOURCE,          /* the blit names a Data word no target has */
    LIVE_TARGET_REFUSE_NO_DESTINATION,
    LIVE_TARGET_REFUSE_REDECLARED,         /* a header re-registered with other geometry or format */
    LIVE_TARGET_REFUSE_SWIZZLED,           /* a 2D blit of a swizzled surface, addressing not measured */
    LIVE_TARGET_REFUSE_OPERATION,          /* an operation other than SRCCOPY */
    LIVE_TARGET_REFUSE_BLIT_FORMAT,        /* a blit colour code outside 1, 4, 6, 7, 0xA */
    LIVE_TARGET_REFUSE_BLIT_FORMAT_MISMATCH, /* the blit's colour format and the surface's bytes per pixel differ */
    LIVE_TARGET_REFUSE_INFERRED_BYTE_FORMAT, /* R5G6B5 or Y8 without LIVE_TARGET_INFER_BYTE_FORMATS */
    LIVE_TARGET_REFUSE_PITCH,              /* the blit's pitch is not the registered target's pitch */
    LIVE_TARGET_REFUSE_OUT_OF_BOUNDS,      /* a rectangle row or column past the surface (clamping beyond the pitch rule unmeasured) */
    LIVE_TARGET_REFUSE_OFFSET_CLAMP,       /* a clamped row with a non-zero x offset: unmeasured */
    LIVE_TARGET_REFUSE_PRESENT_UNREGISTERABLE, /* the front buffer's header words do not decode */
    LIVE_TARGET_REFUSE_COUNT
} live_target_refusal;

typedef enum {
    LIVE_TARGET_FORMAT_A8R8G8B8 = 1,
    LIVE_TARGET_FORMAT_X8R8G8B8 = 2,
    LIVE_TARGET_FORMAT_R5G6B5 = 3
} live_target_format;

typedef struct {
    uint32_t data;           /* header Data word, the target's name */
    uint32_t format_word;
    uint32_t size_word;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;          /* bytes, linear: from the Size word, swizzled: width * bytes per pixel */
    uint32_t bytes_per_pixel;
    live_target_format format;
    bool swizzled;
} live_target_desc;

typedef struct {
    uint32_t source_data;
    uint32_t destination_data;
    uint32_t color_format;   /* LIVE_BLIT_FORMAT_* */
    uint32_t source_pitch;
    uint32_t destination_pitch;
    uint32_t in_x, in_y, out_x, out_y;
    uint32_t width;          /* pixels of the colour format, as the SIZE word gave them */
    uint32_t height;
    uint32_t operation;
} live_target_blit;

typedef enum { LIVE_ALPHA_KEEP = 0, LIVE_ALPHA_FORCE_FF = 1, LIVE_ALPHA_FORCE_00 = 2 } live_alpha_mode;

typedef struct {
    bool empty;              /* zero width or height: nothing moves, no inference */
    uint32_t rows;
    uint32_t row_bytes;      /* after the narrower-pitch clamp */
    uint32_t source_offset;  /* byte offset of the first row in the source image */
    uint32_t destination_offset;
    uint32_t source_pitch;
    uint32_t destination_pitch;
    uint32_t bytes_per_pixel;
    bool clamped;            /* the row was cut to the narrower pitch */
    bool overlap;            /* same surface, byte columns and rows both intersect: ordered copy, rows ascending */
    bool same_surface;
    live_alpha_mode alpha;
    bool inferred;           /* a byte format blit: needs LIVE_TARGET_INFER_BYTE_FORMATS */
    bool copy_image_ok;      /* a plain vkCmdCopyImage of an A8R8G8B8 region is valid for this plan */
    uint32_t destination_data;
} live_target_blit_plan;

typedef struct live_target_registry live_target_registry;

typedef struct {
    uint64_t registered;
    uint64_t blits_planned;
    uint64_t blits_applied;   /* run on CPU images */
    uint64_t blits_empty;
    uint64_t blits_clamped;
    uint64_t blits_overlapped;
    uint64_t blits_refused;
    uint64_t refusals[LIVE_TARGET_REFUSE_COUNT];
} live_target_stats;

const char *live_target_refusal_name(live_target_refusal reason);

/** Decode a header's Format and Size words. Refusal NONE (OK) on success. Pure. */
live_target_refusal live_target_desc_from_words(uint32_t data, uint32_t format_word, uint32_t size_word,
                                                live_target_desc *out);

live_target_registry *live_target_registry_create(void);
void live_target_registry_destroy(live_target_registry *registry);
size_t live_target_registry_count(const live_target_registry *registry);
live_target_stats live_target_registry_stats(const live_target_registry *registry);

/** Register or confirm a target by header words. `keep_pixels` allocates a zeroed CPU image the first time. A
 * second registration of the same Data word with other geometry or format is REFUSED (REDECLARED), not replaced. */
live_target_refusal live_target_register(live_target_registry *registry, uint32_t data, uint32_t format_word,
                                         uint32_t size_word, bool keep_pixels);
const live_target_desc *live_target_find(const live_target_registry *registry, uint32_t data);
/** Writes since registration (blits and live_target_note_written). 0 for an unknown target. */
uint64_t live_target_generation(const live_target_registry *registry, uint32_t data);
/** A draw or an external write changed the target. */
void live_target_note_written(live_target_registry *registry, uint32_t data);
uint8_t *live_target_pixels(live_target_registry *registry, uint32_t data);

/** Plan a CopyRects blit (HQ58 rules above). Counts the census. `allowed` is LIVE_TARGET_INFER_*. */
live_target_refusal live_target_plan_blit(live_target_registry *registry, const live_target_blit *blit,
                                          uint32_t allowed, live_target_blit_plan *plan);
/** The reference executor: rows ascending, one memmove per row, then the alpha patch. Both images use their own pitch. */
void live_target_execute_plan(const live_target_blit_plan *plan, const uint8_t *source, uint8_t *destination);
/** Plan and, when both targets keep a CPU image, execute and bump the destination's generation. */
live_target_refusal live_target_apply_blit(live_target_registry *registry, const live_target_blit *blit, uint32_t allowed,
                                     live_target_blit_plan *plan, bool *executed);

/* --- present schedule --- */
#define LIVE_PRESENT_QUEUE 8u
#define LIVE_PRESENT_DEFAULT_LATENCY 1u

typedef struct {
    uint64_t number;         /* d3d8_frame_record.number */
    uint32_t interval;
    uint64_t vblank;         /* the modelled vblank count at the Swap */
    uint64_t media_time_ns;  /* set only by the playback-mode timestamped queue */
    bool media_timed;
    uint32_t data, format_word, size_word;
} live_present_event;

typedef enum { LIVE_LAYER_FRONT = 1, LIVE_LAYER_OVERLAY = 2 } live_layer_kind;

typedef struct {
    bool front_new;          /* a new front buffer became visible at this vblank */
    bool front_held;         /* the previous front is shown again */
    uint32_t front_data;
    uint64_t front_number;
    uint32_t layer_count;    /* 0 (black), 1 or 2 */
    live_layer_kind layers[2]; /* bottom to top */
} live_present_frame;

typedef struct {
    live_present_event queue[LIVE_PRESENT_QUEUE];
    uint32_t count;
    uint32_t latency;
    bool shown_any;
    uint64_t last_shown_vblank;
    uint32_t last_data;
    uint64_t last_number;
    uint64_t submitted, shown, superseded, held, overlay_frames, dropped_full;
} live_present_schedule;

void live_present_init(live_present_schedule *schedule, uint32_t latency);
/** Queue the Swap's front buffer; registers its target. Refused when the header does not decode. */
live_target_refusal live_present_submit(live_present_schedule *schedule, live_target_registry *registry,
                                        const live_present_event *event);
/** One modelled vblank (`vblank` rising). Fills the frame composition. */
void live_present_vblank(live_present_schedule *schedule, uint64_t vblank, bool overlay_active,
                         live_present_frame *frame);
/** Playback-mode variant: only timestamped events at or before the audio media clock may be released. */
void live_present_media_vblank(live_present_schedule *schedule, uint64_t vblank, bool overlay_active,
                               bool media_clock_valid, uint64_t media_time_ns, live_present_frame *frame);
/** Queue a Swap's front buffer with the modelled guest nanosecond used by playback pictures. */
live_target_refusal live_present_submit_media(live_present_schedule *schedule, live_target_registry *registry,
                                              const live_present_event *event);

#endif /* TSFP_GPU_LIVE_TARGET_H */
