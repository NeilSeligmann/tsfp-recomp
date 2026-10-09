/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Swap, replayed (T84a): at each present the commands the instantaneous GPU recorded since the
 * previous one are decoded by gpu_pgraph (T84) and replayed through gpu_vsh_draw into an RGBA frame
 * on a real Vulkan device. OPT-IN AND OFF BY DEFAULT: until d3d8_swap_replay_enable succeeds,
 * d3d8_swap does exactly what it did, no device is opened and nothing is decoded, so existing
 * behaviour and boot traces are unchanged.
 *
 * WHEN THE COMMANDS ARE DECODED, AND SO WHEN VERTEX BYTES ARE READ (T262). The instantaneous GPU runs a
 * range of the ring at the kick (d3d8_gpu.h) and records it. With the replay enabled the recording is
 * decoded right there, from d3d8_gpu_set_recorded_observer, and the decoder copies each draw's vertex
 * bytes out of guest memory when it sees the draw's BEGIN_END(0) (gpu_pgraph_set_vertex_capture). So
 * the bytes a draw replays are the guest memory at the kick that handed the GPU the end of that draw:
 *   - a buffer the title rewrites AFTER that kick (the next draw of the frame, or a later frame), or an
 *     earlier render target pass's vertices reused later, cannot change what the draw replays;
 *   - draws that end inside ONE kick all see the memory as it is at that kick. Two draws kicked together
 *     with the buffer rewritten between them in the guest's program order replay the LATER bytes for both.
 *     That is the instantaneous GPU's own ordering (it runs them together, after the CPU wrote both), and
 *     real hardware would have raced the CPU there, so a title cannot depend on it. NOT MEASURED against
 *     the title: how often the title kicks between draws is whatever the ported library's ring segments
 *     and fences do, which is why the ordering is stated, not assumed to be one draw per kick;
 *   - commands recorded BEFORE the replay was enabled are decoded at the first hook after, so their bytes
 *     are read then (stale by however long enabling was delayed).
 * `vertex_budget_bytes` bounds the frame's snapshot pool, a frame needing more is refused loudly; 0
 * turns the snapshot off and the replay reads vertex bytes at the present, as before T262.
 *
 * ONE MODEL PER RUN, ONE DRAW LIST PER FRAME. Hardware state persists across a present (the program
 * file, the constants, the vertex arrays), so one gpu_pgraph model decodes the whole run and
 * gpu_pgraph_begin_frame clears only the draw list (after the frame was replayed). A frame is the
 * recorded commands between two Swap(4) calls (the second call of the title's pair is the one that records a frame,
 * d3d8_present.h). The recording is released after it is decoded so it never fills.
 *
 * d3d8_gpu_reset (CreateDevice, T84a3). The reset clears the recording, so a model decoded from the
 * old one and any pending render target switches are stale. The next present or render target hook
 * after a reset REBUILDS the model (a new, empty gpu_pgraph with the same strictness), forgets the
 * switches and the decode position, and CLEARS the latch (it described the old model). Counters
 * accumulate across it (`model_resets` counts it). The device, the module cache and the last images
 * stay. No disable and enable is needed.
 *
 * THE RENDER TARGET SIZE. The recorded stream carries no SET_SURFACE_* (the emitters that write
 * them are elided, d3d8_gpu_note_elided), so the size cannot come from the stream. It comes from
 * the render target's header (device+0x1A04's Size word, or its Format word exponents when
 * swizzled), measured when the target is bound (below) or, when the frame never switched, at the
 * present. An explicit `width` and `height` in the config override it, for a frame with ONE target.
 *
 * PER-TARGET PASSES (T84a1). Nothing in the stream says which target a draw went to, so
 * SetRenderTarget calls d3d8_swap_replay_on_render_target, which notes a switch (recording index, the
 * number of draws decoded so far, both targets, both sizes). The frame's draw list is split at the
 * switches (T262: it used to be decoded in segments at the present, now it is decoded once, at the
 * kicks, and replayed by draw range) and each segment with draws is replayed into its own image of ITS
 * target's size. The presented frame is the pass of the target
 * bound at the present, the other passes are offscreen images (d3d8_swap_replay_offscreen_frame).
 * Refused loudly: a segment whose target's size is not measured, no target bound, one target
 * drawn in two separate passes (the replay cannot draw onto an existing image), an explicit size
 * with more than one target, a bracket open at a switch, more than 64 switches, and a chain of
 * switches that does not end at the target bound at the present.
 *
 * STRICT MODE (default on). A method outside gpu_pgraph's measured list refuses the frame, and the
 * refusal is loud: it is logged through d3d8_hle_log with the method, the pair index and the frame,
 * counted, and (optionally) fatal. A refused frame LATCHES the replay: the model is mid-stream, so
 * every later frame is counted as skipped and says why, never replayed from a broken model. With
 * strict off, unhandled methods are still logged once per method and counted, and the draws replay
 * with that state ignored.
 *
 * The SPIR-V modules are read from one directory, `<name>.spv`, where the name is the table's module
 * name (`static_<sha256>` or `generated_<sha256>`, tools/nv2a/vsh_modules.py writes them to
 * generated/shaders/vsh/spv). The selector table is built from the directory listing, so no
 * generated header is needed at build time.
 */

#ifndef TSFP_GPU_D3D8_SWAP_REPLAY_H
#define TSFP_GPU_D3D8_SWAP_REPLAY_H

#include "gpu_standin_pattern.h"
#include "gpu_standin_units.h"
#include "gpu_device.h"
#include "gpu_pgraph_replay.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 64 MiB: a frame of a few thousand draws of a few thousand vertices fits several times over. */
#define D3D8_SWAP_REPLAY_DEFAULT_VERTEX_BUDGET (64u * 1024u * 1024u)

/* T497. The one combiner inference a stand-in texture needs beyond the combiner option's own (CONSTANT_BYTES and
 * COLOUR_RANGE): the stand-in is sampled nearest, clamped, row 0 on top (TEXTURE_SAMPLING). MEASURED on the title's
 * steady loop: the stream writes the texture stage program (word 54, method 0x1E70) itself, as mode 1 for the draws
 * that read t0, so STAGE_PROGRAM (a stage program the stream never wrote) is NOT allowed: a stream without it
 * refuses a texture read by name, stand-in or not. Not in GPU_PGRAPH_INFER_ALL. Allowed by
 * `--gpu-replay-standin-texture` and by nothing else. */
#define D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING
/* T510. The inferences a render target texture needs: the combiner's own TEXTURE_SAMPLING (a texture is sampled, clamped) and
 * GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE (the earlier pass's image is the surface the hardware samples, its texels addressed in texel
 * units, bilinear). Allowed by `--gpu-replay-rt-texture` and by nothing else. */
#define D3D8_SWAP_REPLAY_INFER_RT_TEXTURE \
    (GPU_PGRAPH_INFER_OUTPUT_TEXTURE_BRIDGE | GPU_PGRAPH_INFER_COMBINER_TEXTURE_SAMPLING)
/* T633 (1) and T596. The inference a surface with no pass of its frame needs (kept image, else guest memory) beyond the render target texture's own and
 * the blit model: GPU_PGRAPH_INFER_OUTPUT_SURFACE_SOURCE. Allowed by `--gpu-replay-surface-source` and by nothing else. */
#define D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE GPU_PGRAPH_INFER_OUTPUT_SURFACE_SOURCE
/* T633 (2). Allowed by `--gpu-replay-target-persist` and by nothing else. */
#define D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST GPU_PGRAPH_INFER_OUTPUT_TARGET_PERSIST
/* T510. The only texture Format word the bridge takes: A8R8G8B8 linear (hardware 0x12), 2D, one level, DMA 1, border colour, measured
 * on the title's back buffer header (docs/d3d8-copy-composition.md). */
#define D3D8_SWAP_REPLAY_RT_TEXTURE_FORMAT 0x00011229u
/* T510. SetTexture bindings noted per frame, and the distinct census lines kept. */
#define D3D8_SWAP_REPLAY_TEXTURE_EVENTS 4096u
#define D3D8_SWAP_REPLAY_TEXTURE_CENSUS 16u
/* T578. With the BLIT output group the replay keeps the last image of up to this many surfaces (by their Data word, so a blit
 * can read a surface an earlier frame drew and write one no pass of its frame drew into). Beyond it the oldest is evicted and
 * counted. Nothing is kept without the group. */
#define D3D8_SWAP_REPLAY_SURFACES 16u
/* The edge of a stand-in texture, in texels (the combiner planner refuses anything outside 1..4096). */
#define D3D8_SWAP_REPLAY_STANDIN_MAX_EDGE 4096u

typedef struct {
    const char *spv_directory;    /* required: the directory of <module>.spv files, as given */
    const char *dump_directory;   /* NULL: no dump. Else frame_<number>.png per replayed frame */
    const char *device_selector;  /* gpu_device_create_selected; NULL uses $VKRUN_DEVICE, else first */
    uint32_t width;               /* 0 with 0: measured from the bound render target */
    uint32_t height;
    uint32_t allowed_inferences;  /* GPU_PGRAPH_INFER_* bits a draw may depend on */
    bool viewport_inverse_modules; /* the directory holds --undo-viewport modules (T96) */
    bool window_clip_modules;      /* directory holds --window-to-clip modules (T714 xemu-level none class;
                                    * z-only xy remains open, HQ21) */
    bool viewport_from_target;     /* T477: infer c58/c59 from measured target size plus the measured
                                     * D3D emitter map. The register feed remains T96 inference. */
    bool flip_y;                  /* gpu_pgraph_backend.flip_y (T100f): reverse the rows of each frame. Which
                                   * orientation the NV2A has is UNDECIDED (T100f), so the default is off */
    float line_width;             /* gpu_pgraph_backend.line_width (T84b): 0 (default) = unstated, a line is
                                   * drawn one pixel wide and the replay needs INFER_LINE_WIDTH, exactly the
                                   * earlier behaviour. 1.0 = stated, needs no inference. Anything else is
                                   * refused by enable: the device has no wideLines, only 1.0 is defined */
    size_t vertex_budget_bytes;   /* T262: per-frame cap on the vertex snapshot pool (default
                                   * D3D8_SWAP_REPLAY_DEFAULT_VERTEX_BUDGET in the default config). A frame
                                   * over it is REFUSED, nothing is truncated. 0 = no snapshot: vertex
                                   * bytes are read at the present, as before. A config built by hand
                                   * (not from the default) therefore keeps the old behaviour */
    bool strict;
    bool visibility; /* T998: ordered real query events, live renderer opt-in only. */                  /* refuse unhandled methods (default true) */
    bool fatal_on_refusal;        /* d3d8_hle_fatal at the first refusal instead of latching */
    float clear[4];               /* the frame's clear colour (default opaque black) */
    bool combiner;                /* T478: replace the fixed fragment stage (oD0 unchanged) with the register combiner
                                   * the stream programmed (T75): the model decodes the 57 combiner words and each draw
                                   * selects its `combiner_<sha256>.spv` module from the SAME directory as the vertex
                                   * modules (names beginning `combiner_`, never offered as vertex programs). Default
                                   * off: the words stay unhandled, strict refuses them and the fragment stage is the
                                   * vertex colour, byte for byte as before. enable refuses it when the directory holds
                                   * no combiner module. The combiner's inferences (GPU_PGRAPH_INFER_COMBINER_*, not in
                                   * GPU_PGRAPH_INFER_ALL) are still allowed or refused by `allowed_inferences` */
    bool standin_texture;         /* T497: ONE stand-in texture for texture stage 0 of the combiner, wired to
                                   * `gpu_pgraph_backend.test_textures[0]`. The title's own textures are not decoded
                                   * (T480), so a draw whose combiner reads t0 is refused ("no test texture was supplied
                                   * for stage 0") and the refusal latches the replay. With this on those draws read a
                                   * `standin_width` x `standin_height` texture of `standin_pattern` (solid `standin_rgba` by default)
                                   * instead. IT IS NOT THE TITLE'S TEXTURE and every frame that sampled it says so
                                   * (d3d8_swap_replay_standin_summary). Needs `combiner` (enable refuses it otherwise)
                                   * and the inference D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE in `allowed_inferences`
                                   * (a draw that needs one that is not allowed is refused by name). The stage program
                                   * must be the stream's own: a stand-in does not stand in for word 54. The texture
                                   * coordinate is whatever the vertex program wrote to oT0: a program that writes none
                                   * is refused, naming the varyings, never given a coordinate. Default off */
    bool standin_texel_units;     /* T713: sample the stand-in with oT0 in TEXELS (the T510 unnormalised path), INFERRED; needs `standin_texture` */
    gpu_standin_unit_rule standin_unit_rules[GPU_STANDIN_UNIT_RULES]; /* T719: per-draw unit by vertex program digest, INFERRED,
                                   * needs `standin_texture`, refused with `standin_texel_units`. A draw that samples the stand-in
                                   * with a program in no rule refuses the frame */
    uint32_t standin_unit_rule_count;
    const char *draw_dump_path;   /* T724: append a JSON line per draw (gpu_pgraph_backend.draw_dump_path), NULL off */
    uint32_t standin_width;       /* 1..D3D8_SWAP_REPLAY_STANDIN_MAX_EDGE, only read when `standin_texture` */
    uint32_t standin_height;
    gpu_standin_pattern standin_pattern; /* default solid; opt-in checker exposes varying UV */
    uint8_t standin_rgba[4];      /* the texel: R, G, B, A */
    bool render_target_texture;   /* T510: a texture stage bound to a render target header the replay already drew in an EARLIER pass of
                                   * the same frame is sampled from that pass's image (the combiner's RGBA texture interface), instead of
                                   * being refused. Needs `combiner`, the TEXTURE output group and D3D8_SWAP_REPLAY_INFER_RT_TEXTURE, and is
                                   * exclusive with `standin_texture` and `flip_y` (enable refuses all of them by name). SetTexture is
                                   * observed (d3d8_swap_replay_on_texture): the binding at each draw is the header, Data, Format and Size
                                   * words as they were at the call. A draw whose combiner reads a stage with no earlier image, an unsupported
                                   * Format, a partial alias, an ambiguous mapping, the target it draws into, a producer later in the frame,
                                   * an address mode that is not clamp or a filter other than the measured 0x02062000 is REFUSED BY NAME,
                                   * never a white or default texture, never a skipped draw. Default off */
    bool render_target_texture_census; /* T510: with `render_target_texture`, CENSUS ONLY: at each present classify every draw by what its
                                   * texture stage would resolve to or why it is refused (the same per stage resolution the replay runs),
                                   * replay NO pixel, latch nothing, so one run counts every draw past the first refusal. The result is
                                   * d3d8_swap_replay_texture_census_at. Default off */
    bool live_only;               /* T838: the M9 live renderer draws the frames (d3d8_swap_replay_set_live_hook), so the CPU replay is
                                   * skipped: every present decodes the model, hands it to the live hook and discards the frame, no
                                   * Vulkan device of the replay is opened. Needs a live hook at the present (without one nothing is
                                   * drawn). Default off */
    bool surface_source;          /* T633 (1) and T596, OPT-IN, the rule MEASURED in xemu (T736, xemu-level), the timing INFERRED: the surface a texture stage samples or a blit
                                   * reads or writes, when no pass of the frame drew it (the pass-written image is still taken first), is its CURRENT memory image, the KEPT
                                   * image an earlier frame's pass left under its Data word (exact size, pitch and Format), else the GUEST MEMORY under it read at the
                                   * present as A8R8G8B8 (INFERRED: the bytes at the present are the bytes at the draw)
                                   * (the host never writes a GPU result there: MEASURED ARGB 0 for every texel of the back buffer on the retail boots, so a
                                   * never drawn surface is transparent black). Refused BY NAME: a kept image whose size, pitch or Format differs from the binding's, a kept image
                                   * over guest memory that is not zero (the CPU wrote it, the order against the pass is unobserved), an alias of another kept surface at a different offset, an
                                   * unreadable range, and (with a blit before the draw in the frame) a surface a blit of the frame wrote before the draw reads it
                                   * (in-frame blit results are not fed to draws). Also feeds the CopyRects blits over surfaces the replay holds no image of: a
                                   * source or destination is built from the guest rows it touches, and the Y8 and R5G6B5 byte path blits (and any of the three formats between
                                   * byte surfaces) are SRCCOPY over guest byte surfaces, refusing a clamped row and an overlap by name (HQ58). Needs `render_target_texture` and the BLIT output
                                   * group, and D3D8_SWAP_REPLAY_INFER_SURFACE_SOURCE. Default off */
    bool target_persist;          /* T633 (2), OPT-IN, MEASURED in xemu (T736, xemu-level: a pass without a clear starts from the previous image): a render target pass starts from the kept image its Data word held after an earlier frame's pass
                                   * (and the blits since) instead of a fresh image of the clear colour: a title that does not clear every frame draws over its previous
                                   * frame. A surface with no kept image starts fresh as ever. Refused BY NAME: a kept image whose size, pitch or Format differs from
                                   * the target's, one over guest memory that is not zero (a CPU write), and `flip_y`. Needs D3D8_SWAP_REPLAY_INFER_TARGET_PERSIST. Default off */
    uint32_t dump_every;          /* T484: with a dump directory, write the frames whose present number is a multiple
                                   * of this. 0 (default) = every replayed frame unless `dump_last` is set */
    bool dump_last;               /* T484: write the last replayed frame (and its offscreen targets) at
                                   * d3d8_swap_replay_dump_last, once, unless the cadence already wrote it. With
                                   * `dump_every` 0 nothing else is written */
    uint32_t output_groups;       /* T267: GPU_PGRAPH_OUTPUT_* groups the model decodes AND the replay applies
                                   * (default 0, nothing: those methods stay unhandled and strict refuses
                                   * them). Bits that are not an output group are refused by enable. Each
                                   * group's inferences (GPU_PGRAPH_INFER_OUTPUT_*) are still allowed by
                                   * `allowed_inferences`, and are NOT in GPU_PGRAPH_INFER_ALL */
} d3d8_swap_replay_config;

typedef struct {
    uint64_t presents;            /* Swap(4) calls seen while enabled */
    uint64_t frames_replayed;     /* frames whose presented target was drawn and replayed */
    uint64_t frames_offscreen_only; /* frames with draws only into targets other than the presented one */
    uint64_t passes_replayed;     /* per-target images replayed (the presented one and the others) */
    uint64_t frames_empty;        /* frames with no draw in them (not replayed) */
    uint64_t frames_refused;      /* frames the decoder or the replay refused (at most 1, it latches) */
    uint64_t frames_skipped;      /* frames after the latch */
    uint64_t model_resets;        /* times the model was rebuilt because d3d8_gpu_reset ran (T84a3) */
    uint64_t draws;               /* draws replayed, over all passes */
    uint64_t commands_decoded;
    uint64_t dumps_written;
    uint64_t dump_failures;
    uint64_t draws_snapshotted;   /* T262: draws whose vertex bytes were copied at the kick */
    uint64_t vertex_bytes_captured; /* T262: snapshot bytes copied out of guest memory, all frames */
    uint64_t clears_not_replayed; /* T267: clear events (0x1D94) no replayed pass applied: in a pass with no draw,
                                   * or before the first pass that has one. Counted and logged, never applied to
                                   * another render target. 0 unless output_groups has the clear group */
    uint64_t copies_applied;      /* T578: 2D engine blits (CopyRects) that moved pixels between two surface images */
    uint64_t copies_empty;        /* T578: blits of a zero width or height, nothing moves (INFERRED from xemu) */
    uint64_t copies_pending;      /* T578: blits the model decoded that no present has applied yet (CopyRects after the last Swap) */
    uint64_t surfaces_evicted;    /* T578: stored surface images dropped to make room (more than D3D8_SWAP_REPLAY_SURFACES live) */
    uint64_t surface_kept_samples;  /* T633 (1): (draw, stage) pairs that sampled a kept image of an earlier frame */
    uint64_t surface_guest_samples; /* T633 (1): (draw, stage) pairs that sampled the guest memory under a surface no replayed pass ever drew */
    uint64_t surface_guest_zero_samples; /* T633 (1): of those, the samples whose bytes were all zero (transparent black, MEASURED) */
    uint64_t targets_persisted;     /* T633 (2): passes that started from a kept image instead of a fresh one */
    uint64_t blit_guest_surfaces;   /* T596: blit surfaces built from guest memory because the replay held no image of them */
    uint64_t byte_copies_applied;   /* T596: Y8, R5G6B5 and byte surface blits moved over guest byte surfaces */
    uint64_t frames_under_overlay;  /* T633 (3): replayed frames (presented or offscreen) while the overlay was on (UpdateOverlay seen, EnableOverlay odd): the frame is the framebuffer under it, the overlay picture is not composed */
    uint64_t frames_under_color_key; /* T633 (3): of those, the frames whose last UpdateOverlay ENABLED the colour key (never measured, REFUSED by name; the title's own calls pass it disabled) */
    uint64_t rt_texture_draws;    /* T510: (draw, stage) pairs whose combiner sampled a render target image of an earlier pass */
    uint64_t census_frames;       /* T510: frames classified by the census (census mode replays none) */
    uint64_t census_draws;        /* T510: draws classified, each exactly once */
    uint64_t texture_bindings;    /* T510: SetTexture calls the replay noted (a repeat of the stage's current binding is not noted) */
    uint64_t standin_draws;       /* T497: replayed draws whose combiner sampled the stand-in texture (not the title's) */
    uint64_t offset_applied;      /* T511/T552: replayed TRIANGLE draws that ran with a Vulkan depth bias (polygon offset, report.offset_applied) */
    uint64_t offset_unobserved;   /* T511/T552: replayed draws with an enabled non-zero polygon offset that cannot show (no depth test,
                                   * or a line or point list, which is never biased): counted, no
                                   * effect (report.offset_unobserved). Both 0 unless output_groups has the polygon offset group */
    uint64_t snapshots_peak;      /* T1268: most state snapshots, draws and indices ONE frame held (all frames, kept across model */
    uint64_t draws_peak;          /*   rebuilds), against GPU_PGRAPH_MAX_SNAPSHOTS, _MAX_DRAWS and _MAX_INDICES: the headroom */
    uint64_t indices_peak;
    uint64_t pairs_ignored;       /* T511: pairs of the IGNORED output group (dither, 09F8) decoded and skipped on purpose, all
                                   * models (gpu_pgraph_stats.pairs_ignored, folded over a d3d8_gpu_reset). They change no pixel */
    uint64_t unhandled_methods;   /* distinct unhandled methods the CURRENT model counted */
    uint32_t used_inferences;     /* GPU_PGRAPH_INFER_* bits some replayed draw depended on */
    uint32_t last_width;          /* size of the last presented frame */
    uint32_t last_height;
    bool latched;
    char error[512];              /* the refusal that latched, "" otherwise */
} d3d8_swap_replay_stats;

/** A config with the defaults: strict, opaque black, no inference allowed, nothing dumped, vertex
 * bytes snapshotted at the kick within D3D8_SWAP_REPLAY_DEFAULT_VERTEX_BUDGET. */
d3d8_swap_replay_config d3d8_swap_replay_default_config(void);

/**
 * T478: the inference bits the host's replay flags allow, so that `main.c` (not a ctest binary) holds no mask of its own. Always
 * GPU_PGRAPH_INFER_ALL, plus GPU_PGRAPH_INFER_EXECUTION_MODE_UNWRITTEN for `--gpu-replay-assume-program-mode`,
 * GPU_PGRAPH_INFER_OUTPUT_ALL for `--gpu-replay-output-state`, GPU_PGRAPH_INFER_COMBINER_REPLAY for
 * `--gpu-replay-combiner`, GPU_PGRAPH_INFER_VIEWPORT_FROM_TARGET for the T477 target viewport and (T497)
 * D3D8_SWAP_REPLAY_INFER_STANDIN_TEXTURE for `--gpu-replay-standin-texture`.
 */
uint32_t d3d8_swap_replay_host_inferences(bool assume_program_mode, bool output_state, bool combiner,
                                          bool viewport_from_target, bool standin_texture);

/**
 * Turn the replay on. False with `error` filled (when not NULL) for a missing or empty module
 * directory. Replaces any earlier enable. The Vulkan device opens at the first replayed frame, and
 * failing to open one is a loud refusal then, never here.
 */
bool d3d8_swap_replay_enable(const d3d8_swap_replay_config *config, char *error,
                             size_t error_size);

/**
 * The configuration as enable took it (strings point at copies owned here). All zero when not
 * enabled. For reporting, and so a caller can check what an option parser handed over.
 */
d3d8_swap_replay_config d3d8_swap_replay_get_config(void);

/** Turn it off and free everything (the model, the device, the modules, the last frame). */
void d3d8_swap_replay_disable(void);

bool d3d8_swap_replay_enabled(void);

/** The counters since enable. All zero when not enabled. */
d3d8_swap_replay_stats d3d8_swap_replay_get_stats(void);

/**
 * T497: one line saying what the stand-in texture is and how much of the replay used it. Empty (returns 0, `text` is "")
 * when the option is off. Otherwise it names the size and colour, says in so many words that it is NOT the title's
 * texture and that the frames which sampled it are not renderings of the title, lists the inferences, and counts
 * the replayed draws that sampled it against all replayed draws. Truncated to `size` (always NUL terminated). Returns
 * the length written. The host prints it at the end of the run.
 */
size_t d3d8_swap_replay_standin_summary(char *text, size_t size);

/**
 * T510: called by SetTexture (0x003D4070, d3d8_set_texture) with the stage and the header bound (0 unbinds), after the device state
 * changed. No-op unless enabled with `render_target_texture`. It hands the ring to the consumer (a drain, no fence completes) so the
 * binding is placed between the draws recorded so far and the next one, and notes the header's words as they are now (the header may
 * be released or reused by the present). A repeat of the stage's current binding is not noted. A frame with more than
 * D3D8_SWAP_REPLAY_TEXTURE_EVENTS bindings is refused by name at the present.
 */
void d3d8_swap_replay_on_texture(uint32_t stage, uint32_t texture);

/**
 * T510: what each render target texture binding resolved to, over the run: the distinct source lines (a render target header, its Data, size
 * and Format, "drawn by an earlier pass of the same frame") with the number of (draw, stage) samples each had. `text` is truncated to `size`.
 * False past the count. The host prints them at the end. Empty when the option is off.
 */
size_t d3d8_swap_replay_texture_census_count(void);
bool d3d8_swap_replay_texture_census_at(size_t index, char *text, size_t size, uint64_t *draws);

/** T510: one line saying what the render target texture option does and how much of the replay used it, INFERRED items called out. Empty
 * (returns 0) when the option is off. */
size_t d3d8_swap_replay_rt_texture_summary(char *text, size_t size);

/**
 * T633, T596: the surface model's announcement and counters, one line, empty (returns 0) when none of `surface_source` and `target_persist` is on and
 * no frame was replayed under the overlay. States each choice with its INFERRED label, and the overlay colour key (T537): MEASURED disabled in
 * the title's own calls (so no effect), an enabled key REFUSED by name, and the overlay picture NOT composed over the replayed frames (they are the
 * framebuffer under it, not the displayed picture); the xemu measurement that would decide it is HQ60.
 */
size_t d3d8_swap_replay_surface_summary(char *text, size_t size);

/** The name of the Vulkan device in use, or NULL before the first replayed frame opened one. */
const char *d3d8_swap_replay_device_name(void);

/** The last replayed frame, or NULL when none has been. Owned here, valid until the next frame. */
const gpu_image *d3d8_swap_replay_last_frame(void);

/* Observe each fresh image replayed directly from the live in-memory pushbuffer. The hook is
 * called synchronously at the title's present and the image remains valid through the callback. */
typedef void (*d3d8_swap_replay_frame_hook)(const gpu_image *image, uint64_t frame, void *context);
void d3d8_swap_replay_set_frame_hook(d3d8_swap_replay_frame_hook hook, void *context);

/** The offscreen images of the last replayed frame: one per other render target drawn into (a texture
 * the title rendered to), in the order they were drawn. NULL past the count. `target_header` (when
 * not NULL) gets the guest header of the target. Owned here, valid until the next replayed frame. */
size_t d3d8_swap_replay_offscreen_count(void);
const gpu_image *d3d8_swap_replay_offscreen_frame(size_t index, uint32_t *target_header);

/** T578: the kept image of the surface whose header Data word is `data` (the name a 2D blit gives it), as the newest replayed
 * pass left it and the blits since changed it, or NULL (always NULL without the BLIT output group). Owned here, valid until the
 * next replayed frame. */
const gpu_image *d3d8_swap_replay_surface_image(uint32_t data);

/**
 * Called by SetRenderTarget (0x003D3800, d3d8_set_render_target) with the target bound before and the
 * one it binds, `target` already resolved (a null argument keeps the current one, so previous ==
 * target and nothing happens). No-op when not enabled. Closes the pass: it hands the ring to the
 * consumer (a drain, no fence completes) so the recording up to this point is what ran against
 * `previous`, and notes the recording index with both targets' measured sizes.
 */
void d3d8_swap_replay_on_render_target(uint32_t previous, uint32_t target);

/** T484: with `dump_last`, write the last replayed frame (and the offscreen targets of the last frame that had
 * any) that the cadence has not already written. Safe to call twice (the second writes nothing). No-op when
 * not enabled, with no dump directory, or without `dump_last`. The host calls it when the run ends. */
void d3d8_swap_replay_dump_last(void);

/** T838 (M9 host hook-up): the live renderer's seam. The hook runs at each present of a frame the decoder accepted, with the model
 * while its draw list is still the frame's (before gpu_pgraph_begin_frame clears it), on the guest thread that called Swap(4).
 * `live_binding` is the SetTexture history (the binding stage `stage` held at draw `draw` of that frame), false for a stage that
 * was never bound or is unbound, `*reason` then a stable string. `live_backend` is the replay's own backend (tables, module loaders,
 * allowed inferences, output groups, combiner) WITHOUT a texture provider, valid until d3d8_swap_replay_disable; false when the
 * replay is not enabled. Setting a hook also makes SetTexture noted (as `render_target_texture` does). */
typedef void (*d3d8_swap_replay_live_hook)(const gpu_pgraph *model, uint64_t frame, void *context);
void d3d8_swap_replay_set_live_hook(d3d8_swap_replay_live_hook hook, void *context);
bool d3d8_swap_replay_live_binding(size_t draw, uint32_t stage, uint32_t *header, uint32_t *format, uint32_t *size_word, uint32_t *data,
                                   const char **reason);
/* T1246: a copy of the SetTexture history taken at the Swap (inside the live model hook), answered like d3d8_swap_replay_live_binding
 * but from any thread afterwards. */
typedef struct d3d8_swap_replay_live_bindings d3d8_swap_replay_live_bindings;
d3d8_swap_replay_live_bindings *d3d8_swap_replay_live_bindings_create(void);
void d3d8_swap_replay_live_bindings_destroy(d3d8_swap_replay_live_bindings *copy);
void d3d8_swap_replay_live_bindings_capture(d3d8_swap_replay_live_bindings *copy);
bool d3d8_swap_replay_live_bindings_lookup(const d3d8_swap_replay_live_bindings *copy, size_t draw, uint32_t stage, uint32_t *header,
                                           uint32_t *format, uint32_t *size_word, uint32_t *data, const char **reason);
bool d3d8_swap_replay_live_backend(gpu_pgraph_backend *out);
/** Frames whose model was handed to the live hook. */
uint64_t d3d8_swap_replay_live_frames(void);
/** T838: the render target header names noted by SetRenderTarget are reported to this hook (header addresses, 0 for none), so the live
 * renderer can register them as T793 targets. Same thread as the caller of SetRenderTarget. */
typedef void (*d3d8_swap_replay_live_target_hook)(uint32_t header, void *context);
void d3d8_swap_replay_set_live_target_hook(d3d8_swap_replay_live_target_hook hook, void *context);

/** T847: a maker for the modules of the `d3d8_swap_replay_live_backend` that are in no table (gpu_pgraph_module_make_fn, e.g.
 * live_module_maker_make). When it returns true the module's `<name>.spv` is in the replay's module directory and the swap replay adds
 * the name to its vertex or fragment table, the lookup is retried. NULL (the default) removes it: a miss stays a refusal. */
void d3d8_swap_replay_set_live_module_maker(gpu_pgraph_module_make_fn maker, void *context);

/** Called by d3d8_swap at the end of Swap(4), after the frame record. No-op when not enabled. */
void d3d8_swap_replay_on_present(uint64_t frame_number);

#endif
