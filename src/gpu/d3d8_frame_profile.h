/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * T422: whether the presents of a long run are real frames. DEFAULT OFF, observation only.
 *
 * `d3d8_frame_queue` keeps the newest 256 presents and the GPU recording is bounded at 65536
 * commands. A steady state boot makes far more, so this summarises ALL of them, one note per present
 * (the end of Swap(4)), and keeps nothing that grows:
 *
 *   - the swap counter and the virtual vblank count at the present, and whether each rose by exactly
 *     one, stood still or fell since the previous present,
 *   - digests of the frame's GPU commands: the FULL one (an FNV-1a 64 hash over every (method, data)
 *     pair in order), a NORMALISED one with the allocator-moved fields masked (see below) and a
 *     VERTEX one over the primitive, index count and captured vertex bytes of every draw, so
 *     identical consecutive frames and the number of DISTINCT frames can be counted,
 *   - which methods differ between consecutive frames of equal length, counted over the whole run,
 *   - the first and last rows in full.
 *
 * HOW IT SEES A FRAME. It takes the recorded-commands observer at the GPU model's kick (the same seam
 * the Swap replay uses, so it is refused when the replay is on), feeds a LENIENT gpu_pgraph decoder
 * with vertex capture and the register-combiner decode on (so the unhandled list is what remains even
 * then), and at each present reads the frame, hashes it, starts the decoder's next frame
 * and releases the frame's commands from the bounded recording. Nothing else reads the recording
 * while it is on.
 *
 * NORMALISED MASK: the data of 0x1E9C and 0x1EA0 (the vertex program load and start slot) and the low
 * 24 bits of 0x1810 (DRAW_ARRAYS start index). MEASURED in the six-flag no-disc boot: consecutive
 * frames differ in exactly those four commands, each advancing like a ring cursor. Any other change
 * (a constant, a texture, a different draw) changes the normalised digest too.
 *
 * COMBINER CENSUS (T478). For every decoded draw the combiner plan is built from the draw's own state
 * (gpu_combiner_plan_build, every inference allowed, NO test texture) and the outcomes are tallied: which
 * distinct modules the draws would select and which distinct refusals (a texture register read with no texture,
 * the fog register, a configuration the translator cannot translate, no combiner word written). It says what
 * `--gpu-replay-combiner` would do with each draw without stopping at the first refusal, which the replay must.
 * T497: the same census is tallied a second time with a stand-in texture for stage 0 (a 1x1 texel, only its existence matters to
 * the planner, nothing is sampled here), every inference allowed. It says which draws `--gpu-replay-standin-texture` would let
 * plan and what inferences they need. The vertex program is not looked at (the replay refuses a draw whose program writes no oT0
 * when it draws it, which this census cannot see). The stand-in is NOT the title's texture.
 *
 * WHAT IT CANNOT SEE: texture and surface contents, and vertex bytes of a slot the decoder does not
 * capture (counted as uncaptured draws). Equal digests mean equal as far as the pushbuffer and the
 * captured vertex bytes can tell. The frame number is the guest's own present count, never a wall
 * time, so two boots cut at the same guest progress compare exactly.
 */

#ifndef TSFP_GPU_D3D8_FRAME_PROFILE_H
#define TSFP_GPU_D3D8_FRAME_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "d3d8_present.h"
#include "gpu_pgraph.h"

/* Methods the decoder saw and does not interpret, kept in full (a strict replay refuses the frame). T441: it was 64,
 * which silently cut the list once T440's five Clear words took the distinct count past it (the decoder's own table
 * holds GPU_PGRAPH_UNHANDLED_TABLE, now the same, pinned by a static assertion in the .c). */
#define D3D8_FRAME_PROFILE_UNHANDLED 128u
/* Distinct digests remembered per kind. A full table counts the overflow. */
#define D3D8_FRAME_PROFILE_DISTINCT 8192u
/* Rows kept in full at each end of the run. */
#define D3D8_FRAME_PROFILE_EDGE 8u
/* Commands of the previous frame kept for the frame to frame diff. A longer frame is not diffed. */
#define D3D8_FRAME_PROFILE_FRAME_COMMANDS 4096u
/* Distinct methods tracked in the diff table. */
#define D3D8_FRAME_PROFILE_DIFF_METHODS 64u
/* Frame numbers whose content differed from the previous frame, kept in full. */
#define D3D8_FRAME_PROFILE_CHANGES 24u
/* Content changes counted per bucket of this many frames, for the first D3D8_FRAME_PROFILE_BUCKETS. */
#define D3D8_FRAME_PROFILE_BUCKET_FRAMES 500u
#define D3D8_FRAME_PROFILE_BUCKETS 64u
/* Guest words sampled at every present (--profile-watch). */
#define D3D8_FRAME_PROFILE_WATCHES 8u
/* The first changes of each watched word, kept as (sample index, new value) so a run shows WHEN a state moved (T636). */
#define D3D8_FRAME_PROFILE_WATCH_LOG 32u

typedef struct {
    uint64_t number;
    uint32_t swap_counter;
    uint64_t vblank;
    uint64_t digest;     /* full command digest */
    uint64_t normalised; /* command digest with the allocator-moved fields masked */
    uint64_t vertex;     /* draws: primitive, index count and captured vertex bytes */
    uint64_t commands;
    uint64_t draws;
    uint64_t clears;
    uint64_t fences_inserted; /* GPU model total at this present */
    uint64_t kicks;
} d3d8_frame_profile_row;

typedef struct {
    uint32_t method;
    uint64_t frames_differing; /* frame pairs in which this method's data differed */
    uint32_t example_before;
    uint32_t example_after;
} d3d8_frame_profile_diff_method;

/* A guest dword read at every present, to show whether the state the title polls ever moves. */
typedef struct {
    uint32_t address;
    uint64_t samples;
    uint64_t unreadable;  /* presents where the read failed */
    uint64_t changes;     /* presents where the value differed from the previous sample */
    uint32_t first;
    uint32_t last;
    uint32_t minimum;
    uint32_t maximum;
    size_t logged;
    uint64_t change_sample[D3D8_FRAME_PROFILE_WATCH_LOG]; /* the sample index (present count) of each logged change */
    uint32_t change_value[D3D8_FRAME_PROFILE_WATCH_LOG];  /* the value it changed to */
} d3d8_frame_profile_watch;

/* Distinct combiner outcomes tallied (planned modules and refusals together). A full table counts the overflow. */
#define D3D8_FRAME_PROFILE_COMBINER_OUTCOMES 32u

/* One outcome of gpu_combiner_plan_build over a draw (T478), with how many draws had it. */
typedef struct {
    bool planned;           /* true: a module would be selected, false: the plan was refused */
    uint64_t draws;
    uint64_t first_frame;   /* the first present whose draw had this outcome */
    uint32_t first_draw;    /* and the draw's index in that frame */
    char text[200];         /* planned: the module, stages, texture stages, inference bits. Refused: the message */
    uint32_t words[GPU_PGRAPH_COMBINER_WORDS]; /* T497, stand-in census only: the 57 combiner words of the first draw
                                                * with this planned outcome (the definition a module is made from).
                                                * All zero for a refusal and in the T478 census */
} d3d8_frame_profile_combiner_outcome;

/* A census of gpu_combiner_plan_build over every draw (T497, the stand-in texture one; the T478 census below keeps its flat fields). */
typedef struct {
    uint64_t draws;
    uint64_t planned;
    uint64_t refused;
    uint64_t overflow;
    size_t outcome_count;
    d3d8_frame_profile_combiner_outcome outcomes[D3D8_FRAME_PROFILE_COMBINER_OUTCOMES];
} d3d8_frame_profile_census;

typedef struct {
    uint64_t frames;
    uint64_t swap_step_one;     /* swap counter rose by exactly 1 */
    uint64_t swap_step_other;   /* any other change, including no change */
    uint64_t vblank_step_one;
    uint64_t vblank_step_zero;
    uint64_t vblank_step_more;  /* rose by more than 1 */
    uint64_t vblank_fell;
    uint64_t distinct_digests;
    uint64_t distinct_normalised;
    uint64_t distinct_vertex;
    uint64_t distinct_content;  /* distinct (normalised, vertex) pairs */
    uint64_t digest_overflow;   /* digests not remembered because a table was full */
    uint64_t same_as_previous;  /* full digest equal to the previous frame's */
    uint64_t normalised_same_as_previous;
    uint64_t content_same_as_previous;
    uint64_t content_last_change; /* frame number whose content differed from the one before it */
    uint64_t content_last_new;    /* frame number that last produced a content digest not seen before */
    uint64_t content_changes;     /* how many frames differed from the one before them */
    uint64_t content_changes_per_bucket[D3D8_FRAME_PROFILE_BUCKETS];
    uint64_t content_change_frames[D3D8_FRAME_PROFILE_CHANGES]; /* the first of those frame numbers */
    uint64_t commands_min;
    uint64_t commands_max;
    uint64_t commands_total;
    uint64_t draws_min;
    uint64_t draws_max;
    uint64_t draws_total;
    uint64_t clears_total;
    uint64_t draws_uncaptured;  /* draws whose vertex bytes the decoder did not capture */
    uint64_t pairs_diffed;      /* consecutive frames of equal length that were compared */
    uint64_t pairs_length_differs;
    uint64_t pairs_not_kept;    /* a frame longer than the kept buffer */
    uint64_t commands_differing; /* total differing commands over all compared pairs */
    uint64_t diff_methods_overflow;
    size_t diff_method_count;
    d3d8_frame_profile_diff_method diff_methods[D3D8_FRAME_PROFILE_DIFF_METHODS];
    size_t unhandled_count;      /* distinct methods the lenient decoder counted and skipped, as listed */
    uint32_t unhandled_method[D3D8_FRAME_PROFILE_UNHANDLED];
    uint64_t unhandled_pairs[D3D8_FRAME_PROFILE_UNHANDLED];
    uint64_t unhandled_overflow; /* pairs of methods that did not fit the decoder's table */
    size_t watch_count;
    d3d8_frame_profile_watch watches[D3D8_FRAME_PROFILE_WATCHES];
    uint64_t combiner_draws;      /* T478: draws the census examined */
    uint64_t combiner_planned;    /* of them, how many have a plan */
    uint64_t combiner_refused;
    uint64_t combiner_overflow;   /* draws whose outcome did not fit the table */
    size_t combiner_outcome_count;
    d3d8_frame_profile_combiner_outcome combiner_outcomes[D3D8_FRAME_PROFILE_COMBINER_OUTCOMES];
    d3d8_frame_profile_census standin_census; /* T497: the same plans with a stand-in texture at stage 0 */
    bool decoder_failed;
    char decoder_error[160];
    uint64_t first_row_count;
    uint64_t last_row_count;
    d3d8_frame_profile_row first[D3D8_FRAME_PROFILE_EDGE];
    d3d8_frame_profile_row last[D3D8_FRAME_PROFILE_EDGE]; /* oldest first */
} d3d8_frame_profile_summary;

/**
 * Turn the profile on or off and clear it. Turning it on takes the recorded-commands observer and
 * returns false (and stays off) when the Swap replay is enabled or memory is refused. Quiescent only.
 */
bool d3d8_frame_profile_enable(bool enabled);
bool d3d8_frame_profile_enabled(void);

/** Sample the guest dword at `address` at every present. Up to D3D8_FRAME_PROFILE_WATCHES, kept
 * across d3d8_frame_profile_enable. False when full. Quiescent only. */
bool d3d8_frame_profile_add_watch(uint32_t address);

/** One present. Installed as the `d3d8_present_observer` by `d3d8_frame_profile_enable`, public so a
 * suite can drive it. No effect when off. */
void d3d8_frame_profile_note(const d3d8_frame_record *record);

/** The summary so far. */
d3d8_frame_profile_summary d3d8_frame_profile_get(void);

/** Print the summary. */
void d3d8_frame_profile_report(FILE *out);

#endif /* TSFP_GPU_D3D8_FRAME_PROFILE_H */
