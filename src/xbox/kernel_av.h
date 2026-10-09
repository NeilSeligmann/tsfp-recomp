/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The audio/video kernel exports: ordinals 1 AvGetSavedDataAddress, 2
 * AvSendTVEncoderOption, 3 AvSetDisplayMode and 4 AvSetSavedDataAddress.
 *
 * ORDINAL NUMBERS, RESOLVED NOT RECALLED, out of `tools/kernel_ordinals.py` lines 59-62.
 * None is in `SUSPECT_ON_XDK_5849`.
 *
 * THIS MODULE FABRICATES HARDWARE. There is no AV encoder and no console. Every answer
 * `AvSendTVEncoderOption` gives to a query is a claim about hardware that does not exist,
 * so the claim is an operator-visible choice (`--av-pack`), derived from the XBE where
 * the binary can derive it, logged when made, and counted. The evidence for each value is
 * in docs/av-policy.md, labelled MEASURED or INFERRED. This header carries only what a
 * caller needs.
 *
 * SIGNATURES (all stdcall), arities independent of the measured table
 *
 *     ULONG    AvGetSavedDataAddress(void);                                    0 args
 *     NTSTATUS AvSendTVEncoderOption(PVOID RegisterBase, ULONG Option,
 *                                    ULONG Param, ULONG *Result);             4 args
 *     ULONG    AvSetDisplayMode(PVOID RegisterBase, ULONG Step, ULONG Mode,
 *                               ULONG Format, ULONG Pitch, PVOID FrameBuffer); 6 args
 *     void     AvSetSavedDataAddress(PVOID Address);                           1 arg
 *
 * The 4 counts come from a forced stack balance, not from push counting. A solver walks
 * the one function holding AvSetDisplayMode, AvGetSavedDataAddress, AvSetSavedDataAddress
 * and three AvSendTVEncoderOption call sites (0x003D8450) and requires the stack to be
 * level at every join and at the `ret 4`. Over arities 0..8 for ordinals 3 and 2, 0..4 for
 * ordinal 4 and 0..2 for ordinal 1, exactly one assignment balances: 6, 4, 1 and 0.
 * AvSetSavedDataAddress has ONE call site and AvSetDisplayMode has none the scanner sees
 * (both calls are `call edi`), so neither is covered by the 3-site quorum in
 * `stack_args_for()`.
 *
 * WHAT IS REAL AND WHAT IS NOT
 *   - Option 6 (query AV capabilities), 0xF (query current field) and 0x10 (query encoder
 *     type) write a FABRICATED answer to *Result.
 *   - Options 9, 0xB and 0xE are recorded and have NO effect: there is no encoder.
 *   - AvSetDisplayMode records its arguments and reports the mode set finished in one
 *     step. It programs no CRTC, scans nothing out and waits for no vertical blank. The
 *     recorded FrameBuffer, Pitch and Format are the scanout surface, exposed through
 *     kernel_av_display() for the GPU path.
 *   - AvGetSavedDataAddress answers 0 unless something was stored: a cold boot leaves no
 *     framebuffer behind. This title only ever clears the address (see docs/av-policy.md).
 *   - Any option the title is not known to send is REFUSED, not accepted quietly. So is any
 *     Param it is not known to send with a known option (6, 0xF, 0x10: 0. 9: 0 or 1.
 *     0xB: 0 or 5. 0xE: 0, all MEASURED at the nine call sites), and so is a result pointer
 *     on 9, 0xB or 0xE, which the title always passes as NULL.
 */

#ifndef TSFP_XBOX_KERNEL_AV_H
#define TSFP_XBOX_KERNEL_AV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel_call.h"
#include "kernel_hle.h"

#define KERNEL_AV_ORD_GET_SAVED_DATA 1u
#define KERNEL_AV_ORD_SEND_TV_ENCODER_OPTION 2u
#define KERNEL_AV_ORD_SET_DISPLAY_MODE 3u
#define KERNEL_AV_ORD_SET_SAVED_DATA 4u

/* The data export HalBootSMCVideoMode. The title reads it through XGetVideoFlags. The
 * guest window slot is the HOST's to write, see kernel_av_smc_video_mode(). */
#define KERNEL_AV_ORD_HAL_BOOT_SMC_VIDEO_MODE 356u

/* Options this title sends, from the 9 call sites. */
#define KERNEL_AV_OPTION_QUERY_AV_CAPABILITIES 6u
#define KERNEL_AV_OPTION_BLANK_SCREEN 9u
#define KERNEL_AV_OPTION_FLICKER_FILTER 0xBu
#define KERNEL_AV_OPTION_SOFT_DISPLAY_FILTER 0xEu
#define KERNEL_AV_OPTION_QUERY_FIELD 0xFu
#define KERNEL_AV_OPTION_QUERY_ENCODER_TYPE 0x10u

/* The capability word the option 6 answer is built from. Low byte: pack. Bits 8-15:
 * standard. Bits 16-31: capability flags. The title tests every one of these bits. */
#define KERNEL_AV_PACK_MASK 0x000000FFu
#define KERNEL_AV_STANDARD_MASK 0x0000FF00u
#define KERNEL_AV_STANDARD_NTSC_M 0x00000100u
#define KERNEL_AV_STANDARD_NTSC_J 0x00000200u
#define KERNEL_AV_STANDARD_PAL_I 0x00000300u
#define KERNEL_AV_FLAG_480P 0x00080000u
#define KERNEL_AV_FLAG_60HZ 0x00400000u
#define KERNEL_AV_FLAG_50HZ 0x00800000u

/* AV pack codes. Only 3, 4 and 5 have rows in the title's mode table, so every other
 * value takes the same composite fallback block. Pack 5 (VGA) is deliberately NOT offered:
 * none of its rows matches the pixel-aspect flag the title always requests (av-policy.md). */
#define KERNEL_AV_PACK_CODE_STANDARD 1u
#define KERNEL_AV_PACK_CODE_HDTV 4u
#define KERNEL_AV_PACK_CODE_SVIDEO 6u

/* XBE certificate game-region bits. */
#define KERNEL_AV_REGION_NA 0x1u
#define KERNEL_AV_REGION_JAPAN 0x2u
#define KERNEL_AV_REGION_REST_OF_WORLD 0x4u

/* ExQueryNonVolatileSetting indices the title reads for the same claim. */
#define KERNEL_AV_SETTING_VIDEO_FLAGS 8u
#define KERNEL_AV_SETTING_AV_REGION 0x103u

/* The bounded set the operator chooses from. */
typedef enum {
    KERNEL_AV_PACK_COMPOSITE = 0,
    KERNEL_AV_PACK_SVIDEO,
    KERNEL_AV_PACK_HDTV,
    KERNEL_AV_PACK_COUNT
} kernel_av_pack;

/* What the title was last told to display. `valid` is false until AvSetDisplayMode runs. */
typedef struct {
    bool valid;
    uint32_t register_base;
    uint32_t mode;
    uint32_t format;
    uint32_t pitch;
    uint32_t frame_buffer;
    unsigned calls;
} kernel_av_display;

/** Register ordinals 1 to 4. Returns how many bound. */
unsigned kernel_av_register(void);

/** Forget the configuration, the saved address, the display record and the counters. */
void kernel_av_reset(void);

/** Look a pack up by name ("composite", "svideo", "hdtv"). False for any other. */
bool kernel_av_parse_pack(const char *name, kernel_av_pack *out);

/** The name `kernel_av_parse_pack` accepts for `pack`, or "?" out of range. */
const char *kernel_av_pack_name(kernel_av_pack pack);

/** The default, derived from the title's own highest requested mode: see av-policy.md. */
kernel_av_pack kernel_av_default_pack(void);

/**
 * Choose the fabricated AV answer and say so in the log.
 *
 * `certificate_region` is the XBE certificate's game-region word. The video standard is
 * derived from it (NA gives NTSC-M, Japan NTSC-J, rest of world PAL-I, in that priority
 * when several bits are set). False, with nothing changed, when no region bit is set.
 * Until this succeeds an option 6 query is REFUSED rather than answered with a default.
 */
bool kernel_av_configure(kernel_av_pack pack, uint32_t certificate_region);

/**
 * The same, reading the certificate region out of the mapped XBE header at `image_base`.
 *
 * Reads the "XBEH" magic, SizeOfHeaders at +0x108, the certificate address at +0x118 and
 * the game-region word at certificate +0xA0, and requires the certificate to lie inside
 * the header region. False, with nothing changed and the reason logged, otherwise.
 * src/xbox cannot link src/loader, so the host passes the base.
 */
bool kernel_av_configure_from_image(kernel_av_pack pack, kernel_guest_ptr image_base);

/** Whether `kernel_av_configure` has succeeded since the last reset. */
bool kernel_av_is_configured(void);

/** The option 6 answer: pack | standard | capability flags. 0 when not configured. */
uint32_t kernel_av_capabilities(void);

/** The dword for ExQueryNonVolatileSetting index 0x103 (XGetVideoStandard reads byte 1). */
uint32_t kernel_av_av_region_setting(void);

/** The dword for ExQueryNonVolatileSetting index 8 (XGetVideoFlags reads bits 16..22). */
uint32_t kernel_av_video_flags_setting(void);

/**
 * The value `*HalBootSMCVideoMode` must hold for the title to see the same claim: 1 is
 * the only value under which XGetVideoFlags keeps the 480p bit. The word lives in the
 * host's guest thunk window, which src/xbox cannot name, so the HOST writes it.
 */
uint32_t kernel_av_smc_video_mode(void);

/**
 * Store the two ExQueryNonVolatileSetting values through the config module, so the
 * title's three routes to the same claim agree. Returns how many were stored (0 or 2).
 */
unsigned kernel_av_install_settings(void);

/** The address AvSetSavedDataAddress last stored, or 0. */
kernel_guest_ptr kernel_av_saved_data_address(void);

/** The last AvSetDisplayMode request. False until one arrives. */
bool kernel_av_display_get(kernel_av_display *out);

/** Answers written to a query: a count of FABRICATED hardware claims made. */
unsigned kernel_av_fabricated_count(void);

/** Calls refused: unreadable arguments, unknown options, unconfigured queries. */
unsigned kernel_av_refused_count(void);

/** Option 9, 0xB and 0xE calls accepted and ignored, because there is no encoder. */
unsigned kernel_av_ignored_option_count(void);

#endif /* TSFP_XBOX_KERNEL_AV_H */
